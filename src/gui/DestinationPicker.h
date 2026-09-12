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

// The destination chooser: a search field and a list of PLATFORM SECTIONS, in a
// modal.
//
// WHY A MODAL AND NOT A MENU. There are fifty-five destinations. As a menu that is
// four levels deep (File -> Quickly export as -> a section -> the destination), and
// at any level the full list is forty rows that wrap into two columns and run off
// the bottom of the screen. A menu cannot be typed into, and typing is exactly what
// makes fifty-five rows navigable: three letters of "telegram", or "sticker" to see
// every sticker destination, is faster than reading any grouping.
//
// PLATFORM SECTIONS. The list is grouped by the platform a person is posting to
// ("I am posting to Instagram"), which is the one thing they know before they
// start, and within a section by the destination's label, so a platform's variants
// ("Instagram Reels", "Instagram (photo)") sit together under one heading. The
// sections and their order come from the <groups> block of the data file; a heading
// is drawn only when its section has a row to show.
//
// The escape hatch for a platform the list does not know is a button, not a
// destination, so it cannot be filtered away.
class DestinationPicker : public QDialog {
public:
    // aCurrentId preselects a destination (empty = none).
    //
    // The two constructors differ only in where the rows come from. The rows and
    // their sections are the SAME data file, so the convenience overload is the one
    // the application uses: a call site that loaded the rows from one path and the
    // sections from another could only ever be a mistake.
    DestinationPicker(GUIResources& aResources, const QString& aCurrentId, QWidget* aParent);
    DestinationPicker(const QList<ctrl::ExportTarget>& aTargets,
                      const QList<ctrl::ExportGroup>& aGroups, GUIResources& aResources,
                      const QString& aCurrentId, QWidget* aParent);

    // The chosen destination id, or empty for "my platform isn't listed".
    QString chosenId() const { return mChosenId; }

    // Rows currently shown, for tests and probes (the escape hatch excluded).
    int visibleRowCount() const;

private:
    void applyFilter(const QString& aQuery);
    void chooseCurrent();

    // The nearest destination to a list position. A heading is a label, not an
    // answer, but Qt lets the current index sit on one (its arrow keys and its
    // mouse presses do not skip a non-selectable item, which is what a heading has
    // to be), so every route that can move the highlight goes through here instead.
    // aDirection: +1 searches downwards first, -1 upwards, 0 downwards and then up.
    QListWidgetItem* nearestRow(int aFromRow, int aDirection) const;

    bool eventFilter(QObject* aWatched, QEvent* aEvent) override;

    QLineEdit* mSearch = nullptr;
    QListWidget* mList = nullptr;
    QPushButton* mManualButton = nullptr;
    QString mChosenId;
};

} // namespace gui

#endif // GUI_DESTINATIONPICKER_H
