#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QWidget>

#include "gui/prop/prop_PropertyRow.h"

namespace gui {
namespace prop {

    PropertyRow::PropertyRow(const QString& aLabel, bool aTopLevel, QWidget* aParent):
        QWidget(aParent),
        mKeyHost(new QWidget(this)),
        mKeyLayout(new QHBoxLayout(mKeyHost)),
        mLabel(new QLabel(aLabel, this)),
        mFieldLayout(nullptr) {
        this->setObjectName("propertyRow");
        this->setAttribute(Qt::WA_StyledBackground, true);
        Q_UNUSED(aTopLevel);
        // One row height everywhere: a top-level row is not taller than a subrow,
        // so the field-to-field gaps cannot differ between them.
        this->setFixedHeight(27);

        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(16, 0, 16, 0);
        row->setSpacing(0);

        mKeyHost->setFixedWidth(20);
        mKeyLayout->setContentsMargins(0, 0, 0, 0);
        mKeyLayout->setSpacing(0);
        row->addWidget(mKeyHost);
        row->addSpacing(6);

        mLabel->setObjectName("propertyRowLabel");
        mLabel->setFixedWidth(72);
        mLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        row->addWidget(mLabel);
        row->addSpacing(10);

        auto* fieldHost = new QWidget(this);
        fieldHost->setObjectName("propertyRowFields");
        mFieldLayout = new QHBoxLayout(fieldHost);
        mFieldLayout->setContentsMargins(0, 0, 0, 0);
        mFieldLayout->setSpacing(6);
        row->addWidget(fieldHost, 1);
    }

    void PropertyRow::setKeyWidget(QWidget* aWidget) {
        if (!aWidget)
            return;
        mKeyLayout->addWidget(aWidget, 0, Qt::AlignCenter);
    }

    void PropertyRow::addField(QWidget* aWidget) {
        if (!aWidget)
            return;
        // One field height for every row, so a combo does not make its row
        // taller than a spin box next to it.
        aWidget->setFixedHeight(23);
        mFieldLayout->addWidget(aWidget, 0, Qt::AlignVCenter);
    }

    void PropertyRow::addFieldsFromLayout(QLayout* aLayout) {
        if (!aLayout)
            return;
        while (QLayoutItem* child = aLayout->takeAt(0)) {
            if (QWidget* widget = child->widget()) {
                widget->setFixedHeight(23);
                mFieldLayout->addWidget(widget, 0, Qt::AlignVCenter);
            }
            delete child;
        }
    }

    void PropertyRow::setLabelAlignment(Qt::Alignment aAlignment) {
        mLabel->setAlignment(aAlignment | Qt::AlignVCenter);
    }

} // namespace prop
} // namespace gui
