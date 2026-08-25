#include "gui/prop/EasingPickWidget.h"
#include <QApplication>
#include <QClipboard>
#include <QMouseEvent>
#include <QAbstractSpinBox>
#include <QLineEdit>
#include "gui/AppStyle.h"
#include "gui/prop/bezierCurveEditor.h"
#include <cmath>

#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include "gui/theme/Colors.h"

namespace gui {
namespace prop {

namespace {

// Layout constants (px) measured from the reference geometry dump. These are
// the single source of truth for the geometry verification.
constexpr int kPopoverWidth = 300;
constexpr int kRootMargin = 1;
constexpr int kHeaderH = 40; // on the 4px grid (was 36/flex)
constexpr int kSegPad = 2;
constexpr int kSegGap = 2;
constexpr int kSegBtnH = 24; // clean grid value (was 22); the mock's was ~25 flex
constexpr int kBodyMarginTop = 9, kBodyMarginBottom = 8, kBodyMarginLr = 12;
constexpr int kBodyGapSegPipe = 8;
constexpr int kBodyGapTrowBlend = 6;
constexpr int kPipelineH = 3;
constexpr int kTile = 32;      // on the 4px grid (was 30); fills the row, leaves ~9px
constexpr int kTileGlyph = 28; // on the 4px grid (was 26), fits in the 32px tile
constexpr int kTileGap = 4;
constexpr int kVsepW = 9; // thin hairline separator (1px line + 4px margins), not a tile slot
constexpr int kBeGlyphW = 18, kBeGlyphH = 12;
constexpr int kSliderHead = 18; // knob (14px) + 2px margin top/bottom so its top edge isn't clipped

// value-axis headroom (mirrors the mock's VMIN/VMAX)
constexpr double kVMIN = -0.35, kVMAX = 1.35;

// Picker tile catalog in display order (mock PRIMARY + MORE; the vsep sits
// between index 3 and 4). Keyed by position, not enum value.
const util::Easing::Type kTileOrder[13] = {
    util::Easing::Type_Linear, util::Easing::Type_Floor, util::Easing::Type_Round,
    util::Easing::Type_Cubic,  util::Easing::Type_Sine,  util::Easing::Type_Quad,
    util::Easing::Type_Quart,  util::Easing::Type_Quint, util::Easing::Type_Expo,
    util::Easing::Type_Circ,   util::Easing::Type_Back,  util::Easing::Type_Elastic,
    util::Easing::Type_Bounce,
};

QString tileName(int aIdx) { return QString::fromLatin1("tile-%1").arg(aIdx); }

util::Easing::Param mkParam(util::Easing::Type aType, util::Easing::Range aRange,
                            float aWeight) {
    util::Easing::Param p;
    p.type = aType;
    p.range = aRange;
    p.weight = aWeight;
    return p;
}

// A slider that owns the mouse grab for the duration of a drag. Inside the
// app the picker is a Qt::Popup; a popup steals the pointer grab (it's
// watching for the click-outside that dismisses it), so once the cursor
// leaves the popup rect a plain QSlider stops receiving moves and the drag
// stalls. An explicit widget-level grab keeps every move flowing to the
// slider until release. A normal-window slider (e.g. the volume slider)
// needs this only because nothing there steals the implicit press grab.
class GrabSlider : public QSlider {
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent* aEvent) override {
        grabMouse();
        QSlider::mousePressEvent(aEvent);
    }
    void mouseReleaseEvent(QMouseEvent* aEvent) override {
        QSlider::mouseReleaseEvent(aEvent);
        if (mouseGrabber() == this) releaseMouse();
    }
};

QString rgba(QColor c, double aAlpha) {
    c.setAlphaF(aAlpha);
    return c.name(QColor::HexArgb);
}

const theme::Colors& C() { return theme::Colors::current(); }

// Close ✕ — a thin drawn X matching the mock's SVG glyph. The Unicode "✕"
// text glyph renders ~5px and off-center at 12px; a 10px drawn X with round
// caps matches the reference (10x10 bbox, ~1.5px stroke, centered in a 22px
// button). Reads #popover-close QSS for the hover raised background.
class CloseGlyph : public QToolButton {
public:
    explicit CloseGlyph(QWidget* aParent = nullptr)
        : QToolButton(aParent) {
        setObjectName("popover-close");
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(22, 22);
        setAutoRaise(true);
        setText(QString()); // no text glyph; we draw the X
    }

protected:
    void paintEvent(QPaintEvent* aEvent) override {
        QToolButton::paintEvent(aEvent); // QSS bg (transparent / hover raised)
        // The exact Phosphor "x" icon, taken from the canonical
        // phosphor-icons/core ASSETS set (assets/regular/x.svg), NOT the raw
        // set (raw/regular/x.svg is two <line> strokes). The assets glyph is
        // a single filled path with fill="currentColor", viewBox 0 0 256 256.
        // The mock renders it as a 14x14 <svg> centered in the 22x22 button
        // (variant-a-gauge.html's .btn-ghost), so scale to the 14px icon box.
        QPainter pt(this);
        pt.setRenderHint(QPainter::Antialiasing, true);
        const theme::Colors& c = theme::Colors::current();
        // Hover brightens to the text color (matches .btn-ghost:hover), rest
        // is the mock's --text-muted (#737373) — the dim token, since the dark
        // theme's textMuted (#a3a3a3) is too bright.
        const QColor col = underMouse() ? c.text : c.textMutedDim;
        static const QByteArray kSvgTmpl = QByteArrayLiteral(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 256 256\""
            " fill=\"currentColor\"><path d=\"M205.66,194.34a8,8,0,0,1-11.32,"
            "11.32L128,139.31,61.66,205.66a8,8,0,0,1-11.32-11.32L116.69,128,"
            "50.34,61.66A8,8,0,0,1,61.66,50.34L128,116.69l66.34-66.35a8,8,0,0,"
            "1,11.32,11.32L139.31,128Z\"/></svg>");
        QByteArray svg = kSvgTmpl;
        svg.replace("currentColor", col.name(QColor::HexRgb).toUtf8());
        QSvgRenderer r(svg);
        const qreal s = 14.0;
        const qreal ox = (width() - s) / 2.0, oy = (height() - s) / 2.0;
        r.render(&pt, QRectF(ox, oy, s, s));
    }
};

// Animation preview gauge (.be-strip in the mock): an 18px strip to the right
// of the bezier editor showing the live motion value on the value axis — a
// dot (ball) that moves as the preview animates (vertical track + accent ball).
class PreviewGauge : public QWidget {
public:
    explicit PreviewGauge(QWidget* aParent = nullptr) : QWidget(aParent) {
        setObjectName("be-strip");
        setFixedWidth(18);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }
    void setValue(float v) { mValue = v; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter pt(this);
        pt.setRenderHint(QPainter::Antialiasing, true);
        const theme::Colors& c = theme::Colors::current();
        // Mock .be-y-dot: an 8px white dot pinned to the strip's right edge
        // (right:0), top = (1.5 - v)/2 * height — the SAME value axis as the
        // bezier stage (-0.5..1.5). No track line.
        constexpr double kVMIN = -0.5, kVMAX = 1.5;
        const double v = std::clamp((double)mValue, kVMIN, kVMAX);
        const qreal top = (kVMAX - v) / (kVMAX - kVMIN) * height();
        pt.setPen(Qt::NoPen);
        pt.setBrush(c.text); // --text (near-white), not accent-bright
        pt.drawEllipse(QPointF(width() - 5.0, top + 5.0), 5.0, 5.0);
    }
private:
    float mValue = 0.5f;
};

} // namespace

// ---------------------------------------------------------------------------
// FlowLayout — minimal flex-wrap: place left->right, wrap when the row fills,
// uniform gap. Reproduces the mock's flex-wrap catalog (trow) so it wraps at
// the same point.
// ---------------------------------------------------------------------------
class FlowLayout : public QLayout {
public:
    FlowLayout(QWidget* aParent, int aGap) : QLayout(aParent), mGap(aGap) {
        setContentsMargins(0, 0, 0, 0);
    }
    ~FlowLayout() {
        while (QLayoutItem* it = takeAt(0)) delete it;
    }
    void addItem(QLayoutItem* aItem) override { mItems.append(aItem); }
    int count() const override { return mItems.size(); }
    QLayoutItem* itemAt(int aIndex) const override {
        return (aIndex < 0 || aIndex >= mItems.size()) ? nullptr : mItems.at(aIndex);
    }
    QLayoutItem* takeAt(int aIndex) override {
        if (aIndex < 0 || aIndex >= mItems.size()) return nullptr;
        return mItems.takeAt(aIndex);
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    QSize sizeHint() const override {
        return QSize(200, doLayout(QRect(0, 0, 200, 0), true));
    }
    QSize minimumSize() const override { return QSize(0, 0); }
    int heightForWidth(int aWidth) const override {
        return doLayout(QRect(0, 0, aWidth, 0), true);
    }
    bool hasHeightForWidth() const override { return true; }
    void setGeometry(const QRect& aRect) override {
        QLayout::setGeometry(aRect);
        doLayout(aRect, false);
    }

private:
    int doLayout(const QRect& aRect, bool aTestOnly) const {
        int x = aRect.x(), y = aRect.y(), rowH = 0;
        for (QLayoutItem* it : mItems) {
            if (it->isEmpty()) continue;
            const QSize sz = it->sizeHint();
            if (x + sz.width() > aRect.right() + 1 && x > aRect.x()) {
                x = aRect.x();
                y += rowH + mGap;
                rowH = 0;
            }
            if (!aTestOnly) it->setGeometry(QRect(QPoint(x, y), sz));
            x += sz.width() + mGap;
            rowH = qMax(rowH, sz.height());
        }
        return y + rowH - aRect.y();
    }

    QList<QLayoutItem*> mItems;
    int mGap;
};

// ---------------------------------------------------------------------------
// CurveGlyph
// ---------------------------------------------------------------------------
CurveGlyph::CurveGlyph(QWidget* aParent) : QWidget(aParent) {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}
void CurveGlyph::setParam(const util::Easing::Param& aParam) { mParam = aParam; update(); }
void CurveGlyph::setGlyphColor(const QColor& aColor) { mColor = aColor; update(); }
void CurveGlyph::setStrokeWidth(double aWidth) { mStrokeWidth = aWidth; update(); }

void CurveGlyph::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal w = width(), h = height();
    if (w <= 1 || h <= 1) return;

