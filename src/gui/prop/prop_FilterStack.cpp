#include "gui/prop/prop_FilterStack.h"

#include <QApplication>
#include <QAction>
#include <QFontMetrics>
#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <functional>

#include "core/BlurKey.h"
#include "core/HsvKey.h"
#include "core/TimeKeyExpans.h"
#include "gui/GUIResources.h"
#include "gui/prop/prop_Items.h"
#include "gui/prop/prop_KeyStateButton.h"
#include "gui/theme/Colors.h"
#include "ctrl/TimeLineUtil.h"

namespace gui {
namespace prop {

namespace {
constexpr int kRowHeight = 30;
constexpr int kRowGap = 2;
constexpr int kIconSide = 16;

QString friendlyTypeName(core::TimeKeyType aType) {
    switch (aType) {
    case core::TimeKeyType_HSV: return FilterStackPanel::tr("HSV");
    case core::TimeKeyType_Blur: return FilterStackPanel::tr("Blur");
    default: return QString();
    }
}

bool isDirectionalBlur(const core::TimeLine& aLine, int aFrame) {
    if (const core::TimeKey* key = aLine.timeKey(core::TimeKeyType_Blur, aFrame))
        return static_cast<const core::BlurKey*>(key)->data().isDirectional();
    return false;
}

void setStyled(QWidget& aWidget, const char* aProperty, bool aValue) {
    aWidget.setProperty(aProperty, aValue);
    aWidget.style()->unpolish(&aWidget);
    aWidget.style()->polish(&aWidget);
    aWidget.update();
}

// A row control's icon is quiet at rest and only reaches full contrast on
// hover. One helper for eye/trash/caret/add, so no control invents its own
// "white by default" weight.
QIcon dimmedIcon(const QIcon& aIcon, qreal aOpacity) {
    const QPixmap src = aIcon.pixmap(16, 16);
    QPixmap pm(src.size());
    pm.fill(Qt::transparent);
    {
        QPainter painter(&pm);
        painter.setOpacity(aOpacity);
        painter.drawPixmap(0, 0, src);
    }
    return QIcon(pm);
}

QIcon quietIcon(const QIcon& aIcon) {
    QIcon out;
    const QPixmap full = aIcon.pixmap(16, 16);
    const QPixmap rest = dimmedIcon(aIcon, 0.55).pixmap(16, 16);
    out.addPixmap(rest, QIcon::Normal, QIcon::Off);
    out.addPixmap(rest, QIcon::Disabled, QIcon::Off);
    out.addPixmap(full, QIcon::Active, QIcon::Off);
    out.addPixmap(full, QIcon::Selected, QIcon::Off);
    return out;
}
} // namespace

//-------------------------------------------------------------------------------------------------
struct FilterStackPanel::Row: public QWidget {
    FilterStackPanel* owner = nullptr;
    core::TimeKeyType type = core::TimeKeyType_TERM;
    QWidget* header = nullptr;
    QWidget* detailSlot = nullptr;
    QLabel* caret = nullptr;
    QPixmap caretRight;
    QPixmap caretDown;
    QPixmap caretRightQuiet;
    QPixmap caretDownQuiet;
    QIcon eyeOn;
    QIcon eyeOff;
    QIcon trashIcon;
    QToolButton* eye = nullptr;
    QLabel* name = nullptr;
    KeyStateButton* keyButton = nullptr;
    QLabel* summary = nullptr;
    QToolButton* remove = nullptr;
    bool enabled = true;
    bool expanded = false;
    bool hovered = false;
    bool pressing = false;
    QPoint pressPos;

