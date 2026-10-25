// Gate for the shared brush model (ctrl/Brush.h): the falloff every target
// deposits with, and the stroke sampler that turns cursor samples into spaced
// stamps with interpolated pressure. Headless: no widgets, no GL, no project.
//
// Why this is a gate rather than a detail of one tool: every painting context
// (raster masks, the FFD mesh warp, bone influence / weight painting, pose
// erasing) reads these two functions, so a change here changes all of them - and
// the defect that motivated the sampler is invisible in a single-stamp test
// (stamping once per drag event left fast strokes as dots).
#include <QVector2D>
#include <cmath>
#include <cstdio>
#include <vector>

// Nothing here touches XCAssert, but Brush.h pulls util/NonCopyable only; the
// test needs no assert sink.
#include "ctrl/Brush.h"

namespace {

using ctrl::brushFalloff;

int failures = 0;
void check(bool aOk, const char* aWhat) {
    if (!aOk) {
        std::printf("FAIL  %s\n", aWhat);
        ++failures;
    }
}
bool nearly(float a, float b, float aEpsilon = 1.0e-5f) { return std::fabs(a - b) <= aEpsilon; }

ctrl::BrushParam makeParam(float aRadius, float aHardness, float aSpacing) {
    ctrl::BrushParam param;
    param.radius = aRadius;
    param.hardness = aHardness;
    param.spacing = aSpacing;
    param.flow = 1.0f;
    param.opacity = 1.0f;
    param.pressureSize = false;
    param.pressureFlow = true;
    return param;
}

struct Collected {
    std::vector<ctrl::BrushStamp> stamps;
    void operator()(const ctrl::BrushStamp& aStamp) { stamps.push_back(aStamp); }
};

} // namespace