    // Reference (curveD in mock.js) computes the path in a 100x100 viewBox
    // with pad=2 and value axis [kVMIN,kVMAX], then non-uniformly scales to
    // the widget via preserveAspectRatio="none". Reproduce that scaling.
    constexpr double VW = 100.0, VH = 100.0;
    const double pad = 2.0, span = VH - 2.0 * pad;
    const double x0 = pad, x1 = VW - pad;
    auto toScreen = [&](double vx, double vy) {
        return QPointF(vx * w / VW, vy * h / VH);
    };

    QPen pen(mColor.isValid() ? mColor : palette().color(QPalette::WindowText));
    pen.setWidthF(mStrokeWidth);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);

    if (mParam.type == util::Easing::Type_Floor) {
        const double y0 = pad + (kVMAX - 0.0) / (kVMAX - kVMIN) * span;
        const double y1 = pad + (kVMAX - 1.0) / (kVMAX - kVMIN) * span;
        QPainterPath path;
        path.moveTo(toScreen(x0, y0));
        path.lineTo(toScreen(x1, y0));
        path.lineTo(toScreen(x1, y1));
        p.drawPath(path);
        return;
    }

    const int N = 64;
    QPainterPath path;
    bool first = true;
    for (int i = 0; i <= N; ++i) {
        const double x = (double)i / N;
        const double y = util::Easing::calculate(mParam, x, 0.0f, 1.0f, 1.0f);
        const double vx = x0 + x * (x1 - x0);
        const double vy = pad + (kVMAX - y) / (kVMAX - kVMIN) * span;
        const QPointF s = toScreen(vx, vy);
        if (first) { path.moveTo(s); first = false; }
        else path.lineTo(s);
    }
    p.drawPath(path);
}

// ---------------------------------------------------------------------------
// EasingPickWidget
// ---------------------------------------------------------------------------
EasingPickWidget::~EasingPickWidget() {
    // The slider's proxy style is owned by the slider widget itself
    // (QWidget::setStyle transfers ownership); it is cleaned up when the
    // child slider is destroyed during ~QWidget. Do NOT delete it here: the
    // base destructor runs after this body and the slider would be painted
    // with a dangling style.
}

