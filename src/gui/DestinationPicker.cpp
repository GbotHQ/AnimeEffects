#include "gui/DestinationPicker.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "gui/GUIResources.h"

namespace gui {

namespace {

// What search matches, per destination. The label first (which is what the user
// reads), then the things they might type instead: the id ("telegram.sticker"),
// the family name from the table ("stickers and emoji"), the family KEY
// ("sticker", "chat", "post", "overlay"), the artifact the row produces
// ("image", "png", "sequence", "gif") and the container. Typing "sticker" has to
// find every sticker destination even though most of their labels do not contain
// the word, and typing "png" has to find the still destinations.
QString haystackFor(const ctrl::ExportTarget& aTarget, const QString& aFamilyTitle) {
    QString h = aTarget.label + QLatin1Char(' ') + aTarget.id + QLatin1Char(' ') + aTarget.family
                + QLatin1Char(' ') + aFamilyTitle + QLatin1Char(' ') + aTarget.container;
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

} // namespace

DestinationPicker::DestinationPicker(const QList<ctrl::ExportTarget>& aTargets,
                                     GUIResources& aResources, const QString& aCurrentId,
                                     QWidget* aParent)
    : QDialog(aParent) {
    setWindowTitle(tr("Where are you posting this?"));
    setObjectName(QString::fromUtf8("destinationPicker"));
    setModal(true);
    resize(560, 560);

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
    mList->setUniformItemSizes(true);
    mList->setAlternatingRowColors(false);
    layout->addWidget(mList, 1);

    const QList<ctrl::ExportTarget> targets = aTargets;
    // Family titles, for search and for the tooltip. The same words the dialog and
    // the menus use, so one taxonomy is visible everywhere.
    struct Family { const char* key; const char* title; };
    const Family families[] = {
        { "chat", QT_TR_NOOP("Send in a chat") },
        { "post", QT_TR_NOOP("Post to a platform") },
        { "sticker", QT_TR_NOOP("Stickers and emoji") },
        { "overlay", QT_TR_NOOP("Overlays and transparency") },
        { "file", QT_TR_NOOP("A file") },
        { "archive", QT_TR_NOOP("Archive") },
    };
    auto familyTitle = [&families](const QString& aKey) -> QString {
        for (const Family& f : families) {
            if (aKey == QLatin1String(f.key)) return QObject::tr(f.title);
        }
        return QString();
    };

    for (const ctrl::ExportTarget& t : targets) {
        auto* item = new QListWidgetItem(aResources.icon(t.icon), t.label, mList);
        item->setData(Qt::UserRole, t.id);
        const QString title = familyTitle(t.family);
        item->setData(Qt::UserRole + 1, haystackFor(t, title));
        // The family and the destination's own note, on hover: the list stays one
        // line per destination, and the explanation is still one hover away.
        item->setToolTip(title.isEmpty() ? t.id : title + QStringLiteral("\n") + t.id);
        if (t.id == aCurrentId) {
            mList->setCurrentItem(item);
        }
    }
    // Ordered by the label, which begins with the platform: the scan axis is the
    // platform, and a platform with several destinations keeps them adjacent.
    mList->sortItems(Qt::AscendingOrder);

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
        if (!mList->item(i)->isHidden()) ++n;
    }
    return n;
}

void DestinationPicker::applyFilter(const QString& aQuery) {
    const QString q = aQuery.trimmed().toLower();
    int firstVisible = -1;
    for (int i = 0; i < mList->count(); ++i) {
        QListWidgetItem* item = mList->item(i);
        const QString hay = item->data(Qt::UserRole + 1).toString();
        const bool match = q.isEmpty() || hay.contains(q);
        item->setHidden(!match);
        if (match && firstVisible < 0) firstVisible = i;
    }
    if (firstVisible >= 0) mList->setCurrentRow(firstVisible);
    mList->scrollToItem(mList->currentItem());
}

void DestinationPicker::chooseCurrent() {
    QListWidgetItem* item = mList->currentItem();
    if (!item || item->isHidden()) {
        // Nothing matched: the honest answer is the escape hatch, which is what the
        // user would have to use anyway.
        mChosenId.clear();
        accept();
        return;
    }
    mChosenId = item->data(Qt::UserRole).toString();
    accept();
}

bool DestinationPicker::eventFilter(QObject* aWatched, QEvent* aEvent) {
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
