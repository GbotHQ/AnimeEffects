// The destination picker, offscreen: what the user actually gets.
//
// Every question about this list is a question about a person, and the person's own
// answer is already in the application: MainWindow passes the REMEMBERED
// destination in (QSettings "export_destination"), and DestinationPicker.h says that
// id "preselects a destination". It did not. The constructor set the current item,
// and then the last line of the constructor called applyFilter("") which does
// `setCurrentRow(firstVisible)` - so the list opened with the FIRST row current, and
// "A video for a chat app" was what Enter (or the dialog's default button) would
// export to, whatever the user chose last time. A remembered setting that the
// screen contradicts is worse than no remembered setting.
//
// This is a display-free test on purpose: the assertions are about the list's state,
// and the GUI probes need a display, an accessibility bus and a window manager to
// ask the same question - which is how this went unnoticed.
//
// Run: QT_QPA_PLATFORM=offscreen ./build/tests/Debug/destination_picker
#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QTextStream>
#include <cstdio>

#include "ctrl/ExportTarget.h"
#include "gui/DestinationPicker.h"
#include "gui/GUIResources.h"

static int failures = 0;

static void check(bool ok, const QString& what) {
    QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

static QString currentId(QListWidget* aList) {
    QListWidgetItem* item = aList ? aList->currentItem() : nullptr;
    return item ? item->data(Qt::UserRole).toString() : QString();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    gui::GUIResources resources(QStringLiteral(AE_RESOURCE_DIR));
    const QList<ctrl::ExportTarget> targets =
        ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH));
    check(targets.size() > 40,
          QStringLiteral("the destination table loads (%1 rows)").arg(targets.size()));
    if (targets.isEmpty()) return 1;

    // 1. The remembered destination is the selected one, which is what makes Enter
    //    the same answer as last time.
    {
        gui::DestinationPicker picker(targets, resources, QStringLiteral("telegram.sticker"), nullptr);
        QListWidget* list = picker.findChild<QListWidget*>(QStringLiteral("destinationList"));
        check(list != nullptr, "the picker has a destination list");
        if (!list) return 1;
        const QString shown = currentId(list);
        check(shown == QStringLiteral("telegram.sticker"),
              QStringLiteral("the remembered destination is selected, not the first row "
                             "(selected: %1)").arg(shown.isEmpty() ? QStringLiteral("nothing") : shown));
    }

    // 2. With nothing remembered there is still a row to press Enter on: the first
    //    destination, so the dialog is never in a state where the default button
    //    does something undefined.
    {
        gui::DestinationPicker picker(targets, resources, QString(), nullptr);
        QListWidget* list = picker.findChild<QListWidget*>(QStringLiteral("destinationList"));
        check(!currentId(list).isEmpty(), "with no remembered destination, the first row is selected");
    }

    // 3. Typing narrows the list and selects the first match, so the search field
    //    plus Enter is a complete answer.
    {
        gui::DestinationPicker picker(targets, resources, QString(), nullptr);
        QListWidget* list = picker.findChild<QListWidget*>(QStringLiteral("destinationList"));
        QLineEdit* search = picker.findChild<QLineEdit*>(QStringLiteral("destinationSearch"));
        check(search != nullptr, "the picker can be typed at");
        if (!search || !list) return 1;
        search->setText(QStringLiteral("wechat sticker"));
        const QString shown = currentId(list);
        check(shown.contains(QStringLiteral("wechat")),
              QStringLiteral("a search selects one of its matches (%1)").arg(shown));
        check(picker.visibleRowCount() >= 1 && picker.visibleRowCount() < targets.size(),
              QStringLiteral("a search narrows the list (%1 of %2 rows)")
                  .arg(picker.visibleRowCount()).arg(targets.size()));
    }

    QTextStream(stdout) << (failures == 0 ? "=== destination_picker: OK ===\n"
                                          : QStringLiteral("=== destination_picker: %1 FAILED ===\n").arg(failures));
    return failures == 0 ? 0 : 1;
}