EasingPickWidget::EasingPickWidget(const util::Easing::Param& aInitial, QWidget* aParent)
    : QFrame(aParent), mDraft(aInitial), mInitial(aInitial) {
    setObjectName("popover");
    setFixedWidth(kPopoverWidth);
    // Rounded corners need a real-alpha window: a Qt::Popup (top-level) plus
    // a translucent backing, with FramelessWindowHint so the backing is a
    // composited alpha surface rather than an opaque square (on X11/Wayland
    // the compositor handles the alpha; on Windows the masked paint in
    // paintEvent clips the corner). Setting the flags here means the widget
    // is also a correct popup when created standalone (tests), independent of
    // the owner also setting them (setting twice is a no-op).
    if (!testAttribute(Qt::WA_TranslucentBackground))
        setAttribute(Qt::WA_TranslucentBackground, true);
    if (!(windowFlags() & Qt::FramelessWindowHint))
        setWindowFlag(Qt::FramelessWindowHint, true);
    // WA_StyledBackground would fill the backing (black corners); the rounded
    // body is painted in paintEvent instead. Leave it off.

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(kRootMargin, kRootMargin, kRootMargin, kRootMargin);
    root->setSpacing(0);
    root->addWidget(buildHeader());
    mBody = buildBody();
    root->addWidget(mBody);
    root->addWidget(buildFooter());

    // Live motion preview: animate the pipeline fill with the current easing.
    // mAnimTimer was declared but never created/connected — that's why the
    // preview was frozen. Create it here and start it in showEvent.
    mAnimTimer = new QTimer(this);
    mAnimTimer->setInterval(16); // ~60fps
    connect(mAnimTimer, &QTimer::timeout, this, &EasingPickWidget::animStep);

    applyAppearance();
    refresh();

    // Clicking a non-text child (the curve editor, a label, the background)
    // must release the active spinbox's edit focus. Those widgets are
    // Qt::NoFocus, so a plain click there never hands focus off the field.
    // Watch every widget so any press that isn't on a text editor clears it.
    installEventFilter(this);
    for (QWidget* w : findChildren<QWidget*>())
        w->installEventFilter(this);
}

void EasingPickWidget::applyAppearance() {
    const theme::Colors& c = C();
    // When mHideText is set, every textual glyph is painted fully transparent
    // (alpha 0) — each widget's box, background and border stay identical, so
    // the layout is bit-for-bit unchanged. Graphics that are drawn directly
    // (curve glyphs, bezier editor, gauge, close ✕, slider) use their own
    // explicit colors and are unaffected by these QSS color rules.
    // Emit opaque text as #RRGGBB and hidden text as the #AARRGGBB zero-alpha
    // form (QColor::name() alone would drop the alpha and paint opaque black).
    const auto textCol = [](bool aHide, const QColor& aC) {
        return aHide ? QStringLiteral("#00000000") : aC.name();
    };
    const QString text = textCol(mHideText, c.text);
    // The mock's muted glyphs/labels are --text-muted (#737373), but the dark
    // theme reads textMuted as neutral-400 (#a3a3a3, brighter). Use the dim
    // token so the picker's muted text matches the mock.
    const QString muted = textCol(mHideText, c.textMutedDim);
    const QString bright = textCol(mHideText, c.accentBright);
    const QString accentText = textCol(mHideText, c.accentText);
    QString q;
    q += "QFrame#popover { background: " + c.base.name() + "; }\n";
    q += "QWidget#popover-head { border-bottom: 1px solid " + c.hairline.name() + "; }\n";
    q += "QWidget#foot { border-top: 1px solid " + c.hairline.name() + "; }\n";
    q += "QLabel#popover-title { font-size: 12px; font-weight: 600; color: " + text + "; }\n";
    q += "QToolButton#popover-close { background: transparent; border: 0; border-radius: 4px; color: " + muted + "; font-size: 12px; }\n";
    q += "QToolButton#popover-close:hover { color: " + text + "; background: " + c.raised.name() + "; }\n";
    q += "QToolButton[easingRole=\"seg\"] { background: transparent; border: 0; border-radius: 4px; color: " + muted + "; font-size: 11px; padding: 0 4px; }\n";
    q += "QToolButton[easingRole=\"seg\"]:hover { color: " + text + "; background: " + c.raised.name() + "; }\n";
    q += "QToolButton[easingRole=\"seg\"]:checked { background: " + c.active.name() + "; color: " + text + "; }\n";
    q += "QToolButton[easingRole=\"tile\"] { background: transparent; border: 0; border-radius: 5px; color: " + muted + "; }\n";
    q += "QToolButton[easingRole=\"tile\"]:hover { background: " + c.raised.name() + "; color: " + text + "; }\n";
    q += "QToolButton[easingRole=\"tile\"]:checked { background: " + rgba(c.accent, 0.22) + "; color: " + bright + "; }\n";
    q += "QPushButton[role=\"cancel\"] { background: " + c.raised.name() + "; border: 1px solid " + c.raised.name() + "; border-radius: 4px; color: " + text + "; font-size: 11px; padding: 0 12px; }\n";
    q += "QPushButton[role=\"cancel\"]:hover { background: " + c.raisedHover.name() + "; border-color: " + c.hover.name() + "; }\n";
    q += "QPushButton[role=\"apply\"] { background: " + c.accent.name() + "; border: 1px solid " + c.accent.name() + "; border-radius: 4px; color: " + accentText + "; font-size: 11px; padding: 0 12px; }\n";
    q += "QPushButton[role=\"apply\"]:hover { background: " + c.accentHover.name() + "; border-color: " + c.accentHover.name() + "; }\n";
    q += "QPushButton[role=\"bact\"] { background: " + c.raised.name() + "; border: 0; border-radius: 4px; color: " + muted + "; font-size: 10px; padding: 5px; }\n";
    q += "QPushButton[role=\"bact\"]:hover { background: " + c.raisedHover.name() + "; color: " + text + "; }\n";
    // bezier inputs (labeled fields, mock .fl/.fi): the app field standard —
    // a geometry-reserving 1px border (raised at rest) that only recolors on
    // hover/focus, matching the dock spinbox feedback (border 0 previously
    // killed the inherited hover; no pressed/active state existed).
    q += "QLabel[role=\"fl\"] { font-size: 9px; color: " + muted + "; }\n";
    q += "QDoubleSpinBox[role=\"fi\"] { background: " + c.raised.name() + "; border: 1px solid " + c.raised.name() + "; border-radius: 4px; color: " + text + "; font-size: 11px; padding: 3px 5px; }\n";
    q += "QDoubleSpinBox[role=\"fi\"]:hover { border-color: " + c.hover.name() + "; }\n";
    q += "QDoubleSpinBox[role=\"fi\"]:focus { border-color: " + c.focus.name() + "; }\n";
    q += "QDoubleSpinBox[role=\"fi\"]:hover:pressed { background: " + c.recessed.name() + "; }\n";
    setStyleSheet(q);
}