int main() {
    // ------------------------------------------------------------- [1] falloff
    {
        // hardness 1: a hard disc - full strength everywhere inside, nothing at the rim
        check(brushFalloff(0.0f, 10.0f, 1.0f) == 1.0f, "hardness 1 is full strength at the centre");
        check(brushFalloff(9.999f, 10.0f, 1.0f) == 1.0f, "hardness 1 stays full strength to the rim");
        check(brushFalloff(10.0f, 10.0f, 1.0f) == 0.0f, "the rim itself weighs nothing");
        check(brushFalloff(12.0f, 10.0f, 1.0f) == 0.0f, "outside the radius weighs nothing");

        // hardness 0: a fully feathered disc - smoothstep, 1 at the centre, 0.5 at the
        // half radius (smoothstep(0.5) == 0.5), 0 at the rim
        check(brushFalloff(0.0f, 10.0f, 0.0f) == 1.0f, "hardness 0 is full strength at the centre");
        check(nearly(brushFalloff(5.0f, 10.0f, 0.0f), 0.5f), "hardness 0 is half strength at half the radius");
        check(brushFalloff(10.0f, 10.0f, 0.0f) == 0.0f, "hardness 0 ends at the rim");

        // hardness 0.5: a plateau to the half radius, then the band
        check(brushFalloff(4.0f, 10.0f, 0.5f) == 1.0f, "the core is at full strength for hardness 0.5");
        check(nearly(brushFalloff(7.5f, 10.0f, 0.5f), 0.5f), "the band's midpoint is half strength");
        check(brushFalloff(10.0f, 10.0f, 0.5f) == 0.0f, "the band ends at the rim");

        // monotone and inside [0, 1] over the whole disc
        float previous = 1.0f;
        bool monotone = true;
        bool bounded = true;
        for (int i = 0; i <= 200; ++i) {
            const float d = 0.05f * float(i);
            const float w = brushFalloff(d, 10.0f, 0.4f);
            if (w > previous + 1.0e-6f)
                monotone = false;
            if (w < 0.0f || w > 1.0f)
                bounded = false;
            previous = w;
        }
        check(monotone, "the falloff never rises with distance");
        check(bounded, "the falloff stays inside [0, 1]");

        // a degenerate brush deposits nothing rather than dividing by zero
        check(brushFalloff(0.0f, 0.0f, 0.5f) == 0.0f, "a zero radius weighs nothing");
        check(brushFalloff(0.0f, 10.0f, 2.0f) == 1.0f, "a hardness above 1 is clamped, not undefined");

        if (failures == 0)
            std::printf("[1] the shared falloff (plateau + smoothstep)\n");
    }

    // -------------------------------------------------------------- [2] spacing
    {
        ctrl::BrushStroke stroke;
        Collected collected;
        const float radius = 10.0f;
        const float step = radius * 0.25f; // 2.5 px
        stroke.begin(makeParam(radius, 0.5f, 0.25f), QVector2D(0.0f, 0.0f), 1.0f, [&](const ctrl::BrushStamp& s) {
            collected(s);
        });
        check(collected.stamps.size() == 1, "the press point stamps immediately");
        stroke.move(QVector2D(100.0f, 0.0f), 1.0f);

        check(collected.stamps.size() >= 40, "a 100px move at a 10px radius and 0.25 spacing stamps every 2.5px");
        float maxGap = 0.0f;
        float maxOffAxis = 0.0f;
        for (size_t i = 1; i < collected.stamps.size(); ++i) {
            const QVector2D delta = collected.stamps[i].center - collected.stamps[i - 1].center;
            maxGap = std::max(maxGap, delta.length());
            maxOffAxis = std::max(maxOffAxis, std::fabs(collected.stamps[i].center.y()));
        }
        std::printf("MEASURED spacing stamps=%d maxGap=%.3f (step %.3f)\n", int(collected.stamps.size()), maxGap, step);
        check(maxGap <= step + 1.0e-3f, "no gap between stamps is larger than the step");
        check(maxOffAxis <= 1.0e-4f, "the stamps stay on the segment");
        check(nearly(collected.stamps.back().center.x(), 100.0f, step), "the last stamp lands at the end of the move");

        // consecutive moves must continue the same spacing across the event boundary
        const size_t before = collected.stamps.size();
        stroke.move(QVector2D(200.0f, 0.0f), 1.0f);
        check(collected.stamps.size() > before, "a second move keeps stamping");
        const float crossGap =
            (collected.stamps[before].center - collected.stamps[before - 1].center).length();
        check(crossGap <= step + 1.0e-3f, "the spacing carries across drag events (no gap, no double stamp)");

        stroke.end();
        const size_t atEnd = collected.stamps.size();
        stroke.move(QVector2D(300.0f, 0.0f), 1.0f);
        check(collected.stamps.size() == atEnd, "an ended stroke emits nothing");
        check(!stroke.isActive(), "end() deactivates the stroke");

        if (failures == 0)
            std::printf("[2] stroke spacing, continuous across events\n");
    }

    // ------------------------------------------------------------- [3] pressure
    {
        ctrl::BrushStroke stroke;
        Collected collected;
        stroke.begin(makeParam(20.0f, 0.5f, 0.5f), QVector2D(0.0f, 0.0f), 0.2f, [&](const ctrl::BrushStamp& s) {
            collected(s);
        });
        stroke.move(QVector2D(60.0f, 0.0f), 1.0f);

        bool rising = true;
        bool bounded = true;
        for (size_t i = 1; i < collected.stamps.size(); ++i) {
            if (collected.stamps[i].amount < collected.stamps[i - 1].amount - 1.0e-6f)
                rising = false;
            if (collected.stamps[i].amount < 0.0f || collected.stamps[i].amount > 1.0f)
                bounded = false;
        }
        std::printf(
            "MEASURED pressure stamps=%d amount %.3f -> %.3f\n", int(collected.stamps.size()),
            collected.stamps.empty() ? -1.0f : collected.stamps.front().amount,
            collected.stamps.empty() ? -1.0f : collected.stamps.back().amount
        );
        check(!collected.stamps.empty(), "the pressure stroke stamped");
        check(nearly(collected.stamps.front().amount, 0.2f), "the first stamp uses the press pressure");
        check(rising, "a pressure ramp raises the amount monotonically");
        check(bounded, "the amount stays inside [0, 1]");
        check(nearly(collected.stamps.back().amount, 1.0f, 0.02f), "the last stamp reaches the pen's pressure");

        // pressure drives SIZE only when asked to
        ctrl::BrushParam sized = makeParam(20.0f, 0.5f, 0.5f);
        sized.pressureSize = true;
        ctrl::BrushStroke sizedStroke;
        Collected sizedStamps;
        sizedStroke.begin(sized, QVector2D(0.0f, 0.0f), 0.25f, [&](const ctrl::BrushStamp& s) { sizedStamps(s); });
        check(nearly(sizedStamps.stamps.front().radius, 5.0f), "pressureSize scales the radius with pressure");
        check(nearly(sizedStamps.stamps.front().amount, 0.25f), "pressureFlow is independent of pressureSize");

        ctrl::BrushStroke fixedStroke;
        Collected fixed;
        ctrl::BrushParam fixedParam = makeParam(20.0f, 0.5f, 0.5f);
        fixedParam.pressureFlow = false;
        fixedStroke.begin(fixedParam, QVector2D(0.0f, 0.0f), 0.25f, [&](const ctrl::BrushStamp& s) { fixed(s); });
        check(nearly(fixed.stamps.front().amount, 1.0f), "pressureFlow off makes the amount ignore pressure");
        check(nearly(fixed.stamps.front().radius, 20.0f), "pressureSize off keeps the radius");

        if (failures == 0)
            std::printf("[3] pressure ramps and the two pressure switches\n");
    }

    // --------------------------------------------------------------- [4] edges
    {
        ctrl::BrushStroke stroke;
        Collected collected;
        stroke.begin(makeParam(10.0f, 0.5f, 0.25f), QVector2D(5.0f, 5.0f), 1.0f, [&](const ctrl::BrushStamp& s) {
            collected(s);
        });
        stroke.move(QVector2D(5.0f, 5.0f), 0.5f); // no travel
        check(collected.stamps.size() == 1, "a move with no travel stamps nothing");
        check(nearly(collected.stamps.front().amount, 1.0f), "the press stamp keeps its own pressure");

        // the step is floored, so a hairline brush cannot stamp thousands of times per pixel
        ctrl::BrushParam hairline = makeParam(0.1f, 0.5f, 0.125f);
        check(nearly(hairline.stampStep(), 0.5f), "the stamp step is floored at half a pixel");

        // erase is carried through both channels
        ctrl::BrushParam erase = makeParam(10.0f, 0.5f, 0.25f);
        erase.erase = true;
        ctrl::BrushStroke eraseStroke;
        Collected erasing;
        eraseStroke.begin(erase, QVector2D(0.0f, 0.0f), 1.0f, [&](const ctrl::BrushStamp& s) { erasing(s); });
        check(erasing.stamps.size() == 1 && erasing.stamps.front().erase, "the erase flag reaches the target");
        check(nearly(erasing.stamps.front().hardness, 0.5f), "the stamp carries the hardness");

        if (failures == 0)
            std::printf("[4] zero-travel, the step floor and the erase flag\n");
    }

    if (failures == 0)
        std::printf("brush_stroke: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
