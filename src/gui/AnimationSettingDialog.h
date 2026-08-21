#ifndef GUI_ANIMATIONSETTINGDIALOG_H
#define GUI_ANIMATIONSETTINGDIALOG_H

#include <QCheckBox>
#include <QSpinBox>
#include "gui/EasyDialog.h"
#include "core/Project.h"

namespace gui {

// Dialog behind the playback bar's options button. The frame range (start and
// end) and frames per second are project attributes (they affect export); the
// loop flag is the live playback loop.
class AnimationSettingDialog: public EasyDialog {
    Q_OBJECT
public:
    AnimationSettingDialog(core::Project& aProject, bool aCurLoop, QWidget* aParent);

    bool isLoopChecked() const { return mLoopBox->isChecked(); }
    util::Range frameRange() const { return util::Range(mStartBox->value(), mEndBox->value()); }
    int fps() const { return mFPSBox->value(); }

private:
    core::Project& mProject;
    QCheckBox* mLoopBox;
    QSpinBox* mStartBox;
    QSpinBox* mEndBox;
    QSpinBox* mFPSBox;
};

} // namespace gui

#endif // GUI_ANIMATIONSETTINGDIALOG_H
