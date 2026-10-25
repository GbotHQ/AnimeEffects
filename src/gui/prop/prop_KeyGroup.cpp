#include <QVBoxLayout>

#include "gui/prop/prop_KeyGroup.h"
#include "gui/prop/prop_PropertyRow.h"

namespace gui {
namespace prop {

    KeyGroup::KeyGroup(const QString& aTitle, int aLabelWidth, GUIResources* aGUIResources):
        QFrame(nullptr), mHeader(nullptr), mLayout(nullptr), mItems(), mRows() {
        Q_UNUSED(aLabelWidth);
        Q_UNUSED(aGUIResources);

        this->setObjectName("keyGroup");
        this->setFrameShape(QFrame::NoFrame);
        this->setFocusPolicy(Qt::NoFocus);

        mLayout = new QVBoxLayout(this);
        mLayout->setContentsMargins(0, 0, 0, 0);
        mLayout->setSpacing(0);

        mHeader = new PropertyRow(aTitle, true, this);
        mLayout->addWidget(mHeader);
    }

    KeyGroup::~KeyGroup() {
        for (ItemBase* item : mItems)
            delete item;
    }

    void KeyGroup::addItem(const QString& aLabel, ItemBase* aItem) {
        if (!aItem)
            return;
        mItems.push_back(aItem);

        if (aLabel.isEmpty()) {
            // A single-value property lives on the title row.
            if (aItem->itemWidget())
                mHeader->addField(aItem->itemWidget());
            else if (aItem->itemLayout())
                mHeader->addFieldsFromLayout(aItem->itemLayout());
            return;
        }

        auto* row = new PropertyRow(aLabel, false, this);
        if (aItem->itemWidget())
            row->addField(aItem->itemWidget());
        else if (aItem->itemLayout())
            row->addFieldsFromLayout(aItem->itemLayout());
        mLayout->addWidget(row);
        mRows.push_back(row);
    }

    void KeyGroup::setItemVisible(ItemBase* aItem, bool aVisible) {
        aItem->setItemVisible(aVisible);
        const int index = mItems.indexOf(aItem);
        if (index >= 0) {
            // The header owns the single-field row; subrows own the rest.
            if (index > 0 && index - 1 < mRows.size())
                mRows[index - 1]->setVisible(aVisible);
        }
    }

    void KeyGroup::addHeaderAction(QWidget* aWidget) {
        if (mHeader)
            mHeader->setKeyWidget(aWidget);
    }

    void KeyGroup::setHeaderCollapsible(bool) {}
    void KeyGroup::inlineFields() {}
    void KeyGroup::makeSureExpand() {}

} // namespace prop
} // namespace gui
