#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QKeyEvent>
#include <algorithm>
#include "core/TimeKeyExpans.h"
#include "core/KeyEasing.h"
#include "core/BlurKey.h"
#include "core/TimeFormat.h"
#include "cmnd/BasicCommands.h"
#include "cmnd/ScopedMacro.h"
#include "ctrl/CmndName.h"
#include "gui/TimeLineEditorWidget.h"
#include "gui/MouseSetting.h"
#include "gui/obj/obj_Item.h"
#include "gui/prop/prop_Items.h"
#include "gui/prop/prop_KeyAccessor.h"
#include "qapplication.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QMimeData>
#include "gui/theme/Colors.h"

namespace gui {

namespace {
// The window's two fields, compared literally: ctrl::TimeLineEditor::PlayWindow is the
// timeline editor's own struct and carries no operator==. Used only to decide whether
// the playWindowChanged signal has something to announce.
bool samePlayWindow(
    const ctrl::TimeLineEditor::PlayWindow& aLhs, const ctrl::TimeLineEditor::PlayWindow& aRhs
) {
    return aLhs.start == aRhs.start && aLhs.end == aRhs.end;
}
} // namespace

//-------------------------------------------------------------------------------------------------
TimeCursor::TimeCursor(QWidget* aParent):
    QWidget(aParent),
    mNumberColor(theme::Colors::current().text),
    mEdgeColor(theme::Colors::current().accentBright),
    mFrameText(),
    mBadgeColor(),
    mLineX(2) {
    this->setAutoFillBackground(false);
}

void TimeCursor::setCursorPos(const QPoint& aPos, int aHeight) {
    const QFontMetrics fm(font());
    const int badgeWidth = fm.horizontalAdvance(mFrameText) + ctrl::TimeLineEditor::kPillPadding * 2;
    // The playhead line sits in the middle of the widget; widen the widget as
    // needed so the frame-number pill fits around the line. aPos.y() is the
    // ruler header's top, so the pill lands exactly on the numbers row.
    mLineX = std::max(1, badgeWidth / 2);

    QRect rect;
    rect.setTopLeft(aPos - QPoint(mLineX, 0));
    rect.setBottomRight(aPos + QPoint(mLineX, 0) + QPoint(0, aHeight));
    this->setGeometry(rect);
    update();
}

void TimeCursor::setHeaderInfo(const QString& aFrameText, const QColor& aBadgeColor) {
    mFrameText = aFrameText;
    mBadgeColor = aBadgeColor;
    update();
}

void TimeCursor::paintEvent(QPaintEvent* aEvent) {
    (void)aEvent;
    QPainter painter;
    painter.begin(this);

    // The playhead pill wraps the number-text row. Its top edge rides one
    // pixel below the number text's top (kNumberTop + 1) so the pill and the
    // ruler numbers shift together as kNumberTop moves.
    const int pillTop = ctrl::TimeLineEditor::kNumberTop + 1;

    // Playhead line: starts at the bottom of the frame-number row (the pill
    // covers the row above it) and runs down the tracks.
    painter.setPen(QPen(mEdgeColor, 1));
    painter.drawLine(mLineX, pillTop + ctrl::TimeLineEditor::kNumberHeight, mLineX, this->geometry().height());

    // Frame-number pill on the numbers row: an accentBright rounded pill —
    // the playhead's identity. It must read on the plain ruler AND on the
    // temporary playback window's band, so it is token-driven (accentBright
    // fill + max-contrast text), not base-on-base like the old badge.
    if (!mFrameText.isEmpty()) {
        const QFontMetrics fm(font());
        const int textWidth = fm.horizontalAdvance(mFrameText);
        const int pillWidth = textWidth + ctrl::TimeLineEditor::kPillPadding * 2;
        const QRect pill(mLineX - pillWidth / 2, pillTop, pillWidth, ctrl::TimeLineEditor::kNumberHeight);
        QPainterPath path;
        path.addRoundedRect(
            QRectF(pill), ctrl::TimeLineEditor::kPillRadius, ctrl::TimeLineEditor::kPillRadius, Qt::AbsoluteSize
        );
        painter.fillPath(path, mBadgeColor);
        // Draw the number with the exact same rect and alignment as the ruler
        // numbers (time_Renderer::renderHeader) so it sits in the same row.
        const QRect numberRect(
            mLineX - textWidth / 2, ctrl::TimeLineEditor::kNumberTop, textWidth, ctrl::TimeLineEditor::kNumberHeight
        );
        painter.setPen(mNumberColor);
        painter.drawText(numberRect, mFrameText);
    }

    painter.end();
}

QColor TimeCursor::edgeColor() const { return mEdgeColor; }

void TimeCursor::setEdgeColor(const QColor& cursorEdgeColor) { mEdgeColor = cursorEdgeColor; }

QColor TimeCursor::numberColor() const { return mNumberColor; }

void TimeCursor::setNumberColor(const QColor& cursorNumberColor) { mNumberColor = cursorNumberColor; }

//-------------------------------------------------------------------------------------------------
TimeLineEditorWidget::TimeLineEditorWidget(ViaPoint& aViaPoint, QWidget* aParent):
    QWidget(aParent),
    mViaPoint(aViaPoint),
    mProject(),
    mTimeLineSlot(),
    mTreeRestructSlot(),
    mProjectAttrSlot(),
    mEditor(),
    mCamera(),
    mTimeCursor(this),
    mKeyCommandMap(*aViaPoint.keyCommandMap()),
    mCopyKey(),
    mPasteKey(),
    mDeleteKey(),
    mEasingHost(),
    mTargets(),
    mCopyTargets(),
    mPastePos(),
    mOnPasting(),
    mTimelineTheme() {
    mTimeCursor.show();

    mEditor.reset(new ctrl::TimeLineEditor());
    this->setMouseTracking(true);
    // The surface is where Ctrl+A lives (see keyPressEvent): ClickFocus, not
    // StrongFocus, so a click in the lanes hands the keyboard to this widget (the
    // press still propagates to TimeLineWidget's gestures, because every mouse
    // handler here ignores) without adding a Tab stop beside the timeline's own.
    this->setFocusPolicy(Qt::ClickFocus);
    this->setContextMenuPolicy(Qt::CustomContextMenu);

    // THE CHIP FOLLOWS THE SELECTION, NOT THE EVENT LOOP. A selection another view
    // makes is announced by the set itself (ctrl::time::Selection::onChanged): the
    // animation graph panel writes the very same set (the panel shares it - see
    // ctrl::time::Selection and ctrl::GraphEditor::setSelectionSource), and the only
    // other announcement it used to have was a repaint request, which Qt DROPS while
    // this widget is hidden, collapsed or not yet laid out. The notification has no
    // such gap, so there is nothing left for a reconcile timer to cover - and one
    // source for one fact is the point (see pushEasingChipState). The push is
    // idempotent (prop::EasingChip ignores a state equal to the one it holds), so a
    // change the chip does not show costs one compare.
    mEditor->selection().onChanged.connect(this, &TimeLineEditorWidget::pushEasingChipState);

    // The ruler strip: own paint and own input, over the track surface and
    // under the playhead (raised below so its pill overlays the window pills).
    mTimeRuler.reset(new TimeRuler(*this, this));
    mTimeRuler->setGeometry(0, 0, this->width(), ctrl::TimeLineEditor::kHeaderHeight);
    mTimeRuler->show();
    mTimeCursor.raise();
    this->connect(mTimeRuler.data(), &TimeRuler::frameChanged, this, &TimeLineEditorWidget::rulerFrameChanged);

    this->connect(this, &QWidget::customContextMenuRequested, this, &TimeLineEditorWidget::onContextMenuRequested);

    {
        mCopyKey = new QAction(tr("Copy key"), this);
        mCopyKey->connect(mCopyKey, &QAction::triggered, this, &TimeLineEditorWidget::onCopyKeyTriggered);

        mPasteKey = new QAction(tr("Paste key"), this);
        mPasteKey->connect(mPasteKey, &QAction::triggered, this, &TimeLineEditorWidget::onPasteKeyTriggered);

        mDeleteKey = new QAction(tr("Delete key"), this);
        mDeleteKey->connect(mDeleteKey, &QAction::triggered, this, &TimeLineEditorWidget::onDeleteKeyTriggered);

        mCopyToClipboard = new QAction(tr("Copy key to clipboard"), this);
        mCopyToClipboard->connect(
            mCopyToClipboard, &QAction::triggered, this, &TimeLineEditorWidget::onCopyCBTriggered
        );

        mSelectSpacing = new QAction(tr("Select spacing between keys"), this);
        mSelectSpacing->connect(mSelectSpacing, &QAction::triggered, [=] {onSelectSpacingTriggered();});
    }
    this->update();
    {
        auto key = mKeyCommandMap.get("Copy");
        if (key)
            key->addListener([=]() {
                mTargets = core::TimeLineEvent();
                mEditor->retrieveSelectionTargets(mTargets);
                mCopyKey->trigger();
            });
    }
    {
        auto key = mKeyCommandMap.get("Paste");
        if (key)
            key->addListener([=]() {
                if (!mCopyTargets.targets().isEmpty()) {
                    // Anchor keyboard paste at the playhead, not the mouse:
                    // QCursor::pos() is a screen coordinate, and mapped into
                    // this widget it only denotes a frame while the cursor
                    // actually hovers the timeline. Elsewhere (viewport,
                    // object tree) it maps to an arbitrary frame, so Ctrl+V
                    // looked like it pasted at random. The context menu keeps
                    // its own click-position anchor (onContextMenuRequested).
                    const auto& scale = mEditor->timeScale();
                    const int x =
                        scale.pixelWidth(mEditor->currentFrame().get()) + ctrl::TimeLineEditor::kTimeLineMargin;
                    mPastePos = QPoint(x, 0);
                    mPasteKey->trigger();
                }
            });
    }
    {
        auto key = mKeyCommandMap.get("Delete");
        if (key)
            key->addListener([=]() {
                mTargets = core::TimeLineEvent();
                if (mEditor->retrieveSelectionTargets(mTargets)) {
                    mDeleteKey->trigger();
                }
            });
    }
}

void TimeLineEditorWidget::setProject(core::Project* aProject) {
    if (mProject) {
        mProject->onTimeLineModified.disconnect(mTimeLineSlot);
        mProject->onTreeRestructured.disconnect(mTreeRestructSlot);
        mProject->onProjectAttributeModified.disconnect(mProjectAttrSlot);
    }

    if (aProject) {
        mProject = aProject->pointee();
        mTimeLineSlot = aProject->onTimeLineModified.connect(this, &TimeLineEditorWidget::onTimeLineModified);

        mTreeRestructSlot = aProject->onTreeRestructured.connect(this, &TimeLineEditorWidget::onTreeRestructured);

        mProjectAttrSlot =
            aProject->onProjectAttributeModified.connect(this, &TimeLineEditorWidget::onProjectAttributeModified);
    } else {
        mProject.reset();
    }

    const ctrl::TimeLineEditor::PlayWindow windowBefore = mEditor->playWindow();
    mEditor->setProject(aProject);
    // ctrl::TimeLineEditor::setProject clears the temporary window (it is a gesture on
    // one project's ruler); announce it so the graph's overlay goes with it.
    if (!samePlayWindow(windowBefore, mEditor->playWindow()))
        emit playWindowChanged();
    updateSize();
    // The project owns the ruler's content (frame range, fps, play window):
    // the surface repaint alone leaves the strip stale until the next
    // camera update.
    if (mTimeRuler)
        mTimeRuler->update();
    // The keys went with the old project too: ctrl::TimeLineEditor::setProject
    // clears the selection (clearRows), and the repaint above is what re-reads it
    // for the header's chip.
}

void TimeLineEditorWidget::setFrame(core::Frame aFrame) {
    mEditor->setFrame(aFrame);

    // particial rendering
    updateTimeCursorPos();
}

core::Frame TimeLineEditorWidget::currentFrame() const { return mEditor->currentFrame(); }

void TimeLineEditorWidget::updateTimeCursorPos() {
    if (mCamera) {
        auto pos = mEditor->currentTimeCursorPos();
        const core::TimeFormat format(mEditor->frameRange(), mEditor->fps());
        const QString frameText = format.frameToString(mEditor->currentFrame().get(), mEditor->timeFormatType());
        // The playhead pill is token-driven (accentBright fill, max-contrast
        // text, accentBright line): it must read on the plain ruler AND on
        // the temporary window's band, and follow accent hue changes, so the
        // QSS-substituted colors do not drive it.
        mTimeCursor.setHeaderInfo(frameText, theme::Colors::current().accentBright);
        mTimeCursor.setNumberColor(theme::Colors::current().accentBrightText);
        mTimeCursor.setEdgeColor(theme::Colors::current().accentBright);
        // The playhead pill overlays the ruler numbers, so repaint the full
        // ruler strip (22px) each tick.
        if (mTimeRuler)
            mTimeRuler->update();
        // The playhead overlays the viewport: pinned to the top (on the fixed
        // ruler) and following the content horizontally.
        const QPoint viewportPos(mCamera->leftTopPos().toPoint().x() + pos.x(), 0);
        mTimeCursor.setCursorPos(viewportPos, mCamera->screenHeight());
    }
}

void TimeLineEditorWidget::updateCamera(const core::CameraInfo& aCamera) {
    mCamera = &aCamera;
    updateSize();
    updateTimeCursorPos();
    if (mTimeRuler)
        mTimeRuler->setCamera(&aCamera);
}

void TimeLineEditorWidget::resizeEvent(QResizeEvent* aEvent) {
    QWidget::resizeEvent(aEvent);
    if (mTimeRuler)
        mTimeRuler->setGeometry(0, 0, this->width(), ctrl::TimeLineEditor::kHeaderHeight);
}

void TimeLineEditorWidget::keyPressEvent(QKeyEvent* aEvent) {
    // Ctrl+A selects everything the timeline shows (see
    // ctrl::TimeLineEditor::selectAllKeys for the exact enumeration) and repaints
    // only when the set actually changed - a second Ctrl+A is not a repaint.
    // The modifiers must be EXACTLY Ctrl: Ctrl+Shift+A is a different binding, the
    // graph panel's own rule (GraphPanelWidget::panelKeyAction).
    if (aEvent->key() == Qt::Key_A && aEvent->modifiers() == Qt::ControlModifier) {
        if (mEditor->selectAllKeys())
            this->update();
        aEvent->accept();
        return;
    }
    // Everything else is not this surface's: the base ignores it, which is what
    // propagates it to TimeLineWidget's arrow-key handling and then to the
    // application's own key map.
    QWidget::keyPressEvent(aEvent);
}

void TimeLineEditorWidget::updateLines(QTreeWidgetItem* aTopNode) {
    mEditor->clearRows();

    if (!aTopNode || !aTopNode->isExpanded())
        return;

    for (int i = 0; i < aTopNode->childCount(); ++i) {
        updateLinesRecursive(aTopNode->child(i));
    }
    updateSize();
}

void TimeLineEditorWidget::updateLinesRecursive(QTreeWidgetItem* aItem) {
    if (!aItem)
        return;

    const bool isClosedFolder = !aItem->isExpanded() && aItem->childCount() > 0;
    obj::Item* objItem = obj::Item::cast(aItem);
    if (objItem && !objItem->isTopNode()) {
        int screenTop = mCamera ? static_cast<int>(-mCamera->leftTopPos().y()) : 0;

        const QRect vrect = objItem->visualRect();
        const int t = vrect.top() + screenTop;
        const int b = vrect.bottom() + screenTop;

        mEditor->pushRow(&objItem->node(), util::Range(t, b), isClosedFolder);
    }

    if (!aItem->isExpanded())
        return;

    for (int i = 0; i < aItem->childCount(); ++i) {
        updateLinesRecursive(aItem->child(i));
    }
    updateSize();
}

void TimeLineEditorWidget::updateLineSelection(core::ObjectNode* aRepresent) {
    mEditor->updateRowSelection(aRepresent);
    this->update();
}

bool TimeLineEditorWidget::updateCursor(const core::AbstractCursor& aCursor, Qt::KeyboardModifiers aModifiers) {
    const ctrl::TimeLineEditor::PlayWindow windowBefore = mEditor->playWindow();
    ctrl::TimeLineEditor::UpdateFlags flags = mEditor->updateCursor(aCursor, aModifiers);
    // THE WINDOW'S OWN ANNOUNCEMENT: the ruler's shift-drag sets, moves or collapses
    // the temporary playback window through this call, and it can do so without moving
    // the current frame (so rulerFrameChanged and ModFrame are both silent). The graph
    // panel draws the same window, so the GUI is told here, where the gesture lands.
    if (!samePlayWindow(windowBefore, mEditor->playWindow()))
        emit playWindowChanged();

    if (flags & ctrl::TimeLineEditor::UpdateFlag_ModView) {
        updateTimeCursorPos();
        // The repaint re-reads the selection for the header's chip (see
        // paintEvent): ModView is the editor's own "the selection and its
        // highlight changed" report, and every selection mutator sets it.
        this->update();
    }
    return (flags & ctrl::TimeLineEditor::UpdateFlag_ModFrame);
}

bool TimeLineEditorWidget::updateWheel(QWheelEvent* aEvent, int aMouseX, int& aPixelAfter) {
    const double oldSpacing = mEditor->pixelsPerFrame();
    const bool changed =
        mEditor->updateWheel(aEvent->angleDelta().y(), mViaPoint.mouseSetting().invertTimeLineScaling);
    if (changed) {
        // Pixel-exact zoom anchor: the content point under the mouse stays put,
        // regardless of whether it lies inside the frame range.
        aPixelAfter = mEditor->anchorPixelAfterScale(aMouseX, oldSpacing);
        updateSize();
    } else {
        // Zoom clamped at floor/ceiling: keep the view exactly still instead of
        // re-deriving a scroll offset (frame<->pixel rounding would drift it).
        aPixelAfter = aMouseX;
    }
    return changed;
}

void TimeLineEditorWidget::updateSize() {
    // The widget is a viewport-fixed surface sized by TimeLineWidget; content
    // size changes only need a repaint (the paint translates by the camera).
    this->update();
}

void TimeLineEditorWidget::updateProjectAttribute() {
    const ctrl::TimeLineEditor::PlayWindow windowBefore = mEditor->playWindow();
    mEditor->updateProjectAttribute();
    // A frame-range change invalidates the temporary window (ctrl::TimeLineEditor's
    // rule), so the panel's overlay must be told the same thing the ruler is.
    if (!samePlayWindow(windowBefore, mEditor->playWindow()))
        emit playWindowChanged();
    updateTimeCursorPos();
    updateSize();
}

void TimeLineEditorWidget::updateTheme(theme::Theme& aTheme) {
    Q_UNUSED(aTheme) // TODO
    mTimelineTheme.reset();
    if (mTimeRuler)
        mTimeRuler->updateTheme();
    updateTimeCursorPos();
}
QSize TimeLineEditorWidget::getEditorSize() const {
    return mEditor->modelSpaceSize();
}

int TimeLineEditorWidget::frameStartPixel() const {
    return mEditor->frameStartPixel();
}

int TimeLineEditorWidget::frameEndPixel() const {
    return mEditor->frameEndPixel();
}

void TimeLineEditorWidget::paintEvent(QPaintEvent* aEvent) {
    QPainter painter;
    painter.begin(this);
    if (mCamera) {
        // The surface is viewport-fixed; the content translates by the view
        // origin. Everything then lands at its viewport position — fixed UI
        // included — and nothing is clipped by the frame range.
        const QPoint origin = mCamera->leftTopPos().toPoint();
        painter.translate(origin);
        mEditor->render(painter, *mCamera, mTimelineTheme, aEvent->rect().translated(-origin));
        // The hairline under the ruler separates it from the track area. It
        // sits on the first track row (y = kHeaderHeight), below the ruler
        // widget, and stays fixed like the strip it frames.
        painter.translate(-origin);
        painter.setPen(QPen(QBrush(mTimelineTheme.rulerLineColor()), 1));
        painter.drawLine(0, ctrl::TimeLineEditor::kHeaderHeight, this->width(), ctrl::TimeLineEditor::kHeaderHeight);
    }
    painter.end();

    // The paint is where this widget re-reads the selection it renders, and the
    // header's chip names that same selection - so the repaint is a cheap
    // RECONCILIATION of the chip's state, not its only source: the set announces
    // every change itself (ctrl::time::Selection::onChanged, connected in the
    // constructor), whichever view made it, and a document modification pushes too
    // (see onTimeLineModified). The chip ignores a state equal to the one it holds,
    // so a repaint that changed nothing costs one compare.
    pushEasingChipState();
}

void TimeLineEditorWidget::onTimeLineModified(core::TimeLineEvent& aEvent, bool aUndo) {
    if (!mOnPasting) {
        mCopyTargets = core::TimeLineEvent();
    }
    mEditor->updateKey();
    // Creating keys always selects its output: clear the prior selection and
    // keep only the keys a push/copy just created. An undo/redo that REMOVES
    // keys (aUndo=true) must not select a notification over keys that are no
    // longer in their lane — refresh() already dropped them, so only select on
    // a forward (exec/redo) creation.
    if (!aUndo &&
        (aEvent.type() == core::TimeLineEvent::Type_PushKey || aEvent.type() == core::TimeLineEvent::Type_CopyKey)) {
        mEditor->selectCreatedKeys(aEvent);
    }
    // A modification can move, drop or re-ease the keys under the selection (an
    // undo, a delete, or the picker's own commit). The repaint re-reads them for the
    // header's chip, and this push makes that independent of the paint: the document
    // signal arrives whether or not this widget is visible, so the chip names the
    // post-edit truth - and re-derives the Auto context from the document - even with
    // the dock's surface hidden.
    this->update();
    pushEasingChipState();
}

void TimeLineEditorWidget::onTreeRestructured(core::ObjectTreeEvent&, bool) { mCopyTargets = core::TimeLineEvent(); }

void TimeLineEditorWidget::onProjectAttributeModified(core::ProjectEvent&, bool) {
    mCopyTargets = core::TimeLineEvent();
}

void TimeLineEditorWidget::onContextMenuRequested(const QPoint& aPos) {
    // The ruler's context menu is reserved for the temporary playback
    // window: it shows only while a window is active, and only when the
    // click is on the ruler itself — the window lives there, so its clear
    // action sits next to it, and the track menu stays key-focused. The
    // ruler region is the editor's predicate, the same one the gesture
    // machinery uses.
    if (mEditor->isOnRuler(aPos.y())) {
        if (!mEditor->playWindow().active())
            return;
        QMenu menu(this);
        auto* clearWindow = menu.addAction(tr("Clear playback window"));
        clearWindow->connect(clearWindow, &QAction::triggered, [this]() {
            const bool wasActive = mEditor->playWindow().active();
            mEditor->clearPlayWindow();
            if (wasActive)
                emit playWindowChanged();
            this->update();
            // The window's band and pills are the ruler's paint; the surface
            // repaint alone would leave them on screen until the next move.
            if (mTimeRuler)
                mTimeRuler->update();
        });
        menu.exec(this->mapToGlobal(aPos));
        this->update();
        return;
    }

    QMenu menu(this);

    // customContextMenuRequested delivers surface coordinates, but every
    // editor query below runs in model space: rows are pushed there
    // (updateLinesRecursive shifts them by the view origin) and mouse
    // events land there through AbstractCursor's toWorldPos. Feeding raw
    // surface coordinates silently missed every key whenever the view was
    // scrolled — no highlight, no key actions — and anchored paste at a
    // shifted frame. The ruler check above stays in surface space on
    // purpose: the ruler strip is pinned to the widget top.
    const QPoint worldPos = mCamera ? mCamera->toWorldPos(QPointF(aPos)).toPoint() : aPos;

    mTargets = core::TimeLineEvent();
    const bool clickedOnKey = mEditor->selectKeysAt(mTargets, worldPos);
    if (!clickedOnKey) {
        // Not on a key: the current selection is what the menu acts on ("keys stay
        // operable from anywhere once selected"). It is empty when nothing is
        // selected at all, and buildKeyMenu reads that as the paste-only menu.
        mEditor->retrieveSelectionTargets(mTargets);
    }

    mPastePos = worldPos;

    // Both shapes of the menu are buildKeyMenu's, which is also the graph panel's
    // route into this menu (see openKeyMenu).
    buildKeyMenu(menu);
    menu.exec(this->mapToGlobal(aPos));
    // A right-click on an unselected key lent it a temporary selection for
    // this menu only; the previous selection is not restored, so drop the
    // temporary selection now that the menu is gone, then repaint so the
    // highlight clears (see ctrl::TimeLineEditor::endTemporarySelection).
    mEditor->endTemporarySelection();
    this->update();
}

//-------------------------------------------------------------------------------------------------
void TimeLineEditorWidget::buildKeyMenu(QMenu& aMenu) {
    // ONE BUILDER, TWO CALLERS: the timeline's own right-click above and the
    // animation graph panel's (openKeyMenu, reached through gui::TimeLineWidget and
    // gui::MainDisplayWidget). Both hand this the same state - mTargets, the whole
    // selection in the timeline's currency (one target per KEY), mCopyTargets (what
    // a paste would drop) and mPastePos (the frame it would land on) - and both get
    // the same items, the same labels and the same edit path as a result. There is
    // no second menu to keep in step.
    //
    // THE ITEMS ACT ON THE SET. Every action below reads mTargets - the selection the
    // caller set - and not the one key a press landed on, so with N keys selected an
    // activation is one edit over all N of them:
    //   * Copy key: mCopyTargets = mTargets, the whole selection (plus the clipboard
    //     write when the user's "auto copy to clipboard" setting is on).
    //   * Copy key to clipboard: the clipboard write over mCopyTargets, which the item
    //     above has just filled with the whole selection. A STANDALONE activation keeps
    //     whatever was copied last - onCopyCBTriggered's own long-standing rule, the
    //     same in the timeline's menu.
    //   * Paste key: drops mCopyTargets at mPastePos, the frame this menu was opened on
    //     (the selection supplies the anchor here; what is written is the copied set).
    //   * Select spacing between keys: one dialog, then every target of mTargets moves.
    //   * Delete key: one deleteCheckedKeys(mTargets) command over the whole set.
    // Easing is deliberately absent: it lives in the dock header's own chip
    // (openEasingPicker), so a menu entry would be a second route to one edit.
    mPasteKey->setEnabled(mCopyTargets.hasAnyTargets());

    if (mTargets.targets().isEmpty()) {
        aMenu.addAction(mPasteKey); // no key involved: paste is all there is
        return;
    }

    aMenu.addAction(mCopyKey);
    aMenu.addAction(mCopyToClipboard);
    aMenu.addAction(mPasteKey);
    aMenu.addSeparator();
    aMenu.addAction(mSelectSpacing);
    aMenu.addSeparator();
    aMenu.addAction(mDeleteKey);
}

// Type, frame, easing type, easing range, easing weight.
static void addVecToJson(QJsonObject* json, const QString& title, const QVector2D vec) {
    json->insert(title + "X", vec.x());
    json->insert(title + "Y", vec.y());
}

// Tf means Type & Frame in case you're wondering.
static void addTfToObj(QJsonObject* json, int keyType, const core::TimeKey* timeKey) {
    json->insert("Type", core::TimeLine::getTimeKeyName(static_cast<core::TimeKeyType>(keyType)));
    json->insert("Frame", timeKey->frame());
}
template<typename keyData>
static void addStandardToObj(QJsonObject* json, keyData data, const int keyType, core::TimeKey* timeKey) {
    addTfToObj(json, keyType, timeKey);
    // The clipboard payload has one easing slot per key, so it carries component
    // 0's curve; the paste broadcasts it to every component (a lossless reading
    // of a one-easing payload). Per-component round trips travel in the .anie
    // file, which stores all of them. "Auto" is written for the Auto PRESET
    // (Custom with both handle sides Auto), which is what an old payload's
    // retired Auto type meant too; the reader turns it back into those modes.
    const util::Easing::Param& easing = data.easing(0);
    const bool autoPreset = easing.type == util::Easing::Type_Custom
        && easing.handleIn == util::Easing::HandleMode_Auto
        && easing.handleOut == util::Easing::HandleMode_Auto;
    json->insert("eType", autoPreset ? QStringLiteral("Auto") : util::Easing::getTypeName(easing.type));
    json->insert("eRange", util::Easing::getRangeName(easing.range));
    json->insert("eWeight", easing.weight);
}

QJsonObject getKeyTypeSerialized(int keyType, core::TimeKey* timeKey, core::ObjectNode* node) {
    switch (keyType) {
    // General data for all keys: Type, Frame and Easing {type, range & weight}
    case core::TimeKeyType_Move: {
        core::MoveKey::Data data = dynamic_cast<const core::MoveKey*>(timeKey)->data();
        // Data types: pos(Vec2D), centroid(Vec2D), spline(int) //
        QJsonObject move;
        addStandardToObj(&move, data, keyType, timeKey);
        addVecToJson(&move, "Pos", data.pos());
        addVecToJson(&move, "Centre", data.centroid());
        move["Spline"] = core::MoveKey::getSplineName(data.spline());
        return move;
    }
    case core::TimeKeyType_Rotate: {
        auto data = dynamic_cast<const core::RotateKey*>(timeKey)->data();
        // Data types: Rotate (float) //
        QJsonObject rotate;
        addStandardToObj(&rotate, data, keyType, timeKey);
        rotate["Rotate"] = data.rotate();
        return rotate;
    } break;
    case core::TimeKeyType_Scale: {
        auto data = dynamic_cast<const core::ScaleKey*>(timeKey)->data();
        // Data types: Scale (vec2d) //
        QJsonObject scale;
        addStandardToObj(&scale, data, keyType, timeKey);
        addVecToJson(&scale, "Scale", data.scale());
        return scale;
    }
    case core::TimeKeyType_Depth: {
        auto data = dynamic_cast<const core::DepthKey*>(timeKey)->data();
        // Data types: Depth (float) //
        QJsonObject depth;
        addStandardToObj(&depth, data, keyType, timeKey);
        depth["Depth"] = data.depth();
        return depth;
    }
    case core::TimeKeyType_Opa: {
        auto data = dynamic_cast<const core::OpaKey*>(timeKey)->data();
        // Data types: Opacity (float) //
        QJsonObject opa;
        addStandardToObj(&opa, data, keyType, timeKey);
        opa["Opacity"] = data.opacity();
        return opa;
    }
    case core::TimeKeyType_Bone: {
        auto data = dynamic_cast<const core::BoneKey*>(timeKey)->data();
        /* Data types:
        LocalPos(vec2d), LocalAngle(float), Range(vec2d * 2),
        ---
        Shape[Valid(bool), SegmentStart(vec2d), SegmentDir(vec2d), Unit(vec2d),
        DirAngle(float), Length(float), Radius (vec2d * 2), RootBendAngle(float *2),
        TailBendAngle(float * 2), Bounding(vec2d), Polygon{PolyCount(int), Vertices(vec2d)}],
        ---
        WorldPos(vec2d), WorldAngle(float), Rotate(float),
        Children(Bones with same data type as previously described minus children of their own)
        */
        QJsonObject bones;
        addTfToObj(&bones, keyType, timeKey);
        QJsonArray boneArray;
        for (auto bone : data.topBones()) {
            QJsonObject topBone;
            topBone["Bone"] = bone->serializeToJson();
            boneArray.append(topBone);
        }
        bones["Bones"] = boneArray;
        return bones;
    }
    case core::TimeKeyType_Pose: {
        auto data = dynamic_cast<const core::PoseKey*>(timeKey)->data();
        // Data types: Same as the bone key.
        QJsonObject pose;
        addTfToObj(&pose, keyType, timeKey);
        QJsonArray boneArray;
        for (auto bone : data.topBones()) {
            QJsonObject topBone;
            topBone["Bone"] = bone->serializeToJson();
            boneArray.append(topBone);
        }
        pose["Poses"] = boneArray;
        return pose;
    }
    case core::TimeKeyType_Mesh: {
        auto data = dynamic_cast<const core::MeshKey*>(timeKey)->data();
        // Data for MeshKey: Origin Offset (vec2d), Vertex count (int), Vertices (vec2d), Edge count (int),
        // Edges (ints), Face count (int), Faces (ints)
        QJsonObject mesh;
        addTfToObj(&mesh, keyType, timeKey);
        mesh["Mesh"] = data.serializeToJson();
        return mesh;
    } break;
    case core::TimeKeyType_FFD: {
        auto data = dynamic_cast<const core::FFDKey*>(timeKey)->data();
        // Data for FFDKey: Vertex count (int), Vertex positions (vec3)
        QJsonObject ffd;
        addStandardToObj(&ffd, data, keyType, timeKey);
        ffd["FFD"] = dynamic_cast<const core::FFDKey*>(timeKey)->serializeToJson();
        // ⚠ IF THE IMAGE'S GRIDMESH DOESN'T MATCH THE TARGET'S THE DEFORMATION WON'T OCCUR ⚠ //
        /* We need the cell size to keep consistency with the vertex data, as the same image will
        always generate the same mesh for any given cell size, we also need to check if there is an
        image key close to our target to get the correct cellsize for the image being deformed. */
        if (node->timeLine()->map(core::TimeKeyType_Image).size() != 0) {
            int dataFrame = timeKey->frame();
            int closest = 0;
            int index = 0;
            for (auto nodeM : node->timeLine()->map(core::TimeKeyType_Image)) {
                if (nodeM->frame() == dataFrame || nodeM->frame() - dataFrame <= closest - dataFrame) {
                    closest = index;
                }
                index++;
            }
            ffd["cellSize"] =
                dynamic_cast<const core::ImageKey*>(node->timeLine()->timeKey(core::TimeKeyType_Image, closest))
                ?
                dynamic_cast<const core::ImageKey*>(node->timeLine()->timeKey(core::TimeKeyType_Image, closest))
                      ->data()
                      .gridMesh()
                      .cellSize() : 0;
        } else {
            ffd["cellSize"] = node->timeLine()->current().areaImageKey()->data().gridMesh().cellSize();
        }
        return ffd;
    }
    case core::TimeKeyType_Image: {
        auto data = dynamic_cast<const core::ImageKey*>(timeKey)->data();
        /* Data for ImageKey: Identifier(QString), BlendMode(QString), Offset(vec2d), GridMesh[Size(qsize),
        OriginOffset(vec2d), CellPx(int), IndexCount(int), Indices(uint32 * indexcount),
        VertexCount(int), Positions(vec3d), Offsets(vec3d), TexCoords(vec2d), Normals (vec3d),
        ConnectSize(size_t), XCMemSize(uint64), XCMemData (char*)]
        ---
        Regarding the correct identification of image resources, it is entirely possible for this method as currently
        implemented to not get the correct image because there is more than one resource with the same identifier.
        If this happens to you all I can say is git gud lmao.
        */
        QJsonObject img;
        addStandardToObj(&img, data, keyType, timeKey);
        img["Image"] = dynamic_cast<const core::ImageKey*>(timeKey)->serializeToJson();
        // qDebug() << QJsonDocument(img).toJson(QJsonDocument::Indented).toStdString().c_str();
        return img;
    }
    case core::TimeKeyType_HSV: {
        auto data = dynamic_cast<const core::HSVKey*>(timeKey)->data();
        // Data types: HSV (List<int>) //
        QJsonObject hsv;
        addStandardToObj(&hsv, data, keyType, timeKey);
        hsv["Hue"] = data.hsv()[0];
        hsv["Saturation"] = data.hsv()[1];
        hsv["Value"] = data.hsv()[2];
        hsv["Absolute"] = data.hsv()[3];
        return hsv;
    }
    case core::TimeKeyType_Blur: {
        auto data = dynamic_cast<const core::BlurKey*>(timeKey)->data();
        // Data types: Amount, BlurX, BlurY, Angle (floats) //
        QJsonObject blur;
        addStandardToObj(&blur, data, keyType, timeKey);
        blur["Amount"] = data.amount();
        blur["Directional"] = data.isDirectional();
        blur["BlurX"] = data.blurX();
        blur["BlurY"] = data.blurY();
        blur["Angle"] = data.angleDeg();
        return blur;
    }
    default: {
        return {};
    }
    }
}

void TimeLineEditorWidget::onCopyKeyTriggered(bool) {
    mCopyTargets = mTargets;
    QSettings settings;
    auto cbCopy = settings.value("generalsettings/keys/autocb");
    bool copyToCb = !cbCopy.isValid() || cbCopy.toBool();
    if (copyToCb) {
        onCopyCBTriggered(true);
    }
}

void TimeLineEditorWidget::onCopyCBTriggered(bool) {
    if (!mCopyTargets.hasAnyTargets()) {
        mCopyTargets = mTargets;
    }
    QJsonObject targets;
    QJsonArray keys;
    targets["TargetsSize"] = mCopyTargets.targets().size();
    qDebug() << mCopyTargets.targets().size();
    for (auto cTarget : mCopyTargets.targets()) {
        qDebug() << cTarget.pos.key();
        core::TimeKeyType keyType = cTarget.pos.key()->type();
        qDebug() << keyType;
        core::TimeKey* timeKey = cTarget.node->timeLine()->timeKey(keyType, cTarget.pos.key()->frame());
        keys.append(getKeyTypeSerialized(keyType, timeKey, cTarget.node));
    }
    targets["Keys"] = keys;
    // There be IO dragons here
    // qDebug() << QJsonDocument(targets).toJson(QJsonDocument::Indented).toStdString().c_str();
    QClipboard* cb = QGuiApplication::clipboard();
    cb->setText(QJsonDocument(targets).toJson(QJsonDocument::Indented).toStdString().c_str());
}

void TimeLineEditorWidget::onPasteKeyTriggered(bool) {
    mOnPasting = true;
    if (!mEditor->pasteCopiedKeys(mCopyTargets, mPastePos)) {
        QMessageBox::warning(nullptr, tr("Operation Error"), tr("Failed to paste keys. "
        "\n(If you want to paste across objects copy the key to the clipboard and right-click and paste to the node instead)"));
    }
    mOnPasting = false;
}

// The easing a timeline key carries for ONE graphable component, BY REFERENCE:
// the picker reads it (the header chip's state) and writes it (a commit), so
// there is one per-type table here rather than a reader and a writer that can
// drift apart. aComponent is the component being edited - a selection item's own
// component (item.component) or 0 for a whole-key entry, whose whole-key edit
// iterates every component instead of collapsing onto the first.
//
// Null for a type that carries no easing at all (bone, mesh) and for an image
// key, whose offset is step-held - playback never consults its param, and the
// timeline's easing edit has never covered it (see core/AutoEasing.h's note on
// image keys). Pose and FFD are NOT in that group: their single shared easing
// (component 0) shapes the interval playback plays, so they are edited here like
// any other.
util::Easing::Param* keyEasingOf(core::TimeKey& aKey, core::TimeKeyType aType, int aComponent) {
    switch (aType) {
    case core::TimeKeyType_Move:
        return &static_cast<core::MoveKey&>(aKey).data().easing(aComponent);
    case core::TimeKeyType_Rotate:
        return &static_cast<core::RotateKey&>(aKey).data().easing(aComponent);
    case core::TimeKeyType_Scale:
        return &static_cast<core::ScaleKey&>(aKey).data().easing(aComponent);
    case core::TimeKeyType_Depth:
        return &static_cast<core::DepthKey&>(aKey).data().easing(aComponent);
    case core::TimeKeyType_Opa:
        return &static_cast<core::OpaKey&>(aKey).data().easing(aComponent);
    case core::TimeKeyType_Pose:
        return &static_cast<core::PoseKey&>(aKey).data().easing(aComponent);
    case core::TimeKeyType_FFD:
        return &static_cast<core::FFDKey&>(aKey).data().easing(aComponent);
    case core::TimeKeyType_HSV:
        return &static_cast<core::HSVKey&>(aKey).data().easing(aComponent);
    case core::TimeKeyType_Blur:
        return &static_cast<core::BlurKey&>(aKey).data().easing(aComponent);
    default:
        return nullptr;
    }
}

void TimeLineEditorWidget::assignEasingToTargets(
    const std::function<util::Easing::Param(const util::Easing::Param&)>& aNext) {
    if (!mProject)
        return;

    // WHAT CAN BE WRITTEN, collected BEFORE the macro is opened: cmnd::Stack
    // appends a macro even when it holds no command and counts it as an edit, so an
    // edit that found nothing to write must not open one (the same guard the graph
    // panel's own selection-easing action carries).
    struct Edit {
        util::Easing::Param* param;
        util::Easing::Param next;
        core::ObjectNode* node;
        core::TimeKeyType type;
        int frame;
    };
    QVector<Edit> edits;
    if (mEditor) {
        // The live selection, which is the same set mTargets came from and the
        // only one that names a COMPONENT: a timeline gesture selects whole keys
        // (kKeyLevel), a graph-panel press selects one marker. A whole-key entry
        // edits every component the type stores; a marker edits only its own.
        for (const ctrl::time::Selection::Item& item : mEditor->selection().items()) {
            // The selection is iterated by CONST reference, but TimeKeyPos::key() is
            // a non-const member (it hands out the mutable key), so the position is
            // taken by value and asked there: three words of copy, no cast, and the
            // key it names is the document's own.
            core::TimeLineEvent::Target target = item.target;
            core::TimeKeyPos pos = target.pos;
            core::TimeKey* key = pos.key();
            if (!key || !target.node)
                continue; // a stale selection names a key that is not in the document

            const int stored = core::easingStorageCount(pos.type());
            const bool wholeKey = (item.component == ctrl::time::Selection::kKeyLevel);
            if (!wholeKey && (item.component < 0 || stored <= item.component))
                continue;

            for (int component = 0; component < stored; ++component) {
                if (!wholeKey && component != item.component)
                    continue;

                util::Easing::Param* param = keyEasingOf(*key, pos.type(), component);
                if (!param)
                    continue; // a key of a type that carries no easing (image included)
                // The type's single shared easing (Pose, FFD) is addressable at 0 only.
                const util::Easing::Param next = aNext(*param);
                if (next == *param)
                    continue; // nothing would change: no history entry for a non-change
                edits.push_back({param, next, target.node, pos.type(), key->frame()});
            }
        }
    }
    if (edits.isEmpty())
        return;

    // ONE macro and ONE notifier for the whole selection, so a multi-key edit is
    // ONE entry in the history - the picker commits a whole session at once, and
    // the keys it was opened for must not need one undo per key. The shape is the
    // graph panel's own selection-easing action
    // (ctrl::GraphEditor::setSelectedKeysEasing), and the command per key is the
    // one the timeline's per-key route pushes (TimeLineUtil::assignKeyEasing: a
    // cmnd::Assign of the key's util::Easing::Param plus a Type_ChangeKeyValue
    // notifier). That per-key helper CANNOT be called in this loop: it opens a
    // macro of its own, and cmnd::Stack::beginMacro asserts on a nested one.
    cmnd::ScopedMacro macro(mProject->commandStack(), CmndName::tr("Change key(s) easing"));
    auto* notifier = new ctrl::TimeLineUtil::Notifier(*mProject);
    notifier->event().setType(core::TimeLineEvent::Type_ChangeKeyValue);
    macro.grabListener(notifier);
    for (const Edit& edit : edits) {
        // One notification per KEY: the components of one key are one change to
        // its timeline.
        bool named = false;
        for (const core::TimeLineEvent::Target& pushed : notifier->event().targets()) {
            if (pushed.node == edit.node && pushed.pos.type() == edit.type
                && pushed.pos.index() == edit.frame) {
                named = true;
                break;
            }
        }
        if (!named) {
            notifier->event().pushTarget(*edit.node, edit.type, edit.frame);
        }
        mProject->commandStack().push(new cmnd::Assign<util::Easing::Param>(edit.param, edit.next));
    }
}

bool targetsAreSeparated(QList<core::TimeLineEvent::Target> &targets){
    int frame = 0;
    bool hasFrame = false;
    bool isDifferent = false;
    for(auto target: targets){
        if(!hasFrame){ frame = target.pos.key()->frame(); hasFrame = true; }
        if(target.pos.key()->frame() != frame){ isDifferent = true; }
    }
    return isDifferent;
}
QList<QList<core::TimeLineEvent::Target>*> sortTargets(QList<core::TimeLineEvent::Target>* targets){
    QList<QMap<int, core::TimeKey*>> targetMap;
    QList<QList<core::TimeLineEvent::Target>*> targetLists;
    for(auto target: *targets){
        if(targetMap.empty()){
            targetMap.emplaceBack(target.pos.line()->map(target.pos.key()->type()));
            targetLists.emplace_back(new QList{target});
        }
        else{
            bool containsKey = false;
            // Peak programming right here
            for(const auto& map: targetMap){
                for(const auto& key: map){
                    if(key == target.pos.key()){ containsKey = true; }
                }
            }
            if(containsKey) { targetLists.back()->emplace_back(target); }
            else{
                targetMap.emplaceBack(target.pos.line()->map(target.pos.key()->type()));
                targetLists.emplaceBack(new QList{target});
            }
        }
    }
    for(auto target: targetLists){
        std::sort(target->begin(), target->end(), ctrl::TimeLineUtil::MoveFrameOfKey::greaterThan);
    }
    XC_ASSERT(targetMap.size() == targetLists.size());
    return targetLists;
}

static QString keyToString(const core::TimeKeyType key) {
    switch (key) {
    case core::TimeKeyType_Bone:
        return "Bone";
    case core::TimeKeyType_Mesh:
        return "Mesh";
    case core::TimeKeyType_Image:
        return "Image";
    case core::TimeKeyType_HSV:
        return "HSV";
    case core::TimeKeyType_Blur:
        return "Blur";
    case core::TimeKeyType_FFD:
        return "FFD";
    case core::TimeKeyType_Depth:
        return "Depth";
    case core::TimeKeyType_Move:
        return "Move";
    case core::TimeKeyType_Opa:
        return "Opacity";
    case core::TimeKeyType_Pose:
        return "Pose";
    case core::TimeKeyType_Rotate:
        return "Rotate";
    case core::TimeKeyType_Scale:
        return "Scale";
    default:
        return "Unknown";
    }
}

void TimeLineEditorWidget::onSelectSpacingTriggered() {
    if(!targetsAreSeparated(mTargets.targets())){
        QMessageBox msg;
        msg.setText(tr("A minimum of two keys in different frames are needed."));
        msg.setWindowTitle(tr("Not enough targets"));
        msg.setIcon(QMessageBox::Information);
        msg.setStandardButtons(QMessageBox::Ok);
        msg.exec();
        return;
    }
    auto *diag = new QDialog;
    diag->setObjectName("Dialog");
    diag->resize(400, 100);
    QSizePolicy sizePolicy(QSizePolicy::Policy::MinimumExpanding, QSizePolicy::Policy::MinimumExpanding);
    diag->setSizePolicy(sizePolicy);
    auto verticalLayout = new QVBoxLayout(diag);
    verticalLayout->setObjectName("verticalLayout");
    auto label = new QLabel(diag);
    label->setObjectName("label");
    QSizePolicy sizePolicy1(QSizePolicy::Policy::Minimum, QSizePolicy::Policy::Maximum);
    label->setSizePolicy(sizePolicy1);
    verticalLayout->addWidget(label);

    auto frameSpacing = new QSpinBox(diag);
    // Spacing selects every Nth frame within the project's frame range: the
    // span, not the (possibly negative) end frame, bounds it.
    frameSpacing->setMaximum(qMax(1, mProject->attribute().frameRange().diff()));
    frameSpacing->setMinimum(1);
    frameSpacing->setValue(1);
    frameSpacing->setObjectName("frameSpacing");
    verticalLayout->addWidget(frameSpacing);

    auto *buttonBox = new QDialogButtonBox(diag);
    buttonBox->setObjectName("buttonBox");
    buttonBox->setOrientation(Qt::Horizontal);
    buttonBox->setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttonBox->setCenterButtons(true);
    verticalLayout->addWidget(buttonBox);

    diag->setWindowTitle(tr("Key spacing"));
    label->setText(
        R"(<html><head/><body><p align="center">)" + tr("Number of frames the selected keys should be spaced by:") +
        "</p></body></html>"
    );
    bool connected= connect(buttonBox, &QDialogButtonBox::accepted, this, [diag](){ diag->accept();});
    connected = connected && connect(buttonBox, &QDialogButtonBox::rejected, this, [diag](){ diag->reject();});
    if(!connected){
        diag->deleteLater();
        return;
    }
    QMetaObject::connectSlotsByName(diag);
    frameSpacing->setFocus();
    diag->exec();

    if(diag->result() == QDialog::Accepted){
        // We sort in reverse as to avoid most conflicts while moving keys
        auto targets = sortTargets(&mTargets.targets());
        int spacing = frameSpacing->value();
        // We ignore the first keyframe for each target, which are at the back in this case
        QList<core::TimeKey*> ignoredTargets;
        for(auto target: targets) { ignoredTargets.emplaceBack(target->back().pos.key()); }
        // We move the keys and check if the move was successful. Destinations
        // are unconstrained: the frame range does not gate the keying system,
        // so keys may be spaced past the last frame.
        QList<QPair<core::TimeLineEvent::Target, int>> conflicts;
        for(auto target: targets) {
            int tSize = static_cast<int>(target->size()) - 1;
            int initialFrame = target->back().pos.key()->frame();
            for(auto key : *target) {
                int frameAccumulation = spacing * tSize;
                if (!ignoredTargets.contains(key.pos.key())) {
                    int frame = key.pos.key()->frame();
                    int dest = initialFrame + frameAccumulation;
                    const core::TimeKeyType keyType = key.pos.type();
                    if(!key.pos.line()->move(keyType, frame, dest)) {
                        conflicts.emplace_back(key, dest);
                    }
                    else {
                        // Sigh...
                        void(true);
                    }
                    tSize -= 1;
                }
            }
        }
        if (!conflicts.empty()) {
            QMessageBox msg;
            QStringList errorLog;
            for (const auto& [key, frame] : conflicts) {
                errorLog.append(
                    tr("Key conflict detected for key type ") + keyToString(key.pos.type()) +
                    tr(" in node ") + key.node->name() + tr(" at frame ") + QString::number(frame));
            }
            msg.setWindowTitle(tr("Move error"));
            msg.setText(tr("Unable to move ") + QString::number(conflicts.size()) + tr(" key(s), due to the following reasons: "));
            msg.setDetailedText(errorLog.join("\n"));
            msg.exec();
        }
    }
    diag->deleteLater();
}

void TimeLineEditorWidget::onDeleteKeyTriggered(bool) { mEditor->deleteCheckedKeys(mTargets); }

QColor TimeLineEditorWidget::headerContentColor() const { return mTimelineTheme.headerContentColor(); }

void TimeLineEditorWidget::setHeaderContentColor(const QColor& headerContentColor) {
    mTimelineTheme.setHeaderContentColor(headerContentColor);
}

QColor TimeLineEditorWidget::headerBackgroundColor() const { return mTimelineTheme.headerBackgroundColor(); }

void TimeLineEditorWidget::setHeaderBackgroundColor(const QColor& headerBackgroundColor) {
    mTimelineTheme.setHeaderBackgroundColor(headerBackgroundColor);
}

QColor TimeLineEditorWidget::rulerLineColor() const { return mTimelineTheme.rulerLineColor(); }

void TimeLineEditorWidget::setRulerLineColor(const QColor& rulerLineColor) {
    mTimelineTheme.setRulerLineColor(rulerLineColor);
}


QColor TimeLineEditorWidget::trackColor() const { return mTimelineTheme.trackColor(); }

void TimeLineEditorWidget::setTrackColor(const QColor& trackColor) { mTimelineTheme.setTrackColor(trackColor); }

QColor TimeLineEditorWidget::trackTextColor() const { return mTimelineTheme.trackTextColor(); }

void TimeLineEditorWidget::setTrackTextColor(const QColor& trackTextColor) {
    mTimelineTheme.setTrackTextColor(trackTextColor);
}

QColor TimeLineEditorWidget::trackEdgeColor() const { return mTimelineTheme.trackEdgeColor(); }

void TimeLineEditorWidget::setTrackEdgeColor(const QColor& trackEdgeColor) {
    mTimelineTheme.setTrackEdgeColor(trackEdgeColor);
}

QColor TimeLineEditorWidget::trackSelectColor() const { return mTimelineTheme.trackSelectColor(); }

void TimeLineEditorWidget::setTrackSelectColor(const QColor& trackSelectColor) {
    mTimelineTheme.setTrackSelectColor(trackSelectColor);
}

QColor TimeLineEditorWidget::trackSeperatorColor() const { return mTimelineTheme.trackSeperatorColor(); }

void TimeLineEditorWidget::setTrackSeperatorColor(const QColor& trackSeperatorColor) {
    mTimelineTheme.setTrackSeperatorColor(trackSeperatorColor);
}

//-------------------------------------------------------------------------------------------------
bool TimeLineEditorWidget::openKeyMenu(const core::TimeLineEvent::Target& aPressedKey, const QPoint& aGlobalPos) {
    // THE KEY MENU'S ENTRY POINT for a caller whose pointer is in its own space:
    // the animation graph panel hit-tests the key itself (gui::panelKeyAt, in the
    // panel's pixels) and hands the triple over - it cannot name a pixel in THIS
    // widget's lane geometry, so ctrl::TimeLineEditor::selectKeysAt's hit test is
    // not reachable from it. What that hit test's rule decides is therefore decided
    // here, on the one shared selection, by key identity:
    ctrl::time::Selection& selection = mEditor->selection();

    if (!aPressedKey.node || !aPressedKey.pos.isExist())
        return false; // not a key the document holds (a stale key from the panel's last frame)

    // The pressed key is ALREADY selected: the menu is the SELECTION's, so it acts on
    // every selected key - that is the multi-key case, and nothing is lent or dropped.
    // Any other key replaces the selection for the menu's lifetime (the temporary loan
    // the timeline's own route makes: the pressed key shows as selected while the menu
    // is up, every action consumes that set, and the previous selection is not
    // restored).
    core::TimeKey* lentKey = nullptr;
    if (!selection.contains(aPressedKey)) {
        core::TimeLineEvent single;
        single.pushTarget(*aPressedKey.node, aPressedKey.pos);
        selection.set(single);
        lentKey = selection.items().isEmpty() ? nullptr : selection.items().front().key;
    }

    // WHAT THE ITEMS ACT ON, read AFTER the rule above: the whole selection, one
    // target per key - the set every item in buildKeyMenu works from. It is non-empty
    // by construction (the pressed key is a key the document holds, so the loan put it
    // in, and the other branch left the selection standing), which is what makes this
    // the key menu and never buildKeyMenu's paste-only shape.
    mTargets = core::TimeLineEvent();
    mEditor->retrieveSelectionTargets(mTargets);

    // THE PASTE ANCHOR: the pressed key's frame, in the timeline's own pixel currency -
    // the same composition the keyboard paste uses for the playhead
    // (TimeLineEditorWidget's Ctrl+V listener), so a paste from this menu lands where
    // the key the menu was opened on is.
    mPastePos = QPoint(
        mEditor->timeScale().pixelWidth(aPressedKey.pos.index()) + ctrl::TimeLineEditor::kTimeLineMargin, 0
    );

    QMenu menu(this);
    buildKeyMenu(menu);
    // exec(), not popup(): the whole point of this route is that the loan above is
    // spent by the menu and dropped when it closes - "after the menu" has to be a
    // line in this function. The panel's right-click is inside a press handler; a
    // nested event loop there is what the timeline's own context-menu handler runs
    // too (QMenu owns the interaction either way).
    menu.exec(aGlobalPos);

    // ...AND THE LOAN IS DROPPED, unless an action replaced the selection with
    // something else while the menu was up: paste selects the keys it created
    // (ctrl::TimeLineEditor::selectCreatedKeys) and delete takes its keys out of the
    // set, so only a selection that still names nothing but the lent key is the loan
    // left standing (ctrl::TimeLineEditor::endTemporarySelection is the same drop for
    // the timeline's own route, where the editor holds the flag this widget cannot set).
    if (lentKey) {
        const ctrl::time::Selection& after = mEditor->selection();
        bool stillLent = !after.empty();
        for (const ctrl::time::Selection::Item& item : after.items()) {
            if (item.key != lentKey) {
                stillLent = false;
                break;
            }
        }
        if (stillLent)
            mEditor->selection().clear();
    }

    this->update(); // the selection changed under this widget's own ink
    return true;
}

//-------------------------------------------------------------------------------------------------
void TimeLineEditorWidget::pushEasingChipState() {
    if (!mEasingHost)
        return;

    QVector<util::Easing::Param> values;
    util::Easing::AutoContext autoContext;
    if (mEditor) {
        const ctrl::time::Selection& selection = mEditor->selection();
        bool haveShown = false;
        for (int i = 0; i < selection.items().size(); ++i) {
            const ctrl::time::Selection::Item& item = selection.items()[i];
            // The key is resolved FROM THE DOCUMENT, never taken off the stored
            // pointer: a selection entry outlives the key it names (an undo, a
            // deletion, a project reload), and ctrl::time::Selection documents that
            // its stored key is only ever compared against map entries, never
            // dereferenced (ctrl::time/time_Selection.h). This runs on a PAINT path,
            // so a dereference here would be a use-after-free on a document change
            // this widget had not repainted yet. The (line, type, frame) triple is
            // the identity every other reader resolves through, this widget's own
            // gestures included.
            core::TimeLineEvent::Target target = item.target; // non-const: pos.line() writable
            core::TimeLine* line = target.pos.line();
            if (!line)
                continue;
            core::TimeKey* key = line->timeKey(target.pos.type(), target.pos.index());
            if (!key)
                continue; // a stale entry names a key the document no longer holds
            // ONE EASING PER KEY for the CHIP'S DISPLAY. Each marker now carries
            // its own curve, so the value read is the component of the FIRST
            // entry naming this key; the WRITE path (assignEasingToTargets) is
            // per component and does not collapse them. Keeping one value per
            // key is what makes the chip's "N key(s)" count true.
            const int component =
                (item.component == ctrl::time::Selection::kKeyLevel) ? 0 : item.component;
            bool alreadyNamed = false;
            for (int j = 0; j < i; ++j) {
                const core::TimeLineEvent::Target& earlier = selection.items()[j].target;
                if (earlier.node == target.node && earlier.pos.type() == target.pos.type()
                    && earlier.pos.index() == target.pos.index()) {
                    alreadyNamed = true;
                    break;
                }
            }
            if (alreadyNamed)
                continue;
            const util::Easing::Param* value = keyEasingOf(*key, target.pos.type(), component);
            if (!value)
                continue;
            values.push_back(*value);
            // The chip SHOWS the first key's easing (see prop::EasingChip), so that
            // is the one whose neighbours shape its glyph - the key's own Auto
            // context, which is a document fact (prop::autoContextOf).
            if (!haveShown) {
                autoContext = prop::autoContextOf(target, *key, component);
                haveShown = true;
            }
        }
    }
    mEasingHost->setSelection(values, autoContext);
}

//-------------------------------------------------------------------------------------------------
void TimeLineEditorWidget::setEasingItemHost(prop::EasingItem* aHost) {
    if (mEasingHost) {
        // The host is going away (or being replaced): its callbacks capture this
        // widget, so they must not outlive it.
        mEasingHost->onEasingApplied = nullptr;
        mEasingHost->onPickerClosed = nullptr;
    }
    mEasingHost = aHost;
    if (!mEasingHost) {
        // No chip to keep in step. Nothing to tear down either: the selection's own
        // notification is what pushes it, and pushEasingChipState is a no-op while
        // no host is attached (so a selection made with no chip costs one test).
        return;
    }

    // The one commit path: the control hands every edit the picker commits here,
    // and this widget writes the keys the open named.
    mEasingHost->onEasingApplied = [this](util::Easing::Param aEasing) {
        assignEasingToTargets([aEasing](const util::Easing::Param&) { return aEasing; });
    };
    // The popover is gone, so a right-click's temporary selection ends with it:
    // those keys were lent for the open, and the open is over. Clearing the
    // selection is itself a change, so it announces the chip's new state by the
    // same route every other change takes.
    mEasingHost->onPickerClosed = [this]() {
        mEditor->endTemporarySelection();
        this->update(); // repaints, which re-reads the selection for the chip
    };
    pushEasingChipState();
}

//-------------------------------------------------------------------------------------------------
bool TimeLineEditorWidget::openEasingPicker(const core::TimeLineEvent& aTargets, const QPoint& aGlobalAnchor) {
    if (aTargets.targets().isEmpty() || !mEasingHost)
        return false; // nothing to change the easing of (or no header hosts a picker)

    // What the picker opens on: the FIRST key the open names, with the Auto
    // context that key's own neighbours give it (prop::autoContextOf - an Auto key
    // stores no control points, so without them the tile would draw a shape no key
    // plays). A key of a type that carries no easing is skipped; a target list of
    // only those opens nothing, and nothing is ever written.
    // The COMPONENT the open names, when the shared selection names one: a
    // graph-panel press selects a marker (Move X), and the picker must open on
    // that component's curve rather than the key's first. A whole-key selection
    // (every timeline gesture) has no marker entry and reads component 0.
    const auto componentOf = [this](const core::TimeLineEvent::Target& aTarget) -> int {
        if (!mEditor)
            return 0;
        for (const ctrl::time::Selection::Item& item : mEditor->selection().items()) {
            if (item.component != ctrl::time::Selection::kKeyLevel && item.target.node == aTarget.node
                && item.target.pos.type() == aTarget.pos.type()
                && item.target.pos.index() == aTarget.pos.index()) {
                return item.component;
            }
        }
        return 0;
    };

    util::Easing::Param shown;
    util::Easing::AutoContext autoContext;
    bool hasShown = false;
    for (const core::TimeLineEvent::Target& target : aTargets.targets()) {
        // By value, for the same reason as in assignEasingToTargets: the target list
        // is const here and TimeKeyPos::key() is not a const member.
        core::TimeKeyPos pos = target.pos;
        core::TimeKey* key = pos.key();
        if (!key)
            continue;
        const int component = componentOf(target);
        const util::Easing::Param* easing = keyEasingOf(*key, pos.type(), component);
        if (easing) {
            shown = *easing;
            autoContext = prop::autoContextOf(target, *key, component);
            hasShown = true;
            break;
        }
    }
    if (!hasShown)
        return false;

    mTargets = aTargets;
    // The chip names the same keys the popover edits, so the two must agree the
    // moment it opens - the anchor may be a right-click in the animation graph
    // panel, which changed the shared selection and whose repaint of this surface
    // may not have happened yet.
    pushEasingChipState();
    mEasingHost->openPicker(aGlobalAnchor, this, shown, autoContext);
    return true;
}

} // namespace gui
