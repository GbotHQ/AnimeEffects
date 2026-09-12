// Mipmap verification probe.
//
// WHAT THIS VERIFIES
//
// AnimeEffects samples layer artwork through a mipmapped texture (core/ImageKey), so that
// minified layers are filtered from a prefiltered chain instead of aliasing on level 0.
// Mipmap generation is only correct for ALPHA-PREMULTIPLIED data: glGenerateMipmap
// box-filters every channel independently, so averaging STRAIGHT color mixes the RGB of
// transparent texels into the edge texels of every level. Artwork routinely stores black
// (or arbitrary) RGB under alpha 0, so that average darkens the silhouette - the classic
// mipmap dark halo. Light halos are the same bug with a bright RGB under alpha 0.
//
// WHAT IS THE GROUND TRUTH
//
// There is no third-party author to diff against here (Krita/GIMP do not do GL mip
// selection), so ground truth is ANALYTIC: the correct content of mip level N of a known
// input is the box average of that input, computed independently on the CPU, once for
// premultiplied color (correct) and once for straight color (the naive implementation we
// must not ship). Each pattern below is chosen so those two references are far apart, and
// the composited consequence over an opaque backdrop is known in closed form.
//
// THE TEST MUST BE ABLE TO FAIL
//
// A mipmapping test that only asserts "the GPU agrees with the premultiplied reference"
// passes just as happily if mipmaps are never generated at all, or if the driver ignores
// the request. So every case also:
//   - asserts the two references genuinely DIFFER (the discriminant is real, not vacuous),
//   - asserts the GPU chain matches the PREMULTIPLIED reference and NOT the straight one,
//   - asserts the mip chain actually has more than one level.
// The straight-alpha chain is exercised too, as a positive control that the harness
// detects the halo it is supposed to prevent.
//
// Suites:
//   M1  mip chain generation vs the analytic CPU reference (production gl::Texture)
//   M2  the production core::ImageKey texture (premultiply + mipmap + filter parameters)
//
// Run: tools/verify_mipmap/build_and_run.sh

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include <QApplication>
#include <QMainWindow>
#include <QOpenGLVersionFunctionsFactory>
#include <QOpenGLWidget>
#include <QScopedPointer>
#include <QSize>
#include <QString>
#include <QSurfaceFormat>

#include "XC.h"
#include "core/ImageKey.h"
#include "gl/DeviceInfo.h"
#include "gl/Global.h"
#include "gl/Texture.h"
#include "gl/VertexArrayObject.h"
#include "gui/AudioPlaybackWidget.h"
#include "img/ResourceData.h"
#include "img/ResourceNode.h"

//-------------------------------------------------------------------------------------------------
// reporting
namespace {

int gChecks = 0;
int gFails = 0;

bool expect(bool aCond, const QString& aWhat) {
    ++gChecks;
    if (!aCond) {
        ++gFails;
        std::printf("    [FAIL] %s\n", aWhat.toUtf8().constData());
    }
    return aCond;
}

void suiteHeader(const char* aName) { std::printf("== %s ==\n", aName); }

void caseReport(const QString& aName, bool aOk, const QString& aDetails) {
    std::printf(
        "  [%s] %-52s %s\n", aOk ? "PASS" : "FAIL", aName.toUtf8().constData(), aDetails.toUtf8().constData()
    );
}

bool near(double aValue, double aExpected, double aTol) { return std::fabs(aValue - aExpected) <= aTol; }

//-------------------------------------------------------------------------------------------------
// images (RGBA8, straight alpha, as the loaders deliver them)

struct Img {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> d;

