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
#include <map>
#include <memory>
#include <vector>

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QMainWindow>
#include <QOpenGLVersionFunctionsFactory>
#include <QOpenGLWidget>
#include <QScopedPointer>
#include <QSize>
#include <QString>
#include <QSurfaceFormat>

#include "XC.h"
#include "core/Animator.h"
#include "core/BlurKey.h"
#include "core/CameraInfo.h"
#include "core/ClippingFrame.h"
#include "core/DestinationTexturizer.h"
#include "core/FilterFrame.h"
#include "core/FolderNode.h"
#include "core/HsvKey.h"
#include "core/ImageKey.h"
#include "core/LayerNode.h"
#include "core/ObjectTree.h"
#include "core/Project.h"
#include "core/RenderInfo.h"
#include "core/ScaleKey.h"
#include "core/TimeInfo.h"
#include "gl/DeviceInfo.h"
#include "gl/Framebuffer.h"
#include "gl/Util.h"
#include "ctrl/Exporter.h"
#include "ctrl/ProjectLoader.h"
#include "gl/Global.h"
#include "gl/Texture.h"
#include "gl/VertexArrayObject.h"
#include "gui/AudioPlaybackWidget.h"
#include "img/ResourceData.h"
#include "util/IProgressReporter.h"
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

    const QString prefix = "ImageKey texture";

    // BOTH patterns are required here, and the reason is not cosmetic. When the transparent
    // region's RGB is 0 - which is what most writers store - premultiplied and straight
    // averaging produce NUMERICALLY IDENTICAL chains, because premultiplying an opaque texel
    // is the identity and the transparent texel contributes 0 either way. So a probe that
    // only tests "bright shape on transparent black" cannot see a missing premultiply at
    // all: it stayed green when the premultiply was deliberately deleted from ImageKey.
    // The discriminating pattern is the bright-under-alpha-0 one, where the two chains
    // genuinely differ (0 vs 128 in the red channel at level 1).
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
    }
}


//-------------------------------------------------------------------------------------------------
// M3: the production RENDER path. M1/M2 prove the texture is built correctly; these prove the
// shader consumes it correctly, which is the other half of premultiplied mipmapping - the
// layer shader has to divide the coverage back out (PREMULTIPLIED_INPUT).

// The layer's source image: constant RGB in every texel, with alpha as a checkerboard. Every
// mip level of this image is therefore constant RGB with alpha = coverage, which is exactly
// what correctly prefiltered artwork looks like. A straight-alpha chain instead multiplies the
// RGB by the coverage, and the shader then multiplies by coverage again - the halo.
Img makeAlphaChecker(int aSize, const uint8_t aRgb[3]) {
    Img img;
    img.w = img.h = aSize;
    img.d.assign((size_t)aSize * aSize * 4, 0);
    for (int y = 0; y < aSize; ++y) {
        for (int x = 0; x < aSize; ++x) {
            uint8_t* p = img.at(x, y);
            p[0] = aRgb[0];
            p[1] = aRgb[1];
            p[2] = aRgb[2];
            p[3] = ((x + y) % 2 == 0) ? 255 : 0;
        }
    }
    return img;
}

// The same content already prefiltered by hand: the equivalence reference.
Img makeUniform(int aSize, const uint8_t aRgb[3], uint8_t aAlpha) {
    Img img;
    img.w = img.h = aSize;
    img.d.assign((size_t)aSize * aSize * 4, 0);
    for (int y = 0; y < aSize; ++y) {
        for (int x = 0; x < aSize; ++x) {
            uint8_t* p = img.at(x, y);
            p[0] = aRgb[0];
            p[1] = aRgb[1];
            p[2] = aRgb[2];
            p[3] = aAlpha;
        }
    }
    return img;
}

img::ResourceNode* makeResource(const QString& aId, const Img& aSrc) {
    auto* node = new img::ResourceNode(aId);
    uint8_t* data = new uint8_t[aSrc.d.size()];
    std::memcpy(data, aSrc.d.data(), aSrc.d.size());
    node->data().grabImage(XCMemBlock(data, aSrc.d.size()), aSrc.size(), img::Format_RGBA8);
    return node;
}

// Mirrors MainDisplayWidget::paintGL's per-frame state so the render goes through the same
// wiring the app uses.
struct RenderFixture {
    QSize size;
    QScopedPointer<core::ClippingFrame> clipping;
    QScopedPointer<core::FilterFrame> filters;
    QScopedPointer<core::DestinationTexturizer> texturizer;
    QScopedPointer<gl::Texture> targetTex;
    QScopedPointer<gl::Framebuffer> targetFbo;

    void init(const QSize& aSize) {
        size = aSize;
        clipping.reset(new core::ClippingFrame());
        clipping->resize(aSize);
        filters.reset(new core::FilterFrame());
        filters->resize(aSize);
        texturizer.reset(new core::DestinationTexturizer());
        texturizer->resize(aSize);
        targetTex.reset(new gl::Texture());
        targetTex->create(aSize);
        targetTex->setFilter(GL_LINEAR, GL_LINEAR);
        targetTex->setWrap(GL_CLAMP_TO_EDGE);
        targetFbo.reset(new gl::Framebuffer());
        targetFbo->setColorAttachment(0, targetTex->id());
        XC_ASSERT(targetFbo->isComplete());
    }

    std::vector<uint8_t> render(
        core::ObjectTree& aTree, const core::CameraInfo& aCamera, int aFrame, const uint8_t aBackdrop[3],
        bool aIsGrid = false, bool aWorkingCache = false
    ) {
        auto& ggl = gl::Global::functions();

        clipping->clearTexture();
        clipping->resetClippingId();
        texturizer->clearTexture();
        filters->clearAll();

        ggl.glBindFramebuffer(GL_FRAMEBUFFER, targetFbo->id());
        gl::Util::setViewportAsActualPixels(size);
        // an OPAQUE backdrop, as the clear colour - which is exactly the initial content of the
        // destination texture the layer shader reads. Opaque black gives M3 a clean analytic
        // value; a mid grey is what makes the blend modes actually discriminate in M4, because
        // most blend functions evaluated against a black backdrop collapse to the same answer.
        gl::Util::clearColorBuffer(
            aBackdrop[0] / 255.0f, aBackdrop[1] / 255.0f, aBackdrop[2] / 255.0f, 1.0f
        );
        gl::Util::resetRenderState();

        core::TimeInfo time;
        time.fps = 24;
        time.frameRange = util::Range(0, 200);
        time.loop = false;
        time.frame = core::Frame(aFrame);

        core::RenderInfo info;
        info.time = time;
        info.framebuffer = targetFbo->id();
        info.dest = targetTex->id();
        info.isGrid = aIsGrid;
        info.nonPosed = false;
        info.originMesh = false;
        info.clippingId = 0;
        info.clippingFrame = clipping.data();
        info.destTexturizer = texturizer.data();
        info.filterFrame = filters.data();
        info.opacityScale = 1.0f;
        info.camera = aCamera;

        aTree.render(info, aWorkingCache);

        std::vector<uint8_t> out((size_t)size.width() * size.height() * 4, 0);
        ggl.glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        ggl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return out;
    }
};

core::LayerNode* addScaledLayer(
    core::ObjectTree& aTree, core::ObjectNode* aParent, img::ResourceNode* aRes, const QString& aName,
    const QVector2D& aPos, float aScale, img::BlendMode aMode
) {
    auto* layer = new core::LayerNode(aName, aTree.shaderHolder());
    layer->setDefaultImage(aRes->handle(), aMode);
    // setDefaultPosture is what creates the Move/Rotate/Scale default keys the transform
    // chain reads; without it the layer is never laid out and renders nothing. The position
    // matters too: the camera maps world (0,0) to the canvas ORIGIN, not its centre, so an
    // image centred on the world origin would hang off the top-left of the canvas.
    layer->setDefaultPosture(aPos);
    layer->setDefaultOpacity(1.0f);
    core::getOrCreateDefaultKey<core::ScaleKey, core::TimeKeyType_Scale>(*layer->timeLine())
        ->setScale(QVector2D(aScale, aScale));
    aParent->children().pushBack(layer);
    return layer;
}

// Renders one layer (source image, scale, blend mode) over an opaque black backdrop.
std::map<QString, std::unique_ptr<RenderFixture>>& renderFixtures() {
    static std::map<QString, std::unique_ptr<RenderFixture>> fixtures;
    return fixtures;
}

// Drops the cached fixtures. MUST be called while the GL context is still current: the
// fixtures own GL objects, and a static destructor would run at process exit, after the
// context is gone (glDeleteFramebuffers through a dangling function pointer - the crash this
// harness originally had at exit).
void clearRenderFixtures() { renderFixtures().clear(); }

std::vector<uint8_t> renderLayer(
    const QSize& aCanvas, const Img& aImage, float aScale, img::BlendMode aMode,
    const uint8_t aBackdrop[3] = nullptr
) {
    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(aCanvas.width()).arg(aCanvas.height());
    auto it = fixtures.find(key);
    if (it == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(aCanvas);
        it = fixtures.emplace(key, std::move(fx)).first;
    }
    RenderFixture& fx = *it->second;

    core::ObjectTree tree;
    auto* root = new core::FolderNode("root");
    tree.grabTopNode(root);
    auto* res = makeResource(QString("mip_src_%1").arg((int)aImage.d[0]), aImage);
    addScaledLayer(tree, root, res, "layer", QVector2D(aCanvas.width() * 0.5f, aCanvas.height() * 0.5f), aScale, aMode);

    core::CameraInfo camera;
    camera.reset(aCanvas, 1.0, aCanvas, QPoint(0, 0));
    const uint8_t black[3] = {0, 0, 0};
    return fx.render(tree, camera, 0, aBackdrop ? aBackdrop : black);
}

