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
// The list is also SECTIONED BY PLATFORM, and a section heading is a line of text in
// the same widget as the destinations. So the second question here is the one that
// follows from that: can a heading ever be mistaken for an answer? It carries no id,
// it is not selectable, and the tests below walk the keyboard and click on one to
// prove that neither reaches it.
//
// This is a display-free test on purpose: the assertions are about the list's state,
// and the GUI probes need a display, an accessibility bus and a window manager to
// ask the same question - which is how this went unnoticed.
//
// Run: QT_QPA_PLATFORM=offscreen ./build/tests/Debug/destination_picker
#include <QApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QSet>
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

// A heading is a line with no destination behind it, which is exactly what an empty
// id means to the picker (and to chooseCurrent()).
static bool isHeading(QListWidgetItem* aItem) {
    return aItem && aItem->data(Qt::UserRole).toString().isEmpty();
}

static QString idOf(QListWidgetItem* aItem) {
    return aItem ? aItem->data(Qt::UserRole).toString() : QString();
}

static QString currentId(QListWidget* aList) {
    return idOf(aList ? aList->currentItem() : nullptr);
}

static QList<QListWidgetItem*> headings(QListWidget* aList) {
    QList<QListWidgetItem*> out;
    for (int i = 0; i < aList->count(); ++i) {
        if (isHeading(aList->item(i))) out.push_back(aList->item(i));
    }
    return out;
}

static QStringList headingTexts(QListWidget* aList) {
    QStringList out;
    for (QListWidgetItem* item : headings(aList)) out << item->text();
    return out;
}

static QListWidgetItem* headingNamed(QListWidget* aList, const QString& aText) {
    for (QListWidgetItem* item : headings(aList)) {
        if (item->text() == aText) return item;
    }
    return nullptr;
}

static int visibleRows(QListWidget* aList) {
    int n = 0;
    for (int i = 0; i < aList->count(); ++i) {
        QListWidgetItem* item = aList->item(i);
        if (!item->isHidden() && !isHeading(item)) ++n;
    }
    return n;
}

