#include "gui/DockSlots.h"

#include <QMainWindow>
#include <QTabBar>
#include <algorithm>

namespace gui {

namespace {

/// Where an area's rail sits, recorded on the bar when it is adopted: Qt builds
/// the bar for a group, so it belongs to the slot rather than to any panel in it.
QTabBar* railFor(const QMainWindow& aWindow, Qt::DockWidgetArea aArea) {
    for (QTabBar* bar : aWindow.findChildren<QTabBar*>()) {
        if (bar->objectName() != QLatin1String("dockTabBar")) continue;
        if (bar->property("aeDockArea").toInt() != int(aArea)) continue;
        if (bar->isVisible()) return bar;
    }
    return nullptr;
}

bool occupiesScreen(const QWidget* aWidget, const QRect& aWindow) {
    if (!aWidget || !aWidget->isVisible()) return false;
    const QRect rect = aWidget->geometry();
    return !rect.isEmpty() && aWindow.intersects(rect);
}

} // namespace

QVector<DockSlot> dockSlots(const QMainWindow& aWindow) {
    static const Qt::DockWidgetArea kOrder[] = {
        Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea,
        Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea,
    };

    QMap<int, QVector<QDockWidget*>> byArea;
    for (auto* dock : aWindow.findChildren<QDockWidget*>()) {
        if (dock->objectName() == QLatin1String("aeDockPreview")) continue;
        const Qt::DockWidgetArea area = aWindow.dockWidgetArea(dock);
        if (area == Qt::NoDockWidgetArea || dock->isFloating()) continue;
        byArea[int(area)].push_back(dock);
    }

    const QRect windowRect = aWindow.rect();
    QVector<DockSlot> found; // not "slots": Qt defines that as a macro
    for (Qt::DockWidgetArea area : kOrder) {
        const auto it = byArea.constFind(int(area));
        if (it == byArea.constEnd()) continue;

        DockSlot slot;
        slot.area = area;
        slot.panels = it.value();
        // Creation order, which is the rail's order (see DockTabBar): the tab text
        // is matched by name there, and the order is fixed so a tab does not jump
        // to the front when it is activated.
        std::stable_sort(slot.panels.begin(), slot.panels.end(),
                         [](QDockWidget* a, QDockWidget* b) {
                             return a->property("aeDockOrder").toInt()
                                  < b->property("aeDockOrder").toInt();
                         });
        for (QDockWidget* panel : slot.panels) {
            if (occupiesScreen(panel, windowRect)) slot.onScreen.push_back(panel);
        }
        for (QDockWidget* panel : slot.onScreen) {
            const QRect rect = panel->geometry();
            slot.rect = slot.rect.isNull() ? rect : slot.rect.united(rect);
        }
        if (QTabBar* rail = railFor(aWindow, area)) {
            const QRect rect = rail->geometry();
            slot.rect = slot.rect.isNull() ? rect : slot.rect.united(rect);
        }
        if (!slot.rect.isNull()) found.push_back(slot);
    }
    return found;
}

const DockSlot* slotFor(const QVector<DockSlot>& aSlots, const QDockWidget* aPanel) {
    for (const DockSlot& candidate : aSlots) {
        if (candidate.panels.contains(const_cast<QDockWidget*>(aPanel))) return &candidate;
    }
    return nullptr;
}

} // namespace gui
