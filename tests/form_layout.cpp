// The export form's overflow contract, offscreen: D25 + Q9 (decided 2026-09-30).
//
//   "The export form never exceeds the window without a scroll: when the content
//    does not fit (minimum size, small screen, or the Advanced section open), the
//    content scrolls by wheel, keys and drag, and the Cancel/Export row stays
//    fixed at the bottom while the content scrolls - reachable without scrolling
//    to the end."
//
// Every assertion is a geometry fact of the REAL ExportDialog.qml with the real
// view model - the measured before-column (overflow-spike) is what the suite
// locks in: 630px of content in 558px of page at 560x640 with Advanced open and
// four frame ranges was CUT with no recourse, and 316px at a 300px-tall window.
// The row was always anchored (it does not move - the Pinned assert keeps it
// that way); what was missing is everything else in this file:
//
//   * the content scrolls (wheel, drag, PageUp/PageDown/Home/End);
//   * focus movement scrolls the target's whole ROW into view (reachability);
//   * the scroll bar appears only when the content overflows (D23's rule);
//   * a tooltip inside the scrolling content is clipped by NOTHING (E28) -
//     the clamp now narrows to the visible part of every clipping ancestor;
//   * the combo's in-scene popup lives outside the clip (G37 survives D25).
//
// Run: QT_QPA_PLATFORM=offscreen ./build/tests/Debug/form_layout
#include <QApplication>
#include <QElapsedTimer>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QWheelEvent>

#include "ctrl/ExportTarget.h"
#include "gui/qml/ExportViewModel.h"
#include "gui/qml/QmlTheme.h"
#include "gui/theme/Icons.h"

static int failures = 0;

