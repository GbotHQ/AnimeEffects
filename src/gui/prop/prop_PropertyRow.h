#ifndef GUI_PROP_PROPERTYROW_H
#define GUI_PROP_PROPERTYROW_H

#include <QWidget>

class QHBoxLayout;
class QLabel;
class QLayout;

namespace gui {
namespace prop {

// ONE row of the inspector grid:
//
//   [key gutter 20] [label 72] [fields ...]
//
// The gutter is present even when a property has no key, so keyable and
// non-keyable labels start on the same x and their fields start on the same x.
// This is the ONLY row builder for property values; the key button is just an
// optional child of the gutter.
class PropertyRow: public QWidget {
public:
    PropertyRow(const QString& aLabel, bool aTopLevel, QWidget* aParent = nullptr);

    void setKeyWidget(QWidget* aWidget);
    void addField(QWidget* aWidget);
    // Move a vector item's widgets (not the layout itself) into this row.
    void addFieldsFromLayout(QLayout* aLayout);

    void setLabelAlignment(Qt::Alignment aAlignment);
    QLabel* label() const { return mLabel; }
    QHBoxLayout* fieldLayout() const { return mFieldLayout; }

private:
    QWidget* mKeyHost;
    QHBoxLayout* mKeyLayout;
    QLabel* mLabel;
    QHBoxLayout* mFieldLayout;
};

} // namespace prop
} // namespace gui

#endif // GUI_PROP_PROPERTYROW_H
