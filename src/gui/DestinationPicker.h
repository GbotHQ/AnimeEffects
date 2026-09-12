#ifndef GUI_DESTINATIONPICKER_H
#define GUI_DESTINATIONPICKER_H

#include <QDialog>
#include <QList>
#include <QString>
#include "ctrl/ExportTarget.h"

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace gui {

class GUIResources;

// The destination chooser: a search field and a list, in a modal.
//
// WHY A MODAL AND NOT A MENU. There are fifty-five destinations. As a menu that is
// four levels deep (File -> Quickly export as -> a family -> the destination), and
// at any level the full list is forty rows that wrap into two columns and run off
// the bottom of the screen. A menu cannot be typed into, and typing is exactly what
// makes fifty-five rows navigable: three letters of "telegram", or "sticker" to see
// every sticker destination, is faster than reading any grouping.
//
// PLATFORM FIRST. The list is ordered by the destination's own label, which begins
// with the platform for every row that has one, so the scan axis is the thing a
// person knows before they start ("I am posting to Instagram"). Purpose is the
// search axis instead of the layout axis: typing "sticker", "chat", "photo" or
// "overlay" filters by what you are making, including the family names, without
// making everyone walk a purpose tree to reach a platform they already know.
class DestinationPicker : public QDialog {
public:
    // aCurrentId preselects a destination (empty = none). The "my platform isn't
    // listed" row is always present: it is the answer for the person this list
    // fails, so it cannot be filtered away.
    DestinationPicker(const QList<ctrl::ExportTarget>& aTargets, GUIResources& aResources,
                      const QString& aCurrentId, QWidget* aParent);

    // The chosen destination id, or empty for "my platform isn't listed".
    QString chosenId() const { return mChosenId; }

    // Rows currently shown, for tests and probes (the escape hatch excluded).
    int visibleRowCount() const;

private:
    void applyFilter(const QString& aQuery);
    void chooseCurrent();
    bool eventFilter(QObject* aWatched, QEvent* aEvent) override;

    QLineEdit* mSearch = nullptr;
    QListWidget* mList = nullptr;
    QPushButton* mManualButton = nullptr;
    QString mChosenId;
};

} // namespace gui

#endif // GUI_DESTINATIONPICKER_H