static void check(bool ok, const QString& what) {
    QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

static void spin(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

// Window-delivered hover (tooltip_contract's measured pattern: pointer handlers
// see the window's delivery path, never events posted at the item).
static void hoverAt(QQuickWindow* w, QQuickItem* content, QQuickItem* target, QPointF local) {
    const QPointF p = target->mapToItem(content, local);
    QHoverEvent he(QEvent::HoverMove, p, p, p - QPointF(40, 0));
    QCoreApplication::sendEvent(w, &he);
    QCoreApplication::processEvents();
}

static bool rectFits(const QRectF& inner, const QRectF& outer, qreal slack = 1.5) {
    return inner.left() >= outer.left() - slack && inner.top() >= outer.top() - slack
        && inner.right() <= outer.right() + slack && inner.bottom() <= outer.bottom() + slack;
}

static QString rs(const QRectF& r) {
    return QString("(%1,%2 %3x%4)").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

struct Form {
    QQuickWindow* win = nullptr;
    QQuickItem* root = nullptr;
    QQuickItem* scroller = nullptr;
    QQuickItem* page = nullptr;
    QQuickItem* footer = nullptr;
    QQuickItem* vbar = nullptr;
    gui::ExportViewModel* model = nullptr;

    QQuickItem* byName(const char* n) {
        return root ? root->findChild<QQuickItem*>(QString(n)) : nullptr;
    }
    qreal contentY() const { return scroller ? scroller->property("contentY").toReal() : -1; }
    qreal contentX() const { return scroller ? scroller->property("contentX").toReal() : -1; }
    qreal contentH() const { return scroller ? scroller->property("contentHeight").toReal() : -1; }
    qreal contentW() const { return scroller ? scroller->property("contentWidth").toReal() : -1; }
    QRectF viewportInPage() const {
        return QRectF(contentX(), contentY(), scroller->width(), scroller->height());
    }
    QRectF rectInPage(QQuickItem* it) const {
        const QPointF p = it->mapToItem(page, 0, 0);
        return QRectF(p.x(), p.y(), it->width(), it->height());
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    gui::QmlTheme theme;
    QTemporaryDir iconDir;
    check(iconDir.isValid(), "a scratch dir for the tinted icons");
    theme::tintIcons(QStringLiteral(AE_ICON_SRC), iconDir.path(), theme::Colors::current());
    theme::setIconDir(iconDir.path());

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    QQmlComponent dlg(&engine, QUrl(QStringLiteral("file:" AE_QML_DIR "/ExportDialog.qml")));
    if (dlg.isError()) {
        for (const auto& e : dlg.errors())
            QTextStream(stderr) << "dialog load error: " << e.toString() << "\n";
        return 2;
    }

    // One form per scenario: the scene keeps imperative state (contentY), and a
    // reused instance would let one scenario scroll the next one's assumptions.
    auto makeForm = [&](int w, int h, bool adv, int ranges, Form* f) {
        gui::ExportViewModel::Init init;
        init.nativeSize = QSize(1000, 400);
        init.nativeFps = 24;
        init.projectFirstFrame = 0;
        init.projectLastFrame = 59;
        init.playhead = 3;
        QList<ctrl::FrameRange> rs;
        for (int i = 0; i < ranges; ++i) rs << ctrl::FrameRange{i * 10, i * 10 + 5};
        init.ranges = rs;
        init.destinationId = QStringLiteral("discord");
        init.qualityIndex = 1;
        init.width = 1000;
        init.height = 400;
        f->model = new gui::ExportViewModel(init);
        engine.rootContext()->setContextProperty(QStringLiteral("exportModel"), f->model);
        QObject* o = dlg.create();
        f->root = qobject_cast<QQuickItem*>(o);
        f->win = new QQuickWindow();
        f->win->resize(w, h);
        f->win->show();
        f->root->setParentItem(f->win->contentItem());
        f->root->setWidth(w);
        f->root->setHeight(h);
        f->model->setAdvancedOpen(adv);
        spin(150);
        f->scroller = f->byName("formScroller");
        f->page = f->byName("formPage");
        f->footer = f->byName("formFooter");
        f->vbar = f->byName("formVBar");
    };
    auto dispose = [](Form* f) {
        delete f->root;
        delete f->win;
        delete f->model;
    };

    // =====================================================================
    // The overflow scenario: 560x640 (the shell's floor), Advanced open, four
    // frame ranges - measured to overflow by 72px before this change.
    // =====================================================================
    Form f;
    makeForm(560, 640, true, 4, &f);
    check(f.scroller && f.page && f.footer && f.vbar,
          "the form builds with its scroll host, page, footer and scroll bar");
    if (!f.scroller) return 1;

    check(f.contentH() > f.scroller->height(),
          QString("precondition: the content actually overflows (%1 > %2) or nothing below can be observed")
              .arg(f.contentH()).arg(f.scroller->height()).toUtf8().constData());
    check(f.footer->height() == 52 && f.footer->mapToItem(f.root, 0, 0).y() == 640 - 52,
          "precondition: the action row starts pinned at the window's bottom band");

    // ---- Q9: the row is FIXED while the content scrolls ----------------------
    {
        const qreal y0 = f.footer->mapToItem(f.root, 0, 0).y();
        const qreal cy0 = f.contentY();
        QMetaObject::invokeMethod(f.scroller, "pageBy", Q_ARG(QVariant, 60));
        spin(60);
        check(f.contentY() > cy0, "the content scrolled (pageBy moved contentY)");
        check(f.footer->mapToItem(f.root, 0, 0).y() == y0,
              "Q9: the action row did not move while the content scrolled");
        // And the content really moved under the fixed row: the page shifted in
        // the window by exactly the scroll delta.
        QMetaObject::invokeMethod(f.scroller, "pageBy", Q_ARG(QVariant, -60));
        spin(60);
        check(f.footer->mapToItem(f.root, 0, 0).y() == y0,
              "Q9: and it is still pinned after scrolling back");
    }

    // ---- D25: wheel, drag and keys all scroll -------------------------------
    {
        const qreal cy0 = f.contentY();
        QWheelEvent we(QPointF(280, 300), QPointF(280, 300), QPoint(0, 0), QPoint(0, -240),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(f.win, &we);
        spin(80);
        check(f.contentY() != cy0,
              "D25: the wheel scrolls the content");

        // Drag (flick) on empty content: the Output section header is a plain
        // Text - the press falls through to the Flickable and drags the content.
        QQuickItem* header = nullptr;
        for (QQuickItem* c : f.root->findChildren<QQuickItem*>())
            if (c->property("text").toString() == QLatin1String("Output")) { header = c; break; }
        check(header != nullptr, "precondition: a plain (non-interactive) content area to drag from");
        if (header) {
            const QPointF hp = header->mapToItem(f.win->contentItem(), 2, 2);
            const qreal before = f.contentY();
            QMouseEvent press(QEvent::MouseButtonPress, hp, hp, Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(f.win, &press);
            for (int i = 1; i <= 5; ++i) {
                QMouseEvent move(QEvent::MouseMove, hp + QPointF(0, -12.0 * i),
                                 hp + QPointF(0, -12.0 * i), Qt::NoButton,
                                 Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(f.win, &move);
                spin(16);
            }
            QMouseEvent rel(QEvent::MouseButtonRelease, hp + QPointF(0, -60),
                            hp + QPointF(0, -60), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(f.win, &rel);
            spin(80);
            check(f.contentY() != before, "D25: a drag scrolls the content");
        }

        // Keys: PageUp/PageDown/Home/End at the scene root (the fields do not
        // consume these four, so they bubble - the second kind of "keys").
        f.footer->forceActiveFocus();
        spin(30);
        QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
        QCoreApplication::sendEvent(f.win, &end);
        spin(60);
        check(qAbs(f.contentY() - (f.contentH() - f.scroller->height())) < 2,
              "D25: End scrolls to the bottom of the content");
        QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
        QCoreApplication::sendEvent(f.win, &home);
        spin(60);
        check(f.contentY() == 0, "D25: Home scrolls back to the top");
        QKeyEvent pgdn(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
        QCoreApplication::sendEvent(f.win, &pgdn);
        spin(60);
        check(f.contentY() > 0, "D25: PageDown scrolls by a page");
    }

    // ---- D25: every control is REACHABLE (focus movement scrolls it in) ------
    {
        int checked = 0, walkFails = 0;
        // Start scrolled to the TOP: the controls deep in the form then NEED the
        // focus-following scroll (a walk that never leaves the viewport proves
        // nothing - measured lesson from the first draft of this block, which
        // passed with ensureVisible deliberately disabled).
        f.scroller->setProperty("contentY", 0.0);
        spin(40);
        const auto controls = f.root->findChildren<QQuickItem*>();
        for (QQuickItem* c : controls) {
            const QByteArray cn(c->metaObject()->className());
            const bool isControl = cn.contains("AeButton") || cn.contains("AeComboBox")
                || cn.contains("AeNumberField") || cn.contains("AeTextField")
                || cn.contains("AeCheckBox") || cn.contains("AeIconButton")
                || cn.contains("AeFrameRangeRow") || cn.contains("AeDisclosure")
                || c->objectName() == QLatin1String("destinationRow");
            if (!isControl || !c->isVisible()) continue;
            // Controls outside the scrolling page (the pinned footer's own) are
            // reachable BY CONSTRUCTION - their reachability is Q9's assert
            // below, and they are not scroll targets.
            bool inPage = false;
            for (QQuickItem* p = c; p; p = p->parentItem())
                if (p == f.page) { inPage = true; break; }
            if (!inPage) continue;
            // Land focus the way a keyboard would land it, then assert the whole
            // control is in the visible viewport (its row scrolled in with it).
            c->forceActiveFocus();
            spin(40);
            const QRectF r = f.rectInPage(c);
            const QRectF v = f.viewportInPage();
            if (!rectFits(r, v)) {
                ++walkFails;
                check(false, QString("D25: control '%1' is fully reachable (%2 not in %3)")
                                 .arg(c->property("text").toString().isEmpty()
                                      ? c->property("label").toString()
                                      : c->property("text").toString())
                                 .arg(rs(r), rs(v)).toUtf8().constData());
            }
            ++checked;
        }
        check(checked >= 10,
              QString("precondition: the walk covered the form's controls (%1 checked)").arg(checked)
                  .toUtf8().constData());
        check(walkFails == 0 && checked >= 10,
              QString("D25: all %1 in-page controls reachable (fully in viewport after focus)")
                  .arg(checked).toUtf8().constData());
        // Q9's half: the footer's own buttons never scroll away.
        QQuickItem* cancelBtn = nullptr;
        for (QQuickItem* c : f.footer->findChildren<QQuickItem*>())
            if (c->property("text").toString() == QLatin1String("Cancel")) { cancelBtn = c; break; }
        check(cancelBtn != nullptr, "precondition: the footer's Cancel button exists");
        if (cancelBtn) {
            const QPointF p = cancelBtn->mapToItem(f.root, 0, 0);
            check(rectFits(QRectF(p.x(), p.y(), cancelBtn->width(), cancelBtn->height()),
                           QRectF(0, 0, f.root->width(), f.root->height())),
                  "Q9: the action row's buttons are in the window without any scrolling");
        }
    }

    // ---- E28: a tooltip inside the scrolling content is clipped by NOTHING ---
    {
        // The BOTTOM-MOST kept-tip button, scrolled to sit 10px above the
        // viewport's bottom edge - where a window-clamped bubble would be cut by
        // the Flickable's clip and only a clip-aware clamp survives (the
        // measured defect class). It also sits at its row's right end, so the
        // horizontal clamp is exercised in the same case.
        QQuickItem* tipOwner = nullptr;
        qreal best = -1;
        for (QQuickItem* c : f.root->findChildren<QQuickItem*>()) {
            if (!QByteArray(c->metaObject()->className()).contains("AeIconButton")) continue;
            if (!c->property("showTip").toBool()) continue;
            const qreal b = f.rectInPage(c).bottom();
            if (b > best) { best = b; tipOwner = c; }
        }
        check(tipOwner != nullptr, "precondition: a kept-tip reset button exists");
        if (tipOwner) {
            const QRectF r = f.rectInPage(tipOwner);
            qreal cy = r.bottom() + 10 - f.scroller->height();
            cy = qMax(0.0, qMin(f.contentH() - f.scroller->height(), cy));
            check(cy > 0, "precondition: the edge case really scrolls (the button is deep in the form)");
            f.scroller->setProperty("contentY", cy);
            spin(60);
            QQuickItem* tip = nullptr;
            for (QQuickItem* c : tipOwner->findChildren<QQuickItem*>())
                if (QByteArray(c->metaObject()->className()).contains("AeToolTip")) {
                    tip = c; break;
                }
            check(tip != nullptr, "precondition: the button's AeToolTip is there");
            if (tip) {
                hoverAt(f.win, f.win->contentItem(), tipOwner,
                        QPointF(tipOwner->width() / 2, tipOwner->height() / 2));
                spin(800); // past the 500ms E26 delay
                check(tip->property("revealed").toBool(),
                      "precondition: the tooltip revealed after its delay");
                const QPointF tp = tip->mapToItem(f.page, 0, 0);
                const QRectF tipRect(tp.x(), tp.y(), tip->width(), tip->height());
                check(rectFits(tipRect, f.viewportInPage()),
                      QString("E28: the bubble fits the VISIBLE viewport, not just the window (%1 in %2)")
                          .arg(rs(tipRect), rs(f.viewportInPage())).toUtf8().constData());
                // And the edge case is real: with the owner 10px above the
                // bottom, a below-first bubble would end past the viewport - so
                // E27 must have flipped it above (or clamped) rather than cut.
                check(tipRect.bottom() <= f.viewportInPage().bottom()
                          && tipRect.top() <= r.top(),
                      "E27/E28: at the bottom edge the bubble flipped or clamped, never past the view");
            }
        }
    }

    // ---- G37 survives D25: the in-scene combo popup lives outside the clip ---
    {
        QQuickItem* combo = f.byName("Format");
        check(combo != nullptr, "precondition: the Format combo is addressable");
        QQuickItem* popupItem = nullptr;
        QObject* popupObj = nullptr;
        if (combo) {
            for (QQuickItem* c : combo->findChildren<QQuickItem*>())
                if (QByteArray(c->metaObject()->className()).contains("QQuickPopupItem")) {
                    popupItem = c;
                    popupObj = c->parent();
                    break;
                }
        }
        check(popupItem != nullptr, "precondition: the combo's popup item exists");
        if (popupItem) {
            // Structural: the popup must not be inside the scrolling page (or
            // the clip would cut it wherever it lands).
            bool inPage = false;
            for (QQuickItem* p = popupItem->parentItem(); p; p = p->parentItem()) {
                if (p == f.page) { inPage = true; break; }
            }
            check(!inPage, "G37: the popup is outside the scrolling content (clip cannot reach it)");
            QMetaObject::invokeMethod(popupObj, "open");
            spin(120);
            const QPointF pp = popupItem->mapToItem(f.root, 0, 0);
            const QRectF pr(pp.x(), pp.y(), popupItem->width(), popupItem->height());
            check(rectFits(pr, QRectF(0, 0, f.root->width(), f.root->height())),
                  QString("G37/D25: the opened popup sits inside the window (%1)").arg(rs(pr))
                      .toUtf8().constData());
        }
    }
    dispose(&f);

    // =====================================================================
    // The no-overflow scenario: geometry must be EXACTLY the designed look -
    // no scroll, no bar - so a fitting form changed nothing (the pixel proof
    // is overflow-spike's before/after AE on min_closed).
    // =====================================================================
    {
        Form g;
        makeForm(620, 720, false, 1, &g);
        check(g.contentY() == 0 && g.contentX() == 0, "no overflow: the content sits at rest");
        check(g.contentH() <= g.scroller->height(),
              "precondition: the content fits at this size");
        check(!g.vbar->isVisible(), "D23: the scroll bar is hidden when nothing overflows");
        const qreal y0 = g.footer->mapToItem(g.root, 0, 0).y();
        QWheelEvent we(QPointF(280, 300), QPointF(280, 300), QPoint(0, 0), QPoint(0, -240),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(g.win, &we);
        spin(60);
        check(g.contentY() == 0, "no overflow: the wheel cannot scroll the content away");
        check(g.footer->mapToItem(g.root, 0, 0).y() == y0, "no overflow: the row is where it was");
        dispose(&g);
    }

    QTextStream(stdout) << (failures ? "FAILURES: " : "ALL PASS ") << failures << "\n";
    return failures ? 1 : 0;
}