// max |a - b| over all channels, over the interior only: the layer's mesh insets its border
// vertices and the canvas edge samples the CLAMP_TO_BORDER (transparent) margin, so the outer
// band legitimately differs and is excluded, exactly like the golden suite's borderCrop.
int interiorMaxDelta(const std::vector<uint8_t>& aA, const std::vector<uint8_t>& aB, const QSize& aSize, int aMargin) {
    int worst = 0;
    for (int y = aMargin; y < aSize.height() - aMargin; ++y) {
        for (int x = aMargin; x < aSize.width() - aMargin; ++x) {
            const size_t i = ((size_t)y * aSize.width() + x) * 4;
            for (int c = 0; c < 4; ++c)
                worst = std::max(worst, std::abs((int)aA[i + c] - (int)aB[i + c]));
        }
    }
    return worst;
}

// mean over the interior, per channel, of the first channel only (enough for a solid color)
double interiorMeanRed(const std::vector<uint8_t>& aImg, const QSize& aSize, int aMargin) {
    double sum = 0;
    int n = 0;
    for (int y = aMargin; y < aSize.height() - aMargin; ++y) {
        for (int x = aMargin; x < aSize.width() - aMargin; ++x) {
            sum += aImg[((size_t)y * aSize.width() + x) * 4];
            ++n;
        }
    }
    return n ? sum / n : 0.0;
}

void suiteRender() {
    suiteHeader("M3 production render: minified layer, no halo");
    const QSize canvas(64, 64);
    const int imageSize = 64;
    const uint8_t color[3] = {200, 100, 50};
    // at scale 0.5 the quad is the central 32x32 of the 64x64 canvas; keep the crop inside it
    const int margin = 19;

    // The layer is drawn at half scale, so the artwork is minified 2x and the sampler reads
    // mip level 1 - which, for this source, is constant RGB at 50% coverage.
    const std::vector<uint8_t> rendered =
        renderLayer(canvas, makeAlphaChecker(imageSize, color), 0.5f, img::BlendMode_Normal);

    // GROUND TRUTH, analytic. The layer composites onto opaque black with straight-alpha
    // blending, so a correct chain leaves the silhouette's own color scaled by coverage,
    // while a chain that keeps straight data is darkened by coverage a second time.
    const double correctRed = color[0] * 0.5;
    const double haloRed = color[0] * 0.25;
    const double discrimination = std::fabs(correctRed - haloRed);

    const double measuredRed = interiorMeanRed(rendered, canvas, margin);
    const double measuredGreen = [&] {
        double sum = 0;
        int n = 0;
        for (int y = margin; y < canvas.height() - margin; ++y)
            for (int x = margin; x < canvas.width() - margin; ++x) {
                sum += rendered[((size_t)y * canvas.width() + x) * 4 + 1];
                ++n;
            }
        return n ? sum / n : 0.0;
    }();

    const int failsBefore = gFails;
    const QString prefix = "minified layer over black";

    expect(discrimination > 30.0,
        QString("%1: the halo must be distinguishable from the correct value (margin %2)")
            .arg(prefix).arg(discrimination, 0, 'f', 1));
    // THE ASSERTION: the rendered silhouette keeps its color, scaled only by coverage
    expect(near(measuredRed, correctRed, 4.0),
        QString("%1: red must be %2 (source*coverage), got %3").arg(prefix).arg(correctRed, 0, 'f', 1).arg(measuredRed, 0, 'f', 1));
    expect(near(measuredGreen, color[1] * 0.5, 4.0),
        QString("%1: green must be %2, got %3").arg(prefix).arg(color[1] * 0.5, 0, 'f', 1).arg(measuredGreen, 0, 'f', 1));
    expect(!near(measuredRed, haloRed, 4.0),
        QString("%1: red must NOT be the haloed %2 (got %3)").arg(prefix).arg(haloRed, 0, 'f', 1).arg(measuredRed, 0, 'f', 1));

    // EQUIVALENCE: a correctly mipmapped checkerboard must render exactly like the same
    // content prefiltered by hand (constant color at the equivalent coverage). This reference
    // needs no mipmapping at all - the texture is uniform, so every level is identical - which
    // makes it a ground truth independent of the mip machinery under test.
    const std::vector<uint8_t> reference =
        renderLayer(canvas, makeUniform(imageSize, color, 128), 0.5f, img::BlendMode_Normal);
    const int equivDelta = interiorMaxDelta(rendered, reference, canvas, margin);
    expect(equivDelta <= 4,
        QString("%1: a correct chain must match the hand-prefiltered reference (delta %2)").arg(prefix).arg(equivDelta));

    caseReport(
        prefix,
        gFails == failsBefore,
        QString("red=%1 (correct %2, halo %3) equivDelta=%4")
            .arg(measuredRed, 0, 'f', 1)
            .arg(correctRed, 0, 'f', 1)
            .arg(haloRed, 0, 'f', 1)
            .arg(equivDelta)
    );
}


//-------------------------------------------------------------------------------------------------
// M4: every blend mode. The halo fix lives in the shader's handling of the sampled texel, and
// each blend mode takes a different branch over that texel - so the mode sweep is where a
// mode-specific regression would hide.
//
// The reference here is the EQUIVALENCE one from M3: a correctly mipmapped checkerboard must
// render like the same content prefiltered by hand (constant color, constant coverage). That
// reference is mode-independent, which is what makes the sweep affordable - it needs no CPU
// replica of 28 blend formulas.
//
// Its limitation is worth stating plainly: both renders go through the SAME shader, so a
// global shader error cancels out and the equivalence check cannot see it. Only the analytic
// assertion in M3 has power against a wrong PREMULTIPLIED_INPUT (verified by mutation). The
// sweep's job is narrower and still real: catch anything that affects the mipmapped content
// differently per mode.
void suiteBlendModes() {
    suiteHeader("M4 all blend modes: minified content == hand-prefiltered content");
    const QSize canvas(64, 64);
    const int imageSize = 64;
    const uint8_t color[3] = {200, 100, 50};
    const int margin = 19;

    const uint8_t backdrop[3] = {128, 128, 128};

    std::vector<double> perModeMean;
    int worstDelta = 0;
    img::BlendMode worstMode = img::BlendMode_Normal;
    int modeFails = 0;

    for (int m = img::BlendMode_Normal; m < img::BlendMode_TERM; ++m) {
        const img::BlendMode mode = (img::BlendMode)m;
        const std::vector<uint8_t> mipmapped =
            renderLayer(canvas, makeAlphaChecker(imageSize, color), 0.5f, mode, backdrop);
        const std::vector<uint8_t> prefiltered =
            renderLayer(canvas, makeUniform(imageSize, color, 128), 0.5f, mode, backdrop);

        const int delta = interiorMaxDelta(mipmapped, prefiltered, canvas, margin);
        if (delta > worstDelta) {
            worstDelta = delta;
            worstMode = mode;
        }
        if (delta > 4) {
            ++modeFails;
            expect(false,
                QString("blend mode %1 (0x%2): minified content must match the hand-prefiltered content (delta %3)")
                    .arg((int)mode).arg((int)mode, 0, 16).arg(delta));
        }
        perModeMean.push_back(interiorMeanRed(mipmapped, canvas, margin));
    }

    // NON-VACUITY: the sweep only means something if the modes actually changed the result.
    // If blend-mode plumbing silently fell back to Normal, every entry would be identical and
    // the equivalence assertion above would be trivially satisfied 28 times over.
    std::vector<double> distinct = perModeMean;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end(),
                       [](double a, double b) { return std::fabs(a - b) < 0.5; }),
        distinct.end());

    expect((int)perModeMean.size() == img::BlendMode_TERM,
        QString("swept %1 of %2 blend modes").arg(perModeMean.size()).arg(img::BlendMode_TERM));
    expect(distinct.size() >= 8,
        QString("the modes must produce genuinely different results (%1 distinct of %2) - otherwise the sweep is vacuous")
            .arg(distinct.size())
            .arg(perModeMean.size()));
    expect(modeFails == 0, QString("%1 blend mode(s) disagreed with the reference").arg(modeFails));

    caseReport(
        QString("all blend modes"),
        modeFails == 0,
        QString("%1 modes, %2 distinct results, worst delta %3 (mode 0x%4)")
            .arg(perModeMean.size())
            .arg(distinct.size())
            .arg(worstDelta)
            .arg((int)worstMode, 0, 16)
    );
}


//-------------------------------------------------------------------------------------------------
// M5: the feature paths a layer can be drawn through. Same equivalence reference as M3/M4 - a
// correctly mipmapped checkerboard must render like the same content prefiltered by hand - so
// it applies unchanged to each path.
//
// Covers the layer HSV branch, the folder composite path (which renders the subtree into a
// FilterFrame slot and then blends that slot against the scene), the two combined, and the
// grid/mesh overlay.
struct LayerSetup {
    const char* name = "?";
    img::BlendMode mode = img::BlendMode_Normal;
    bool hsv = false;                                  // an HSV key on the layer
    bool inFolder = false;                             // wrap the layer in a composite folder
    img::BlendMode folderMode = img::BlendMode_Multiply; // non-Normal forces the composite path
    bool grid = false;                                 // render the mesh overlay instead
    bool clipping = false;                             // add a clipper that half-covers the layer
    float blur = 0.0f;                                 // layer blur: routes through FilterFrame
};