void EasingPickWidget::setHideText(bool aHide) {
    if (mHideText == aHide) return;
    mHideText = aHide;
    // The title and blend-value labels carry their own inline stylesheet, so
    // re-apply them with a transparent (or normal) text glyph to match. Use the
    // HexArgb transparent form (the zero-alpha "#00000000").
    const QString tcol = mHideText ? QStringLiteral("#00000000") : C().text.name();
    const QString mcol = mHideText ? QStringLiteral("#00000000") : C().textMutedDim.name();
    if (mTitle)
        mTitle->setStyleSheet("font-size:12px;font-weight:600;color:" + tcol);
    if (mBlendVal)
        mBlendVal->setStyleSheet("font-size:10px;color:" + mcol);
    applyAppearance();
    refresh();
}

QWidget* EasingPickWidget::buildHeader() {
    QWidget* head = new QWidget(this);
    mHead = head;
    head->setObjectName("popover-head");
    head->setFixedHeight(kHeaderH);
    head->setAttribute(Qt::WA_StyledBackground, true);
    QHBoxLayout* l = new QHBoxLayout(head);
    l->setContentsMargins(12, 0, 8, 0);
    l->setSpacing(6);
    mTitle = new QLabel(tr("Easing"), head);
    mTitle->setObjectName("popover-title");
    mTitle->setStyleSheet("font-size:12px;font-weight:600;color:" + C().text.name());
    l->addWidget(mTitle);
    l->addStretch(1);
    // CloseGlyph draws a thin X matching the mock's SVG glyph (SetText("✕")
    // Unicode was ~5px and off-center). Uses the QSS #popover-close rules for
    // the hover background.
    CloseGlyph* close = new CloseGlyph(head);
    close->setToolTip(tr("Close"));
    connect(close, &QToolButton::clicked, this, &EasingPickWidget::cancel);
    l->addWidget(close);
    return head;
}

QWidget* EasingPickWidget::buildBody() {
    QWidget* body = new QWidget(this);
    mBody = body;
    body->setObjectName("popover-body");
    QVBoxLayout* l = new QVBoxLayout(body);
    l->setContentsMargins(kBodyMarginLr, kBodyMarginTop, kBodyMarginLr, kBodyMarginBottom);
    // Uniform 8px body spacing so that a hidden widget (the motion-preview bar
    // and the normal panel in Custom mode) collapses away entirely, matching
    // the reference mock where the hidden pipeline's margin disappears. The
    // seg<->pipeline and pipeline<->trow gaps are both 8px, so a single
    // spacing value reproduces the normal-mode layout exactly.
    l->setSpacing(kBodyGapSegPipe);

    l->addWidget(buildSeg((util::Easing::Range)mDraft.range));
    l->addWidget(buildPipeline());

    mNormalPanel = new QWidget(body);
    // The popup is fixed to a per-mode height (kNormalHeight). Make this panel
    // fill the body's leftover height so the tile grid sits just below the
    // header and the blend slider is pushed to the bottom (end-aligned, above
    // the footer), with the surplus as a gap between them — instead of the
    // blend getting centered in the middle of a stretched panel.
    mNormalPanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    QVBoxLayout* n = new QVBoxLayout(mNormalPanel);
    n->setContentsMargins(0, 0, 0, 0);
    n->setSpacing(0);
    n->addWidget(buildTrow());
    n->addStretch(1);   // surplus space goes between the tiles and the blend
    n->addWidget(buildBlend());
    l->addWidget(mNormalPanel);

    mBezierPanel = buildBezierPanel();
    mBezierPanel->setVisible(false);
    mBezierPanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    l->addWidget(mBezierPanel);

    return body;
}

QWidget* EasingPickWidget::buildSeg(util::Easing::Range) {
    QFrame* seg = new QFrame(this);
    seg->setObjectName("seg");
    seg->setAttribute(Qt::WA_StyledBackground, true);
    // Scope to #seg so the selector-less rule doesn't cascade `background:
    // recessed` onto the child buttons (which would override their :checked
    // active fill).
    seg->setStyleSheet("#seg { background:" + C().recessed.name() + ";border-radius:6px; }");
    mSegLayout = new QGridLayout(seg);
    mSegLayout->setContentsMargins(kSegPad, kSegPad, kSegPad, kSegPad);
    mSegLayout->setSpacing(kSegGap);
    for (int i = 0; i < 4; ++i) mSegLayout->setColumnStretch(i, 1);

    const char* labels[4] = { "In", "Out", "In\u2013Out", "Custom" };
    const char* names[4] = { "seg-r-0", "seg-r-1", "seg-r-2", "seg-r-custom" };
    for (int i = 0; i < 4; ++i) {
        QToolButton* b = new QToolButton(seg);
        b->setObjectName(names[i]);
        b->setText(QString::fromUtf8(labels[i]));
        b->setCheckable(true);
        b->setProperty("easingRole", "seg");
        b->setFixedHeight(kSegBtnH);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setCursor(Qt::PointingHandCursor);
        mSegBtns.append(b);
        mSegLayout->addWidget(b, 0, i);
    }
    connect(mSegBtns[0], &QToolButton::clicked, this, [this] { setRange(util::Easing::Range_In); });
    connect(mSegBtns[1], &QToolButton::clicked, this, [this] { setRange(util::Easing::Range_Out); });
    connect(mSegBtns[2], &QToolButton::clicked, this, [this] { setRange(util::Easing::Range_InOut); });
    connect(mSegBtns[3], &QToolButton::clicked, this, [this] { setCustom(); });
    return seg;
}

