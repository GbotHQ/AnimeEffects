#include <QPainter>
#include <algorithm>
#include <cmath>
#include "cmnd/ScopedMacro.h"
#include "core/Constant.h"
#include "core/FilterMask.h"
#include "core/FolderNode.h"
#include "core/LayerNode.h"
#include "core/TimeKeyExpans.h"
#include "core/TimeLineEvent.h"
#include "ctrl/AssignPixels.h"
#include "ctrl/CmndName.h"
#include "ctrl/MaskEditor.h"
#include "ctrl/TimeLineUtil.h"

using namespace core;

namespace ctrl {

MaskEditor::MaskEditor(core::Project& aProject):
    mProject(aProject), mTarget(nullptr), mParam(), mStroke(), mCursorPos(), mPainting(false),
    mCommand(nullptr) {}

void MaskEditor::setParam(const MaskParam& aParam) {
    const float value = aParam.value;
    mParam = aParam;
    // the panel owns `value`; a channel change is the panel's to re-seed (see
    // MaskParam::setChannel), so nothing is forced here
    mParam.value = value;
}

bool MaskEditor::setTarget(core::ObjectNode* aTarget) {
    if (mPainting)
        endStroke();
    mTarget = aTarget;
    return contentRect().isValid();
}

QRectF MaskEditor::contentRect() const {
    if (!mTarget || !mTarget->timeLine())
        return QRectF();

    if (mTarget->type() == core::ObjectType_Layer) {
        // a layer's mask covers its IMAGE pixels - the space the filter pass maps
        // with, so the field sticks to the artwork
        const gl::Texture* texture = mTarget->timeLine()->current().areaTexture();
        if (!texture || texture->size().isEmpty())
            return QRectF();
        return QRectF(QPointF(0.0, 0.0), QSizeF(texture->size()));
    }

    // A folder has no image: its content space IS the canvas (a folder's local
    // frame at identity), so the mask covers the project canvas.
    const QSize canvas = mProject.attribute().imageSize();
    if (canvas.isEmpty())
        return QRectF();
    return QRectF(QPointF(0.0, 0.0), QSizeF(canvas));
}

bool MaskEditor::toContent(const QVector2D& aWorldPos, QVector2D& aContentPos) const {
    if (!mTarget || !mTarget->timeLine())
        return false;

    auto& expans = mTarget->timeLine()->current();
    QMatrix4x4 world = expans.srt().worldCSRTMatrix();
    if (mTarget->type() == core::ObjectType_Layer) {
        if (expans.bone().isAffectedByBinding())
            world = expans.bone().outerMatrix();
    }

    bool invertible = false;
    const QMatrix4x4 inv = world.inverted(&invertible);
    if (!invertible)
        return false;

    QVector2D local = (inv.map(QVector3D(aWorldPos))).toVector2D();
    if (mTarget->type() == core::ObjectType_Layer)
        local -= expans.imageOffset(); // the image box starts at its own (0, 0)
    aContentPos = local;
    return true;
}

float MaskEditor::contentScale() const {
    if (!mTarget || !mTarget->timeLine())
        return 1.0f;
    auto& expans = mTarget->timeLine()->current();
    QMatrix4x4 world = expans.srt().worldCSRTMatrix();
    if (mTarget->type() == core::ObjectType_Layer && expans.bone().isAffectedByBinding())
        world = expans.bone().outerMatrix();
    const QVector2D origin = (world.map(QVector3D(0.0f, 0.0f, 0.0f))).toVector2D();
    const QVector2D unit = (world.map(QVector3D(1.0f, 0.0f, 0.0f))).toVector2D();
    const float scale = (unit - origin).length();
    return scale > 1.0e-6f ? scale : 1.0f;
}

core::FilterMask* MaskEditor::ensureMask() {
    if (!mTarget || !mTarget->timeLine())
        return nullptr;
    const QRectF rect = contentRect();
    if (rect.isEmpty())
        return nullptr;
    if (!mTarget->timeLine()->hasFilter(mParam.lane))
        return nullptr; // no such filter on this node: nothing would consume the mask
    return &mTarget->timeLine()->ensureFilterMask(mParam.lane, mParam.channel, rect);
}

core::FilterMask* MaskEditor::currentMask() {
    if (!mTarget || !mTarget->timeLine())
        return nullptr;
    return mTarget->timeLine()->filterMask(mParam.lane, mParam.channel);
}

void MaskEditor::clearMask() {
    core::FilterMask* mask = currentMask();
    if (!mask || mask->isEmpty())
        return;

    QImage& image = mask->image();
    cmnd::Stack& stack = mProject.commandStack();
    cmnd::ScopedMacro macro(stack, CmndName::tr("Clear filter mask"));
    auto* notifier = new TimeLineUtil::Notifier(mProject);
    notifier->event().setType(TimeLineEvent::Type_FilterStack);
    macro.grabListener(notifier);

    auto* command = new AssignPixels(*mask);
    command->captureBefore();
    mask->clear(); // back to the channel's neutral value
    command->addAfter(image.rect());
    stack.push(command);

    TimeLineEvent event;
    event.setType(TimeLineEvent::Type_FilterStack);
    mProject.onTimeLineModified(event, false);
}

bool MaskEditor::updateCursor(const CameraInfo&, const AbstractCursor& aCursor) {
    if (!mTarget)
        return false;

    mCursorPos = aCursor.worldPos();
    if (!contentScale())
        return false;

    if (aCursor.emitsLeftPressedEvent()) {
        mCommand = nullptr;
        beginStroke();
    } else if (aCursor.emitsLeftDraggedEvent()) {
        if (mPainting)
            mStroke.move(mCursorPos, aCursor.pressure());
    } else if (aCursor.emitsLeftReleasedEvent()) {
        endStroke();
    }
    return true;
}

void MaskEditor::beginStroke() {
    core::FilterMask* mask = ensureMask();
    if (!mask)
        return;

    // the brush is sized in WORLD pixels (what every tool panel shows); the field
    // lives in content pixels
    BrushParam brush = mParam.brush;
    brush.radius = std::max(mParam.brush.radius / contentScale(), 0.5f);

    QVector2D content;
    if (!toContent(mCursorPos, content))
        return;

    mPainting = true;
    // the stroke owns its own copy of the brush (BrushStroke::begin takes the param by
    // value), so the sink only needs `this`
    mStroke.begin(brush, content, 1.0f, [this](const BrushStamp& aStamp) { paintStamp(aStamp); });
}

void MaskEditor::endStroke() {
    mStroke.end();
    mPainting = false;
    mCommand = nullptr;
}

bool MaskEditor::paintStamp(const BrushStamp& aStamp) {
    core::FilterMask* mask = currentMask();
    if (!mask || mask->isEmpty())
        return false;

    QImage& image = mask->image();
    if (image.isNull())
        return false;

    // the stamp is in CONTENT pixels already (beginStroke converted it), and the
    // field's texels are that rect scaled: one texel is contentRect/size pixels
    const QRectF rect = mask->contentRect();
    const float texelsPerPixelX = float(image.width()) / float(rect.width());
    const float texelsPerPixelY = float(image.height()) / float(rect.height());

    const QVector2D centerTexel(
        (aStamp.center.x() - rect.x()) * texelsPerPixelX - 0.5f,
        (aStamp.center.y() - rect.y()) * texelsPerPixelY - 0.5f
    );
    const float radiusTexels = aStamp.radius * texelsPerPixelX;

    const QRect box(
        int(std::floor(centerTexel.x() - radiusTexels)), int(std::floor(centerTexel.y() - radiusTexels)),
        int(std::ceil(radiusTexels * 2.0f)) + 2, int(std::ceil(radiusTexels * 2.0f)) + 2
    );
    const QRect clipped = box.intersected(image.rect());
    if (clipped.isEmpty())
        return false;

    // the FIRST stamp of a stroke opens the undo step: the field before the stroke
    // plus the pixels it writes are one command (ctrl/AssignPixels.h)
    if (!mCommand) {
        cmnd::Stack& stack = mProject.commandStack();
        cmnd::ScopedMacro macro(stack, CmndName::tr("Paint filter mask"));
        auto* notifier = new TimeLineUtil::Notifier(mProject);
        notifier->event().setType(TimeLineEvent::Type_FilterStack);
        macro.grabListener(notifier);
        mCommand = new AssignPixels(*mask);
        mCommand->captureBefore();
        stack.push(mCommand);
    }

    const float amount = std::min(std::max(aStamp.amount * mParam.brush.opacity, 0.0f), 1.0f);
    const float target = std::min(std::max(mParam.value, 0.0f), 1.0f);
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        uchar* line = image.scanLine(y);
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QVector2D texel(float(x) + 0.5f, float(y) + 0.5f);
            const float distance = (texel - centerTexel).length();
            const float weight = brushFalloff(distance, radiusTexels, aStamp.hardness) * amount;
            if (weight <= 0.0f)
                continue;
            float current = float(line[x]) / 255.0f;
            // move TOWARD the brush's value: repeated stamps converge instead of
            // overshooting, and the same gesture works for decreasing the amount
            // (toward 0) and for adding a delay (toward 1)
            current = current + (target - current) * weight;
            line[x] = uchar(std::min(std::max(current * 255.0f, 0.0f), 255.0f));
        }
    }

    mask->markDirty(clipped);
    mCommand->addAfter(clipped);

    // the field changed: announce it so the viewport repaints the effect
    TimeLineEvent event;
    event.setType(TimeLineEvent::Type_FilterStack);
    mProject.onTimeLineModified(event, false);
    return true;
}