    Row(FilterStackPanel* aOwner, GUIResources* aResources, core::TimeKeyType aType):
        QWidget(aOwner),
        owner(aOwner),
        type(aType) {
        this->setObjectName("filterCard");
        this->setAccessibleName(friendlyTypeName(aType));
        this->setAttribute(Qt::WA_StyledBackground, true);
        this->setMouseTracking(true);

        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        header = new QWidget(this);
        header->setObjectName("filterRow");
        header->setAttribute(Qt::WA_StyledBackground, true);
        header->setFixedHeight(kRowHeight);
        header->setMouseTracking(true);
        header->setCursor(Qt::PointingHandCursor);
        header->installEventFilter(this);
        outer->addWidget(header);

        auto* layout = new QHBoxLayout(header);
        layout->setContentsMargins(8, 0, 6, 0);
        layout->setSpacing(6);

        caretRight = aResources->icon("caret-right").pixmap(14, 14);
        caretDown = aResources->icon("caret-down").pixmap(14, 14);
        caretRightQuiet = dimmedIcon(QIcon(caretRight), 0.45).pixmap(14, 14);
        caretDownQuiet = dimmedIcon(QIcon(caretDown), 0.45).pixmap(14, 14);
        caret = new QLabel(header);
        caret->setObjectName("filterCaret");
        caret->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        caret->setFixedSize(16, 16);
        caret->setAlignment(Qt::AlignCenter);
        caret->setPixmap(caretRightQuiet);
        layout->addWidget(caret);

        eyeOn = aResources->icon("eye-open");
        eyeOff = aResources->icon("eye-close");
        eye = new QToolButton(header);
        eye->setObjectName("filterRowButton");
        eye->setCheckable(true);
        eye->setAutoRaise(true);
        eye->setFixedSize(kIconSide + 4, kIconSide + 4);
        eye->setIconSize(QSize(kIconSide, kIconSide));
        eye->setToolTip(tr("Enable or disable this filter"));
        eye->setAccessibleName(tr("Toggle %1").arg(friendlyTypeName(aType)));
        layout->addWidget(eye);

        keyButton = new KeyStateButton(friendlyTypeName(aType), aType, header);
        keyButton->setFixedSize(18, 20);
        layout->addWidget(keyButton);

        name = new QLabel(friendlyTypeName(aType), header);
        name->setObjectName("filterName");
        name->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        layout->addWidget(name);

        layout->addStretch();

        summary = new QLabel(header);
        summary->setObjectName("filterSummary");
        summary->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        summary->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        layout->addWidget(summary);

        remove = new QToolButton(header);
        remove->setObjectName("filterRowButton");
        remove->setAutoRaise(true);
        remove->setFixedSize(kIconSide + 4, kIconSide + 4);
        remove->setIconSize(QSize(kIconSide, kIconSide));
        trashIcon = aResources->icon("trash");
        remove->setToolTip(tr("Remove filter (undoable)"));
        remove->setAccessibleName(tr("Remove %1").arg(friendlyTypeName(aType)));
        layout->addWidget(remove);

        detailSlot = new QWidget(this);
        detailSlot->setObjectName("filterCardBody");
        auto* detailLayout = new QVBoxLayout(detailSlot);
        detailLayout->setContentsMargins(0, 0, 0, 0);
        detailLayout->setSpacing(0);
        outer->addWidget(detailSlot);

        connect(eye, &QToolButton::toggled, this, [this](bool aChecked) {
            if (enabled != aChecked)
                owner->toggleFilter(type, aChecked);
        });
        connect(remove, &QToolButton::clicked, this, [this]() { owner->removeFilter(type); });
        connect(keyButton, &QToolButton::clicked, this, [this]() { owner->addKey(type); });
        setFilterEnabled(true);
        setExpanded(false);
        updateRowIcons();
        remove->setVisible(false);
    }

    void setFilterEnabled(bool aEnabled) {
        enabled = aEnabled;
        const QSignalBlocker blocker(eye);
        eye->setChecked(aEnabled);
        eye->setIcon(aEnabled ? quietIcon(eyeOn) : quietIcon(eyeOff));
        setStyled(*header, "filterEnabled", aEnabled);
        name->setProperty("filterEnabled", aEnabled);
        name->style()->unpolish(name);
        name->style()->polish(name);
        name->update();
    }

    void setKeyState(KeyStateButton::State aState) { keyButton->setState(aState); }

    void setExpanded(bool aExpanded) {
        expanded = aExpanded;
        detailSlot->setVisible(aExpanded);
        caret->setPixmap(hovered ? (aExpanded ? caretDown : caretRight)
                                 : (aExpanded ? caretDownQuiet : caretRightQuiet));
        updateRowIcons();
        setStyled(*header, "selected", aExpanded);
        remove->setVisible(aExpanded || hovered);
        this->updateGeometry();
    }

    void updateRowIcons() {
        const qreal eyeOpacity = hovered ? 1.0 : 0.45;
        eye->setIcon(enabled ? dimmedIcon(eyeOn, eyeOpacity) : dimmedIcon(eyeOff, 0.35));
        remove->setIcon(hovered ? trashIcon : dimmedIcon(trashIcon, 0.34));
        caret->setPixmap(hovered ? (expanded ? caretDown : caretRight)
                                 : (expanded ? caretDownQuiet : caretRightQuiet));
    }