QWidget* EasingPickWidget::buildPipeline() {
    QWidget* pipe = new QWidget(this);
    mPipeline = pipe;
    pipe->setObjectName("pipeline");
    pipe->setFixedHeight(kPipelineH);
    pipe->setStyleSheet("background:" + C().raised.name() + ";border-radius:2px;");
    QVBoxLayout* l = new QVBoxLayout(pipe);
    l->setContentsMargins(0, 0, 0, 0);
    mPipelineFill = new QWidget(pipe);
    mPipelineFill->setObjectName("pipeline-fill");
    mPipelineFill->setFixedHeight(kPipelineH);
    mPipelineFill->setFixedWidth(137); // 50% of the 274px pipeline (fixed-width popover)
    mPipelineFill->setStyleSheet("background:" + C().accent.name() + ";border-radius:2px;");
    l->addWidget(mPipelineFill, 0, Qt::AlignLeft);
    return pipe;
}

QWidget* EasingPickWidget::buildTrow() {
    mTrow = new QWidget(this);
    mTrow->setObjectName("trow");
    FlowLayout* flow = new FlowLayout(mTrow, kTileGap);
    for (int i = 0; i < 13; ++i) {
        QToolButton* tile = makeTileAt(i, kTileOrder[i]);
        tile->setObjectName(tileName(i));
        flow->addWidget(tile);
        mTileBtns.append(tile);
        if (i == 3) {
            QFrame* sep = new QFrame(mTrow);
            sep->setObjectName("vsep");
            sep->setFixedSize(kVsepW, kTile);
            QHBoxLayout* hl = new QHBoxLayout(sep);
            hl->setContentsMargins(4, 0, 4, 0);
            QWidget* line = new QWidget(sep);
            line->setFixedSize(1, kTile);
            line->setStyleSheet("background:" + C().hairline.name() + ";");
            hl->addWidget(line);
            flow->addWidget(sep);
        }
    }
    return mTrow;
}

QToolButton* EasingPickWidget::makeTileAt(int aIndex, util::Easing::Type aType) {
    QToolButton* tile = new QToolButton(mTrow);
    tile->setFixedSize(kTile, kTile);
    tile->setCursor(Qt::PointingHandCursor);
    tile->setCheckable(true);
    tile->setAutoRaise(true);
    tile->setProperty("easingRole", "tile");
    QVBoxLayout* vl = new QVBoxLayout(tile);
    vl->setContentsMargins(0, 0, 0, 0); // center the glyph via flex alignment, not padding
    CurveGlyph* g = new CurveGlyph(tile);
    g->setObjectName("t-glyph");
    g->setFixedSize(kTileGlyph, kTileGlyph);
    g->setParam(mkParam(aType, mDraft.range, mDraft.weight));
    g->setGlyphColor(C().textMutedDim);
    vl->addWidget(g, 0, Qt::AlignCenter);
    connect(tile, &QToolButton::clicked, this, [this, aType] { setType(aType); });
    Q_UNUSED(aIndex);
    return tile;
}

QWidget* EasingPickWidget::buildBlend() {
    QWidget* wrap = new QWidget(this);
    wrap->setObjectName("blend-wrap");
    QHBoxLayout* l = new QHBoxLayout(wrap);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(8);

    mBlendLinear = new CurveGlyph(wrap);
    mBlendLinear->setObjectName("be-glyph-linear");
    mBlendLinear->setFixedSize(kBeGlyphW, kBeGlyphH);
    mBlendLinear->setParam(mkParam(util::Easing::Type_Linear, util::Easing::Range_InOut, 1.0f));
    mBlendLinear->setGlyphColor(C().textMutedDim);
    l->addWidget(mBlendLinear);

    mBlendInput = new GrabSlider(Qt::Horizontal, wrap);
    mBlendInput->setObjectName("blend-input");
    mBlendInput->setRange(0, 100);
    mBlendInput->setValue(100);
    mBlendInput->setFixedHeight(kSliderHead); // >= 2*knobRadius so the knob isn't clipped
    // The repo's blend slider is drawn by the AppStyle proxy (CC_Slider): a
    // gradient track and a round accent knob, matching the reference mock.
    // Set the style directly so the proxy (not the app-wide QSS) renders it.
    mSliderStyle = new AppStyle(QApplication::style());
    mBlendInput->setStyle(mSliderStyle);
    mBlendInput->setCursor(Qt::PointingHandCursor);
    mBlendInput->setFocusPolicy(Qt::NoFocus);
    connect(mBlendInput, &QSlider::valueChanged, this,
            [this](int v) { setWeight(v / 100.0f); });
    l->addWidget(mBlendInput, 1);

    mBlendCurve = new CurveGlyph(wrap);
    mBlendCurve->setObjectName("be-glyph-curve");
    mBlendCurve->setFixedSize(kBeGlyphW, kBeGlyphH);
    mBlendCurve->setParam(mkParam(util::Easing::Type_Cubic, util::Easing::Range_InOut, 1.0f));
    mBlendCurve->setGlyphColor(C().textMutedDim);
    l->addWidget(mBlendCurve);

    mBlendVal = new QLabel(wrap);
    mBlendVal->setObjectName("blend-val");
    mBlendVal->setFixedWidth(30);
    mBlendVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    mBlendVal->setStyleSheet("font-size:10px;color:" + C().textMutedDim.name() + ";");
    l->addWidget(mBlendVal);
    return wrap;
}

QWidget* EasingPickWidget::buildFooter() {
    // QFrame + WA_StyledBackground so the QSS #foot { border-top } hairline
    // (separating the footer from the body) renders.
    QFrame* foot = new QFrame(this);
    mFoot = foot;
    foot->setObjectName("foot");
    foot->setAttribute(Qt::WA_StyledBackground, true);
    foot->setFixedHeight(48); // on the 4px grid (was flex ~46)
    QHBoxLayout* l = new QHBoxLayout(foot);
    l->setContentsMargins(12, 12, 12, 12); // center the 24px buttons in the 48px footer
    l->setSpacing(6);
    l->addStretch(1);
    mBtnCancel = new QPushButton(tr("Cancel"), foot);
    mBtnCancel->setObjectName("btn-cancel");
    mBtnCancel->setProperty("role", "cancel");
    mBtnCancel->setFixedHeight(24);
    connect(mBtnCancel, &QPushButton::clicked, this, &EasingPickWidget::cancel);
    l->addWidget(mBtnCancel);
    mBtnApply = new QPushButton(tr("Apply"), foot);
    mBtnApply->setObjectName("btn-apply");
    mBtnApply->setProperty("role", "apply");
    mBtnApply->setFixedHeight(24);
    mBtnApply->setDefault(true);
    connect(mBtnApply, &QPushButton::clicked, this, [this] {
        mInitial = mDraft;
        emit applied(mDraft);
    });
    l->addWidget(mBtnApply);
    return foot;
}