std::vector<uint8_t> renderSceneAtScale(
    const QSize& aCanvas, const Img& aImage, const LayerSetup& aSetup, const uint8_t aBackdrop[3], float aScale,
    float aCameraZoom = 1.0f, bool aWorkingCache = false
) {
    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(aCanvas.width()).arg(aCanvas.height());
    auto it = fixtures.find(key);
    if (it == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(aCanvas);
        it = fixtures.emplace(key, std::move(fx)).first;
    }
    RenderFixture& fx = *it->second;

    core::ObjectTree tree;
    auto* root = new core::FolderNode("root");
    tree.grabTopNode(root);

    const QVector2D center(aCanvas.width() * 0.5f, aCanvas.height() * 0.5f);
    auto* res = makeResource(QString("mip_scene_%1").arg((int)aImage.d[3]), aImage);

    core::ObjectNode* parent = root;
    QVector2D layerPos = center;
    if (aSetup.inFolder) {
        auto* folder = new core::FolderNode("folder");
        folder->setDefaultPosture(center);   // folder at the canvas centre, layer local to it
        folder->setDefaultOpacity(1.0f);
        folder->setBlendMode(aSetup.folderMode);
        root->children().pushBack(folder);
        parent = folder;
        layerPos = QVector2D(0.0f, 0.0f);
    }

    auto* layer = addScaledLayer(tree, parent, res, "layer", layerPos, aScale, aSetup.mode);
    if (aSetup.hsv) {
        // Saturation to 50% is unambiguously non-identity. The key must be PUSHED at a frame,
        // not installed as a "default" key: LayerNode decides whether to run the HSV branch with
        // mTimeLine.isEmpty(TimeKeyType_HSV), which consults the key map, while grabDefaultKey
        // files the key under mDefaultKeys where that check cannot see it - so a default HSV key
        // silently does nothing at all.
        auto* hsv = new core::HSVKey();
        hsv->setHSV(QList<int>{0, 50, 100, 0});   // hue, saturation%, value%, absolute-hue flag
        cmnd::Base* pusher = layer->timeLine()->createPusher(core::TimeKeyType_HSV, 0, hsv);
        pusher->tryExec();
        delete pusher;
    }

    if (aSetup.clipping) {
        // A clipper is a node whose PREVIOUS sibling is clipped (ObjectNodeUtil::isClipper), and
        // children are walked back-to-front, so the clippee above is pushed first and the clipper
        // second. The clipper only half-covers the clippee, so the clip is partial and visible.
        const uint8_t clipColor[3] = {255, 255, 255};
        auto* clipRes = makeResource("mip_clipper", makeUniform(aImage.w, clipColor, 255));
        auto* clipper = new core::LayerNode("clipper", tree.shaderHolder());
        clipper->setDefaultImage(clipRes->handle(), img::BlendMode_Normal);
        clipper->setDefaultPosture(center + QVector2D(8.0f, 0.0f));
        clipper->setDefaultOpacity(1.0f);
        core::getOrCreateDefaultKey<core::ScaleKey, core::TimeKeyType_Scale>(*clipper->timeLine())
            ->setScale(QVector2D(0.5f, 0.5f));
        parent->children().pushBack(clipper);
        layer->setClipped(true);
    }

    if (aSetup.blur > 0.0f) {
        // An active BlurKey puts the layer on its own composite path: the artwork is rendered
        // into a FilterFrame slot and blurred through the downsample ladder. Mip sampling of the
        // artwork still happens on that first render, so the equivalence reference applies.
        auto* blurKey = new core::BlurKey();
        blurKey->setAmount(aSetup.blur);
        cmnd::Base* pusher = layer->timeLine()->createPusher(core::TimeKeyType_Blur, 0, blurKey);
        pusher->tryExec();
        delete pusher;
    }

    core::CameraInfo camera;
    camera.reset(aCanvas, 1.0, aCanvas, QPoint(0, 0));
    if (aCameraZoom != 1.0f)
        camera.setScale(aCameraZoom);
    return fx.render(tree, camera, 0, aBackdrop, aSetup.grid, aWorkingCache);
}

std::vector<uint8_t> renderScene(
    const QSize& aCanvas, const Img& aImage, const LayerSetup& aSetup, const uint8_t aBackdrop[3]
) {
    return renderSceneAtScale(aCanvas, aImage, aSetup, aBackdrop, 0.5f);
}

void suitePaths() {
    suiteHeader("M5 feature paths: HSV, folder composite, grid overlay");
    const QSize canvas(64, 64);
    const int imageSize = 64;
    const uint8_t color[3] = {200, 100, 50};
    const uint8_t backdrop[3] = {128, 128, 128};
    const int margin = 19;

    const LayerSetup setups[] = {
        {"normal", img::BlendMode_Normal, false, false, img::BlendMode_Normal, false, false, 0.0f},
        {"HSV adjust", img::BlendMode_Normal, true, false, img::BlendMode_Normal, false, false, 0.0f},
        {"in composite folder", img::BlendMode_Normal, false, true, img::BlendMode_Multiply, false, false, 0.0f},
        {"HSV in composite folder", img::BlendMode_Normal, true, true, img::BlendMode_Multiply, false, false, 0.0f},
        {"blend mode Multiply", img::BlendMode_Multiply, false, false, img::BlendMode_Normal, false, false, 0.0f},
        {"clipped by a clipper", img::BlendMode_Normal, false, false, img::BlendMode_Normal, false, true, 0.0f},
        {"with layer blur", img::BlendMode_Normal, false, false, img::BlendMode_Normal, false, false, 4.0f},
    };

    // the plain baseline, for the non-vacuity check below
    const LayerSetup plain = setups[0];
    const std::vector<uint8_t> baseRender =
        renderScene(canvas, makeAlphaChecker(imageSize, color), plain, backdrop);
    const double baseMean = interiorMeanRed(baseRender, canvas, margin);

    for (const LayerSetup& setup : setups) {
        const int failsBefore = gFails;
        const std::vector<uint8_t> mipmapped =
            renderScene(canvas, makeAlphaChecker(imageSize, color), setup, backdrop);
        const std::vector<uint8_t> prefiltered =
            renderScene(canvas, makeUniform(imageSize, color, 128), setup, backdrop);
        const int delta = interiorMaxDelta(mipmapped, prefiltered, canvas, margin);
        const double mean = interiorMeanRed(mipmapped, canvas, margin);

        expect(delta <= 4,
            QString("path %1: minified content must match the hand-prefiltered content (delta %2)")
                .arg(setup.name).arg(delta));
        // This path must actually have done something, or the assertion above is vacuous.
        // Two deliberate choices here. Compared over ALL channels, because a saturation change
        // leaves the max channel (here red) untouched and only moves the others, so a
        // single-channel check reports "no change" for a path that ran perfectly well. And
        // compared over the WHOLE canvas, not the interior crop: a blur of an already-uniform
        // interior is invisible, so its effect only shows at the quad's edge, which the crop
        // deliberately excludes.
        const int vsBaseline = interiorMaxDelta(mipmapped, baseRender, canvas, 0);
        if (setup.hsv || setup.inFolder || setup.clipping || setup.blur > 0.0f || setup.mode != plain.mode)
            expect(vsBaseline > 8,
                QString("path %1: must change the result vs the plain baseline (max channel delta %2, mean %3 vs %4)")
                    .arg(setup.name).arg(vsBaseline).arg(mean, 0, 'f', 2).arg(baseMean, 0, 'f', 2));

        // The blur path needs an ABSOLUTE expectation, not only the equivalence one. Blurring a
        // uniform region leaves its value where it was, and that value is analytic:
        // coverage*source + (1-coverage)*backdrop. Equivalence alone cannot see a global shader
        // error (both sides of it run through the same shader), so without this the blur case
        // would pass even with PREMULTIPLIED_INPUT wrong.
        if (setup.blur > 0.0f) {
            const double expected = 0.5 * color[0] + 0.5 * backdrop[0];
            expect(near(mean, expected, 4.0),
                QString("blur: a blurred uniform region must keep the analytic value %1, got %2")
                    .arg(expected, 0, 'f', 1)
                    .arg(mean, 0, 'f', 1));
        }

        // The clipping case must prove the CLIPPEE was clipped, not merely that a clipper got
        // drawn: the clipper covers only the right-hand part of the clippee, so left of that
        // boundary the clippee's own color must be absent, and right of it present. Without
        // this, a clipper that drew but clipped nothing would leave the IS_CLIPPEE shader
        // variant unexercised while the equivalence assertion above still passed.
        QString clipInfo = "";
        if (setup.clipping) {
            const auto redAt = [&](int x, int y) { return (int)mipmapped[((size_t)y * canvas.width() + x) * 4]; };
            const int outsideClip = redAt(18, 32); // inside the clippee, outside the clipper
            const int insideClip = redAt(36, 32);  // inside both
            expect(std::abs(outsideClip - (int)backdrop[0]) <= 6,
                QString("clipping: left of the clipper boundary the clippee must be clipped away "
                        "(red %1, backdrop %2)")
                    .arg(outsideClip)
                    .arg(backdrop[0]));
            expect(insideClip > outsideClip + 30,
                QString("clipping: inside the clipper the clippee must show through (red %1 vs %2)")
                    .arg(insideClip)
                    .arg(outsideClip));
            clipInfo = QString(" clip[outside=%1 inside=%2]").arg(outsideClip).arg(insideClip);
        }

        caseReport(
            QString("path: %1").arg(setup.name), gFails == failsBefore,
            QString("equivDelta=%1 mean=%2%3").arg(delta).arg(mean, 0, 'f', 1).arg(clipInfo));
    }

    // the grid/mesh overlay draws wireframe geometry through GridDrawingFrag, which outputs a
    // constant uColor and samples NO texture, so mipmapping provably cannot affect it. Assert
    // it still renders and differs from the artwork - i.e. the path ran and was not skipped.
    {
        const int failsBefore = gFails;
        LayerSetup grid = plain;
        grid.grid = true;
        const std::vector<uint8_t> gridRender = renderScene(canvas, makeAlphaChecker(imageSize, color), grid, backdrop);
        int nonBackdrop = 0;
        for (size_t i = 0; i < gridRender.size(); i += 4) {
            if (std::abs((int)gridRender[i] - backdrop[0]) > 8)
                ++nonBackdrop;
        }
        expect(nonBackdrop > 0, QString("grid overlay must draw its wireframe (found %1 pixels)").arg(nonBackdrop));
        expect(interiorMaxDelta(gridRender, baseRender, canvas, margin) > 8,
            "grid overlay must differ from the artwork render (the grid path really ran)");
        caseReport("path: grid/mesh overlay", gFails == failsBefore,
            QString("%1 wireframe pixels; GridDrawingFrag samples no texture").arg(nonBackdrop));
    }
}


