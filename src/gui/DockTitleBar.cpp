#include "gui/DockTitleBar.h"

#include "gui/DockPanel.h"
#include "gui/theme/Colors.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QFile>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QTime>
#include <cstdio>

namespace gui {

namespace {

/// Text inset, matching the 0.375em/0.5em the QSS title rule used.
int titleInset(const QFontMetrics& aMetrics) {
    return static_cast<int>(aMetrics.height() * 0.5);
}

/// Padding above and below the text, on top of the font's own height.
constexpr int kVerticalPadding = 8;

} // namespace

/// The point of this change is evidence, not a look: one line per gesture
/// carrying the values the design turns on - which platform plugin is live,
/// what the floating window's flags and frame margins actually are, and which
/// gesture claimed the press. Written to stderr and to a file so it can be read
/// back without a terminal attached.
void dockSpikeLog(const QDockWidget* aDock, const QString& aMessage) {
    static const QString path = qEnvironmentVariable(
            "AE_DOCK_SPIKE_LOG", QStringLiteral("/tmp/ae-dock-spike.log"));
    const QString line = QStringLiteral("%1 [%2] %3")
            .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                 aDock ? aDock->objectName() : QStringLiteral("-"), aMessage);
    std::fprintf(stderr, "dock-spike: %s\n", qPrintable(line));
    std::fflush(stderr);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(line.toUtf8());
        file.write("\n");
    }
}

DockTitleBar::DockTitleBar(QDockWidget* aDock):
    QWidget(aDock),
    mDock(aDock) {
    setObjectName(QStringLiteral("DockTitleBar"));
    setMouseTracking(true);
    // The header is painted by hand, so the universal QWidget QSS background
    // must not fill it.
    setAttribute(Qt::WA_TranslucentBackground);
    // constructed docked: opaque to the mouse, so the press is ours to swallow
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    connect(aDock, &QDockWidget::topLevelChanged, this, [this](bool aFloating) {
        setAttribute(Qt::WA_TransparentForMouseEvents, aFloating);
        log(QStringLiteral("header mouse-transparent=%1").arg(aFloating));
    });

    dockSpikeLog(mDock, QStringLiteral("titlebar installed: platform=%1 qt=%2")
            .arg(QGuiApplication::platformName(), QString::fromLatin1(qVersion())));
}

QSize DockTitleBar::sizeHint() const {
    return QSize(0, qMax(18, fontMetrics().height() + kVerticalPadding));
}

QSize DockTitleBar::minimumSizeHint() const {
    return sizeHint();
}

void DockTitleBar::paintEvent(QPaintEvent*) {
    const theme::Colors c = theme::Colors::current();
    QPainter p(this);

    p.fillRect(rect(), c.base);

    // The title text replaces the QSS `QDockWidget::title` box, which Qt no
    // longer draws once a title bar widget exists. The dock carries the bold
    // dock font, so this text is bold without asking.
    const int inset = titleInset(fontMetrics());
    p.setPen(c.text);
    p.drawText(rect().adjusted(inset, 0, -inset, 0),
               Qt::AlignVCenter | Qt::AlignLeft, mDock->windowTitle());
}

void DockTitleBar::mousePressEvent(QMouseEvent* aEvent) {
    if (aEvent->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(aEvent);
        return;
    }

    if (mDock->isFloating()) {
        // unreachable while the attribute is set; kept so a stale attribute
        // cannot turn a floating drag into a swallowed press
        log(QStringLiteral("floating header-press (unexpected: header was hit)"));
        aEvent->ignore();
        return;
    }

    // Docked: consume it. A docked panel is not draggable, and this is what
    // keeps Qt's own drag (and its Wayland platform DnD) from starting.
    log(QStringLiteral("docked header-press -> inert"));
    aEvent->accept();
}

void DockTitleBar::mouseDoubleClickEvent(QMouseEvent* aEvent) {
    if (aEvent->button() == Qt::LeftButton && !mDock->isFloating()) {
        toggleFloating();
        aEvent->accept();
        return;
    }
    // A floating header's double-click is Qt's (it toggles the same way).
    QWidget::mouseDoubleClickEvent(aEvent);
}

void DockTitleBar::contextMenuEvent(QContextMenuEvent* aEvent) {
    popupMenu(aEvent->globalPos());
    aEvent->accept();
}

void DockTitleBar::popupMenu(const QPoint& aGlobalPos) {
    // the docked case: the header is opaque there, so it gets the right-click
    showDockPanelMenu(mDock, aGlobalPos);
}

void DockTitleBar::toggleFloating() {
    const bool floating = !mDock->isFloating();
    log(QStringLiteral("double-click/menu -> setFloating(%1)").arg(floating));
    mDock->setFloating(floating);
}

void DockTitleBar::log(const QString& aMessage) const {
    dockSpikeLog(mDock, aMessage);
}

} // namespace gui
