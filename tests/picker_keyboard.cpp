// The destination picker's keyboard contract, offscreen and windowless.
//
// This is the coverage hole the review named: the picker's search field could not
// keep what it was given. B7/B8/B9 say which key belongs to whom while the field has
// focus, and nothing tested it, because the GUI probes cannot: driving a keystroke
// needs a window manager, and the whole point of these lines is the ROUTING of a key
// between the field and the list, which is a question about a scene.
//
// So the scene is built and the keys are injected in-process, which is what Qt's own
// autotests do. Measured before relying on it (and worth knowing): a key sent
// straight to a Qt Quick item is delivered to the item's Keys filter with NO window
// and NO active focus - the filter consumes it, and everything it does not consume
// still reaches the editor (a character key really does grow the text and move the
// caret). That is why this needs neither a display nor a GL context, and why it can
// tell "the filter ate it" apart from "the editor ate it": Left would otherwise
// move the caret.
//
// The observable is the MODEL, not the widgets: what a person is affected by is the
// query, the highlighted row and which rows are on screen, and those are the same
// objects the scene binds to. The scene is the real DestinationPicker.qml, so the
// wiring between the field and the model is under test and not a reimplementation
// of it.
//
// Deliberately NOT here, because it needs a window and this file has none:
//   B12 the pointer is an I-beam over the text area (a cursor shape is a property of
//       a real window, and X11 screen grabs do not contain the pointer either - this
//       needs `ffmpeg -f x11grab -draw_mouse 1` against a visible window).
//   B11 clicking outside the field removes its focus (there is no focus without a
//       window, so there is nothing to remove).
//
// Run: QT_QPA_PLATFORM=offscreen ./build/tests/Debug/picker_keyboard
#include <QApplication>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUrl>
#include <QColor>

#include "ctrl/ExportTarget.h"
#include "gui/qml/DestinationPickerModel.h"
#include "gui/qml/QmlTheme.h"
#include "gui/theme/Icons.h"

using Model = gui::DestinationPickerModel;

static int failures = 0;

