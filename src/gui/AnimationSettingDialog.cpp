#include "gui/AnimationSettingDialog.h"
#include "core/ObjectNodeUtil.h"
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <limits>

namespace gui {

AnimationSettingDialog::AnimationSettingDialog(core::Project& aProject, bool aCurLoop, QWidget* aParent):
    EasyDialog(tr("Animation settings"), aParent),
    mProject(aProject),
    mLoopBox(nullptr),
    mStartBox(nullptr),
    mEndBox(nullptr),
    mFPSBox(nullptr) {
    const util::Range curRange = aProject.attribute().frameRange();
    const int curFPS = aProject.attribute().fps();

    auto form = new QFormLayout();
    form->setFormAlignment(Qt::AlignHCenter | Qt::AlignTop);
    form->setLabelAlignment(Qt::AlignRight);

    {
        auto layout = new QHBoxLayout();
        mLoopBox = new QCheckBox();
        mLoopBox->setChecked(aCurLoop);
        layout->addWidget(mLoopBox);
        form->addRow(tr("Loop animation"), layout);
    }
    {
        // Frame range: two linked spinboxes. Either bound may be negative
        // (pre-roll frames before 0 are a valid project extent); the end
        // cannot drop below the start. Keys may live outside the range — the
        // range is a playback/export bound, not a constraint on key positions
        // — so every valid start <= end change is accepted.
        auto layout = new QHBoxLayout();
        mStartBox = new QSpinBox();
        mStartBox->setRange(std::numeric_limits<int>::min(), 0x7fffffff);
        mStartBox->setValue(curRange.min());
        layout->addWidget(mStartBox);

        mEndBox = new QSpinBox();
        mEndBox->setRange(std::numeric_limits<int>::min(), 0x7fffffff);
        mEndBox->setValue(curRange.max());
        layout->addWidget(mEndBox);

        mStartBox->setMaximum(mEndBox->value());
        mEndBox->setMinimum(mStartBox->value());
        mStartBox->connect(mStartBox, &QSpinBox::valueChanged, [=](int aValue) {
            mEndBox->setMinimum(aValue);
        });
        mEndBox->connect(mEndBox, &QSpinBox::valueChanged, [=](int aValue) {
            mStartBox->setMaximum(aValue);
        });
        form->addRow(tr("Frame range"), layout);
    }
    {
        auto layout = new QHBoxLayout();
        mFPSBox = new QSpinBox();
        mFPSBox->setRange(1, 0x7fffffff);
        mFPSBox->setValue(curFPS);
        layout->addWidget(mFPSBox);
        form->addRow(tr("Frames per second"), layout);
    }

    this->setMainLayout(form);

    this->setOkCancel([=](int) -> bool {
        // Keys may live outside the frame range — the range is a
        // playback/export bound, not a constraint on key positions — so every
        // range change is valid.
        return true;
    });
    this->fixSize();
}

} // namespace gui
