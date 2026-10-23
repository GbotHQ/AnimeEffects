#include "ctrl/time/time_Renderer.h"
#include "ctrl/TimeLineEditor.h"
#include "ctrl/time/time_BodyModel.h"
#include "gui/theme/Colors.h"

#include <cmath>

using namespace core;

namespace ctrl {
namespace time {

    static QColor withAlpha(QColor aColor, int aAlpha) { aColor.setAlpha(aAlpha); return aColor; }

    // The focused key's fill: the channel hue mixed 0.45 of the way to the
    // TEXT colour (the user's ruling for both views; the mock mixes toward
    // its --key-highlight, and the text colour is the app's equivalent
    // readable ink in either theme). Per channel:
    // round(hue + (text - hue) * 0.45), alpha carried through unchanged. The
    // size never changes.
    constexpr double kSelectFillMix = 0.45;
    static QColor mixTowardText(const QColor& aColor) {
        const QColor text = theme::Colors::current().text;
        const auto mix = [](int aChannel, int aTarget) {
            return static_cast<int>(std::lround(aChannel + (aTarget - aChannel) * kSelectFillMix));
        };
        return QColor(
            mix(aColor.red(), text.red()),
            mix(aColor.green(), text.green()),
            mix(aColor.blue(), text.blue()),
            aColor.alpha()
        );
    }

    // The lane label's ink: the channel's hue at 0.42 (design s1 — "the ink
    // becomes the channel's hue at 0.42", one step quieter than the keys). The
    // value is the mock's own (hexA(hue, 0.42) in its band-name pass).
    constexpr int kLabelAlpha = 107; // 0.42
    // The selected key's ring is the shared token in time_Renderer.h
    // (kSelectRingAlpha / kSelectRingOffset): the key's OWN channel hue at 0.85,
    // 1px, drawn OUTSIDE the marker at keyRadius + 2.6. The graph panel's marker
    // pass reads the same two cells, so the two rings cannot diverge.
    constexpr double kSelectRingRadius = keyRadius + kSelectRingOffset;

    // The channel hue of a key type: THE CHANNEL HUE TABLE in theme::Colors
    // (design section 7). A timeline key belongs to the TYPE, not to one
    // component, so it takes the type's FIRST component's hue - which is the
    // X axis colour for Move, Scale, Image and Blur, and the type's own hue for
    // a type with no axis (Opa amber, Rotate purple, ...). One lookup for the
    // keys, their lane's label and the graph panel's curves, so the two views
    // can never disagree about a channel's colour.
    static QColor keyColor(TimeKeyType aType) {
        return theme::channelColor(theme::Colors::current(), aType, 0);
    }

    Renderer::Renderer(QPainter& aPainter, const CameraInfo& aCamera, const theme::TimeLine& aTheme):
        mPainter(aPainter), mCamera(aCamera), mTheme(aTheme), mMargin(), mRange(), mScale() {}

    void Renderer::renderLines(const QVector<TimeLineRow>& aRows, const QRect& aCameraRect, const QRect& aCullRect) {
        // THE LANE PASS IS A MODEL NOW (gui/TimeBodyModel.h): the geometry and ink are
        // stated as data, so the pass can be re-hosted (the timeline is migrating to
        // Canvas Painter - see AGENTS.md). This renderer paints that model through the
        // shared QPainter rendering, which is byte-for-byte the pass it drew before.
        Inks inks;
        inks.body = mTheme.trackColor();
        inks.bodySelected = mTheme.trackSelectColor();
        inks.edge = mTheme.trackEdgeColor();
        inks.separator = mTheme.trackSeperatorColor();
        paintLines(mPainter, buildLines(aRows, aCameraRect, aCullRect, inks));
    }

    void Renderer::renderRangeMarkers(const QRect& aCameraRect) {
        // A MODEL NOW (ctrl/time/time_BodyModel.h): the two guides are stated as data and
        // painted through the shared QPainter rendering.
        paintGuides(
            mPainter, buildRangeMarkers(aCameraRect, mProjectRange, mPlayWindow.active(), *mScale, mMargin),
            SelectionBox()
        );
    }

    void Renderer::renderKeys(const QVector<TimeLineRow>& aRows, const QRect& aCullRect) {
        mPainter.setRenderHint(QPainter::Antialiasing);
        for (const TimeLineRow& row : aRows) {
            const QRect rect = row.rect;
            // same vertical-band culling as the background pass above
            if (rect.bottom() < aCullRect.top() || aCullRect.bottom() < rect.top())
                continue;
            if (!row.node || !row.node->timeLine())
                continue;

            const QPoint cpos(mMargin, rect.bottom());
            // THE KEY PASS IS A MODEL NOW (ctrl/time/time_BodyModel.h): the marks are stated as
            // data and painted through the shared QPainter rendering, in the same order the pass
            // always used (a closed folder's summary first).
            if (row.closedFolder)
                paintKeys(mPainter, buildChildKeys(row.node, cpos, *mScale, mRange));
            paintKeys(mPainter, buildKeys(row.node, row, *mScale, mRange, mMargin));
        }
    }


    void Renderer::renderSelectionRange(const QRect& aRect) {
        paintGuides(mPainter, {}, buildMarquee(aRect));
    }

} // namespace time
} // namespace ctrl