//-------------------------------------------------------------------------------------------------
// M6: every mip level. The tests above all minify by exactly 2x, so they exercise level 1 only.
// Export renders at its own resolution and the canvas can be zoomed, both of which pick different
// levels - so correctness has to hold at every level, not just the first one.
//
// The source is chosen so the correct interior value is LEVEL-INDEPENDENT: a checkerboard with a
// constant RGB averages to constant RGB with alpha = coverage at every level, so
// coverage*source + (1-coverage)*backdrop is the analytic answer wherever the sampler lands. The
// assertion therefore has power against a wrong level being selected as well as against a wrong
// chain: the value must be the same at every minification factor.
void suiteMipLevels() {
    suiteHeader("M6 mip level sweep: correctness at every minification factor");
    const QSize canvas(64, 64);
    const int imageSize = 64;
    const uint8_t color[3] = {200, 100, 50};
    const uint8_t backdrop[3] = {128, 128, 128};

    // analytic: a half-covered source over this backdrop, independent of which level is sampled
    const double expected = 0.5 * color[0] + 0.5 * backdrop[0];

    const float scales[] = {0.5f, 0.25f, 0.125f};
    const int expectedLevels[] = {1, 2, 3};

    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
        const int failsBefore = gFails;
        const LayerSetup setup;
        std::vector<uint8_t> rendered =
            renderSceneAtScale(canvas, makeAlphaChecker(imageSize, color), setup, backdrop, scales[i]);

        // The crop has to follow the quad: at scale s the layer covers the central 64*s pixels,
        // so a fixed margin would straddle the quad's edge at the smaller scales and average the
        // backdrop into the result (which is exactly what a fixed margin did here, reporting
        // 141.6 instead of 164.0 at 4x).
        const int margin = (int)std::lround(32.0 - 32.0 * scales[i]) + 3;
        const double mean = interiorMeanRed(rendered, canvas, margin);

        // the interior must be uniform AND equal to the analytic value at every level
        expect(near(mean, expected, 4.0),
            QString("scale %1 (level ~%2): interior must be %3, got %4")
                .arg(scales[i], 0, 'f', 3)
                .arg(expectedLevels[i])
                .arg(expected, 0, 'f', 1)
                .arg(mean, 0, 'f', 1));
        caseReport(
            QString("minified %1x (level %2)").arg(1.0 / scales[i], 0, 'f', 0).arg(expectedLevels[i]),
            gFails == failsBefore, QString("interior=%1 (analytic %2)").arg(mean, 0, 'f', 1).arg(expected, 0, 'f', 1));
    }
}


//-------------------------------------------------------------------------------------------------
// M7: is the mip level CONSTANT across the mesh?
//
// This is the seam question. UVs are interpolated linearly within a triangle, so the derivative the
// sampler sees is constant per triangle: trilinear filtering picks one blend per triangle, and if
// adjacent triangles land on different levels the boundary between them is a blurriness step. That
// matters here because LayerMesh keeps static texCoords while positions are deformed, so a deformed
// mesh has triangles of differing screen area - and before mipmapping every layer sampled level 0,
// where such steps cannot exist.
//
// The detector uses a source that is level-SENSITIVE: an opaque white checkerboard on transparent
// black. Level 0 is maximum contrast (opaque white alternating with the black backdrop), while
// every level at or above 1 is a uniform 50% coverage - so ANY region that samples level 0 shows up
// as non-zero local contrast. If LOD varied across the mesh, some region would leak level 0 and the
// per-block contrast would spread.
//
// The same measurement is run at scale 1.0 as a positive control: there LOD is 0 by construction and
// the contrast MUST be high, which proves the detector can see level 0 at all. Without that control
// a "contrast is zero" result would be indistinguishable from a detector that does not work.
struct BlockStats {
    int count = 0;
    double minSd = 0.0;
    double maxSd = 0.0;
    double meanSd = 0.0;
};

BlockStats interiorBlockContrast(
    const std::vector<uint8_t>& aImg, const QSize& aSize, int aMargin, int aBlock
) {
    std::vector<double> sds;
    for (int by = aMargin; by + aBlock <= aSize.height() - aMargin; by += aBlock) {
        for (int bx = aMargin; bx + aBlock <= aSize.width() - aMargin; bx += aBlock) {
            double sum = 0, sum2 = 0;
            const int n = aBlock * aBlock;
            for (int y = by; y < by + aBlock; ++y) {
                for (int x = bx; x < bx + aBlock; ++x) {
                    const double v = aImg[((size_t)y * aSize.width() + x) * 4];
                    sum += v;
                    sum2 += v * v;
                }
            }
            const double mean = sum / n;
            sds.push_back(std::sqrt(std::max(0.0, sum2 / n - mean * mean)));
        }
    }
    BlockStats st;
    if (sds.empty())
        return st;
    st.minSd = sds[0];
    st.maxSd = sds[0];
    double total = 0;
    for (double v : sds) {
        st.minSd = std::min(st.minSd, v);
        st.maxSd = std::max(st.maxSd, v);
        total += v;
    }
    st.meanSd = total / sds.size();
    st.count = (int)sds.size();
    return st;
}

void suiteLodUniformity() {
    suiteHeader("M7 mip level is constant across the mesh (no LOD seams)");
    const QSize canvas(64, 64);
    const int imageSize = 64;
    const uint8_t black[3] = {0, 0, 0};

    const LayerSetup setup;

    // POSITIVE CONTROL: at scale 1.0 the layer is at LOD 0, so the checkerboard is fully visible
    // and the block contrast must be large. This is what proves the detector sees level 0.
    {
        const int failsBefore = gFails;
        const std::vector<uint8_t> rendered =
            renderSceneAtScale(canvas, makeAlphaChecker(imageSize, kPatterns[0].inside), setup, black, 1.0f);
        const BlockStats st = interiorBlockContrast(rendered, canvas, 8, 8);
        expect(st.maxSd > 60.0,
            QString("control at LOD 0 (scale 1.0): block contrast must be high, got max %1")
                .arg(st.maxSd, 0, 'f', 1));
        caseReport("control: scale 1.0 (LOD 0)", gFails == failsBefore,
            QString("block contrast max=%1 mean=%2 - level 0 is visible, so the detector works")
                .arg(st.maxSd, 0, 'f', 1).arg(st.meanSd, 0, 'f', 1));
    }

    // THE ASSERTION: at every integer LOD the whole layer must sit at that level, so no block may
    // retain level-0 contrast. A spread between blocks would mean LOD varies across the mesh.
    const float scales[] = {0.5f, 0.25f, 0.125f};
    const int levels[] = {1, 2, 3};
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
        const int failsBefore = gFails;
        const std::vector<uint8_t> rendered =
            renderSceneAtScale(canvas, makeAlphaChecker(imageSize, kPatterns[0].inside), setup, black, scales[i]);
        const int margin = (int)std::lround(32.0 - 32.0 * scales[i]) + 3;
        const int block = std::max(2, (int)std::lround((32.0 * scales[i]) / 5.0));
        const BlockStats st = interiorBlockContrast(rendered, canvas, margin, block);

        expect(st.maxSd <= 2.0,
            QString("scale %1 (LOD %2): no block may keep level-0 contrast (max block sd %3)")
                .arg(scales[i], 0, 'f', 3)
                .arg(levels[i])
                .arg(st.maxSd, 0, 'f', 2));
        expect(st.maxSd - st.minSd <= 2.0,
            QString("scale %1 (LOD %2): contrast must be spatially uniform (spread %3) - a spread "
                    "means triangles are landing on different mip levels")
                .arg(scales[i], 0, 'f', 3)
                .arg(levels[i])
                .arg(st.maxSd - st.minSd, 0, 'f', 2));
        caseReport(QString("scale %1 (LOD %2)").arg(scales[i], 0, 'f', 3).arg(levels[i]),
            gFails == failsBefore,
            QString("%1 blocks, sd max=%2 min=%3 mean=%4")
                .arg(st.count)
                .arg(st.maxSd, 0, 'f', 2)
                .arg(st.minSd, 0, 'f', 2)
                .arg(st.meanSd, 0, 'f', 2));
    }
}