    void setSummary(const QString& aSummary, const QString& aTooltip) {
        summary->setText(aSummary);
        summary->setToolTip(aTooltip);
    }

    bool inHeader(QMouseEvent* aEvent) const {
        return header->rect().contains(header->mapFromGlobal(aEvent->globalPosition().toPoint()));
    }

    void handlePress(QMouseEvent* aEvent) {
        if (aEvent->button() != Qt::LeftButton || !inHeader(aEvent))
            return;
        pressing = true;
        pressPos = aEvent->globalPosition().toPoint();
        aEvent->accept();
    }

    void handleMove(QMouseEvent* aEvent) {
        if (!pressing || !inHeader(aEvent))
            return;
        const QPoint global = aEvent->globalPosition().toPoint();
        if ((global - pressPos).manhattanLength() > QApplication::startDragDistance())
            owner->beginRowDrag(this, global.y());
        if (owner->mDragRow == this)
            owner->moveRowDrag(global.y());
        aEvent->accept();
    }

    void handleRelease(QMouseEvent* aEvent) {
        if (!pressing || !inHeader(aEvent))
            return;
        pressing = false;
        if (owner->mDragRow == this) {
            owner->endRowDrag();
        } else {
            owner->toggleRow(this);
        }
        aEvent->accept();
    }

    bool eventFilter(QObject* aWatched, QEvent* aEvent) override {
        if (aWatched == header) {
            if (aEvent->type() == QEvent::MouseButtonPress)
                handlePress(static_cast<QMouseEvent*>(aEvent));
            else if (aEvent->type() == QEvent::MouseMove)
                handleMove(static_cast<QMouseEvent*>(aEvent));
            else if (aEvent->type() == QEvent::MouseButtonRelease)
                handleRelease(static_cast<QMouseEvent*>(aEvent));
            else if (aEvent->type() == QEvent::Enter) {
                hovered = true;
                setStyled(*header, "hovered", true);
                remove->setVisible(true);
                updateRowIcons();
            } else if (aEvent->type() == QEvent::Leave) {
                hovered = false;
                setStyled(*header, "hovered", false);
                remove->setVisible(expanded);
                updateRowIcons();
            }
        }
        return QWidget::eventFilter(aWatched, aEvent);
    }

    void mousePressEvent(QMouseEvent* aEvent) override { handlePress(aEvent); }
    void mouseMoveEvent(QMouseEvent* aEvent) override { handleMove(aEvent); }
    void mouseReleaseEvent(QMouseEvent* aEvent) override { handleRelease(aEvent); }
    void enterEvent(QEnterEvent*) override {
        hovered = true;
        setStyled(*header, "hovered", true);
        remove->setVisible(true);
        updateRowIcons();
    }
    void leaveEvent(QEvent*) override {
        hovered = false;
        setStyled(*header, "hovered", false);
        remove->setVisible(expanded);
        updateRowIcons();
    }