    uint8_t* at(int x, int y) { return d.data() + ((size_t)y * w + x) * 4; }
    const uint8_t* at(int x, int y) const { return d.data() + ((size_t)y * w + x) * 4; }
    QSize size() const { return QSize(w, h); }
};

// An input whose transparent region carries a chosen RGB: that value is what a straight
// chain averages into the silhouette, and what a premultiplied chain multiplies away.
struct Pattern {
    const char* name;
    uint8_t inside[3];  // RGB where alpha is opaque
    uint8_t outside[3]; // RGB under alpha 0 (the halo pigment)
};

const Pattern kPatterns[] = {
    {"white on transparent black (dark halo risk)", {255, 255, 255}, {0, 0, 0}},
    {"black on transparent white (light halo risk)", {0, 0, 0}, {255, 255, 255}},
};

// A checkerboard: every 2x2 block is exactly half covered, so level 1 is 50% coverage
// everywhere and the analytic value is exact rather than a specific-pixel accident.
Img makeChecker(const Pattern& aPat, int aSize) {
    Img img;
    img.w = img.h = aSize;
    img.d.assign((size_t)aSize * aSize * 4, 0);
    for (int y = 0; y < aSize; ++y) {
        for (int x = 0; x < aSize; ++x) {
            uint8_t* p = img.at(x, y);
            const bool on = ((x + y) % 2) == 0;
            const uint8_t* c = on ? aPat.inside : aPat.outside;
            p[0] = c[0];
            p[1] = c[1];
            p[2] = c[2];
            p[3] = on ? 255 : 0;
        }
    }
    return img;
}

// Opaque square inside, transparent surround: the realistic silhouette case.
Img makeSquare(const Pattern& aPat, int aSize, int aInset) {
    Img img;
    img.w = img.h = aSize;
    img.d.assign((size_t)aSize * aSize * 4, 0);
    for (int y = 0; y < aSize; ++y) {
        for (int x = 0; x < aSize; ++x) {
            uint8_t* p = img.at(x, y);
            const bool on = x >= aInset && x < aSize - aInset && y >= aInset && y < aSize - aInset;
            const uint8_t* c = on ? aPat.inside : aPat.outside;
            p[0] = c[0];
            p[1] = c[1];
            p[2] = c[2];
            p[3] = on ? 255 : 0;
        }
    }
    return img;
}

//-------------------------------------------------------------------------------------------------
// the two independent CPU references

// What glGenerateMipmap does to STRAIGHT data: each channel averaged on its own.
Img downsampleStraight(const Img& aSrc) {
    Img dst;
    dst.w = std::max(1, aSrc.w / 2);
    dst.h = std::max(1, aSrc.h / 2);
    dst.d.assign((size_t)dst.w * dst.h * 4, 0);
    for (int y = 0; y < dst.h; ++y) {
        for (int x = 0; x < dst.w; ++x) {
            for (int c = 0; c < 4; ++c) {
                int sum = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        sum += aSrc.at(std::min(2 * x + dx, aSrc.w - 1), std::min(2 * y + dy, aSrc.h - 1))[c];
                dst.at(x, y)[c] = (uint8_t)((sum + 2) / 4); // round to nearest
            }
        }
    }
    return dst;
}

// Correct filtering of straight input: premultiply, average. Output is PREMULTIPLIED data,
// which is exactly what the shipped texture holds and what the shader's PREMULTIPLIED_INPUT
// branch expects, so it is directly comparable to a GPU readback of the premultiplied chain.
Img downsamplePremultiplied(const Img& aSrc) {
    Img dst;
    dst.w = std::max(1, aSrc.w / 2);
    dst.h = std::max(1, aSrc.h / 2);
    dst.d.assign((size_t)dst.w * dst.h * 4, 0);
    for (int y = 0; y < dst.h; ++y) {
        for (int x = 0; x < dst.w; ++x) {
            double pr = 0, pg = 0, pb = 0, pa = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* s = aSrc.at(std::min(2 * x + dx, aSrc.w - 1), std::min(2 * y + dy, aSrc.h - 1));
                    const double a = s[3] / 255.0;
                    pr += s[0] * a;
                    pg += s[1] * a;
                    pb += s[2] * a;
                    pa += s[3];
                }
            }
            const auto clamp8 = [](double v) { return (uint8_t)std::lround(std::max(0.0, std::min(255.0, v))); };
            uint8_t* o = dst.at(x, y);
            o[0] = clamp8(pr / 4);
            o[1] = clamp8(pg / 4);
            o[2] = clamp8(pb / 4);
            o[3] = clamp8(pa / 4);
        }
    }
    return dst;
}