//-------------------------------------------------------------------------------------------------
// M8: the configuration ctrl::Exporter puts the render in.
//
// This is not the exporter itself - driving it needs a core::Project and the harness stubs. It is
// the two things the exporter changes about the render call that matter for mipmapping, tested
// directly:
//
//   * the camera. The exporter sets its camera from the project's image size to the OUTPUT size
//     (ctrl::Exporter does camera.reset(originSize, 1.0, originSize, QPoint())), so an export at a
//     size other than the canvas minifies differently from the interactive view and lands on a
//     different mip level than the canvas would.
//   * aUseWorkingCache, which the exporter passes as true (the canvas passes false).
//
// The source is level-independent, so the analytic interior value must hold whatever level the
// export's camera selects - which is the property that makes "correct in export" follow from
// "correct at every level".
void suiteExportLike() {
    suiteHeader("M8 export-like render configuration (camera scale, working cache)");
    const QSize canvas(64, 64);
    const int imageSize = 64;
    const uint8_t color[3] = {200, 100, 50};
    const uint8_t backdrop[3] = {128, 128, 128};
    const double expected = 0.5 * color[0] + 0.5 * backdrop[0];

    const LayerSetup setup;

    // An export camera that renders the same canvas smaller: zooming out minifies the layer
    // further, so between the layer's own 0.5 and the camera this lands at LOD 2.
    struct Case {
        const char* name;
        float cameraZoom;
        bool workingCache;
        int level;
    };
    const Case cases[] = {
        {"canvas view, time cache", 1.0f, false, 1},
        {"export-like, working cache", 1.0f, true, 1},
        {"export-like, camera zoomed out", 0.5f, true, 2},
        {"export-like, camera zoomed out further", 0.25f, true, 3},
    };

    for (const Case& c : cases) {
        const int failsBefore = gFails;
        const std::vector<uint8_t> rendered = renderSceneAtScale(
            canvas, makeAlphaChecker(imageSize, color), setup, backdrop, 0.5f, c.cameraZoom, c.workingCache
        );
        const int margin = (int)std::lround(32.0 - 32.0 * 0.5f * c.cameraZoom) + 3;
        const double mean = interiorMeanRed(rendered, canvas, margin);

        expect(near(mean, expected, 5.0),
            QString("%1: interior must be the analytic %2, got %3")
                .arg(c.name).arg(expected, 0, 'f', 1).arg(mean, 0, 'f', 1));
        caseReport(QString("export-like: %1").arg(c.name), gFails == failsBefore,
            QString("interior=%1 (analytic %2, ~level %3)").arg(mean, 0, 'f', 1).arg(expected, 0, 'f', 1).arg(c.level));
    }

    // the working cache must not change the picture: the canvas and the exporter read the same
    // timeline, only through a different cache
    {
        const int failsBefore = gFails;
        const std::vector<uint8_t> timeCache =
            renderSceneAtScale(canvas, makeAlphaChecker(imageSize, color), setup, backdrop, 0.5f, 1.0f, false);
        const std::vector<uint8_t> workCache =
            renderSceneAtScale(canvas, makeAlphaChecker(imageSize, color), setup, backdrop, 0.5f, 1.0f, true);
        const int delta = interiorMaxDelta(timeCache, workCache, canvas, 19);
        expect(delta == 0,
            QString("the working cache must render identically to the time cache (delta %1)").arg(delta));
        caseReport("export-like: working cache == time cache", gFails == failsBefore, QString("delta=%1").arg(delta));
    }
}


//-------------------------------------------------------------------------------------------------
// M9: the REAL ctrl::Exporter. M8 covered the render configuration the exporter uses; this drives
// the exporter itself, so its own framebuffer setup, camera and PNG write are exercised.
//
// The exporter clears to TRANSPARENT black and writes straight-alpha PNGs, so the value to assert
// is the analytic one composed over black: a half-covered source shows source*coverage, and a
// straight-alpha chain dragged toward the transparent region's colour would show
// source*coverage^2 instead. PNG is straight-alpha while the framebuffer holds premultiplied
// pixels, so the composition is done here rather than comparing raw bytes.
class MipAnimator: public core::Animator {
public:
    int fixedFrame = 0;
    core::Frame currentFrame() const override { return core::Frame(fixedFrame); }
    void stop() override {}
    void suspend() override {}
    void resume() override {}
    bool isSuspended() const override { return false; }
};

class MipReporter: public util::IProgressReporter {
public:
    void setSection(const QString&) override {}
    void setMaximum(int) override {}
    void setProgress(int) override {}
    bool wasCanceled() const override { return false; }
};

void suiteExport() {
    suiteHeader("M9 real ctrl::Exporter: the exported PNG carries no halo");
    const QSize canvas(64, 64);
    const int imageSize = 64;
    const uint8_t color[3] = {200, 100, 50};
    const int margin = 19;

    MipAnimator animator;
    MipReporter reporter;

    core::Project project(QString(), animator, nullptr);
    project.attribute().setImageSize(canvas);
    project.attribute().setFps(24);
    project.attribute().setFrameRange(util::Range(0, 0));

    auto* root = new core::FolderNode("root");
    project.objectTree().grabTopNode(root);
    auto* res = makeResource("export_src", makeAlphaChecker(imageSize, color));
    addScaledLayer(
        project.objectTree(), root, res, "layer", QVector2D(canvas.width() * 0.5f, canvas.height() * 0.5f), 0.5f,
        img::BlendMode_Normal
    );

    const QString dir = QDir::temp().filePath("ae_mipmap_export");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);

    const int failsBefore = gFails;

    bool ok = false;
    {
        // the app builds a NEW Exporter per export (reuse segfaults in the pre-existing GL
        // framebuffer lifecycle, see tests/media_exporter_e2e.cpp)
        ctrl::Exporter ex(project);
        ex.setOverwriteConfirmer([](const QString&) -> bool { return true; });
        ex.setProgressReporter(reporter);
        ctrl::Exporter::CommonParam cp;
        cp.path = dir;
        cp.size = canvas;
        cp.selection = ctrl::FrameSelection::fromRange(0, 0);
        cp.fps = 24;
        ctrl::Exporter::ImageParam ip;
        ip.name = "frame";
        ip.suffix = "png";
        ip.quality = 100;
        ip.singleFile = true;
        ok = (bool)ex.execute(cp, ip);
    }
    expect(ok, "the export must succeed");

    const QStringList pngs = QDir(dir).entryList(QStringList{"*.png"}, QDir::Files);
    expect(!pngs.isEmpty(), "the export must write a PNG");
    if (pngs.isEmpty()) {
        caseReport("exported PNG", false, "no file written");
        return;
    }

    const QString path = QDir(dir).filePath(pngs.first());
    const QImage png(path);
    expect(!png.isNull(), "the exported PNG must decode");
    if (png.isNull()) {
        caseReport("exported PNG", false, "undecodable");
        return;
    }
    expect(png.size() == canvas,
        QString("exported size must be the requested %1x%2, got %3x%4")
            .arg(canvas.width()).arg(canvas.height()).arg(png.width()).arg(png.height()));

    // compose the straight-alpha PNG over black and read the interior
    const QImage rgba = png.convertToFormat(QImage::Format_RGBA8888);
    const int expected = (int)std::lround(color[0] * 0.5);   // source * coverage
    const int haloed = (int)std::lround(color[0] * 0.25);    // source * coverage^2
    double sum = 0;
    int n = 0, maxAlpha = 0;
    for (int y = margin; y < canvas.height() - margin; ++y) {
        for (int x = margin; x < canvas.width() - margin; ++x) {
            const uchar* px = rgba.constScanLine(y) + (size_t)x * 4;
            sum += px[0] * (px[3] / 255.0);   // over black
            maxAlpha = std::max(maxAlpha, (int)px[3]);
            ++n;
        }
    }
    const double mean = n ? sum / n : 0.0;

    expect(maxAlpha > 100, QString("the exported frame must contain artwork (max alpha %1)").arg(maxAlpha));
    expect(near(mean, expected, 5.0),
        QString("the exported frame must show source*coverage (%1) over black, got %2")
            .arg(expected).arg(mean, 0, 'f', 1));
    expect(std::fabs(mean - haloed) > 20.0,
        QString("the exported frame must not be the haloed %1 (got %2)").arg(haloed).arg(mean, 0, 'f', 1));

    caseReport("exported PNG via ctrl::Exporter", gFails == failsBefore,
        QString("%1, alpha<=%2, over-black=%3 (correct %4, halo %5)")
            .arg(pngs.first()).arg(maxAlpha).arg(mean, 0, 'f', 1).arg(expected).arg(haloed));

    QDir(dir).removeRecursively();
}


//-------------------------------------------------------------------------------------------------
// Figure generation: renders REAL artwork through the production pipeline so the change can be
// looked at, not only measured. Driven by --figures; the numbers in the report come from the same
// code path, but a halved red channel is not a picture.
namespace figures {

QImage readbackToImage(const std::vector<uint8_t>& aBytes, const QSize& aSize) {
    QImage img(aBytes.data(), aSize.width(), aSize.height(), QImage::Format_RGBA8888);
    return img.copy().flipped(Qt::Vertical);   // GL row 0 is the bottom
}

// collects image-bearing layers, deepest last
void collectLayers(core::ObjectNode* aNode, std::vector<core::LayerNode*>& aDest) {
    if (!aNode)
        return;
    if (auto* layer = dynamic_cast<core::LayerNode*>(aNode))
        aDest.push_back(layer);
    for (auto* child : aNode->children())
        collectLayers(child, aDest);
}

} // namespace figures

