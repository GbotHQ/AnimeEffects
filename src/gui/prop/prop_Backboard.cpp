#include "gui/prop/prop_Backboard.h"

#include <QFrame>
#include <QTimer>

namespace gui {
namespace prop {

    Backboard::Backboard(ViaPoint& aViaPoint, QWidget* aParent, GUIResources* aGUIResources):
        QWidget(aParent),
        // Declaration order is mViaPoint, mGUIResources, mProject, mLayout, ...;
        // the list is written to match, since that is the order the compiler
        // actually uses.
        mViaPoint(aViaPoint),
        mGUIResources(aGUIResources),
        mProject(),
        mLayout(),
        mConstantPanel(),
        mCurrentKeyPanel() {
        // the dock's single flat surface (see propertywidget.ssa)
        this->setObjectName("backboard");
        resetLayout();
    }

    void Backboard::resetLayout() {
        if (mLayout)
            delete mLayout;
        mLayout = new QVBoxLayout();
        mLayout->setSpacing(1);
        // edge to edge, with 5px of dock-top breathing on top of the
        // panels' 3px top padding (the internal hairline gaps stay 4px);
        // contentsMargins order is left, top, right, bottom
        mLayout->setContentsMargins(0, 5, 0, 0);
        this->setLayout(mLayout);
    }

    bool Backboard::event(QEvent* aEvent) {
        if (aEvent->type() == QEvent::LayoutRequest) {
            // The scroll area (PropertyWidget) sizes this widget to its
            // sizeHint only once; later content changes (group expand/
            // collapse, rows rebuilt) would otherwise leave the board too
            // short and compress the field rows below their sizeHint.
            // Resize synchronously: the old deferred-timer version kept the
            // old height for one paint, clipping expanded content for a
            // frame (collapse only left empty space, so it looked fine).
            const int natural = qMax(sizeHint().height(), parentWidget() ? parentWidget()->height() : 0);
            if (height() != natural) {
                resize(width(), natural);
            }
        }
        return QWidget::event(aEvent);
    }

    void Backboard::setProject(core::Project* aProject) {
        mConstantPanel.reset();
        mCurrentKeyPanel.reset();

        resetLayout();

        mProject = aProject;

        if (mProject) {
            mConstantPanel.reset(new ConstantPanel(mViaPoint, *mProject, this, mGUIResources));
            mLayout->addWidget(mConstantPanel.data());
            mLayout->setAlignment(mConstantPanel.data(), Qt::AlignTop);

            mCurrentKeyPanel.reset(new CurrentKeyPanel(mViaPoint, *mProject, this, mGUIResources));
            mLayout->addWidget(mCurrentKeyPanel.data());
            mLayout->setAlignment(mCurrentKeyPanel.data(), Qt::AlignTop);
        }
        mLayout->addStretch();
    }

    void Backboard::setTarget(core::ObjectNode* aNode) {
        if (mConstantPanel) {
            mConstantPanel->setTarget(aNode);
        }
        if (mCurrentKeyPanel) {
            mCurrentKeyPanel->setTarget(aNode);
        }
    }

    void Backboard::setPlayBackActivity(bool aIsActive) {
        this->setEnabled(!aIsActive);

        if (mConstantPanel) {
            mConstantPanel->setPlayBackActivity(aIsActive);
        }
        if (mCurrentKeyPanel) {
            mCurrentKeyPanel->setPlayBackActivity(aIsActive);
        }
    }

    void Backboard::updateAttribute() {
        if (mConstantPanel) {
            mConstantPanel->updateAttribute();
        }
    }

    void Backboard::updateKey(bool aUpdateKey, bool aUpdateDefaultKey) {
        // One inspector owns both halves now: a default-key edit and an
        // explicit-key edit are the same board update (model 2 selects which
        // key the accessor writes, not which panel the user sees).
        if ((aUpdateKey || aUpdateDefaultKey) && mCurrentKeyPanel) {
            mCurrentKeyPanel->updateKey();
        }
    }

    void Backboard::updateFrame() {
        if (mCurrentKeyPanel) {
            mCurrentKeyPanel->updateFrame();
        }
    }

} // namespace prop
} // namespace gui