// The composited consequence over an opaque black backdrop, for a texture that holds
// PREMULTIPLIED data: the shader divides the coverage out (PREMULTIPLIED_INPUT) and the
// straight-alpha blend multiplies it back in, so the visible value is simply the stored
// premultiplied RGB. A correct chain therefore shows the source color scaled by coverage.
double overBlackPremultiplied(const uint8_t* aPx) { return aPx[0]; }

// ...and for a texture that holds STRAIGHT data (the naive pipeline): the blend multiplies
// the already-averaged color by the averaged coverage a second time.
double overBlackStraight(const uint8_t* aPx) { return aPx[0] * (aPx[3] / 255.0); }

//-------------------------------------------------------------------------------------------------
// GPU readback of a mip level

std::vector<uint8_t> readLevel(GLuint aTex, int aLevel, const QSize& aSize) {
    std::vector<uint8_t> buf((size_t)aSize.width() * aSize.height() * 4, 0);
    auto& ggl = gl::Global::functions();
    ggl.glBindTexture(GL_TEXTURE_2D, aTex);
    ggl.glGetTexImage(GL_TEXTURE_2D, aLevel, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    ggl.glBindTexture(GL_TEXTURE_2D, 0);
    return buf;
}

// Reads the whole chain the GPU built, as one Img per level.
std::vector<Img> readChain(const Img& aSrc, bool aPremultiply) {
    Img upload = aSrc;
    if (aPremultiply) {
        for (size_t i = 0; i < upload.d.size(); i += 4) {
            const uint32_t a = upload.d[i + 3];
            if (a == 255)
                continue;
            upload.d[i + 0] = (uint8_t)((upload.d[i + 0] * a + 127) / 255);
            upload.d[i + 1] = (uint8_t)((upload.d[i + 1] * a + 127) / 255);
            upload.d[i + 2] = (uint8_t)((upload.d[i + 2] * a + 127) / 255);
        }
    }

    gl::Texture tex;
    tex.create(
        upload.size(), upload.d.data(), GL_RGBA, GL_RGBA8, GL_UNSIGNED_BYTE, gl::Texture::GenerateMipmap
    );

    std::vector<Img> levels;
    for (int l = 0; l < tex.mipmapLevels(); ++l) {
        Img img;
        img.w = std::max(1, upload.w >> l);
        img.h = std::max(1, upload.h >> l);
        img.d = readLevel(tex.id(), l, QSize(img.w, img.h));
        levels.push_back(std::move(img));
    }
    return levels;
}

// max |a - b| over one channel, in 8-bit units
int maxChannelDelta(const std::vector<uint8_t>& aA, const std::vector<uint8_t>& aB, int aChannel) {
    int worst = 0;
    for (size_t i = aChannel; i < aA.size() && i < aB.size(); i += 4)
        worst = std::max(worst, std::abs((int)aA[i] - (int)aB[i]));
    return worst;
}

// ...and over every channel. Needed for "is this level genuinely different from that one":
// for a premultiplied chain the discriminating channel can be ALPHA alone (a bright shape on
// a bright transparent region premultiplies to rgb 0 at both levels, so the color channels
// agree while coverage halves).
int maxAnyChannelDelta(const std::vector<uint8_t>& aA, const std::vector<uint8_t>& aB) {
    int worst = 0;
    for (int c = 0; c < 4; ++c)
        worst = std::max(worst, maxChannelDelta(aA, aB, c));
    return worst;
}

//-------------------------------------------------------------------------------------------------
// M1: is the generated chain the premultiplied one (and not the straight one)?

void verifyPattern(const Pattern& aPat, int aSize, bool aChecker) {
    // The inset is deliberately ODD: with even-aligned edges every 2x2 block of level 1
    // falls entirely inside or entirely outside the shape, so level 1 would carry no
    // partial coverage and the case could not distinguish the two pipelines at all.
    const Img src = aChecker ? makeChecker(aPat, aSize) : makeSquare(aPat, aSize, aSize / 4 + 1);

    // Independent CPU references for level 1, each in its own representation:
    //   refStraight - what averaging STRAIGHT data yields, held as straight data
    //   refPremult  - what averaging PREMULTIPLIED data yields, held premultiplied
    const Img refStraight = downsampleStraight(src);
    const Img refPremult = downsamplePremultiplied(src);

    // The invariant the halo violates: over an opaque black backdrop, a correct chain shows
    // the source's own opaque color scaled by coverage - never the color of the transparent
    // region. For a chain that keeps straight data, the same source shows
    // rgb*a, i.e. the silhouette color is dragged toward the transparent region's color.
    const double inside = aPat.inside[0];
    const auto expectedVisible = [&](const uint8_t* aPx) { return inside * (aPx[3] / 255.0); };

    // The two pipelines must genuinely disagree, or the case proves nothing.
    double maxDiscriminant = 0.0;
    for (int i = 0; i < (int)refStraight.d.size(); i += 4) {
        const double correct = overBlackPremultiplied(refPremult.d.data() + i);
        const double naive = overBlackStraight(refStraight.d.data() + i);
        maxDiscriminant = std::max(maxDiscriminant, std::fabs(correct - naive));
    }

    // alpha is coverage and must be identical whichever way color is carried
    const int alphaDelta = maxChannelDelta(refStraight.d, refPremult.d, 3);

    const std::vector<Img> gpuPremult = readChain(src, true);
    const std::vector<Img> gpuStraight = readChain(src, false);
    const bool haveChain = gpuPremult.size() > 1 && gpuStraight.size() > 1;

    int premultRawDelta = 999;
    int straightRawDelta = 999;
    double worstPremultHalo = 0.0;
    double worstStraightHalo = 0.0;
    if (haveChain) {
        premultRawDelta = maxChannelDelta(gpuPremult[1].d, refPremult.d, 0);
        straightRawDelta = maxChannelDelta(gpuStraight[1].d, refStraight.d, 0);
        for (size_t i = 0; i < gpuPremult[1].d.size(); i += 4) {
            const uint8_t* pp = gpuPremult[1].d.data() + i;
            const uint8_t* sp = gpuStraight[1].d.data() + i;
            worstPremultHalo = std::max(worstPremultHalo, std::fabs(overBlackPremultiplied(pp) - expectedVisible(pp)));
            worstStraightHalo = std::max(worstStraightHalo, std::fabs(overBlackStraight(sp) - expectedVisible(sp)));
        }
    } else {
        // no chain: make the halo assertions fail rather than pass vacuously
        worstPremultHalo = 999.0;
    }

    const QString prefix = QString("%1 [%2]").arg(aPat.name).arg(aChecker ? "checker" : "square");
    const int failsBefore = gFails;

    expect(maxDiscriminant > 30.0,
        QString("%1: the two pipelines must differ (max %2) - otherwise the case is vacuous")
            .arg(prefix).arg(maxDiscriminant, 0, 'f', 1));
    expect(alphaDelta == 0,
        QString("%1: coverage must be identical in both references (delta %2)").arg(prefix).arg(alphaDelta));
    expect(haveChain, QString("%1: a mip chain must exist").arg(prefix));

    // the GPU must build the premultiplied chain, and not the straight one
    expect(premultRawDelta <= 3,
        QString("%1: GPU chain must match the premultiplied reference (delta %2)").arg(prefix).arg(premultRawDelta));
    expect(straightRawDelta <= 3,
        QString("%1: the straight control chain must match its reference (delta %2)")
            .arg(prefix).arg(straightRawDelta));

    // THE HALO ASSERTION: the shipped chain preserves the silhouette color...
    expect(worstPremultHalo <= 4.0,
        QString("%1: no halo - visible color must stay source*coverage (worst deviation %2)")
            .arg(prefix).arg(worstPremultHalo, 0, 'f', 1));
    // ...and the positive control must fail it, proving the assertion has power
    expect(worstStraightHalo > 30.0,
        QString("%1: the straight control must exhibit the halo (worst deviation %2)")
            .arg(prefix).arg(worstStraightHalo, 0, 'f', 1));

    caseReport(
        prefix,
        gFails == failsBefore,
        QString("discriminant=%1 | shipped halo=%2 straight halo=%3")
            .arg(maxDiscriminant, 0, 'f', 1)
            .arg(worstPremultHalo, 0, 'f', 1)
            .arg(worstStraightHalo, 0, 'f', 1)
    );
}

void suiteChain() {
    suiteHeader("M1 mip chain generation vs analytic CPU reference");
    const int size = 64;

    for (const Pattern& pat : kPatterns) {
        verifyPattern(pat, size, true);
        verifyPattern(pat, size, false);
    }

    // the chain must be complete: level 0 down to 1x1, all levels present
    const Img src = makeChecker(kPatterns[0], size);
    const std::vector<Img> chain = readChain(src, true);
    const int expectedLevels = 1 + (int)std::floor(std::log2((double)size));
    const QString prefix = "chain completeness";
    const int failsBefore = gFails;
    expect((int)chain.size() == expectedLevels,
        QString("%1: expected %2 levels, got %3").arg(prefix).arg(expectedLevels).arg(chain.size()));
    expect(!chain.empty() && chain.back().w == 1 && chain.back().h == 1,
        QString("%1: the last level must be 1x1").arg(prefix));
    // a full-resolution-sourced checkerboard averages to a constant at every level: the
    // convergence of the chain is itself a check that levels are not stale or uninitialised
    if (chain.size() >= 3) {
        double sum = 0;
        const Img& l3 = chain[3];
        for (int y = 0; y < l3.h; ++y)
            for (int x = 0; x < l3.w; ++x)
                sum += l3.at(x, y)[3];
        const double meanAlpha = sum / (l3.w * l3.h);
        expect(near(meanAlpha, 127.5, 2.0),
            QString("%1: level 3 mean coverage must stay ~50%% (got %2)").arg(prefix).arg(meanAlpha, 0, 'f', 1));
    }
    caseReport(prefix, gFails == failsBefore, QString("%1 levels").arg(chain.size()));
}

//-------------------------------------------------------------------------------------------------
// M2: the production ImageKey texture must carry the premultiplied, mipmapped chain

void suiteImageKey() {
    suiteHeader("M2 production core::ImageKey texture");
    const int size = 64;
    const int expectedLevels = 1 + (int)std::floor(std::log2((double)size));

    int failsBefore = gFails;
    const QString prefix = "ImageKey texture";

    // BOTH patterns are required here, and the reason is not cosmetic. When the transparent
    // region's RGB is 0 - which is what most writers store - premultiplied and straight
    // averaging produce NUMERICALLY IDENTICAL chains, because premultiplying an opaque texel
    // is the identity and the transparent texel contributes 0 either way. So a probe that
    // only tests "bright shape on transparent black" cannot see a missing premultiply at
    // all: it stayed green when the premultiply was deliberately deleted from ImageKey.
    // The discriminating pattern is the bright-under-alpha-0 one, where the two chains
    // genuinely differ (0 vs 128 in the red channel at level 1).
    bool first = true;
    for (const Pattern& pat : kPatterns) {
        const Img src = makeChecker(pat, size);

        // build a real image resource the way the loaders do (straight RGBA8), then let the
        // production ImageKey build its texture from it
        auto* node = new img::ResourceNode(QString("mip_probe_%1").arg(pat.inside[0]));
        uint8_t* data = new uint8_t[src.d.size()];
        std::memcpy(data, src.d.data(), src.d.size());
        node->data().grabImage(XCMemBlock(data, src.d.size()), src.size(), img::Format_RGBA8);

        core::ImageKey key;
        key.setImage(node->handle());
        gl::Texture& tex = key.cache().texture();
        auto& ggl = gl::Global::functions();

        const QString label = QString("%1 [%2]").arg(prefix).arg(pat.name);
        const int caseFails = gFails;

        expect(tex.id() != 0, QString("%1: must be created").arg(label));
        expect(tex.mipmapLevels() == expectedLevels,
            QString("%1: %2 levels expected, got %3").arg(label).arg(expectedLevels).arg(tex.mipmapLevels()));

        // the filter parameters the sampling path depends on
        ggl.glBindTexture(GL_TEXTURE_2D, tex.id());
        GLint minF = 0, magF = 0, maxLevel = -1, wrapS = 0;
        ggl.glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minF);
        ggl.glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magF);
        ggl.glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxLevel);
        ggl.glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapS);
        ggl.glBindTexture(GL_TEXTURE_2D, 0);

        expect(minF == GL_LINEAR_MIPMAP_LINEAR,
            QString("%1: minification must be trilinear (got 0x%2)").arg(label).arg(minF, 0, 16));
        expect(magF == GL_LINEAR, QString("%1: magnification must be GL_LINEAR (got 0x%2)").arg(label).arg(magF, 0, 16));
        expect(maxLevel == expectedLevels - 1,
            QString("%1: GL_TEXTURE_MAX_LEVEL must be %2 (got %3)").arg(label).arg(expectedLevels - 1).arg(maxLevel));
        expect(wrapS == GL_CLAMP_TO_BORDER, QString("%1: wrap must stay CLAMP_TO_BORDER").arg(label));

        // the content must be the premultiplied chain
        const std::vector<uint8_t> lvl0 = readLevel(tex.id(), 0, src.size());
        const std::vector<uint8_t> lvl1 = readLevel(tex.id(), 1, QSize(size / 2, size / 2));
        const Img refPremult = downsamplePremultiplied(src);
        const Img refStraight = downsampleStraight(src);
        const int vsPremult = maxChannelDelta(lvl1, refPremult.d, 0);
        const int vsStraight = maxChannelDelta(lvl1, refStraight.d, 0);
        const int vsLevel0 = maxAnyChannelDelta(lvl1, lvl0);

        expect(vsPremult <= 3, QString("%1: level 1 must match the premultiplied reference (delta %2)")
                                   .arg(label).arg(vsPremult));
        if (vsStraight > 30) {
            // this pattern's two chains genuinely differ, so the texture must not be the
            // straight one - the assertion that catches a deleted premultiply
            expect(vsPremult < vsStraight,
                QString("%1: level 1 must be the premultiplied chain, not the straight one (deltas %2 vs %3)")
                    .arg(label).arg(vsPremult).arg(vsStraight));
        }
        // the chain must be real: level 1 is not a copy of level 0
        expect(vsLevel0 > 30, QString("%1: level 1 must differ from level 0 (delta %2)").arg(label).arg(vsLevel0));

        // THE HALO ASSERTION on the production texture: the visible result must be the
        // silhouette color scaled by coverage, never the transparent region's color
        double worstHalo = 0.0;
        for (size_t i = 0; i < lvl1.size(); i += 4) {
            const uint8_t* p = lvl1.data() + i;
            worstHalo = std::max(worstHalo, std::fabs(overBlackPremultiplied(p) - pat.inside[0] * (p[3] / 255.0)));
        }
        expect(worstHalo <= 4.0, QString("%1: no halo (worst deviation %2)").arg(label).arg(worstHalo, 0, 'f', 1));

        caseReport(
            label,
            gFails == caseFails,
            QString("levels=%1 min=0x%2 max=%3 vsPremult=%4 vsStraight=%5 halo=%6")
                .arg(tex.mipmapLevels())
                .arg(minF, 0, 16)
                .arg(maxLevel)
                .arg(vsPremult)
                .arg(vsStraight)
                .arg(worstHalo, 0, 'f', 1)
        );
        first = false;
    }
}

} // namespace