int runFigures(const QString& aDir, const QString& aTag, const QString& aAnie) {
    QDir().mkpath(aDir);

    gl::DeviceInfo dev;
    dev.vender = "harness";
    dev.renderer = "harness";
    dev.version = "5";
    dev.maxTextureSize = 16384;
    dev.maxRenderBufferSize = 16384;
    gl::DeviceInfo::setInstance(&dev);

    static MipAnimator animator;
    static MipReporter reporter;
    core::Project project(aAnie, animator, nullptr);
    ctrl::ProjectLoader loader;
    if (!loader.load(aAnie, project, dev, reporter)) {
        std::fprintf(stderr, "FATAL: cannot load %s: %s\n", qPrintable(aAnie), qPrintable(loader.log().join("; ")));
        return 3;
    }
    const QSize canvas = project.attribute().imageSize();
    std::printf("figures: %s  %dx%d\n", qPrintable(aAnie), canvas.width(), canvas.height());

    std::vector<core::LayerNode*> layers;
    figures::collectLayers(project.objectTree().topNode(), layers);
    std::printf("figures: %d layers\n", (int)layers.size());

    // a mid-grey opaque backdrop: dark halos read as darkening, light halos as brightening, and a
    // transparent backdrop would hide both
    const uint8_t backdrop[3] = {128, 128, 128};

    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(canvas.width()).arg(canvas.height());
    if (fixtures.find(key) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(canvas);
        fixtures.emplace(key, std::move(fx));
    }
    RenderFixture& fx = *fixtures[key];

    const float zooms[] = {1.0f, 0.5f, 0.25f, 0.125f};
    for (float zoom : zooms) {
        core::CameraInfo camera;
        camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
        if (zoom != 1.0f)
            camera.setScale(zoom);
        const std::vector<uint8_t> px = fx.render(project.objectTree(), camera, 0, backdrop);
        const QString path = QDir(aDir).filePath(QString("%1_zoom%2.png").arg(aTag).arg((int)(zoom * 1000)));
        figures::readbackToImage(px, canvas).save(path);
        std::printf("  wrote %s\n", qPrintable(path));
    }

    // the same frame through the real exporter, for canvas-vs-export comparison
    {
        ctrl::Exporter ex(project);
        ex.setOverwriteConfirmer([](const QString&) -> bool { return true; });
        ex.setProgressReporter(reporter);
        ctrl::Exporter::CommonParam cp;
        cp.path = aDir;
        cp.size = canvas;
        cp.selection = ctrl::FrameSelection::fromRange(0, 0);
        cp.fps = project.attribute().fps() > 0 ? project.attribute().fps() : 24;
        ctrl::Exporter::ImageParam ip;
        ip.name = aTag + "_export";
        ip.suffix = "png";
        ip.quality = 100;
        ip.singleFile = true;
        const bool ok = (bool)ex.execute(cp, ip);
        std::printf("  export %s\n", ok ? "ok" : "FAILED");
    }

    // the largest image layer: the blur and HSV figures go on a LAYER, because keys pushed on the
    // tree's top node measurably do nothing here (maxdiff 0 against the baseline), while the same
    // keys on a layer take the paths M5 verifies numerically
    core::LayerNode* bigLayer = nullptr;
    {
        int bestArea = 0;
        for (auto* layer : layers) {
            auto* key = layer->timeLine()
                            ? static_cast<core::ImageKey*>(layer->timeLine()->defaultKey(core::TimeKeyType_Image))
                            : nullptr;
            if (key && key->hasImage()) {
                const QSize sz = key->data().resource()->image().pixelSize();
                if (sz.width() * sz.height() > bestArea) {
                    bestArea = sz.width() * sz.height();
                    bigLayer = layer;
                }
            }
        }
    }

    // ---- path figures: whole-project variations, so each figure is real artwork through the
    // production path rather than a synthetic stand-in ----
    {
        auto* root = dynamic_cast<core::FolderNode*>(project.objectTree().topNode());
        if (!root) {
            std::printf("  (no folder root; skipping the path strip)\n");
        }
        core::CameraInfo camera;
        camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
        camera.setScale(0.25f);

        auto shot = [&](const QString& aName, bool aGrid) {
            const std::vector<uint8_t> px = fx.render(project.objectTree(), camera, 0, backdrop, aGrid);
            const QString path = QDir(aDir).filePath(QString("%1_path_%2.png").arg(aTag).arg(aName));
            figures::readbackToImage(px, canvas).save(path);
            std::printf("  wrote %s\n", qPrintable(path));
        };

        // baseline for the strip
        shot("plain", false);

        // grid / mesh overlay: isGrid renders the layer meshes as wireframe. GridDrawingFrag
        // outputs a constant colour and samples no texture, so mipmapping cannot reach it.
        shot("grid", true);

        // folder composite: the project root IS a folder, and a non-Normal mode forces the
        // subtree through the composite path (rendered into a FilterFrame slot, then blended)
        if (root) {
            root->setBlendMode(img::BlendMode_Multiply);
            shot("folder_multiply", false);
            root->setBlendMode(img::BlendMode_Screen);
            shot("folder_screen", false);
            root->setBlendMode(img::BlendMode_Normal);
        }

        (void)bigLayer;
    }


    // Key-based figures each get their OWN project load. Pushing a key onto a tree that has
    // already been rendered measurably does nothing (maxdiff 0 against the baseline), because the
    // time cache is not invalidated when a timeline is poked directly rather than through the
    // command stack. Loading fresh sidesteps the stale cache and keeps each figure to exactly one
    // variation.
    auto freshKeyShot = [&](const QString& aName, const std::function<void(core::ObjectNode*)>& aApply) {
        static std::vector<std::unique_ptr<MipAnimator>> animators;
        animators.emplace_back(new MipAnimator());
        core::Project fresh(aAnie, *animators.back(), nullptr);
        ctrl::ProjectLoader ld;
        if (!ld.load(aAnie, fresh, dev, reporter)) {
            std::printf("  (%s figure: reload failed)\n", qPrintable(aName));
            return;
        }
        std::vector<core::LayerNode*> freshLayers;
        figures::collectLayers(fresh.objectTree().topNode(), freshLayers);
        for (auto* layer : freshLayers)
            aApply(layer);
        core::CameraInfo camera;
        camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
        camera.setScale(0.25f);
        const std::vector<uint8_t> px = fx.render(fresh.objectTree(), camera, 0, backdrop);
        const QString path = QDir(aDir).filePath(QString("%1_path_%2.png").arg(aTag).arg(aName));
        figures::readbackToImage(px, canvas).save(path);
        std::printf("  wrote %s\n", qPrintable(path));
    };

    freshKeyShot("blur", [](core::ObjectNode* aLayer) {
        auto* key = new core::BlurKey();
        key->setAmount(12.0f);
        cmnd::Base* pusher = aLayer->timeLine()->createPusher(core::TimeKeyType_Blur, 0, key);
        pusher->tryExec();
        delete pusher;
    });

    freshKeyShot("hsv", [](core::ObjectNode* aLayer) {
        auto* key = new core::HSVKey();
        key->setHSV(QList<int>{140, 100, 100, 0});   // a large hue rotation
        cmnd::Base* pusher = aLayer->timeLine()->createPusher(core::TimeKeyType_HSV, 0, key);
        pusher->tryExec();
        delete pusher;
    });

    // ---- clipping: a real artwork layer clipped by a synthetic circular clipper. The clippee is
    // pushed first and the clipper second, because ObjectNodeUtil::isClipper is true for a node
    // whose PREVIOUS sibling is clipped. ----
    {
        img::ResourceHandle art;
        int bestArea = 0;
        for (auto* layer : layers) {
            auto* key = layer->timeLine()
                            ? static_cast<core::ImageKey*>(layer->timeLine()->defaultKey(core::TimeKeyType_Image))
                            : nullptr;
            if (key && key->hasImage()) {
                const QSize sz = key->data().resource()->image().pixelSize();
                if (sz.width() * sz.height() > bestArea) {
                    bestArea = sz.width() * sz.height();
                    art = key->data().resource();
                }
            }
        }
        if (art) {
            core::ObjectTree tree;
            auto* figRoot = new core::FolderNode("figroot");
            tree.grabTopNode(figRoot);
            const QVector2D centre(canvas.width() * 0.5f, canvas.height() * 0.5f);
            // Fill the canvas with the artwork, and give the clipper the same footprint, so the
            // figure reads at the same size as the other path figures instead of as a speck.
            const QSize artSize = art->image().pixelSize();
            const float fill = (float)canvas.width() / (float)std::max(1, artSize.width());
            const float scale = fill * 0.95f;

            auto* clippee = new core::LayerNode("clippee", tree.shaderHolder());
            clippee->setDefaultImage(art, img::BlendMode_Normal);
            clippee->setDefaultPosture(centre);
            clippee->setDefaultOpacity(1.0f);
            core::getOrCreateDefaultKey<core::ScaleKey, core::TimeKeyType_Scale>(*clippee->timeLine())
                ->setScale(QVector2D(scale, scale));
            figRoot->children().pushBack(clippee);
            clippee->setClipped(true);

            // a circular clipper, so the clipped-away region is obvious
            Img clip;
            clip.w = clip.h = 512;
            clip.d.assign((size_t)512 * 512 * 4, 0);
            for (int y = 0; y < 512; ++y)
                for (int x = 0; x < 512; ++x) {
                    const double dx = x - 256.0, dy = y - 256.0;
                    const bool in = (dx * dx + dy * dy) < 210.0 * 210.0;
                    uint8_t* q = clip.at(x, y);
                    q[0] = q[1] = q[2] = 255;
                    q[3] = in ? 255 : 0;
                }
            auto* clipper = new core::LayerNode("clipper", tree.shaderHolder());
            clipper->setDefaultImage(makeResource("fig_clipper", clip)->handle(), img::BlendMode_Normal);
            clipper->setDefaultPosture(centre);
            clipper->setDefaultOpacity(1.0f);
            const float clipScale = scale * (float)artSize.width() / 512.0f;
            core::getOrCreateDefaultKey<core::ScaleKey, core::TimeKeyType_Scale>(*clipper->timeLine())
                ->setScale(QVector2D(clipScale, clipScale));
            figRoot->children().pushBack(clipper);

            core::CameraInfo camera;
            camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
            camera.setScale(0.25f);
            const std::vector<uint8_t> px = fx.render(tree, camera, 0, backdrop);
            const QString path = QDir(aDir).filePath(QString("%1_path_clip.png").arg(aTag));
            figures::readbackToImage(px, canvas).save(path);
            std::printf("  wrote %s\n", qPrintable(path));
        } else {
            std::printf("  no image layer found for the clipping figure\n");
        }
    }

    // blend-mode / HSV / blur strip on the largest image layer, at a minifying zoom
    if (!layers.empty()) {
        // pick the layer whose image is largest: the most visible one
        core::LayerNode* target = layers.front();
        auto& ggl = gl::Global::functions();
        (void)ggl;
        for (auto* layer : layers) {
            if (layer->timeLine() && layer->timeLine()->defaultKey(core::TimeKeyType_Image)) {
                auto* key = static_cast<core::ImageKey*>(layer->timeLine()->defaultKey(core::TimeKeyType_Image));
                if (key && key->hasImage()) {
                    const QSize sz = key->data().resource()->image().pixelSize();
                    if (target == layers.front() || sz.width() * sz.height() > 0)
                        target = layer;
                }
            }
        }
        std::printf("figures: path strip on layer '%s'\n", qPrintable(target->name()));

        const img::BlendMode modes[] = {
            img::BlendMode_Normal, img::BlendMode_Multiply, img::BlendMode_Screen, img::BlendMode_Overlay,
            img::BlendMode_Luminosity, img::BlendMode_AddGlow,
        };
        for (img::BlendMode mode : modes) {
            target->setBlendMode(mode);   // also re-reserves the shader variant for this mode
            core::CameraInfo camera;
            camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
            camera.setScale(0.25f);
            const std::vector<uint8_t> px = fx.render(project.objectTree(), camera, 0, backdrop);
            const QString path = QDir(aDir).filePath(QString("%1_mode%2.png").arg(aTag).arg((int)mode));
            figures::readbackToImage(px, canvas).save(path);
            std::printf("  wrote %s\n", qPrintable(path));
        }
        target->setBlendMode(img::BlendMode_Normal);
    }

    return 0;
}


