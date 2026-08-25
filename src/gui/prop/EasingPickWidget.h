#ifndef GUI_PROP_EASINGPICKWIDGET_H
#define GUI_PROP_EASINGPICKWIDGET_H

#include <QFrame>
#include <QObject>
#include <QWidget>
#include <QToolButton>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QVector>
#include <QColor>
#include <QTimer>
#include "util/Easing.h"

namespace gui { class AppStyle; } // fwd (slider proxy style)

class QGridLayout;
class QHBoxLayout;
class QVBoxLayout;
class FlowLayout;
class QDoubleSpinBox;
class BezierCurveEditor; // global ns (see bezierCurveEditor.h)

namespace gui {
namespace prop {

// ---------------------------------------------------------------------------
// CurveGlyph — paints an easing curve exactly the way the reference mock's
// SVG does. The reference draws the curve into a virtual 100x100 viewBox
// (curveD in mock.js) with preserveAspectRatio="none" and non-scaling
// stroke; this widget reproduces that by computing the path in the same
// 100x100 space and scaling the coordinates to the widget size, so the
// shape matches the mock pixel-for-pixel.
// ---------------------------------------------------------------------------
class CurveGlyph : public QWidget {
    Q_OBJECT
public:
    explicit CurveGlyph(QWidget* aParent = nullptr);
    void setParam(const util::Easing::Param& aParam);
    void setGlyphColor(const QColor& aColor);
    void setStrokeWidth(double aWidth); // default 1.5

protected:
    void paintEvent(QPaintEvent*) override;

private:
    util::Easing::Param mParam;
    QColor mColor;
    double mStrokeWidth = 1.5;
};

// ---------------------------------------------------------------------------
// EasingPickWidget — the locked "shape-first" easing picker (variant A).
// A self-contained 300px popover: a direction segmented control, a motion
// pipeline bar, the curve-tile catalog, a curve<->linear blend slider, and a
// Cancel/Apply footer. Custom mode swaps the tile catalog for a bezier edit
// panel. Mirrors the reference DOM so the layout can be verified by geometry
// (each sub-widget carries a stable objectName).
//
// Signals mirror EasingPicker's onApply/onCancel. The widget owns its
// sub-widgets; the geometry harness names each part with the mock's selector
// (e.g. "seg", "seg-r-0", "tile-0", "trow", "pipeline", "blend-wrap", "foot").
// ---------------------------------------------------------------------------
class EasingPickWidget : public QFrame {
    Q_OBJECT
public:
    // Known content-heights (logical px) for the two layout modes. The popup
    // is a fixed width (300) and its height is deterministic per mode, so we
    // pin it rather than measuring a racy just-shown geometry(). The tallest
    // (Custom) is what EasingItem reserves so a mode switch never relocates the
    // popup. Logical px scale with DPI automatically (Qt Widgets has no rem).
    static constexpr int kNormalHeight = 250;  // In/Out/In-Out (and Linear/Cubic/etc.)
    static constexpr int kCustomHeight = 490;  // Custom (even, so it stays divisible by 2)

    explicit EasingPickWidget(const util::Easing::Param& aInitial, QWidget* aParent = nullptr);
    ~EasingPickWidget() override;

    util::Easing::Param value() const { return mDraft; }
    void setValue(const util::Easing::Param& aParam, bool aSignal = true);

    // Segment / tile interactions (public so a harness can drive them).
    void setType(util::Easing::Type aType);
    void setRange(util::Easing::Range aRange);
    void setWeight(float aWeight);
    void setCustom();
    void cancel();

    // Hide every label/button/spinbox glyph by painting its text fully
    // transparent (the box geometry and all graphics stay identical), so a
    // harness can screenshot the picker's pure layout. Off by default.
    void setHideText(bool aHide);
    bool hideText() const { return mHideText; }

    int maxContentHeight();
    int liveCustomHeight() const;
    bool verifyMaxHeightMatchesLive(QString* aMsg = nullptr) const;

    // Record the global top-left this popup should stay at. The picker is a
    // Qt::Popup (top-level, no layout), so on a mode switch it re-fits its
    // height in-place — and some platforms/compositors nudge a growing popup
    // to keep it on-screen. Re-asserting the anchor after every refit makes
    // the position deterministic and immune to that nudge.
    void setAnchorPoint(const QPoint& aPoint);
    QPoint anchorPoint() const { return mAnchorPoint; }

signals:
    void applied(util::Easing::Param aParam);
    void cancelled();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void moveEvent(QMoveEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    bool eventFilter(QObject* aWatched, QEvent* aEvent) override;

private:
    QWidget* buildHeader();
    QWidget* buildBody();
    QWidget* buildSeg(util::Easing::Range aRange);
    QWidget* buildPipeline();
    QWidget* buildTrow();
    QWidget* buildBlend();
    QWidget* buildFooter();
    QWidget* buildBezierPanel();

    void refresh();
    void refreshTiles();
    void refreshSeg();
    void updateBlend();
    void renderBezier();
    void animStep();
    void refit();
    void onBezierSpin(int aIndex, double aValue);
    void applyAppearance();

    QToolButton* makeTileAt(int aIndex, util::Easing::Type aType);

    util::Easing::Param mDraft;
    util::Easing::Param mInitial;
    bool mHideText = false;
    QPoint mAnchorPoint; // global top-left the popup is anchored to
    bool mHasAnchor = false;

    QLabel* mTitle = nullptr;
    QWidget* mHead = nullptr; // header chrome (fixed kHeaderH)
    QWidget* mFoot = nullptr; // footer chrome (fixed kFooterH)

    // seg
    QGridLayout* mSegLayout = nullptr;
    QVector<QToolButton*> mSegBtns; // [In, Out, InOut, Custom]

    // pipeline
    QWidget* mPipeline = nullptr;
    QWidget* mPipelineFill = nullptr;
    QTimer* mAnimTimer = nullptr;
    double mAnimT = 0.0;

    // trow
    QWidget* mTrow = nullptr;
    QVector<QToolButton*> mTileBtns; // in display order

    // blend
    QSlider* mBlendInput = nullptr;
    AppStyle* mSliderStyle = nullptr; // proxy that draws the blend slider
    QLabel* mBlendVal = nullptr;
    CurveGlyph* mBlendLinear = nullptr;
    CurveGlyph* mBlendCurve = nullptr;

    // body / bezier
    QWidget* mBody = nullptr;
    QWidget* mNormalPanel = nullptr;   // seg hidden? no: seg is shared; normal = trow+blend
    QWidget* mBezierPanel = nullptr;
    BezierCurveEditor* mBezierEditor = nullptr;
    QWidget* mBezierGauge = nullptr; // PreviewGauge: animation preview beside the graph
    QVector<QDoubleSpinBox*> mBezierSpins;
    float mBezierProgress = 0.0f;
    int mRefitAttempts = 0; // bounded retry for the popup height re-fit

    // footer
    QPushButton* mBtnApply = nullptr;
    QPushButton* mBtnCancel = nullptr;
};

} // namespace prop
} // namespace gui

#endif // GUI_PROP_EASINGPICKWIDGET_H