static void press(QWidget* aTarget, int aKey, int aCount) {
    for (int i = 0; i < aCount; ++i) {
        QKeyEvent event(QEvent::KeyPress, aKey, Qt::NoModifier);
        QApplication::sendEvent(aTarget, &event);
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    gui::GUIResources resources(QStringLiteral(AE_RESOURCE_DIR));
    const QList<ctrl::ExportTarget> targets =
        ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH));
    const QList<ctrl::ExportGroup> groups =
        ctrl::ExportTargets::loadGroups(QStringLiteral(AE_EXPORT_TARGETS_PATH));
    check(targets.size() > 40,
          QStringLiteral("the destination table loads (%1 rows)").arg(targets.size()));
    if (targets.isEmpty()) return 1;

    // 1. The remembered destination is the selected one, which is what makes Enter
    //    the same answer as last time.
    {
        gui::DestinationPicker picker(targets, groups, resources,
                                      QStringLiteral("telegram.sticker"), nullptr);
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
        gui::DestinationPicker picker(targets, groups, resources, QString(), nullptr);
        QListWidget* list = picker.findChild<QListWidget*>(QStringLiteral("destinationList"));
        check(!currentId(list).isEmpty(), "with no remembered destination, the first row is selected");
    }

    // 3. Typing narrows the list and selects the first match, so the search field
    //    plus Enter is a complete answer.
    {
        gui::DestinationPicker picker(targets, groups, resources, QString(), nullptr);
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

    // 4. The sections on screen are the ones the data file declares, in the file's
    //    own order, and only the ones that have a row to show. This is the whole
    //    grouping contract: the ORDER is data (it is what puts "general" first, and
    //    the platforms alphabetically after it), and a heading over an empty section
    //    would be a line the user can click and get nothing from.
    {
        QSet<QString> declared;
        for (const ctrl::ExportGroup& g : groups) declared.insert(g.id);
        check(groups.size() > 20, QStringLiteral("the table declares sections (%1)").arg(groups.size()));

        QStringList undeclared;
        QStringList grouped; // the groups that have at least one row, in use
        for (const ctrl::ExportTarget& t : targets) {
            const QString key = t.group.isEmpty() ? QStringLiteral("general") : t.group;
            if (!declared.contains(key)) undeclared << t.id;
            if (!grouped.contains(key)) grouped << key;
        }
        check(undeclared.isEmpty(),
              QStringLiteral("every row names a declared section%1")
                  .arg(undeclared.isEmpty() ? QString() : QStringLiteral(" (orphans: ") + undeclared.join(QStringLiteral(", ")) + QStringLiteral(")")));

        QStringList expected;
        for (const ctrl::ExportGroup& g : groups) {
            if (grouped.contains(g.id)) expected << g.title;
        }

        gui::DestinationPicker picker(targets, groups, resources, QString(), nullptr);
        QListWidget* list = picker.findChild<QListWidget*>(QStringLiteral("destinationList"));
        if (!list) return 1;
        const QStringList shown = headingTexts(list);
        check(shown == expected,
              QStringLiteral("the headings are the declared sections, in order (%1 shown, %2 expected)")
                  .arg(shown.size()).arg(expected.size()));
        if (shown != expected) {
            QTextStream(stdout) << "      shown:    " << shown.join(QStringLiteral(" | ")) << "\n";
            QTextStream(stdout) << "      expected: " << expected.join(QStringLiteral(" | ")) << "\n";
        }

        check(picker.visibleRowCount() == targets.size(),
              QStringLiteral("with no query every destination is shown (%1 of %2)")
                  .arg(picker.visibleRowCount()).arg(targets.size()));
        check(shown.size() < list->count(),
              QStringLiteral("the list is destinations and headings (%1 headings, %2 items)")
                  .arg(shown.size()).arg(list->count()));
    }

    // 5. A heading is not an answer. It carries no id, it cannot be selected, and
    //    neither the arrow keys nor a click can make it current: Enter must always
    //    mean the destination the screen shows.
    {
        gui::DestinationPicker picker(targets, groups, resources, QString(), nullptr);
        QListWidget* list = picker.findChild<QListWidget*>(QStringLiteral("destinationList"));
        if (!list) return 1;

        const QList<QListWidgetItem*> heads = headings(list);
        check(!heads.isEmpty(), "the list has section headings");
        bool allUnselectable = true, allNamed = true;
        for (QListWidgetItem* h : heads) {
            if (h->flags() & Qt::ItemIsSelectable) allUnselectable = false;
            if (h->text().trimmed().isEmpty()) allNamed = false;
        }
        check(allUnselectable, "a heading cannot be selected");
        check(allNamed, "every heading has a title");

        // Walk the whole list twice, both directions: no step may land on a heading.
        QSet<QString> visited;
        list->setCurrentRow(0);
        bool landedOnHeading = false;
        for (int i = 0; i < targets.size() + groups.size() + 10; ++i) {
            press(list, Qt::Key_Down, 1);
            if (isHeading(list->currentItem())) landedOnHeading = true;
            if (!currentId(list).isEmpty()) visited.insert(currentId(list));
        }
        press(list, Qt::Key_Up, targets.size() + 10);
        if (isHeading(list->currentItem())) landedOnHeading = true;
        check(!landedOnHeading, "the arrow keys never land on a heading");
        check(visited.size() > targets.size() / 2,
              QStringLiteral("and they do reach the destinations (%1 of %2 visited)")
                  .arg(visited.size()).arg(targets.size()));

        // The up/down keys are forwarded from the search field, which is where the
        // focus actually is while the user is typing. That path has its own bugs
        // (the field keeps the text cursor, the list moves the highlight).
        QLineEdit* search = picker.findChild<QLineEdit*>(QStringLiteral("destinationSearch"));
        if (search) {
            search->setFocus();
            search->setText(QStringLiteral("telegram"));
            press(search, Qt::Key_Down, 5);
            check(!isHeading(list->currentItem()),
                  "arrows forwarded from the search field do not land on a heading either");
            check(currentId(list).contains(QStringLiteral("telegram")),
                  QStringLiteral("and they move within the matches (%1)").arg(currentId(list)));
            search->clear();
        }

        // A click. Offscreen this needs the widget laid out, so show it: the question
        // is what a click on a heading does to the selection.
        picker.show();
        QApplication::processEvents();
        auto clickItem = [list](QListWidgetItem* aItem) {
            list->scrollToItem(aItem);
            QApplication::processEvents();
            const QRect rect = list->visualItemRect(aItem);
            if (!rect.isValid() || rect.isEmpty()) return false;
            const QPointF local(rect.center());
            QMouseEvent pressEvent(QEvent::MouseButtonPress, local,
                                   QPointF(list->viewport()->mapToGlobal(rect.center())),
                                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &pressEvent);
            QMouseEvent releaseEvent(QEvent::MouseButtonRelease, local,
                                     QPointF(list->viewport()->mapToGlobal(rect.center())),
                                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &releaseEvent);
            return true;
        };

        // A click on a heading means "THAT platform", not "one row further", so the
        // highlight goes to the first destination under the heading - the same row a
        // key press arriving from above would pick, but deliberately not the row
        // above it, which belongs to the previous platform.
        auto firstRowUnder = [list](QListWidgetItem* aHeading) -> QString {
            for (int i = list->row(aHeading) + 1; i < list->count(); ++i) {
                QListWidgetItem* item = list->item(i);
                if (!item->isHidden() && !isHeading(item)) return idOf(item);
            }
            return QString();
        };
        for (const QString& title :
             QStringList{QStringLiteral("General"), QStringLiteral("Telegram")}) {
            QListWidgetItem* head = headingNamed(list, title);
            check(head != nullptr, QStringLiteral("the %1 heading is in the list").arg(title));
            if (!head || !clickItem(head)) continue;
            check(!isHeading(list->currentItem()),
                  QStringLiteral("clicking the %1 heading does not select it").arg(title));
            check(currentId(list) == firstRowUnder(head),
                  QStringLiteral("and selects the first destination under it (%1, expected %2)")
                      .arg(currentId(list), firstRowUnder(head)));
        }

        // Home and End go to the first and last item, and the first item IS a
        // heading, so this is the case that has to fall back to the other direction.
        list->setCurrentRow(5);
        press(list, Qt::Key_Home, 1);
        check(!isHeading(list->currentItem()),
              QStringLiteral("Home does not leave the highlight on the first heading (%1)")
                  .arg(currentId(list)));
        press(list, Qt::Key_End, 1);
        check(!isHeading(list->currentItem()), "End does not leave the highlight on a heading");
    }

    // 6. Filtering keeps the heading of a section that still has a match, and hides
    //    the ones that lost every row: a heading is shown because it has something
    //    under it, never on its own.
    {
        gui::DestinationPicker picker(targets, groups, resources, QString(), nullptr);
        QListWidget* list = picker.findChild<QListWidget*>(QStringLiteral("destinationList"));
        QLineEdit* search = picker.findChild<QLineEdit*>(QStringLiteral("destinationSearch"));
        if (!list || !search) return 1;
        search->setText(QStringLiteral("instagram"));
        QListWidgetItem* instagram = headingNamed(list, QStringLiteral("Instagram"));
        QListWidgetItem* telegram = headingNamed(list, QStringLiteral("Telegram"));
        QListWidgetItem* general = headingNamed(list, QStringLiteral("General"));
        check(instagram && !instagram->isHidden(), "the matching section keeps its heading");
        check(telegram && telegram->isHidden(), "a section with no match loses its heading");
        check(general && general->isHidden(), "and so does the first one, when nothing in it matches");
        check(visibleRows(list) == picker.visibleRowCount(),
              "visibleRowCount counts destinations, not headings");
        int rows = 0;
        for (const ctrl::ExportTarget& t : targets) {
            if (t.group == QLatin1String("instagram")) ++rows;
        }
        check(visibleRows(list) == rows,
              QStringLiteral("the search shows exactly the section's rows (%1 of %2)")
                  .arg(visibleRows(list)).arg(rows));

        search->clear();
        check(headingTexts(list).size() == headings(list).size() && visibleRows(list) == targets.size(),
              "clearing the search restores every row and every heading");
        bool anyHidden = false;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->isHidden()) anyHidden = true;
        }
        check(!anyHidden, "and nothing stays hidden");
    }

    // 7. The default button answers with a destination, never with nothing: "Use this
    //    destination" is the dialog's own promise, and an empty id means the escape
    //    hatch (the format form), which is a different answer.
    {
        gui::DestinationPicker picker(targets, groups, resources, QString(), nullptr);
        QDialogButtonBox* box = picker.findChild<QDialogButtonBox*>();
        check(box != nullptr, "the picker has its buttons");
        if (box) {
            box->button(QDialogButtonBox::Ok)->click();
            const QString chosen = picker.chosenId();
            check(!chosen.isEmpty(), "the default button answers with a destination");
            check(ctrl::ExportTargets::find(targets, chosen).id == chosen,
                  QStringLiteral("and it is a row of the table (%1)").arg(chosen));
        }
    }

    QTextStream(stdout) << (failures == 0 ? "=== destination_picker: OK ===\n"
                                          : QStringLiteral("=== destination_picker: %1 FAILED ===\n").arg(failures));
    return failures == 0 ? 0 : 1;
}