//-------------------------------------------------------------------------------------------------
// Supersampled ground truth.
//
// Everything in M1-M9 compares the render against something DERIVED from the same machinery: an
// analytic value for a synthetic pattern, or the naive implementation. Neither answers "is the
// minified render CORRECT on real artwork" - the analytic cases are constant-colour patterns, and
// a fixed-vs-naive differential cannot see an error both implementations share.
//
// The independent reference is a supersample: render the scene at ss x the target resolution, so
// the sampler is at or above 1:1 and uses no mip level at all, then box-average ss x ss blocks down
// to the target. That converges on the true integral of the scene over each destination pixel - what
// a minified render is supposed to approximate. Disagreement is then a real defect, not a change.
//
// The residual is honest and worth stating: at finite ss the supersample is itself an approximation
// (the level-0 lookup is bilinear, not an exact area integral), so it converges on the reference
// rather than equalling it. ss is chosen well above the minification factor so the ss render is
// magnifying, not minifying.
QImage boxDownsample(const QImage& aSrc, int aSS) {
    const QImage src = aSrc.convertToFormat(QImage::Format_RGBA8888);
    const int w = src.width() / aSS, h = src.height() / aSS;
    QImage dst(w, h, QImage::Format_RGBA8888);
    const int n = aSS * aSS;
    for (int y = 0; y < h; ++y) {
        uchar* o = dst.scanLine(y);
        for (int x = 0; x < w; ++x) {
            int acc[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < aSS; ++dy) {
                const uchar* r = src.constScanLine(y * aSS + dy) + (size_t)x * aSS * 4;
                for (int dx = 0; dx < aSS; ++dx)
                    for (int c = 0; c < 4; ++c)
                        acc[c] += r[dx * 4 + c];
            }
            for (int c = 0; c < 4; ++c)
                o[x * 4 + c] = (uchar)((acc[c] + n / 2) / n);
        }
    }
    return dst;
}

// renders the project at ss x the canvas with the same field of view as the 1x figure, and returns
// the box-downsampled result at canvas size
QImage supersampledReference(core::Project& aProject, const QSize& aCanvas, float aZoom, int aSS,
                             const uint8_t aBackdrop[3]) {
    const QSize big(aCanvas.width() * aSS, aCanvas.height() * aSS);
    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(big.width()).arg(big.height());
    if (fixtures.find(key) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(big);
        fixtures.emplace(key, std::move(fx));
    }
    RenderFixture& fx = *fixtures[key];

    // The camera maps world units to screen pixels by mScale ALONE (CameraInfo::toScreenVector),
    // and anchors content on centerOffset() = imageSize/2. So to render the same field of view at
    // ss x: keep imageSize at the CANVAS (that is what the artwork is centred on), place the canvas
    // centre at the centre of the larger frame via leftTopPos, and multiply the scale by ss. Passing
    // the big size as imageSize instead - which is what this did first - halves the artwork into a
    // corner at 1/ss scale and makes the "reference" meaningless.
    core::CameraInfo camera;
    camera.reset(
        big, 1.0, aCanvas,
        QPoint((big.width() - aCanvas.width()) / 2, (big.height() - aCanvas.height()) / 2)
    );
    camera.setScale(aZoom * (float)aSS);
    const std::vector<uint8_t> px = fx.render(aProject.objectTree(), camera, 0, aBackdrop);
    return boxDownsample(figures::readbackToImage(px, big), aSS);
}

int runSupersample(const QString& aDir, const QString& aTag, const QString& aAnie, int aSS) {
    QDir().mkpath(aDir);
    gl::DeviceInfo dev;
    dev.vender = "harness";
    dev.renderer = "harness";
    dev.version = "5";
    dev.maxTextureSize = 16384;
    dev.maxRenderBufferSize = 16384;
    gl::DeviceInfo::setInstance(&dev);

    static MipAnimator animator;
    static MipReporter reporter;
    core::Project project(aAnie, animator, nullptr);
    ctrl::ProjectLoader loader;
    if (!loader.load(aAnie, project, dev, reporter)) {
        std::fprintf(stderr, "FATAL: cannot load %s\n", qPrintable(aAnie));
        return 3;
    }
    const QSize canvas = project.attribute().imageSize();
    const uint8_t backdrop[3] = {128, 128, 128};
    std::printf("supersample: %s  %dx%d  ss=%d\n", qPrintable(aAnie), canvas.width(), canvas.height(), aSS);

    const float zooms[] = {1.0f, 0.5f, 0.25f};
    for (float zoom : zooms) {
        // the 1x mipmapped render, exactly as the figures produce it
        auto& fixtures = renderFixtures();
        const QString key = QString("%1x%2").arg(canvas.width()).arg(canvas.height());
        if (fixtures.find(key) == fixtures.end()) {
            std::unique_ptr<RenderFixture> fx(new RenderFixture());
            fx->init(canvas);
            fixtures.emplace(key, std::move(fx));
        }
        core::CameraInfo camera;
        camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
        if (zoom != 1.0f)
            camera.setScale(zoom);
        const QImage mip = figures::readbackToImage(
            fixtures[key]->render(project.objectTree(), camera, 0, backdrop), canvas
        );
        const QImage ref = supersampledReference(project, canvas, zoom, aSS, backdrop);

        const QString z = QString::number((int)(zoom * 1000));
        mip.save(QDir(aDir).filePath(QString("%1_mip_zoom%2.png").arg(aTag).arg(z)));
        ref.save(QDir(aDir).filePath(QString("%1_ref_zoom%2.png").arg(aTag).arg(z)));
        std::printf("  wrote %s mip/ref at zoom %s\n", qPrintable(aTag), qPrintable(z));
    }
    return 0;
}


//-------------------------------------------------------------------------------------------------
// Per-triangle LOD scan.
//
// The one measurement the rest of the harness cannot make. UVs interpolate linearly within a
// triangle, so the derivative the sampler sees is CONSTANT per triangle and trilinear filtering
// picks one blend per triangle: if adjacent triangles land on different levels, the boundary
// between them is a blurriness step. Before mipmapping every layer sampled level 0, where such
// steps cannot exist - so this is the one way the change could plausibly make things worse.
//
// This computes, per triangle, the same rho GL derives, from the real deformed mesh the render
// just used, and reports the spread across the mesh and across EDGE-ADJACENT triangle pairs. A
// large spread, or a large adjacent-pair delta, is what a visible seam looks like numerically.
struct LodStats {
    int triangles = 0;
    double minLod = 0, maxLod = 0, meanLod = 0;
    double maxNeighbourDelta = 0;   // max |dLOD| across an edge-adjacent pair
    double meanNeighbourDelta = 0;
    int neighbours = 0;
    bool valid = false;
};