    void paintEvent(QPaintEvent* aEvent) override {
        QWidget::paintEvent(aEvent); // let QSS paint the card/header background
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        // The grip: three short rules at the header's left edge. It is painted,
        // not a child, so it can never steal the press that starts a reorder.
        QColor ink = theme::Colors::current().textMuted;
        ink.setAlpha(hovered || expanded ? 180 : 90);
        painter.setPen(QPen(ink, 1));
        const int x = 3;
        const int cy = header->geometry().center().y();
        for (int i = -1; i <= 1; ++i) {
            painter.drawPoint(x + 1, cy + i * 4);
            painter.drawPoint(x + 4, cy + i * 4);
        }
    }

private:
    Q_DISABLE_COPY(Row)
};

//-------------------------------------------------------------------------------------------------
FilterStackPanel::FilterStackPanel(core::Project& aProject, QWidget* aParent, GUIResources* aGUIResources):
    Panel(tr("Filters"), aParent, aGUIResources),
    mRows(),
    mEntries(),
    mProject(aProject),
    mResources(aGUIResources),
    mTarget(nullptr),
    mAccessor(),
    mSelected(core::TimeKeyType_TERM),
    mBody(nullptr),
    mRowsHost(nullptr),
    mRowsLayout(nullptr),
    mEmptyLabel(nullptr),
    mAddButton(nullptr),
    mDetailHost(nullptr),
    mDetailLayout(nullptr),
    mHsvEditor(nullptr),
    mHue(nullptr),
    mSaturation(nullptr),
    mValue(nullptr),
    mAbsolute(nullptr),
    mBlurEditor(nullptr),
    mBlurAmount(nullptr),
    mBlurDirectional(nullptr),
    mBlurX(nullptr),
    mBlurY(nullptr),
    mBlurAngle(nullptr),
    mBlurXLabel(nullptr),
    mBlurYLabel(nullptr),
    mBlurAngleLabel(nullptr),
    mDetailItems(),
    mDropLine(nullptr),
    mDragRow(nullptr),
    mDropIndex(-1),
    mDragActive(false) {
    mAccessor.setProject(&aProject);

    mBody = new QWidget(this);
    auto* bodyLayout = new QVBoxLayout(mBody);
    bodyLayout->setContentsMargins(12, 2, 12, 12);
    bodyLayout->setSpacing(4);

    mRowsHost = new QWidget(mBody);
    mRowsLayout = new QVBoxLayout(mRowsHost);
    mRowsLayout->setContentsMargins(0, 0, 0, 0);
    mRowsLayout->setSpacing(kRowGap);
    bodyLayout->addWidget(mRowsHost);

    mEmptyLabel = new QLabel(tr("No filters"), mBody);
    mEmptyLabel->setObjectName("filterEmpty");
    bodyLayout->addWidget(mEmptyLabel);

    mAddButton = new QToolButton(this);
    mAddButton->setObjectName("filterHeaderAdd");
    mAddButton->setAutoRaise(true);
    mAddButton->setFixedSize(22, 22);
    mAddButton->setIconSize(QSize(14, 14));
    mAddButton->setIcon(quietIcon(mResources->icon("plus-bold")));
    mAddButton->setToolTip(tr("Add a filter (searchable)"));
    mAddButton->setAccessibleName(tr("Add filter"));
    this->addHeaderAction(mAddButton);
    connect(mAddButton, &QToolButton::clicked, this, &FilterStackPanel::showFilterPicker);

    // The card body is ONE editor set, moved under the expanded row. It is not
    // part of the panel layout: the row's own body slot owns it while open, so
    // settings never appear detached at the bottom of the stack.
    mDetailHost = new QWidget(this);
    mDetailHost->setObjectName("filterCardBodyInner");
    mDetailLayout = new QVBoxLayout(mDetailHost);
    mDetailLayout->setContentsMargins(10, 8, 12, 10);
    mDetailLayout->setSpacing(4);
    mDetailHost->hide();

    buildDetailEditors();

    mDropLine = new QFrame(mRowsHost);
    mDropLine->setObjectName("filterDropLine");
    mDropLine->setFixedHeight(2);
    mDropLine->hide();

    this->addGroup(mBody);
    this->hide();
}

FilterStackPanel::~FilterStackPanel() {
    qDeleteAll(mDetailItems);
    mDetailItems.clear();
}

QVector<FilterStackPanel::CatalogEntry> FilterStackPanel::catalog() {
    return {
        { tr("HSV"), tr("Hue, saturation and value adjustment."), core::TimeKeyType_HSV, true },
        { tr("Blur"), tr("Directional Gaussian blur in the layer's content space."), core::TimeKeyType_Blur, true },
        // Separate features, deliberately visible here only as stubs: the list is
        // searchable, so a user looking for either can see it is planned without
        // being able to create an empty effect that does nothing.
        { tr("Wave Deform"), tr("Traveling wave distortion. Coming soon."), core::TimeKeyType_TERM, false },
        { tr("Custom GLSL"), tr("A fragment shader with a guided, typed interface. Coming soon."),
          core::TimeKeyType_TERM, false },
    };
}

QString FilterStackPanel::displayName(core::TimeKeyType aType) { return friendlyTypeName(aType); }

core::TimeLine* FilterStackPanel::line() const { return mTarget ? mTarget->timeLine() : nullptr; }

int FilterStackPanel::frame() const { return mProject.animator().currentFrame().get(); }

bool FilterStackPanel::hasKey(core::TimeKeyType aType) const {
    core::TimeLine* timeLine = line();
    return timeLine && isFilterType(aType) && timeLine->hasTimeKey(aType, frame());
}

void FilterStackPanel::setTarget(core::ObjectNode* aTarget) {
    mTarget = aTarget;
    mAccessor.setTarget(aTarget);
    mSelected = core::TimeKeyType_TERM;
    mEntries.clear();
    if (mDetailHost->parentWidget() != this)
        mDetailHost->setParent(this);
    mDetailHost->hide();
    mDragRow = nullptr;
    mDragActive = false;
    mDropLine->hide();
    if (!aTarget || !aTarget->timeLine()) {
        this->hide();
        return;
    }
    this->show();
    updateKey();
}

void FilterStackPanel::updateKey() {
    if (!mTarget || !line()) {
        this->hide();
        return;
    }
    this->show();
    const QVector<core::FilterEntry> entries = line()->filters();
    bool structureChanged = entries.size() != mEntries.size();
    if (!structureChanged) {
        for (int i = 0; i < entries.size(); ++i) {
            if (entries.at(i).type != mEntries.at(i).type)
                structureChanged = true;
        }
    }
    mEntries = entries;
    bool selectedStillExists = false;
    for (const core::FilterEntry& entry : entries) {
        if (entry.type == mSelected)
            selectedStillExists = true;
    }
    if (!selectedStillExists)
        mSelected = core::TimeKeyType_TERM;

    if (structureChanged)
        rebuildRows(entries);
    else
        refreshRowState();
    refreshEditor();
    placeDetail();
    mEmptyLabel->setVisible(mEntries.isEmpty());
    mBody->updateGeometry();
}

void FilterStackPanel::updateFrame() { updateKey(); }

void FilterStackPanel::rebuildRows(const QVector<core::FilterEntry>& aEntries) {
    while (QLayoutItem* item = mRowsLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            if (widget != mDropLine) {
                widget->hide();
                widget->deleteLater();
            }
        }
        delete item;
    }
    mRows.clear();