// The libs reference these globals; src/gui/Main.cpp defines them in the app. Same
// definitions the verify_blur harness uses, so an XC_ASSERT failure is loud instead of a
// silent no-op, and a fatal GL error aborts rather than continuing on a broken context.
class ProbeAssertHandler: public XCAssertHandler {
public:
    void failure() const override { std::fprintf(stderr, "XC_ASSERT failure\n"); }
};
class ProbeErrorHandler: public XCErrorHandler {
public:
    void critical(const QString& aText, const QString& aInfo, const QString& aDetail) const override {
        std::fprintf(
            stderr, "XC_FATAL_ERROR: %s | %s | %s\n", aText.toUtf8().constData(), aInfo.toUtf8().constData(),
            aDetail.toUtf8().constData());
        std::abort();
    }
};
XCAssertHandler* gXCAssertHandler = nullptr;
XCErrorHandler* gXCErrorHandler = nullptr;

// core/Project.cpp references this gui method, but libgui is not linked into the probe
// (the class declaration arrives via core/Project.h), so a lone definition satisfies the
// linker - same workaround as the verify_blur harness.
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

//-------------------------------------------------------------------------------------------------
// GL bootstrap: mirrors MainDisplayWidget / the verify_blur harness so the probe runs on the
// same context the app uses. Offscreen first, X11 fallback (see build_and_run.sh).