LodStats scanLayerLod(const core::LayerNode& aLayer, const core::CameraInfo& aCamera, const QSize& aViewport) {
    LodStats st;
    const core::LayerMesh* mesh = aLayer.currentMesh();
    if (!mesh || mesh->vertexCount() <= 0 || mesh->indexCount() < 3)
        return st;

    // No texture-size lookup is needed: LayerMesh::texCoords() are in TEXEL units (the vertex shader
    // divides by uImageSize to normalise), so the Jacobian below is already texels per screen pixel
    // and log2(rho) is the level directly. This is why the validation reports exactly 2.000 for a
    // 1:1 layer at camera scale 0.25 - rho is 4 texels per pixel.
    const QMatrix4x4 vm = aCamera.viewMatrix();
    const gl::Vector3* pos = mesh->positions();
    const gl::Vector2* uv = mesh->texCoords();
    const GLuint* idx = mesh->indices();
    const int icount = mesh->indexCount();

    // world -> screen pixels, mirroring LayerDrawingVert
    auto toScreen = [&](const gl::Vector3& p) {
        const QVector4D v = vm * QVector4D(p.x, p.y, p.z, 1.0f);
        const float w = (v.w() != 0.0f) ? v.w() : 1.0f;
        const float nx = v.x() / w, ny = v.y() / w;
        return QVector2D(0.5f * aViewport.width() * (nx + 1.0f), 0.5f * aViewport.height() * (ny + 1.0f));
    };

    std::vector<double> lods;
    std::map<std::pair<int, int>, std::vector<int>> edgeToTris;   // undirected edge -> triangles
    std::vector<std::array<int, 3>> triVerts;

    for (int t = 0; t + 2 < icount; t += 3) {
        const int i0 = (int)idx[t], i1 = (int)idx[t + 1], i2 = (int)idx[t + 2];
        if (i0 >= mesh->vertexCount() || i1 >= mesh->vertexCount() || i2 >= mesh->vertexCount())
            continue;

        const QVector2D s0 = toScreen(pos[i0]), s1 = toScreen(pos[i1]), s2 = toScreen(pos[i2]);
        const QVector2D e1 = s1 - s0, e2 = s2 - s0;
        const double det = (double)e1.x() * e2.y() - (double)e1.y() * e2.x();
        if (std::fabs(det) < 1e-9) {   // degenerate on screen: contributes no derivative
            lods.push_back(0.0);
            triVerts.push_back({i0, i1, i2});
            continue;
        }
        // UV in texels
        const QVector2D T0(uv[i0].x, uv[i0].y), T1(uv[i1].x, uv[i1].y), T2(uv[i2].x, uv[i2].y);
        const QVector2D f1 = T1 - T0, f2 = T2 - T0;
        // J = [f1 f2] * inv([e1 e2])  -> du/dscreen, in texels per screen pixel
        const double inv = 1.0 / det;
        const double j00 = (f1.x() * e2.y() - f2.x() * e1.y()) * inv;
        const double j01 = (f2.x() * e1.x() - f1.x() * e2.x()) * inv;
        const double j10 = (f1.y() * e2.y() - f2.y() * e1.y()) * inv;
        const double j11 = (f2.y() * e1.x() - f1.y() * e2.x()) * inv;
        const double gx = std::sqrt(j00 * j00 + j10 * j10);
        const double gy = std::sqrt(j01 * j01 + j11 * j11);
        const double rho = std::max(gx, gy);
        lods.push_back(rho > 0.0 ? std::log2(rho) : -20.0);
        triVerts.push_back({i0, i1, i2});

        const int tri = (int)triVerts.size() - 1;
        const int vs[3] = {i0, i1, i2};
        for (int e = 0; e < 3; ++e) {
            int a = vs[e], b = vs[(e + 1) % 3];
            if (a > b) std::swap(a, b);
            edgeToTris[{a, b}].push_back(tri);
        }
    }

    if (lods.empty())
        return st;
    st.valid = true;
    st.triangles = (int)lods.size();
    st.minLod = *std::min_element(lods.begin(), lods.end());
    st.maxLod = *std::max_element(lods.begin(), lods.end());
    double sum = 0;
    for (double v : lods)
        sum += v;
    st.meanLod = sum / lods.size();

    double nsum = 0;
    for (const auto& kv : edgeToTris) {
        if (kv.second.size() != 2)
            continue;
        const double d = std::fabs(lods[kv.second[0]] - lods[kv.second[1]]);
        st.maxNeighbourDelta = std::max(st.maxNeighbourDelta, d);
        nsum += d;
        ++st.neighbours;
    }
    st.meanNeighbourDelta = st.neighbours ? nsum / st.neighbours : 0.0;
    return st;
}

void collectLayerNodes(core::ObjectNode* aNode, std::vector<const core::LayerNode*>& aDest) {
    if (!aNode)
        return;
    if (auto* l = dynamic_cast<const core::LayerNode*>(aNode))
        aDest.push_back(l);
    for (auto* c : aNode->children())
        collectLayerNodes(c, aDest);
}

int runLodScan(const QString& aAnie, int aFrame) {
    gl::DeviceInfo dev;
    dev.vender = "harness";
    dev.renderer = "harness";
    dev.version = "5";
    dev.maxTextureSize = 16384;
    dev.maxRenderBufferSize = 16384;
    gl::DeviceInfo::setInstance(&dev);

    static std::vector<std::unique_ptr<MipAnimator>> animators;
    animators.emplace_back(new MipAnimator());
    animators.back()->fixedFrame = aFrame;
    core::Project project(aAnie, *animators.back(), nullptr);
    ctrl::ProjectLoader loader;
    static MipReporter reporter;
    if (!loader.load(aAnie, project, dev, reporter)) {
        std::fprintf(stderr, "FATAL: cannot load %s\n", qPrintable(aAnie));
        return 3;
    }
    const QSize canvas = project.attribute().imageSize();
    const uint8_t backdrop[3] = {128, 128, 128};

    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(canvas.width()).arg(canvas.height());
    if (fixtures.find(key) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(canvas);
        fixtures.emplace(key, std::move(fx));
    }
    // render at a minifying zoom so the mip levels are actually in play
    core::CameraInfo camera;
    camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
    camera.setScale(0.25f);
    fixtures[key]->render(project.objectTree(), camera, aFrame, backdrop);

    std::vector<const core::LayerNode*> nodes;
    collectLayerNodes(project.objectTree().topNode(), nodes);

    std::printf("lodscan %s frame %d  canvas %dx%d  layers %d\n", qPrintable(aAnie), aFrame,
                canvas.width(), canvas.height(), (int)nodes.size());
    double worstNeighbour = 0, worstSpread = 0;
    QString worstName;
    int scanned = 0;
    for (const auto* layer : nodes) {
        const LodStats st = scanLayerLod(*layer, camera, canvas);
        if (!st.valid)
            continue;
        ++scanned;
        const double spread = st.maxLod - st.minLod;
        worstNeighbour = std::max(worstNeighbour, st.maxNeighbourDelta);
        if (spread > worstSpread) {
            worstSpread = spread;
            worstName = layer->name();
        }
        // printed unconditionally: a scan that reports a constant needs its absolute values
        // visible, or "spread 0.00" is indistinguishable from a computation returning garbage
        std::printf("  %-28s tris=%4d lod %6.3f..%6.3f (mean %6.3f)  adjDelta max=%.3f mean=%.3f\n",
                    qPrintable(layer->name()), st.triangles, st.minLod, st.maxLod, st.meanLod,
                    st.maxNeighbourDelta, st.meanNeighbourDelta);
    }
    std::printf("  scanned %d meshes | worst spread %.2f ('%s') | worst adjacent delta %.2f\n",
                scanned, worstSpread, qPrintable(worstName), worstNeighbour);
    return 0;
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

    // --lodscan <anie> <frame>
    if (argc > 3 && QString(argv[1]) == "--lodscan") {
        const int rc = runLodScan(
            QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]).toInt()
        );
        clearRenderFixtures();
        return rc;
    }

    // --supersample <dir> <tag> <anie> <ss>
    if (argc > 5 && QString(argv[1]) == "--supersample") {
        const int rc = runSupersample(
            QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]), QString::fromLocal8Bit(argv[4]),
            QString::fromLocal8Bit(argv[5]).toInt()
        );
        clearRenderFixtures();
        return rc;
    }

    // --figures <dir> <tag> <anie>
    if (argc > 4 && QString(argv[1]) == "--figures") {
        const int rc = runFigures(
            QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]), QString::fromLocal8Bit(argv[4])
        );
        clearRenderFixtures();
        return rc;
    }

    suiteChain();
    suiteImageKey();
    suiteRender();
    suiteBlendModes();
    suitePaths();
    suiteMipLevels();
    suiteLodUniformity();
    suiteExportLike();
    suiteExport();

    std::printf("\n%d checks, %d failures\n", gChecks, gFails);
    std::fflush(stdout);

    // tear GL objects down while the context is still current
    clearRenderFixtures();
    return gFails ? 1 : 0;
}