QWidget* EasingPickWidget::buildBezierPanel() {
    QWidget* panel = new QWidget(mBody);
    panel->setObjectName("bezier-panel");
    QVBoxLayout* l = new QVBoxLayout(panel);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(8);

    // The draggable curve editor (reuses the engine's editor; it is
    // authoritative: paintEvent syncs m_points <-> bezier <-> spins).
    mBezierEditor = new BezierCurveEditor(panel, &mDraft.cubicBezier, {}, &mBezierProgress);
    mBezierEditor->setObjectName("bezier-stage");
    // The mock's stage is a 1:1 square (the curve SVG's intrinsic aspect), so
    // pin the editor to its OWN width (274 body content − 18px gauge = 256),
    // not the full body width — a 256x256 square like the reference.
    mBezierEditor->setMinimumHeight(256);
    mBezierEditor->setMaximumHeight(256);
    mBezierEditor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // be-root: the graph is flex:1 (NOT full width) + the animation gauge to
    // its right, per the mock (.be-root{display:flex} .be-strip{width:18px}).
    QWidget* beroot = new QWidget(panel);
    beroot->setObjectName("be-root");
    QHBoxLayout* bel = new QHBoxLayout(beroot);
    bel->setContentsMargins(0, 0, 0, 0);
    bel->setSpacing(0);
    bel->addWidget(mBezierEditor, 1);
    mBezierGauge = new PreviewGauge(beroot);
    bel->addWidget(mBezierGauge);
    l->addWidget(beroot);

    // x1 y1 x2 y2 spins, wired so a typed edit lands in the draft bezier.
    const char* names[4] = { "x1", "y1", "x2", "y2" };
    QHBoxLayout* spins = new QHBoxLayout();
    spins->setSpacing(5);
    for (int i = 0; i < 4; ++i) {
        // bfield: a label (.fl) above the input (.fi), per the mock.
        QWidget* field = new QWidget(panel);
        field->setProperty("role", "bfield");
        QVBoxLayout* f = new QVBoxLayout(field);
        f->setContentsMargins(0, 0, 0, 0);
        f->setSpacing(2);
        QLabel* lab = new QLabel(QLatin1String(names[i]), field);
        lab->setProperty("role", "fl");
        lab->setFixedHeight(12);
        f->addWidget(lab);
        QDoubleSpinBox* s = new QDoubleSpinBox(field);
        s->setObjectName(QString::fromLatin1("bezier-") + QLatin1String(names[i]));
        s->setProperty("role", "fi");
        s->setLocale(QLocale::c());
        s->setRange(-2.0, 2.0);
        s->setSingleStep(0.01);
        s->setDecimals(2);
        s->setButtonSymbols(QAbstractSpinBox::NoButtons);
        s->setFixedHeight(24);
        f->addWidget(s);
        spins->addWidget(field);
        mBezierSpins.append(s);
        QDoubleSpinBox::connect(s, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                                this, [this, i](double v) { onBezierSpin(i, v); });
    }
    l->addLayout(spins);

    // These spins are fed by the editor's paintEvent (which writes the
    // clamped values back); give it the vector so it can update them.
    mBezierEditor->spinBoxes = mBezierSpins;

    // Copy / Paste control values.
    QHBoxLayout* acts = new QHBoxLayout();
    acts->setSpacing(6);
    QPushButton* copy = new QPushButton(tr("Copy"), panel);
    copy->setObjectName("bezier-copy");
    copy->setProperty("role", "bact");
    QPushButton* paste = new QPushButton(tr("Paste"), panel);
    paste->setObjectName("bezier-paste");
    paste->setProperty("role", "bact");
    connect(copy, &QPushButton::clicked, this, [this] {
        const util::Easing::CubicBezier& b = mDraft.cubicBezier;
        QGuiApplication::clipboard()->setText(
            QStringLiteral("%1,%2,%3,%4").arg(b.x1).arg(b.y1).arg(b.x2).arg(b.y2));
    });
    connect(paste, &QPushButton::clicked, this, [this] {
        const QStringList parts = QGuiApplication::clipboard()->text().split(',');
        if (parts.size() != 4) return;
        bool ok[4] = { false, false, false, false };
        const double v[4] = { parts[0].toDouble(&ok[0]), parts[1].toDouble(&ok[1]),
                              parts[2].toDouble(&ok[2]), parts[3].toDouble(&ok[3]) };
        if (!(ok[0] && ok[1] && ok[2] && ok[3])) return;
        mDraft.cubicBezier = { (float)v[0], (float)v[1], (float)v[2], (float)v[3] };
        renderBezier();
    });
    acts->addWidget(copy);
    acts->addWidget(paste);
    l->addLayout(acts);

    // The popup is fixed to kCustomHeight (taller than the bezier panel's
    // natural height), and the bezier content should NOT stretch to fill — it
    // stays at its natural size (editor 256 + spins + actions) with the surplus
    // as a gap at the bottom. The panel itself is Expanding (reaches the
    // footer); this stretch keeps its content packed at the top.
    l->addStretch(1);
    return panel;
}

void EasingPickWidget::onBezierSpin(int aIndex, double aValue) {
    util::Easing::CubicBezier& b = mDraft.cubicBezier;
    if (aIndex == 0) b.x1 = (float)aValue;
    else if (aIndex == 1) b.y1 = (float)aValue;
    else if (aIndex == 2) b.x2 = (float)aValue;
    else b.y2 = (float)aValue;
    // Sync the handle points from the new bezier so the paint event's
    // write-back keeps it (otherwise it reverts to a stale control point).
    if (mBezierEditor) { mBezierEditor->syncFromBezier(); mBezierEditor->update(); }
}

