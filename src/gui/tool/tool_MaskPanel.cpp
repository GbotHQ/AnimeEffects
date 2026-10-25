#include <algorithm>
#include "gui/tool/tool_MaskPanel.h"
#include "gui/tool/tool_ItemTable.h"

namespace {
const int kButtonSize = 24;
const int kButtonSpace = kButtonSize;
// the lanes the mask tool can write, in the order the SingleOutItem shows them
const core::TimeKeyType kLanes[] = {core::TimeKeyType_HSV, core::TimeKeyType_Blur, core::TimeKeyType_Wave};
const int kLaneCount = 3;
} // namespace

namespace gui {
namespace tool {

    MaskPanel::MaskPanel(QWidget* aParent, GUIResources& aResources):
        QGroupBox(aParent), mResources(aResources), mParam(), mLaneGroup(), mChannelGroup(), mValue(), mBrush(),
        mClear() {
        this->setTitle(tr("Filter mask"));
        mResources.onThemeChanged.connect(this, &MaskPanel::onThemeUpdated);
        createItems();
    }

    void MaskPanel::applyIcons() {
        // the lane row reuses the filter icons the stack already uses
        mLaneGroup->setIcons(
            QVector<QIcon>() << mResources.icon("faders-horizontal") << mResources.icon("ffd")
                             << mResources.icon("paint-brush"),
            QSize(18, 18)
        );
        mChannelGroup->setIcons(
            QVector<QIcon>() << mResources.icon("circle-half") << mResources.icon("arrow-clockwise"),
            QSize(18, 18)
        );

    }

    void MaskPanel::onThemeUpdated(theme::Theme&) { applyIcons(); }

    void MaskPanel::createItems() {
        // WHICH filter lane: the mask belongs to a lane, because that is the thing
        // the renderer consults (see core/TimeLine::filterMask)
        mLaneGroup.reset(new SingleOutItem(kLaneCount, QSize(kButtonSpace, kButtonSpace), this));
        mLaneGroup->setChoice(2); // Wave
        mLaneGroup->setToolTips(QStringList() << tr("Mask for HSV") << tr("Mask for blur") << tr("Mask for wave deform"));
        mParam.lane = core::TimeKeyType_Wave;
        mLaneGroup->connect([=](int aIndex) {
            this->mParam.lane = kLanes[aIndex];
            this->onParamUpdated(false);
        });

        // WHICH input of that filter: the amount it scales, or the time it delays
        mChannelGroup.reset(new SingleOutItem(core::FilterMaskChannel_TERM, QSize(kButtonSpace, kButtonSpace), this));
        mChannelGroup->setChoice(int(mParam.channel));
        mChannelGroup->setToolTips(
            QStringList() << tr("How much the filter applies") << tr("How much later the filter applies")
        );
        mChannelGroup->connect([=](int aIndex) {
            this->mParam.setChannel((core::FilterMaskChannel)aIndex);
            this->mValue->setAttribute(util::Range(0, 100), int(this->mParam.value * 100.0f), 10);
            this->onParamUpdated(true);
        });

        // WHAT the brush paints: the field's value moves TOWARD this, so one slider
        // covers both reducing an effect (0) and restoring or adding it (1). The
        // temporary eyedropper (Alt + click) sets it from what is already painted.
        // "Value" is what the brush paints TOWARD; Alt + click on the canvas samples
        // it from the field (the temporary eyedropper).
        mValue.reset(new SliderItem(tr("Value"), this->palette(), this));
        mValue->setAttribute(util::Range(0, 100), int(mParam.value * 100.0f), 10);
        mValue->connectOnChanged([=](int aValue) {
            this->mParam.value = float(aValue) / 100.0f;
            this->onParamUpdated(false);
        });

        applyIcons();

        // the shared brush rows; Opacity is hidden because the mask has VALUE, not an
        // alpha to accumulate - a stamp converges on the value
        mBrush.reset(new BrushPanel(
            this, mParam.brush,
            BrushPanel::Row_Size | BrushPanel::Row_Hardness | BrushPanel::Row_Flow | BrushPanel::Row_Pressure
        ));
        mBrush->onParamUpdated.connect([=](bool aDirect) { this->onParamUpdated(aDirect); });

        mClear.reset(new QPushButton(tr("Clear mask"), this));
        mClear->setToolTip(tr("Clear the field being painted (one undo step)"));
        QObject::connect(mClear.data(), &QPushButton::clicked, this, [this]() { this->onClearRequested(); });
    }

    void MaskPanel::refresh() { mBrush->refresh(); }

    void MaskPanel::setLaneAndChannel(core::TimeKeyType aLane, core::FilterMaskChannel aChannel) {
        setLane(aLane);
        mParam.setChannel(aChannel);
        mChannelGroup->setChoice(int(aChannel));
        mValue->setAttribute(util::Range(0, 100), int(mParam.value * 100.0f), 10);
        onParamUpdated(true);
    }

    void MaskPanel::setValue(float aValue) {
        mParam.value = std::min(std::max(aValue, 0.0f), 1.0f);
        mValue->setAttribute(util::Range(0, 100), int(mParam.value * 100.0f), 10);
        onParamUpdated(false);
    }

    void MaskPanel::setLane(core::TimeKeyType aLane) {
        for (int i = 0; i < kLaneCount; ++i) {
            if (kLanes[i] == aLane) {
                mParam.lane = aLane;
                mLaneGroup->setChoice(i);
                onParamUpdated(false);
                return;
            }
        }
    }

    int MaskPanel::updateGeometry(const QPoint& aPos, int aWidth) {
        static const int kItemLeft = 8;
        const int kItemTop = this->contentsMargins().top();

        const int itemWidth = aWidth - kItemLeft * 2;
        QPoint curPos(kItemLeft, kItemTop);

        curPos.setY(mLaneGroup->updateGeometry(curPos, itemWidth) + curPos.y() + 5);
        curPos.setY(mChannelGroup->updateGeometry(curPos, itemWidth) + curPos.y() + 5);
        curPos.setY(mValue->updateGeometry(curPos, itemWidth) + curPos.y() + 5);
        curPos.setY(mBrush->updateGeometry(curPos, itemWidth) + 5);
        curPos.setY(mClear->geometry().height() + curPos.y() + 5);
        mClear->setGeometry(kItemLeft, curPos.y() - mClear->geometry().height() - 5, itemWidth, 24);

        const int b = this->contentsMargins().bottom();
        this->setGeometry(aPos.x(), aPos.y(), aWidth, curPos.y() + b);
        return aPos.y() + curPos.y() + b;
    }

} // namespace tool
} // namespace gui