const core::FilterMask* MaskEditor::previewMask() const {
    if (!mTarget || !mTarget->timeLine())
        return nullptr;
    const core::FilterMask* mask = mTarget->timeLine()->filterMask(mParam.lane, mParam.channel);
    if (!mask || mask->isEmpty())
        return nullptr;
    return mask;
}

float MaskEditor::sampleValue(const QVector2D& aWorldPos) const {
    if (!mTarget || !mTarget->timeLine())
        return -1.0f;
    const core::FilterMask* mask = mTarget->timeLine()->filterMask(mParam.lane, mParam.channel);
    if (!mask || mask->isEmpty())
        return -1.0f;

    QVector2D content;
    if (!const_cast<MaskEditor*>(this)->toContent(aWorldPos, content))
        return -1.0f;

    const QRectF rect = mask->contentRect();
    if (!rect.contains(content.toPointF()))
        return -1.0f;

    const QImage& image = mask->image();
    const int x = int((content.x() - rect.x()) / rect.width() * image.width());
    const int y = int((content.y() - rect.y()) / rect.height() * image.height());
    if (image.rect().contains(x, y))
        return float(image.constScanLine(y)[x]) / 255.0f;
    return -1.0f;
}

void MaskEditor::updateEvent(EventType) {
    if (mPainting)
        endStroke();
}

void MaskEditor::renderQt(const RenderInfo& aInfo, QPainter& aPainter) {
    if (!mTarget)
        return;

    // the brush: SIZE as the outer circle, HARD CORE as the inner ring, both in the
    // same world units the panel shows
    const QVector2D center = aInfo.camera.toScreenPos(mCursorPos);
    const float radius = aInfo.camera.toScreenLength(mParam.brush.radius);
    if (radius <= 0.0f)
        return;

    const QColor idleColor(100, 100, 255, 128);
    const QColor focusColor(255, 255, 255, 255);
    const QBrush brush(mPainting ? focusColor : idleColor);
    aPainter.setPen(QPen(brush, 1.2f, mParam.brush.erase ? Qt::DotLine : Qt::SolidLine));
    aPainter.setBrush(Qt::NoBrush);
    aPainter.drawEllipse(center.toPointF(), radius, radius);

    const float core = aInfo.camera.toScreenLength(mParam.brush.radius * mParam.brush.hardness);
    if (core > 1.0f) {
        QColor coreColor = brush.color();
        coreColor.setAlpha(96);
        aPainter.setPen(QPen(QBrush(coreColor), 1.0f));
        aPainter.drawEllipse(center.toPointF(), core, core);
    }
}

} // namespace ctrl
