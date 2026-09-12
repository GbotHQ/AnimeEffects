#include "gui/DestinationPicker.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFont>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

#include <algorithm>

#include "gui/GUIResources.h"
#include "gui/theme/Colors.h"

namespace gui {

namespace {

// What a row carries. A SECTION HEADING carries no id: it is not a destination, and
// every question the picker asks ("what is selected", "what did the user choose")
// is a question about an id, so a heading can never be mistaken for an answer.
enum {
    IdRole = Qt::UserRole, // the destination id; EMPTY on a section heading
    HaystackRole,          // what the search field matches
    SectionRole            // index of the section this item belongs to
};

// What search matches, per destination: everything the row declares about itself,
// in the words a user might type for it. The label (what they read), the SECTION
// TITLE (the platform: "telegram", "insta"), the id ("telegram.sticker", the name
// this row has in the data file), the family key and its title ("sticker",
// "Stickers and emoji" - both shown in the tooltip, so a match on them is a hover
// away from being explained), the container, and the artifact.
//
// The heading is what makes a result readable: a match is shown under the platform
// it belongs to, so a query answered by several sections ("sticker") arrives
// grouped rather than as a run of unrelated rows.
QString haystackFor(const ctrl::ExportTarget& aTarget, const QString& aGroupTitle,
                    const QString& aFamilyTitle) {
    QString h = aTarget.label + QLatin1Char(' ') + aTarget.id + QLatin1Char(' ') + aGroupTitle
                + QLatin1Char(' ') + aTarget.family + QLatin1Char(' ') + aFamilyTitle
                + QLatin1Char(' ') + aTarget.container;
    switch (aTarget.artifact) {
    case ctrl::ArtifactKind::Still:
        h += QStringLiteral(" still image illustration png jpg");
        break;
    case ctrl::ArtifactKind::Sequence:
        h += QStringLiteral(" sequence frames numbered stills png jpg");
        break;
    case ctrl::ArtifactKind::Video:
        h += QStringLiteral(" video clip animation");
        break;
    }
    if (aTarget.caps.maxBytes > 0) h += QStringLiteral(" capped size limit");
    return h.toLower();
}

// The human name of a destination's family, for the tooltip and for search. The
// family is what KIND of handover the row is (a sticker, a chat attachment, an
// upload); it is not the layout axis any more, but it is still a word the user
// knows, so it stays searchable where it is also visible.
QString familyTitle(const QString& aKey) {
    struct Family { const char* key; const char* title; };
    static const Family families[] = {
        { "chat", QT_TR_NOOP("Send in a chat") },
        { "post", QT_TR_NOOP("Post to a platform") },
        { "sticker", QT_TR_NOOP("Stickers and emoji") },
        { "overlay", QT_TR_NOOP("Overlays and transparency") },
        { "file", QT_TR_NOOP("A file") },
        { "archive", QT_TR_NOOP("Archive") },
    };
    for (const Family& f : families) {
        if (aKey == QLatin1String(f.key)) return QObject::tr(f.title);
    }
    return QString();
}

// The section a row belongs to. A row with no group (or one the table does not
// declare) still has to be reachable, so it lands in `general`.
QString sectionKeyOf(const ctrl::ExportTarget& aTarget) {
    return aTarget.group.isEmpty() ? QStringLiteral("general") : aTarget.group;
}

} // namespace

// The application's own rows and sections: one data file, so the list cannot be
// grouped by a table the exporter is not planning from.
DestinationPicker::DestinationPicker(GUIResources& aResources, const QString& aCurrentId,
                                     QWidget* aParent)
    : DestinationPicker(ctrl::ExportTargets::load(), ctrl::ExportTargets::loadGroups(),
                        aResources, aCurrentId, aParent) {}

DestinationPicker::DestinationPicker(const QList<ctrl::ExportTarget>& aTargets,
                                     const QList<ctrl::ExportGroup>& aGroups,
                                     GUIResources& aResources, const QString& aCurrentId,
                                     QWidget* aParent)
    : QDialog(aParent) {
    setWindowTitle(tr("Where are you posting this?"));
    setObjectName(QString::fromUtf8("destinationPicker"));
    setModal(true);
    resize(560, 600);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    mSearch = new QLineEdit(this);
    mSearch->setObjectName(QString::fromUtf8("destinationSearch"));
    mSearch->setPlaceholderText(
        tr("Search: telegram, sticker, reels, illustration, 微信..."));
    mSearch->setClearButtonEnabled(true);
    // A name for the accessibility tree: a QLineEdit with only a placeholder is
    // announced as "text" and cannot be addressed by name at all (see
    // scripts/gui/ae_ui.py, which drives the probes through this tree).
    mSearch->setAccessibleName(tr("Search destinations"));
    layout->addWidget(mSearch);

    mList = new QListWidget(this);
    mList->setObjectName(QString::fromUtf8("destinationList"));
    mList->setIconSize(QSize(22, 22));
    // Not uniform any more: a heading is a different face from a row.
    mList->setUniformItemSizes(false);
    mList->setAlternatingRowColors(false);
    layout->addWidget(mList, 1);

    // THE SECTIONS ARE THE PLATFORMS, in the order the data file declares them
    // ("general" first, then the platforms alphabetically). A person knows the
    // platform before they know anything else about the export, so it is the axis
    // they scan; the rows under it are the things that platform accepts from us.
    //
    // Grouping by family was tried and taken out: `post` alone is 29 of the 55 rows,
    // so it does not break up the list, and it mixes what a file IS (sticker) with
    // what it is FOR (post).
    QList<ctrl::ExportGroup> groups = aGroups;
    {
        // A row whose group the table does not declare still has to be reachable, so
        // it gets a section of its own, named by its key, at the end (`general` if it
        // names no group at all).
        QSet<QString> declared;
        for (const ctrl::ExportGroup& g : groups) declared.insert(g.id);
        for (const ctrl::ExportTarget& t : aTargets) {
            const QString key = sectionKeyOf(t);
            if (declared.contains(key)) continue;
            declared.insert(key);
            ctrl::ExportGroup extra;
            extra.id = key;
            extra.title = key;
            groups.push_back(extra);
        }
    }

    int section = 0;
    for (const ctrl::ExportGroup& g : groups) {
        QList<ctrl::ExportTarget> rows;
        for (const ctrl::ExportTarget& t : aTargets) {
            if (sectionKeyOf(t) == g.id) rows.push_back(t);
        }
        // A heading over nothing is a line the user can click and get no
        // destination from, so an empty section is not drawn at all.
        if (rows.isEmpty()) continue;

        // Within a section, by the label. Every row's label begins with the platform
        // the heading already names, so what is being ordered here is the variants
        // ("Instagram Reels" before "Instagram (photo)").
        std::stable_sort(rows.begin(), rows.end(),
                         [](const ctrl::ExportTarget& a, const ctrl::ExportTarget& b) {
                             return QString::localeAwareCompare(a.label, b.label) < 0;
                         });

        auto* heading = new QListWidgetItem(g.title, mList);
        QFont face = heading->font();
        face.setBold(true);
        heading->setFont(face);
        // Quiet, but structural: the headings are the scan column, so they must not
        // compete with the destinations. The stylesheet has no `color` on
        // QAbstractItemView::item precisely so a per-item brush (this one, and the
        // object tree's dimmed labels) survives.
        heading->setForeground(theme::Colors::current().textMuted);
        // Enabled (it is a real line of text, and a screen reader should read it) but
        // never selectable: Qt does not skip a non-selectable item when the arrow keys
        // or a click move the current index, so the picker moves off a heading the
        // moment one becomes current (see the currentItemChanged handler below and
        // nearestRow()). What makes that unambiguous is the id a heading does NOT have:
        // every route that answers the user asks for an id.
        heading->setFlags(Qt::ItemIsEnabled);
        heading->setData(SectionRole, section);

        for (const ctrl::ExportTarget& t : rows) {
            auto* item = new QListWidgetItem(aResources.icon(t.icon), t.label, mList);
            item->setData(IdRole, t.id);
            item->setData(SectionRole, section);
            const QString title = familyTitle(t.family);
            item->setData(HaystackRole, haystackFor(t, g.title, title));
            // What the row is, on hover: the kind of handover, and the id it has in the
            // data file (which is what a bug report should quote).
            item->setToolTip(title.isEmpty() ? t.id : title + QStringLiteral("\n") + t.id);
            if (t.id == aCurrentId) {
                mList->setCurrentItem(item);
            }
        }
        ++section;
    }

    mManualButton = new QPushButton(tr("My platform isn't listed (I'll choose the format)"), this);
    mManualButton->setObjectName(QString::fromUtf8("destinationManualButton"));
    mManualButton->setToolTip(tr("Everything below is a destination we know the rules for. "
                                 "This one lets you state the format and codec yourself."));
    layout->addWidget(mManualButton);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Use this destination"));
    layout->addWidget(buttons);

    // Typing filters and the arrows move the list, without leaving the field: a
    // chooser you have to Tab out of is a chooser you fight.
    connect(mSearch, &QLineEdit::textChanged, this, [this](const QString& aText) {
        applyFilter(aText);
    });
    mSearch->installEventFilter(this);
    connect(mList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        chooseCurrent();
    });
    // A heading can become the current row: the arrow keys walk onto it (Qt does
    // not skip a non-selectable item, which is what a heading has to be). Whenever
    // that happens, move on to the nearest destination IN THE DIRECTION OF TRAVEL:
    // the highlight has to sit on something Enter can export, and Down-then-Up must
    // not bounce the user back to the row they just left. This runs inside the key
    // event, so the heading is never painted as current.
    connect(mList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* aCurrent, QListWidgetItem* aPrevious) {
                if (!aCurrent || !aCurrent->data(IdRole).toString().isEmpty()) return;
                const int heading = mList->row(aCurrent);
                const int from = aPrevious ? mList->row(aPrevious) : heading;
                if (QListWidgetItem* row = nearestRow(heading, heading >= from ? 1 : -1)) {
                    mList->setCurrentItem(row);
                }
            });
    // A click is not a walk: it says "I mean THIS platform", not "one row further".
    // So a press on a heading puts the highlight on the first destination under it,
    // and the press is swallowed so the click cannot leave the current row on the
    // heading (or clear the selection on its way past).
    mList->viewport()->installEventFilter(this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() { chooseCurrent(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(mManualButton, &QPushButton::clicked, this, [this]() {
        mChosenId.clear();
        accept();
    });

    mSearch->setFocus();
    applyFilter(QString());
}

int DestinationPicker::visibleRowCount() const {
    int n = 0;
    for (int i = 0; i < mList->count(); ++i) {
        QListWidgetItem* item = mList->item(i);
        if (!item->isHidden() && !item->data(IdRole).toString().isEmpty()) ++n;
    }
    return n;
}

void DestinationPicker::applyFilter(const QString& aQuery) {
    const QString q = aQuery.trimmed().toLower();
    QSet<int> sectionsWithARow;
    int firstVisible = -1;
    for (int i = 0; i < mList->count(); ++i) {
        QListWidgetItem* item = mList->item(i);
        if (item->data(IdRole).toString().isEmpty()) continue; // a heading: handled below
        const QString hay = item->data(HaystackRole).toString();
        const bool match = q.isEmpty() || hay.contains(q);
        item->setHidden(!match);
        if (match) {
            sectionsWithARow.insert(item->data(SectionRole).toInt());
            if (firstVisible < 0) firstVisible = i;
        }
    }
    // A heading is shown while its section has a row to show, and never on its own:
    // "Telegram" over no Telegram destinations is a claim the list cannot back up.
    for (int i = 0; i < mList->count(); ++i) {
        QListWidgetItem* item = mList->item(i);
        if (!item->data(IdRole).toString().isEmpty()) continue;
        item->setHidden(!sectionsWithARow.contains(item->data(SectionRole).toInt()));
    }
    // Move the highlight only when the current row can no longer be chosen.
    //
    // The constructor selects the REMEMBERED destination (MainWindow passes
    // "export_destination" in as aCurrentId, and the header calls it "preselects a
    // destination"), and this function is called at the end of that constructor -
    // where it used to overwrite the selection with the first visible row. So the
    // picker always opened on row 0, "A video for a chat app", and Enter or the
    // default button exported THERE rather than where the user last exported: a
    // remembered setting the screen contradicts. See tests/destination_picker.cpp,
    // which is display-free and asks exactly this question.
    QListWidgetItem* current = mList->currentItem();
    const bool currentIsAnswer = current && !current->isHidden()
                                 && !current->data(IdRole).toString().isEmpty();
    if (!currentIsAnswer && firstVisible >= 0) {
        mList->setCurrentRow(firstVisible);
        current = mList->currentItem();
    }
    if (current) mList->scrollToItem(current);
}

QListWidgetItem* DestinationPicker::nearestRow(int aFromRow, int aDirection) const {
    const int n = mList->count();
    auto scan = [this, n](int aFrom, int aStep) -> QListWidgetItem* {
        for (int i = aFrom + aStep; i >= 0 && i < n; i += aStep) {
            QListWidgetItem* item = mList->item(i);
            // Hidden rows are not on screen, so they are not answers either.
            if (!item->isHidden() && !item->data(IdRole).toString().isEmpty()) return item;
        }
        return nullptr;
    };
    const int first = aDirection < 0 ? -1 : 1;
    if (QListWidgetItem* item = scan(aFromRow, first)) return item;
    // Nothing in that direction (Home lands on the first heading, which has no row
    // above it): answer with the other direction rather than leaving the highlight
    // on a heading.
    return scan(aFromRow, -first);
}

void DestinationPicker::chooseCurrent() {
    QListWidgetItem* item = mList->currentItem();
    if (!item || item->isHidden() || item->data(IdRole).toString().isEmpty()) {
        // A heading carries no id, so it is not an answer: fall through to the row
        // under it rather than exporting somewhere the screen does not name.
        item = nearestRow(item ? mList->row(item) : -1, 0);
    }
    if (!item) {
        // Nothing matched: the honest answer is the escape hatch, which is what the
        // user would have to use anyway.
        mChosenId.clear();
        accept();
        return;
    }
    mChosenId = item->data(IdRole).toString();
    accept();
}

bool DestinationPicker::eventFilter(QObject* aWatched, QEvent* aEvent) {
    if (aWatched == mList->viewport() && aEvent->type() == QEvent::MouseButtonPress) {
        auto* press = static_cast<QMouseEvent*>(aEvent);
        QListWidgetItem* item = mList->itemAt(press->position().toPoint());
        if (item && item->data(IdRole).toString().isEmpty()) {
            if (QListWidgetItem* row = nearestRow(mList->row(item), 1)) mList->setCurrentItem(row);
            return true;
        }
    }
    if (aWatched == mSearch && aEvent->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(aEvent);
        switch (key->key()) {
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
            // Forward to the list: the search field keeps the text cursor, the list
            // moves the highlight. Up/Down at the ends wrap, like a combo box.
            QApplication::sendEvent(mList, aEvent);
            return true;
        default:
            break;
        }
    }
    return QDialog::eventFilter(aWatched, aEvent);
}

} // namespace gui