// ---- interactions ----------------------------------------------------------
void EasingPickWidget::setType(util::Easing::Type aType) {
    mDraft.type = aType;
    refresh();
}
void EasingPickWidget::setRange(util::Easing::Range aRange) {
    if (mDraft.type == util::Easing::Type_Custom)
        mDraft.type = util::Easing::Type_Cubic;
    mDraft.range = aRange;
    refresh();
}
void EasingPickWidget::setWeight(float aWeight) {
    mDraft.weight = aWeight;
    refresh();
}
void EasingPickWidget::setCustom() {
    mDraft.type = util::Easing::Type_Custom;
    refresh();
    renderBezier();
}
void EasingPickWidget::setValue(const util::Easing::Param& aParam, bool aSignal) {
    mDraft = aParam;
    refresh();
    Q_UNUSED(aSignal);
}
void EasingPickWidget::cancel() {
    mDraft = mInitial;
    refresh();
    emit cancelled();
}

// ---- render -----------------------------------------------------------------
void EasingPickWidget::refresh() {
    const bool inBezier = mDraft.type == util::Easing::Type_Custom;
    mAnimT = 0.0; // restart the motion preview on any change
    refreshSeg();
    mNormalPanel->setVisible(!inBezier);
    mBezierPanel->setVisible(inBezier);
    // In Custom mode the top motion-preview bar is hidden (the animated value
    // rides the gauge beside the curve instead), matching the reference mock.
    if (mPipeline) mPipeline->setVisible(!inBezier);
    if (inBezier) renderBezier();
    else refreshTiles();
    updateBlend();
    // Pin the popup's height to the known per-mode height. The popup is a
    // top-level Qt::Popup with a fixed width; its height is deterministic
    // (kNormalHeight / kCustomHeight), so we set it explicitly instead of
    // measuring a racy just-shown geometry() (which oscillated 478/510) or a
    // heightForWidth that returns -1 in Custom. A fixed per-mode height plus
    // reserving the tallest at open means a mode switch only grows/shrinks the
    // popup in place — it never relocates.
    setFixedHeight(inBezier ? kCustomHeight : kNormalHeight);
    // Defer one event so the layout settles, then re-assert the anchored
    // top-left (drift protection). The height itself is already pinned above.
    QTimer::singleShot(0, this, &EasingPickWidget::refit);
}

void EasingPickWidget::refreshSeg() {
    const bool inBezier = mDraft.type == util::Easing::Type_Custom;
    for (int i = 0; i < 4; ++i) {
        const bool on = (i == 3) ? inBezier : (!inBezier && i == (int)mDraft.range);
        mSegBtns[i]->setChecked(on); // QSS drives hover/checked styling
    }
}

void EasingPickWidget::refreshTiles() {
    for (int i = 0; i < mTileBtns.size(); ++i) {
        QToolButton* tile = mTileBtns[i];
        const util::Easing::Type type = kTileOrder[i];
        const bool on = mDraft.type == type;
        tile->setChecked(on);
        if (CurveGlyph* g = tile->findChild<CurveGlyph*>()) {
            // The selected tile's curve is accentBright (purple) on the
            // accent-tinted fill; the rest are muted. Set it explicitly here
            // rather than relying on the QSS `color` reaching the glyph's
            // palette (it does not repaint on check-state change alone).
            g->setGlyphColor(on ? C().accentBright : C().textMutedDim);
            g->setParam(mkParam(type, mDraft.range, mDraft.weight));
        }
    }
}

void EasingPickWidget::updateBlend() {
    mBlendInput->setValue(qRound(mDraft.weight * 100));
    mBlendVal->setText(QString::number(qRound(mDraft.weight * 100)) + "%");
}

void EasingPickWidget::renderBezier() {
    // The editor is authoritative: paintEvent reads the draft bezier, clamps
    // it, and writes the values back to bezier + the spins. Sync the handle
    // points first so a programmatic bezier change (spin/paste/setValue) is
    // not reverted by the paint event's write-back.
    if (mBezierEditor) { mBezierEditor->syncFromBezier(); mBezierEditor->update(); }
}

void EasingPickWidget::resizeEvent(QResizeEvent* aEvent) {
    QFrame::resizeEvent(aEvent);
}
void EasingPickWidget::moveEvent(QMoveEvent* aEvent) {
    QFrame::moveEvent(aEvent);
    // moveEvent() fires on EVERY move of the popup: the open placement, the
    // anchor re-assert on refit, or a nudge by the WM/compositor to keep a
    // growing popup on-screen. The popup size is pinned per-mode and the anchor
    // is re-asserted on every refit, so any relocation here would be a bug; the
    // picker_pos_probe gate asserts the left edge never drifts.
}
bool EasingPickWidget::eventFilter(QObject* aWatched, QEvent* aEvent) {
    if (aEvent->type() == QEvent::MouseButtonPress) {
        // A press on a spinbox lands on the spinbox or its internal line edit;
        // either way it's a click on the field and must keep focus.
        if (qobject_cast<QAbstractSpinBox*>(aWatched))
            return QFrame::eventFilter(aWatched, aEvent);
        if (auto* edit = qobject_cast<QLineEdit*>(aWatched))
            if (qobject_cast<QAbstractSpinBox*>(edit->parentWidget()))
                return QFrame::eventFilter(aWatched, aEvent);

        // Otherwise the click is on a non-text child (curve editor, label,
        // background, button) — release the active spinbox's edit focus. The
        // focused widget of a spinbox is its internal line edit, so walk up to
        // find the spinbox (if any) before clearing.
        if (auto* fw = QApplication::focusWidget()) {
            auto* fspin = qobject_cast<QAbstractSpinBox*>(fw);
            if (!fspin)
                fspin = qobject_cast<QAbstractSpinBox*>(fw->parentWidget());
            if (fspin)
                fw->clearFocus();
        }
    }
    return QFrame::eventFilter(aWatched, aEvent);
}

void EasingPickWidget::showEvent(QShowEvent* aEvent) {
    QFrame::showEvent(aEvent);
    if (mAnimTimer) mAnimTimer->start();
    // On reopen the popup keeps its previous (possibly tall Custom) height
    // while the content returns to the applied (non-Custom) state — re-fit.
    mAnimT = 0.0;
    QTimer::singleShot(0, this, &EasingPickWidget::refit);
}