namespace {

class ProbeGLWidget: public QOpenGLWidget {
public:
    gl::DeviceInfo mDeviceInfo;
    bool mGLReady = false;

protected:
    void initializeGL() override {
        if (!context()->isValid()) {
            std::fprintf(stderr, "FATAL: invalid OpenGL context\n");
            std::exit(2);
        }
        auto* functions = QOpenGLVersionFunctionsFactory::get<gl::Global::Functions>(context());
        if (!functions || !functions->initializeOpenGLFunctions()) {
            std::fprintf(stderr, "FATAL: failed to get OpenGL 4.0 core functions\n");
            std::exit(2);
        }
        // setContext() asserts it is called exactly once, and initializeGL re-runs when the
        // widget recreates its FBO/context behind our back, so only the first pass installs
        // the globals (same ordering as the verify_blur harness).
        if (!mGLReady) {
            gl::Global::setContext(*this);
            gl::Global::setFunctions(*functions);
            mDeviceInfo.load();
            gl::DeviceInfo::setInstance(&mDeviceInfo);
            const char* version = (const char*)context()->functions()->glGetString(GL_VERSION);
            std::printf("OpenGL: %s\n", version ? version : "(unknown)");
            mGLReady = true;
        } else {
            // initializeGL re-runs when the widget recreates its FBO/context; the factory's
            // functions object is cached per context, so the previously registered one may
            // be dangling by now. Refresh the registration.
            gl::Global::clearFunctions();
            gl::Global::setFunctions(*functions);
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    static ProbeAssertHandler assertHandler;
    static ProbeErrorHandler errorHandler;
    gXCAssertHandler = &assertHandler;
    gXCErrorHandler = &errorHandler;

    {
        QSurfaceFormat format;
        format.setVersion(gl::Global::kVersion.first, gl::Global::kVersion.second);
        format.setProfile(QSurfaceFormat::CoreProfile);
        QSurfaceFormat::setDefaultFormat(format);
    }
    QApplication app(argc, argv);

    QMainWindow window;
    auto* glWidget = new ProbeGLWidget();
    window.setCentralWidget(glWidget);
    window.resize(32, 32);
    window.show();
    app.processEvents();
    if (!glWidget->mGLReady) {
        glWidget->update();
        app.processEvents();
    }
    if (!glWidget->mGLReady) {
        std::fprintf(stderr, "FATAL: GL widget did not initialize on this platform\n");
        return 2;
    }
    glWidget->makeCurrent();

    suiteChain();
    suiteImageKey();

    std::printf("\n%d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