    bool selectedExists = false;
    for (const core::FilterEntry& entry : aEntries) {
        auto* row = new Row(this, mResources, entry.type);
        mRows.append(row);
        mRowsLayout->addWidget(row);
        if (entry.type == mSelected)
            selectedExists = true;
        connect(row, &QWidget::destroyed, this, [this, row]() { mRows.removeOne(row); });
    }
    if (!selectedExists)
        mSelected = aEntries.isEmpty() ? core::TimeKeyType_TERM : aEntries.front().type;
    mEmptyLabel->setVisible(aEntries.isEmpty());
    mDropLine->raise();
    refreshRowState();
    placeDetail();
    mRowsHost->updateGeometry();
    this->updateGeometry();
}

void FilterStackPanel::refreshRowState() {
    core::TimeLine* timeLine = line();
    if (!timeLine)
        return;
    for (Row* row : mRows) {
        row->setFilterEnabled(timeLine->filterEnabled(row->type));
        const bool hasAny = !timeLine->map(row->type).isEmpty();
        row->setKeyState(!hasAny ? KeyStateButton::State::Static
                                 : (timeLine->hasTimeKey(row->type, frame())
                                        ? KeyStateButton::State::Keyed
                                        : KeyStateButton::State::Animated));
        row->setExpanded(row->type == mSelected);
        const auto& current = timeLine->current();
        if (row->type == core::TimeKeyType_Blur) {
            const bool directional = isDirectionalBlur(*timeLine, frame());
            const QString text = directional
                ? tr("%1 × %2 px").arg(double(current.blurX()), 0, 'f', 1).arg(double(current.blurY()), 0, 'f', 1)
                : tr("%1 px").arg(double(current.blurX()), 0, 'f', 1);
            row->setSummary(text, tr("Current blur radius at this frame"));
        } else if (row->type == core::TimeKeyType_HSV) {
            const QList<int> values = current.hsv().hsv();
            const int hue = values.size() > 0 ? values.at(0) : 0;
            const int sat = values.size() > 1 ? values.at(1) : 100;
            const int val = values.size() > 2 ? values.at(2) : 100;
            row->setSummary(
                tr("H %1  S %2  V %3").arg(hue).arg(sat).arg(val),
                tr("Current HSV adjustment at this frame")
            );
        }
    }
}

void FilterStackPanel::toggleRow(Row* aRow) {
    if (!aRow)
        return;
    mSelected = (mSelected == aRow->type) ? core::TimeKeyType_TERM : aRow->type;
    refreshRowState();
    refreshEditor();
    placeDetail();
}

void FilterStackPanel::placeDetail() {
    Row* target = nullptr;
    for (Row* row : mRows) {
        if (row->type == mSelected) {
            target = row;
            break;
        }
    }
    if (!target || (mSelected != core::TimeKeyType_HSV && mSelected != core::TimeKeyType_Blur)) {
        // Never leave the shared editor parented to a row that a rebuild may
        // deleteLater(): collapsing/removing a card would take the editor with it,
        // and the next add would dereference a dead pointer.
        if (mDetailHost->parentWidget() != this)
            mDetailHost->setParent(this);
        mDetailHost->hide();
        return;
    }
    if (mDetailHost->parentWidget() != target->detailSlot) {
        mDetailHost->setParent(target->detailSlot);
        auto* layout = static_cast<QVBoxLayout*>(target->detailSlot->layout());
        layout->addWidget(mDetailHost);
    }
    mDetailHost->show();
    target->updateGeometry();
    mBody->updateGeometry();
}

void FilterStackPanel::showFilterPicker() {
    if (!line())
        return;
    QMenu menu(this);
    menu.setObjectName("filterPickerMenu");
    menu.setMinimumWidth(300);

    auto* search = new QLineEdit(&menu);
    search->setObjectName("filterPickerSearch");
    search->setPlaceholderText(tr("Search filters"));
    search->setAccessibleName(tr("Search filters"));
    search->setClearButtonEnabled(true);
    if (QIcon searchIcon = mResources->icon("magnifying-glass"); !searchIcon.isNull())
        search->addAction(searchIcon, QLineEdit::LeadingPosition);
    auto* searchAction = new QWidgetAction(&menu);
    searchAction->setDefaultWidget(search);
    menu.addAction(searchAction);
    menu.addSeparator();

    struct Item {
        CatalogEntry entry;
        QAction* action = nullptr;
        bool added = false;
    };
    QVector<Item> items;
    const QVector<CatalogEntry> entries = catalog();
    for (const CatalogEntry& entry : entries) {
        Item item;
        item.entry = entry;
        item.added = entry.type != core::TimeKeyType_TERM && line()->hasFilter(entry.type);
        QString text = entry.name;
        if (!entry.available)
            text += QStringLiteral("  ·  ") + tr("Coming soon");
        else if (item.added)
            text += QStringLiteral("  ·  ") + tr("Added");
        QAction* action = menu.addAction(text);
        action->setToolTip(entry.description);
        action->setEnabled(entry.available && !item.added);
        item.action = action;
        if (entry.available && !item.added) {
            const core::TimeKeyType type = entry.type;
            connect(action, &QAction::triggered, this, [this, type]() { addFilter(type); });
        }
        items.append(item);
    }
    QAction* noMatch = menu.addAction(tr("No matching filters"));
    noMatch->setEnabled(false);
    noMatch->setVisible(false);

    connect(search, &QLineEdit::textChanged, this, [&items, noMatch](const QString& aText) {
        const QString needle = aText.trimmed();
        bool any = false;
        for (const Item& item : items) {
            const bool visible = needle.isEmpty()
                || item.entry.name.contains(needle, Qt::CaseInsensitive)
                || item.entry.description.contains(needle, Qt::CaseInsensitive);
            item.action->setVisible(visible);
            any = any || visible;
        }
        noMatch->setVisible(!any);
    });
    connect(search, &QLineEdit::returnPressed, &menu, [&items, &menu]() {
        for (const Item& item : items) {
            if (item.action->isVisible() && item.action->isEnabled()) {
                menu.close();
                QMetaObject::invokeMethod(item.action, "trigger");
                return;
            }
        }
    });

    const QPoint anchor = mAddButton->mapToGlobal(QPoint(0, mAddButton->height()));
    search->setFocus(Qt::OtherFocusReason);
    menu.exec(anchor);
}

void FilterStackPanel::addFilter(core::TimeKeyType aType) {
    if (!mTarget || !isFilterType(aType))
        return;
    ctrl::TimeLineUtil::addFilter(mProject, *mTarget, aType);
    mSelected = aType;
    updateKey();
}

void FilterStackPanel::removeFilter(core::TimeKeyType aType) {
    if (!mTarget)
        return;
    ctrl::TimeLineUtil::removeFilter(mProject, *mTarget, aType);
    if (mSelected == aType)
        mSelected = core::TimeKeyType_TERM;
    updateKey();
}

void FilterStackPanel::toggleFilter(core::TimeKeyType aType, bool aEnabled) {
    if (!mTarget)
        return;
    ctrl::TimeLineUtil::setFilterEnabled(mProject, *mTarget, aType, aEnabled);
    updateKey();
}

void FilterStackPanel::moveFilter(core::TimeKeyType aType, int aIndex) {
    if (!mTarget)
        return;
    ctrl::TimeLineUtil::moveFilter(mProject, *mTarget, aType, aIndex);
    updateKey();
}

void FilterStackPanel::buildDetailEditors() {
    const auto addEditorRow = [this](QFormLayout* aLayout, const QString& aText, ItemBase* aItem, QLabel** aLabelOut = nullptr) {
        auto* label = new QLabel(aText, aItem->itemWidget() ? aItem->itemWidget()->parentWidget() : mDetailHost);
        label->setObjectName("attrLabel");
        label->setMinimumWidth(84);
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (aLabelOut)
            *aLabelOut = label;
        if (aItem->itemWidget()) {
            aItem->itemWidget()->setAccessibleName(aText);
            label->setBuddy(aItem->itemWidget());
        }
        if (aItem->itemLayout())
            aLayout->addRow(label, aItem->itemLayout());
        else
            aLayout->addRow(label, aItem->itemWidget());
    };

    // HSV editor
    mHsvEditor = new QWidget(mDetailHost);
    auto* hsvForm = new QFormLayout(mHsvEditor);
    hsvForm->setContentsMargins(0, 0, 0, 0);
    hsvForm->setHorizontalSpacing(10);
    hsvForm->setVerticalSpacing(2);
    hsvForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    mHue = new IntegerItem(mHsvEditor);
    mHue->setRange(0, 360);
    mHue->box().setSingleStep(1);
    mHue->onValueUpdated = [this](int, int aNext) { mAccessor.assignHSV(aNext, "hue"); };
    addEditorRow(hsvForm, tr("Hue"), mHue);

    mSaturation = new IntegerItem(mHsvEditor);
    mSaturation->setRange(0, 200);
    mSaturation->box().setSingleStep(1);
    mSaturation->onValueUpdated = [this](int, int aNext) { mAccessor.assignHSV(aNext, "sat"); };
    addEditorRow(hsvForm, tr("Saturation"), mSaturation);

    mValue = new IntegerItem(mHsvEditor);
    mValue->setRange(0, 200);
    mValue->box().setSingleStep(1);
    mValue->onValueUpdated = [this](int, int aNext) { mAccessor.assignHSV(aNext, "val"); };
    addEditorRow(hsvForm, tr("Value"), mValue);

    mAbsolute = new CheckItem(mHsvEditor);
    mAbsolute->onValueUpdated = [this](bool aNext) { mAccessor.assignHSV(int(aNext), "abs"); };
    addEditorRow(hsvForm, tr("Absolute color"), mAbsolute);
    mDetailItems << mHue << mSaturation << mValue << mAbsolute;
    mDetailLayout->addWidget(mHsvEditor);
    mHsvEditor->hide();

    // Blur editor
    mBlurEditor = new QWidget(mDetailHost);
    auto* blurForm = new QFormLayout(mBlurEditor);
    blurForm->setContentsMargins(0, 0, 0, 0);
    blurForm->setHorizontalSpacing(10);
    blurForm->setVerticalSpacing(2);
    blurForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    mBlurAmount = new DecimalItem(mBlurEditor);
    mBlurAmount->setRange(0.0, 512.0);
    mBlurAmount->box().setSingleStep(0.5);
    mBlurAmount->onValueUpdated = [this](double, double aNext) { mAccessor.assignBlur(float(aNext)); };
    addEditorRow(blurForm, tr("Amount"), mBlurAmount);

    mBlurDirectional = new CheckItem(mBlurEditor);
    mBlurDirectional->onValueUpdated = [this](bool aNext) {
        mAccessor.assignBlurDirectional(aNext);
        refreshEditor();
    };
    addEditorRow(blurForm, tr("Directional"), mBlurDirectional);

    mBlurX = new DecimalItem(mBlurEditor);
    mBlurX->setRange(0.0, 512.0);
    mBlurX->box().setSingleStep(0.5);
    mBlurX->onValueUpdated = [this](double, double) {
        mAccessor.assignBlurEllipse(float(mBlurX->value()), float(mBlurY->value()), float(mBlurAngle->value()));
    };
    addEditorRow(blurForm, tr("Blur X"), mBlurX, &mBlurXLabel);

    mBlurY = new DecimalItem(mBlurEditor);
    mBlurY->setRange(0.0, 512.0);
    mBlurY->box().setSingleStep(0.5);
    mBlurY->onValueUpdated = [this](double, double) {
        mAccessor.assignBlurEllipse(float(mBlurX->value()), float(mBlurY->value()), float(mBlurAngle->value()));
    };
    addEditorRow(blurForm, tr("Blur Y"), mBlurY, &mBlurYLabel);

    mBlurAngle = new DecimalItem(mBlurEditor);
    mBlurAngle->setRange(-100000.0, 100000.0);
    mBlurAngle->box().setSingleStep(1.0);
    mBlurAngle->onValueUpdated = [this](double, double) {
        mAccessor.assignBlurEllipse(float(mBlurX->value()), float(mBlurY->value()), float(mBlurAngle->value()));
    };
    addEditorRow(blurForm, tr("Angle"), mBlurAngle, &mBlurAngleLabel);

    mDetailItems << mBlurAmount << mBlurDirectional << mBlurX << mBlurY << mBlurAngle;
    mDetailLayout->addWidget(mBlurEditor);
    mBlurEditor->hide();
}

void FilterStackPanel::refreshEditor() {
    core::TimeLine* timeLine = line();
    if (!timeLine) {
        mDetailHost->hide();
        return;
    }
    const bool hsv = mSelected == core::TimeKeyType_HSV;
    const bool blur = mSelected == core::TimeKeyType_Blur;
    mHsvEditor->setVisible(hsv);
    mBlurEditor->setVisible(blur);
    if (hsv) {
        const QList<int> values = timeLine->current().hsv().hsv();
        mHue->setValue(values.size() > 0 ? values.at(0) : 0);
        mSaturation->setValue(values.size() > 1 ? values.at(1) : 100);
        mValue->setValue(values.size() > 2 ? values.at(2) : 100);
        mAbsolute->setValue(values.size() > 3 && values.at(3) != 0, false);
    } else if (blur) {
        const auto& current = timeLine->current();
        mBlurAmount->setValue(double(current.blurX()));
        mBlurDirectional->setValue(isDirectionalBlur(*timeLine, frame()), false);
        mBlurX->setValue(double(current.blurX()));
        mBlurY->setValue(double(current.blurY()));
        mBlurAngle->setValue(double(current.angleDeg()));
        const bool directional = isDirectionalBlur(*timeLine, frame());
        mBlurX->setItemVisible(directional);
        mBlurY->setItemVisible(directional);
        mBlurAngle->setItemVisible(directional);
        mBlurXLabel->setVisible(directional);
        mBlurYLabel->setVisible(directional);
        mBlurAngleLabel->setVisible(directional);
    }
}

void FilterStackPanel::addKey(core::TimeKeyType aType) {
    if (!mTarget || !line())
        return;
    if (hasKey(aType)) {
        mAccessor.removeKey(aType);
    } else if (aType == core::TimeKeyType_HSV) {
        mAccessor.knockNewHSV();
    } else if (aType == core::TimeKeyType_Blur) {
        mAccessor.knockNewBlur();
    }
    updateKey();
}

void FilterStackPanel::beginRowDrag(QWidget* aRow, int aGlobalY) {
    if (mDragRow)
        return;
    mDragRow = aRow;
    mDragActive = true;
    mDropIndex = mRows.indexOf(static_cast<Row*>(aRow));
    mDropLine->show();
    moveRowDrag(aGlobalY);
}

void FilterStackPanel::moveRowDrag(int aGlobalY) {
    if (!mDragRow || !mDragActive)
        return;
    mDropIndex = dropIndexAt(aGlobalY);
    const int y = std::clamp(
        mRowsHost->mapFromGlobal(QPoint(0, aGlobalY)).y() - 1, 0, qMax(0, mRowsHost->height() - 2)
    );
    mDropLine->setGeometry(0, y, mRowsHost->width(), 2);
    mDropLine->raise();
}

int FilterStackPanel::dropIndexAt(int aGlobalY) const {
    int index = 0;
    for (const Row* row : mRows) {
        const QPoint centre = row->mapToGlobal(QPoint(0, row->height() / 2));
        if (aGlobalY > centre.y())
            ++index;
    }
    return index;
}

void FilterStackPanel::endRowDrag() {
    if (!mDragRow)
        return;
    Row* row = static_cast<Row*>(mDragRow);
    const int from = mRows.indexOf(row);
    const int insertion = mDropIndex;
    mDragRow = nullptr;
    mDragActive = false;
    mDropLine->hide();
    if (from < 0)
        return;
    // insertion is an index in the list WITH the row still in it. Removing the row
    // first makes the final index one smaller when it moved down.
    int target = insertion > from ? insertion - 1 : insertion;
    target = std::clamp(target, 0, qMax(0, int(mRows.size()) - 1));
    if (target != from)
        moveFilter(row->type, target);
}

} // namespace prop
} // namespace gui