void EasingPickWidget::hideEvent(QHideEvent* aEvent) {
    QFrame::hideEvent(aEvent);
    if (mAnimTimer) mAnimTimer->stop();
    // The anchor is only meaningful while the popup is shown. Clear it on hide
    // so a reopen's 0-opacity dance doesn't briefly re-assert a stale position
    // (openPopover re-establishes it at the end of the dance).
    mHasAnchor = false;
}

void EasingPickWidget::animStep() {
    mAnimT = std::fmod(mAnimT + 0.007, 1.0);
    if (!mPipeline || !mPipelineFill) return;
    const float v = util::Easing::calculate(mDraft, (float)mAnimT, 0.0f, 1.0f, 1.0f);
    const float f = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    mPipelineFill->setFixedWidth(qRound(f * mPipeline->width()));
    // Drive the Custom-view animation gauge (the ball's Y = the live value).
    if (mBezierGauge) static_cast<PreviewGauge*>(mBezierGauge)->setValue(v);
}

void EasingPickWidget::refit() {
    // The height is pinned by refresh() to the per-mode constant
    // (kNormalHeight / kCustomHeight); we do NOT re-derive it here from
    // heightForWidth, which returns -1 in Custom and is the source of the racy
    // 476/478/510 oscillation. refit only re-asserts the anchored top-left so
    // nothing (a compositor/WM nudge, or a resize) drifts the popup.
    mRefitAttempts = 0;
    if (mHasAnchor && isWindow())
        move(mAnchorPoint);
}

void EasingPickWidget::setAnchorPoint(const QPoint& aPoint) {
    mAnchorPoint = aPoint;
    mHasAnchor = true;
}

int EasingPickWidget::liveCustomHeight() const {
    if (!isVisible()) return -1;
    return geometry().height();
}

bool EasingPickWidget::verifyMaxHeightMatchesLive(QString* aMsg) const {
    if (!isVisible()) {
        if (aMsg) *aMsg = QStringLiteral("not visible");
        return false;
    }
    int hfw = heightForWidth(kPopoverWidth);
    int sh = sizeHint().height();
    int maxH = const_cast<EasingPickWidget*>(this)->maxContentHeight();
    int live = liveCustomHeight();
    QString msg = QStringLiteral("hfw=%1 sh=%2 maxH=%3 live=%4 diff=%5")
        .arg(hfw).arg(sh).arg(maxH).arg(live).arg(live - maxH);
    if (aMsg) *aMsg = msg;
    return qAbs(live - maxH) <= 1;
}

int EasingPickWidget::maxContentHeight() {
    // Cheap hidden estimate for reasoning/tests before the native window
    // exists. The authoritative live height is geometry().height() after show
    // (see liveCustomHeight and the 0-opacity first-frame trick in
    // EasingItem::openPopover). The estimate forces Custom visible, reads
    // sizeHint/heightForWidth, then restores the caller's mode; a hidden
    // popup's heightForWidth can be -1 or stale, so fall back to minimum.
    if (isVisible()) return geometry().height();
    const bool bezier = mDraft.type == util::Easing::Type_Custom;
    if (width() != kPopoverWidth) const_cast<EasingPickWidget*>(this)->setFixedWidth(kPopoverWidth);
    const_cast<EasingPickWidget*>(this)->ensurePolished();
    const_cast<EasingPickWidget*>(this)->mNormalPanel->setVisible(false);
    const_cast<EasingPickWidget*>(this)->mBezierPanel->setVisible(true);
    if (mPipeline) const_cast<EasingPickWidget*>(this)->mPipeline->setVisible(false);
    if (layout()) { const_cast<EasingPickWidget*>(this)->layout()->invalidate(); const_cast<EasingPickWidget*>(this)->layout()->activate(); }
    if (mBody && mBody->layout()) { mBody->layout()->invalidate(); mBody->layout()->activate(); }
    if (mBezierPanel && mBezierPanel->layout()) { mBezierPanel->layout()->invalidate(); mBezierPanel->layout()->activate(); }
    int hCustom = qMax(sizeHint().height(), heightForWidth(kPopoverWidth));
    if (hCustom <= 0) hCustom = minimumSizeHint().height();
    const_cast<EasingPickWidget*>(this)->mNormalPanel->setVisible(!bezier);
    const_cast<EasingPickWidget*>(this)->mBezierPanel->setVisible(bezier);
    if (mPipeline) const_cast<EasingPickWidget*>(this)->mPipeline->setVisible(!bezier);
    if (layout()) { const_cast<EasingPickWidget*>(this)->layout()->invalidate(); const_cast<EasingPickWidget*>(this)->layout()->activate(); }
    return hCustom > 0 ? hCustom : -1;
}

void EasingPickWidget::paintEvent(QPaintEvent* aEvent) {
    // Translucent rounded popup: the window backing is transparent (WA_Translucent-
    // Background), so we must paint the rounded body ourselves. Letting QFrame
    // paint first would fill the square backing. On opaque windows (tests/offscreen
    // before expose) the backing isn't translucent, so also install a rounded
    // window mask — it clips the corner cut reliably on every compositor
    // (including offscreen/platform mismatches that otherwise leave black corners).
    constexpr qreal kRadius = 8.0;
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        // Clear to transparent so the corner cut stays see-through.
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(r, kRadius, kRadius);
        p.fillPath(path, C().base);
        QPen pen(C().raised);
        pen.setWidth(1);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
    // Paint children (header/body/footer/tiles) over the rounded body.
    // Block recursion: QFrame::paintEvent would re-enter this override.
    {
        QPaintEvent ev(aEvent->rect());
        QFrame::paintEvent(&ev);
    }
    // Rounded window mask — cheap and compositor-independent. On Windows the
    // translucent backing would otherwise show the square corner cut as black;
    // the mask clips it reliably on every compositor (including offscreen /
    // platform mismatches). A single cached mask, refreshed on size change.
    static QSize sMaskSize;
    if (sMaskSize != size()) {
        sMaskSize = size();
        QBitmap mask(size());
        mask.fill(Qt::color0);
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::Antialiasing, true);
        mp.setBrush(Qt::color1);
        mp.setPen(Qt::NoPen);
        mp.drawRoundedRect(QRect(QPoint(0, 0), size()), kRadius, kRadius);
        setMask(mask);
    }
}

} // namespace prop
} // namespace gui