static void check(bool ok, const QString& what) {
    QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

// Send one key straight to the item. Returns whether it was accepted, which is a
// useful extra fact but never the assertion by itself: "accepted" cannot tell the
// filter apart from the editor, and that distinction is the whole contract.
static bool sendKey(QQuickItem* aItem, int aKey, const QString& aText = QString()) {
    QKeyEvent event(QEvent::KeyPress, aKey, Qt::NoModifier, aText);
    QCoreApplication::sendEvent(aItem, &event);
    return event.isAccepted();
}

// Which rows are on screen, in order. A group that closes loses its children from
// this list, so comparing two of these is how "an arrow closed a group" is detected
// without trusting any single flag.
static QStringList onScreenRows(const Model& aModel) {
    QStringList out;
    for (int row = 0; row < aModel.rowCount(); ++row) {
        if (aModel.isOnScreen(row)) {
            out << QString::number(row) + QLatin1Char(':') + aModel.labelOf(row);
        }
    }
    return out;
}

// Every open group, so "Left did not expand or collapse ANY group" can be asserted
// even when the current row is not inside one.
static QStringList openSections(const Model& aModel) {
    QStringList out;
    for (int row = 0; row < aModel.rowCount(); ++row) {
        if (aModel.isSection(row) && aModel.isExpanded(row)) out << QString::number(row);
    }
    return out;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // The application's style, so the scene builds the same controls it ships.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    const QList<ctrl::ExportTarget> targets =
        ctrl::ExportTargets::load(QStringLiteral(AE_EXPORT_TARGETS_PATH));
    const QList<ctrl::ExportGroup> groups =
        ctrl::ExportTargets::loadGroups(QStringLiteral(AE_EXPORT_TARGETS_PATH));
    check(targets.size() > 40, QStringLiteral("the destination table loads (%1 rows)").arg(targets.size()));
    if (targets.isEmpty()) return 1;

    // The remembered destination is what the application passes in (QSettings
    // "export_destination"), and it is also what makes the precondition of the group
    // assertions hold: its platform is opened so the preselection is on screen.
    Model model(targets, groups, QStringLiteral("telegram.sticker"));
    gui::QmlTheme theme;

    // The scene's glyphs are read from a scratch dir the application tints on
    // startup (theme::tintIcons -> setIconDir). Doing the same here keeps the log
    // readable: without it every Image in the scene reports "Cannot open:
    // file:///caret-right.svg", and a run full of warnings is a run nobody reads.
    QTemporaryDir iconDir;
    check(iconDir.isValid(), "a scratch dir for the tinted icons");
    theme::tintIcons(QStringLiteral(AE_ICON_SRC), iconDir.path(), theme::Colors::current());
    theme::setIconDir(iconDir.path());

    // What "the picker answered" means to the shell.
    int acceptedCount = 0;
    QString chosen;
    QObject::connect(&model, &Model::accepted, [&model, &acceptedCount, &chosen]() {
        ++acceptedCount;
        chosen = model.chosenId();
    });

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("picker"), &model);
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);

    QQmlComponent component(&engine,
                            QUrl::fromLocalFile(QStringLiteral(AE_QML_DIR "/DestinationPicker.qml")));
    if (component.isError()) {
        QTextStream(stderr) << "the picker's QML does not load:\n";
        for (const QQmlError& error : component.errors()) {
            QTextStream(stderr) << "  " << error.toString() << "\n";
        }
        return 2;
    }
    QObject* created = component.create();
    auto* root = qobject_cast<QQuickItem*>(created);
    if (!root) {
        QTextStream(stderr) << "the picker's root object is not an item\n";
        return 2;
    }

    // The search field, addressed the way the probes address it: by the accessible
    // name the model gives it. A field that is merely "the first TextField" would
    // keep passing if a second one were added above it.
    QQuickItem* search = nullptr;
    const QList<QQuickItem*> items = root->findChildren<QQuickItem*>();
    for (QQuickItem* item : items) {
        if (item->property("accessibleName").toString() == model.searchAccessibleName()) {
            search = item;
            break;
        }
    }
    check(search != nullptr, "the search field is in the scene, by its accessible name");
    if (!search) return 1;

    // B15 (decided): focusing the field selects nothing, so typing refines the query
    // instead of replacing it. The focus itself needs a window; what can be asserted
    // here is that nothing selects the text on its own - which is what would break
    // this if someone added a selectAll() on focus.
    check(search->property("selectedText").toString().isEmpty(),
          "B15: the field holds no selection of its own, so focusing it cannot steal the query");

    // B13: the two selection colours are the design's tokens, not the style's
    // palette. Asserted through the real QmlTheme, so a token rename fails here.
    check(search->property("selectionColor").value<QColor>() == theme.selection(),
          "B13: the selection highlight is theme.selection");
    check(search->property("selectedTextColor").value<QColor>() == theme.selectionText(),
          "B13: the selected text is theme.selectionText");

    // B6: typing reaches the editor AND filters the list. This is the guard against
    // the cheap way to pass B7 - a field that swallows every key would move no caret
    // and alter no text either.
    {
        sendKey(search, Qt::Key_X, QStringLiteral("x"));
        check(search->property("text").toString() == QLatin1String("x"),
              "B6: a typed character reaches the editor");
        check(search->property("cursorPosition").toInt() == 1,
              "B6: and the caret follows it");
        check(model.query() == QLatin1String("x"),
              "B6: and the list is filtered by what was typed");
        // Back to no query, which is the state the group preconditions describe.
        search->setProperty("text", QString());
        check(model.query().isEmpty(), "clearing the field clears the query too");
    }

    // The preconditions the group assertions below need, IN THE TEST rather than
    // assumed: a run that cannot observe a collapse must not report that none
    // happened. Measured, not hoped for.
    check(model.visibleRowCount() >= 3,
          QStringLiteral("precondition: several destinations are on screen (%1)")
              .arg(model.visibleRowCount()));
    check(!openSections(model).isEmpty(),
          "precondition: at least one group is open, so an arrow COULD close it");
    check(!model.idOf(model.currentRow()).isEmpty(),
          "precondition: the remembered destination is the highlighted row");

    // B7: Up/Down move the LIST's highlight while the field keeps everything else.
    {
        const int rowBefore = model.currentRow();
        const QString textBefore = search->property("text").toString();
        const int caretBefore = search->property("cursorPosition").toInt();
        // NOT visibleRowCount(): that counts DESTINATIONS on screen, while a row
        // index also counts the sections between them. The precondition needs an
        // on-screen destination below the current row.
        int below = 0;
        for (int row = rowBefore + 1; row < model.rowCount(); ++row) {
            if (!model.isSection(row) && model.isOnScreen(row)) ++below;
        }
        check(below >= 1, "precondition: an on-screen destination sits below the highlighted row");

        sendKey(search, Qt::Key_Down);
        check(model.currentRow() == rowBefore + 1,
              "B7: Down moves the list's highlight one row");
        check(search->property("text").toString() == textBefore,
              "B7: Down does not alter the query");
        check(search->property("cursorPosition").toInt() == caretBefore,
              "B7: Down does not move the caret");

        sendKey(search, Qt::Key_Up);
        check(model.currentRow() == rowBefore,
              "B7: Up moves it back");
    }

    // B9/B14: Left and Right belong to the CARET. The field must not forward them, so
    // no group may open or close - which is the reported defect, and the reason this
    // assertion tests the groups and not only the caret.
    {
        // A query with a caret in it, so "the caret moved" is observable at all.
        search->setProperty("text", QStringLiteral("telegram"));
        check(model.query() == QLatin1String("telegram"),
              "precondition: the typed query reached the model");
        const int caret = search->property("cursorPosition").toInt();
        check(caret > 0, "precondition: the caret has somewhere to move back to");

        const QStringList rowsBefore = onScreenRows(model);
        const QStringList openBefore = openSections(model);
        check(!openBefore.isEmpty(),
              "precondition: a group is open, so forwarding Left WOULD have collapsed it");

        sendKey(search, Qt::Key_Left);
        check(search->property("cursorPosition").toInt() == caret - 1,
              "B9: Left moves the caret");
        check(openSections(model) == openBefore,
              "B9: Left does not open or close any group");
        check(onScreenRows(model) == rowsBefore,
              "B9: Left does not change what is on screen");
        check(model.query() == QLatin1String("telegram"),
              "B9: Left does not alter the query");

        sendKey(search, Qt::Key_Right);
        check(search->property("cursorPosition").toInt() == caret,
              "B9: Right moves the caret back");
        check(openSections(model) == openBefore,
              "B9: Right does not open or close any group");
        check(onScreenRows(model) == rowsBefore,
              "B9: Right does not change what is on screen");
    }

    // B8: Enter activates the highlighted row, exactly as clicking it would. The
    // query is an id, so "the highlighted row" is not a guess about ordering.
    {
        search->setProperty("text", QStringLiteral("telegram.sticker"));
        check(model.idOf(model.currentRow()) == QLatin1String("telegram.sticker"),
              "precondition: the highlighted row is the destination that was searched for");

        check(acceptedCount == 0, "precondition: nothing has been accepted yet");
        sendKey(search, Qt::Key_Return);
        check(acceptedCount == 1, "B8: Return answers with the highlighted row");
        check(chosen == QLatin1String("telegram.sticker"),
              QStringLiteral("B8: and the answer is that row (%1)").arg(chosen));
    }

    delete created;
    QTextStream(stdout) << (failures ? "=== FAILED " : "=== OK ")
                        << failures << " failures\n";
    return failures ? 1 : 0;
}
