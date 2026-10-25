#ifndef GUI_PROP_KEYGROUP_H
#define GUI_PROP_KEYGROUP_H

#include <QFrame>
#include <QVector>
#include "gui/prop/prop_ItemBase.h"

class QVBoxLayout;

namespace gui {

class GUIResources;

namespace prop {

    class PropertyRow;

    // A keyable property block. Its rows are built by PropertyRow, the same
    // builder the non-keyable rows use; the key button is only an optional
    // child of the row's leading gutter.
    class KeyGroup: public QFrame {
        Q_OBJECT
    public:
        KeyGroup(const QString& aTitle, int aLabelWidth, GUIResources* aGUIResources);
        ~KeyGroup() override;

        void addItem(const QString& aLabel, ItemBase* aItem);
        void setItemVisible(ItemBase* aItem, bool aVisible);
        void addHeaderAction(QWidget* aWidget);

        // Kept for the panel's old API. Every property row is now a flat row,
        // so there is no collapse or "inline" mode to switch.
        void setHeaderCollapsible(bool aCollapsible);
        void inlineFields();
        void makeSureExpand();

    signals:
        void expansionChanged(bool aExpanded);

    private:
        PropertyRow* mHeader;
        QVBoxLayout* mLayout;
        QVector<ItemBase*> mItems;
        QVector<PropertyRow*> mRows;
    };

} // namespace prop
} // namespace gui

#endif // GUI_PROP_KEYGROUP_H
