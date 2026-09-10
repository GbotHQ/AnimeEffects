#include "gui/DockDropOverlay.h"

#include "gui/theme/Colors.h"

#include <QPainter>

namespace gui {

namespace {
/// Share of the window that counts as the drop band for an edge.
constexpr int kBandDivisor = 4;
constexpr int kRadius = 6;
} // namespace

DockDropOverlay::DockDropOverlay(QWidget* aParent):
    QWidget(aParent),
    mArea(Qt::NoDockWidgetArea) {
    setObjectName(QStringLiteral("DockDropOverlay"));
    // never a drop target itself: the position has to reach the main window,
    // which is what owns the hit test
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    hide();
}

void DockDropOverlay::showArea(Qt::DockWidgetArea aArea) {
    if (aArea == Qt::NoDockWidgetArea) {
        clear();
        return;
    }
    if (mArea != aArea) {
        mArea = aArea;
        setGeometry(bandFor(parentWidget()->rect(), aArea));
    }
    raise();
    show();
}

void DockDropOverlay::clear() {
    mArea = Qt::NoDockWidgetArea;
    hide();
}

QRect DockDropOverlay::bandFor(const QRect& aWindow, Qt::DockWidgetArea aArea) {
    const int hBand = qMax(1, aWindow.width() / kBandDivisor);
    const int vBand = qMax(1, aWindow.height() / kBandDivisor);
    switch (aArea) {
    case Qt::LeftDockWidgetArea: return QRect(aWindow.left(), aWindow.top(), hBand, aWindow.height());
    case Qt::RightDockWidgetArea: return QRect(aWindow.right() - hBand + 1, aWindow.top(), hBand, aWindow.height());
    case Qt::TopDockWidgetArea: return QRect(aWindow.left(), aWindow.top(), aWindow.width(), vBand);
    case Qt::BottomDockWidgetArea: return QRect(aWindow.left(), aWindow.bottom() - vBand + 1, aWindow.width(), vBand);
    default: break;
    }
    return QRect();
}

void DockDropOverlay::paintEvent(QPaintEvent*) {
    if (mArea == Qt::NoDockWidgetArea) return;

    const theme::Colors c = theme::Colors::current();
    // the accent, at fill alpha: the drop target reads as the accent colour the
    // app already uses for the active/hover fill, and the edge keeps it legible
    // over whatever the window is showing underneath
    QColor fill = c.accent;
    fill.setAlpha(48);

    QRectF band = QRectF(rect()).adjusted(1, 1, -1, -1);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(band, fill);
    p.setPen(QPen(c.accent, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(band, kRadius, kRadius);
}

} // namespace gui
