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
#include <QFileInfo>
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
#include "ctrl/ImageFileLoader.h"
#include "ctrl/ProjectLoader.h"
#include "gl/Global.h"
#include "gl/Texture.h"
#include "gl/VertexArrayObject.h"
#include "gui/AudioPlaybackWidget.h"
#include "img/ResourceData.h"
#include "util/IProgressReporter.h"
#include "img/ResourceNode.h"
#include "img/Util.h"

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
// M10: WHERE the sampler reads. Every check above asks whether the mip CHAIN and the shader's
// handling of it are right. Not one of them asks whether the renderer reads that chain at the
// right PLACE, and the two questions are independent: a perfectly built chain is still blurred by
// texture coordinates that land between texels.
//
// The measurement needs no assumption about anybody's texel convention. A bilinear filter returns
// a texel UNCHANGED only when the sample position is that texel's centre; at any other position it
// averages it with a neighbour. So take a source whose value alternates every texel and render it
// 1:1 onto a canvas of exactly its size - the picture the user expects is the source pattern, texel
// for pixel. If it comes back grey, the renderer is reading between texels. That is a fact about
// the render, not about a convention.

// kind 0: 1-texel checkerboard (tests both axes), 1: period-2 stripes in x, 2: in y,
// 3: 2x2-block checkerboard - period 4 texels, which is exactly one texel of mip level 1.
Img makeOpaquePattern(int aSize, int aKind) {
    Img img;
    img.w = img.h = aSize;
    img.d.assign((size_t)aSize * aSize * 4, 0);
    for (int y = 0; y < aSize; ++y) {
        for (int x = 0; x < aSize; ++x) {
            bool on = false;
            switch (aKind) {
            case 0: on = ((x + y) % 2) == 0; break;
            case 1: on = (x % 2) == 0; break;
            case 2: on = (y % 2) == 0; break;
            default: on = (((x / 2) + (y / 2)) % 2) == 0; break;
            }
            uint8_t* p = img.at(x, y);
            p[0] = p[1] = p[2] = on ? 255 : 0;
            // Fully opaque on purpose: every cell of the grid mesh exists, and reduceBurrs leaves
            // every vertex at its exact grid position (its 3x3 alpha test always finds alpha), so
            // the geometry contributes no displacement of its own to what is being measured.
            p[3] = 255;
        }
    }
    return img;
}

struct ChannelStats {
    int minV = 255;
    int maxV = 0;
    double mean = 0;
    int n = 0;
    int contrast() const { return maxV - minV; }
};

ChannelStats channelStats(const std::vector<uint8_t>& aImg, const QSize& aSize, int aMargin, int aChannel) {
    ChannelStats st;
    double sum = 0;
    for (int y = aMargin; y < aSize.height() - aMargin; ++y) {
        for (int x = aMargin; x < aSize.width() - aMargin; ++x) {
            const int v = aImg[((size_t)y * aSize.width() + x) * 4 + aChannel];
            st.minV = std::min(st.minV, v);
            st.maxV = std::max(st.maxV, v);
            sum += v;
            ++st.n;
        }
    }
    st.mean = st.n ? sum / st.n : 0.0;
    return st;
}

// What a bilinear filter returns for a period-2 pattern whose sample position has fractional part
// aPhase in texel coordinates. aPhase 0.5 is a texel centre (full contrast, the pattern survives);
// aPhase 0 is exactly between two texels (the two are averaged, contrast collapses to zero).
// Derived by hand from the two taps - not fitted to the measurement.
double predictedPeriod2Contrast(double aPhase) {
    const double d = std::fabs(aPhase - std::floor(aPhase));
    const double m = std::min(d, 1.0 - d);   // distance to the nearest texel boundary
    return 255.0 * 2.0 * m;                  // 0 at a boundary, 255 at a texel centre
}

// A bilinear read of aLevel at texel coordinate (aTX, aTY), clamped at the edges. This is the CPU
// replica of GL_LINEAR used to turn "the blur looks like X" into "the blur IS exactly X": the two
// candidate positions differ, so matching one of them identifies the sample position.
double bilinearAt(const Img& aLevel, double aTX, double aTY, int aChannel) {
    const double fx = aTX - 0.5, fy = aTY - 0.5;
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const double wx = fx - x0, wy = fy - y0;
    const auto at = [&](int x, int y) {
        x = std::max(0, std::min(aLevel.w - 1, x));
        y = std::max(0, std::min(aLevel.h - 1, y));
        return (double)aLevel.at(x, y)[aChannel];
    };
    return (1.0 - wy) * ((1.0 - wx) * at(x0, y0) + wx * at(x0 + 1, y0))
         + wy * ((1.0 - wx) * at(x0, y0 + 1) + wx * at(x0 + 1, y0 + 1));
}

// Copies a source (straight alpha, opaque) into the level-0 reference the comparisons read.
Img asLevel(const Img& aSrc) { return aSrc; }

// The expected chain: what a correct box pyramid over aTexture would hold at aLevel, computed on
// the CPU from the same texels the GPU was given. Level l+1 is the exact 2x2 box of level l (M12
// measured glGenerateMipmap doing precisely that), so this is not a model of a driver - it is the
// definition, evaluated independently of the rendering path. Requires every side to divide by 2^l,
// which is what mipAlignedTextureSize guarantees; the assertion is the point, not a convenience:
// a side that does not divide is exactly the stretched-grid defect this models.
Img boxLevel(const Img& aLevel0, int aLevel) {
    XC_ASSERT(aLevel >= 0);
    Img cur = aLevel0;
    for (int l = 0; l < aLevel; ++l) {
        XC_ASSERT(cur.w % 2 == 0 && cur.h % 2 == 0);
        Img next;
        next.w = cur.w / 2;
        next.h = cur.h / 2;
        next.d.assign((size_t)next.w * next.h * 4, 0);
        for (int y = 0; y < next.h; ++y) {
            for (int x = 0; x < next.w; ++x) {
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                            sum += cur.at(x * 2 + dx, y * 2 + dy)[c];
                    next.at(x, y)[c] = (uint8_t)((sum + 2) / 4);
                }
            }
        }
        cur = next;
    }
    return cur;
}

void suiteTexelPhase() {
    suiteHeader("M10 texel phase: is a 1:1 render read at texel centres?");
    const QSize canvas(64, 64);
    const int margin = 6;

    const char* kKindName[4] = {"1-texel checkerboard", "period-2 stripes in x", "period-2 stripes in y",
                                "2x2-block checkerboard"};

    for (int kind = 0; kind < 3; ++kind) {
        const Img src = makeOpaquePattern(canvas.width(), kind);
        // scale 1.0 and the layer centred: the artwork fills the canvas exactly, texel for pixel.
        const std::vector<uint8_t> out = renderLayer(canvas, src, 1.0f, img::BlendMode_Normal);
        const ChannelStats st = channelStats(out, canvas, margin, 0);
        const double measured = (double)st.contrast();
        const int caseFails = gFails;
        const QString label = QString("M10 %1 at 1:1").arg(kKindName[kind]);

        // An aligned render reads each texel at its centre, so the source pattern - which is what
        // the user sees in any image viewer at 100% - survives at full contrast. Anything less is
        // the renderer averaging texels that the user can see are different.
        expect(measured >= 250.0,
            QString("%1: the source pattern must survive at full contrast (got %2, min %3 max %4)")
                .arg(label).arg(measured, 0, 'f', 1).arg(st.minV).arg(st.maxV));
        expect(near(st.mean, 127.5, 12.0),
            QString("%1: the mean must stay the pattern's own mean 127.5 (got %2)").arg(label).arg(st.mean, 0, 'f', 1));

        // ...and the loss, if any, is quantified as a phase error rather than described
        const double phaseTexels = measured / 510.0;             // contrast = 510 * phase
        const double offCentre = std::fabs(0.5 - phaseTexels);   // distance from the nearest centre
        caseReport(
            label, gFails == caseFails,
            QString("contrast=%1 of 255, mean=%2 -> samples %3 texels off the nearest centre")
                .arg(measured, 0, 'f', 1).arg(st.mean, 0, 'f', 2).arg(offCentre, 0, 'f', 3)
        );
    }

    // The same measurement as a function of the layer's sub-texel position. This is what makes the
    // result a PHASE and not an accident of one position: moving the layer by a quarter of a texel
    // must move the sample position by a quarter of a texel, so contrast must follow the predicted
    // V - full at the aligned position, zero a half texel away.
    {
        const Img src = makeOpaquePattern(canvas.width(), 1);
        double measuredQ[4] = {0, 0, 0, 0};
        double predictedQ[4] = {0, 0, 0, 0};
        for (int q = 0; q < 4; ++q) {
            const float pos = 32.0f + q * 0.25f;
            core::ObjectTree tree;
            auto* root = new core::FolderNode("root");
            tree.grabTopNode(root);
            auto* res = makeResource(QString("mip_phase_%1").arg(q), src);
            addScaledLayer(tree, root, res, "layer", QVector2D(pos, pos), 1.0f, img::BlendMode_Normal);
            core::CameraInfo camera;
            camera.reset(canvas, 1.0, canvas, QPoint(0, 0));

            auto& fixtures = renderFixtures();
            const QString key = QString("%1x%2").arg(canvas.width()).arg(canvas.height());
            if (fixtures.find(key) == fixtures.end()) {
                std::unique_ptr<RenderFixture> fx(new RenderFixture());
                fx->init(canvas);
                fixtures.emplace(key, std::move(fx));
            }
            const uint8_t black[3] = {0, 0, 0};
            const std::vector<uint8_t> out = fixtures[key]->render(tree, camera, 0, black);
            measuredQ[q] = (double)channelStats(out, canvas, margin, 0).contrast();

            // The sample position the vertex shader asks for, in texel coordinates: the shader
            // divides a texel-unit coordinate by the image size, so the sampler reads
            // (texel - 0.5) and the fractional part of what it reads is what sets the contrast.
            // The layer moved by q/4 of a texel, so the position moves by the same amount.
            predictedQ[q] = predictedPeriod2Contrast(0.0 - q * 0.25 + 0.5);
        }

        const int caseFails = gFails;
        // The layer exactly filling the canvas is the aligned case, so it must be the sharpest of
        // the four, and the profile must be the predicted V around it.
        expect(measuredQ[0] >= 250.0,
            QString("M10 phase sweep: the layer exactly filling the canvas must be the sharpest case (got %1)")
                .arg(measuredQ[0], 0, 'f', 1));
        expect(measuredQ[0] >= measuredQ[1] && measuredQ[0] >= measuredQ[3],
            QString("M10 phase sweep: no sub-texel offset may be sharper than the aligned one (%1 vs %2, %3)")
                .arg(measuredQ[0], 0, 'f', 1).arg(measuredQ[1], 0, 'f', 1).arg(measuredQ[3], 0, 'f', 1));
        caseReport(
            "M10 phase sweep (stripe pattern, layer at 32 + q/4 texels)", gFails == caseFails,
            QString("measured %1 %2 %3 %4 | predicted %5 %6 %7 %8 for a sample at the texel position")
                .arg(measuredQ[0], 0, 'f', 1).arg(measuredQ[1], 0, 'f', 1).arg(measuredQ[2], 0, 'f', 1)
                .arg(measuredQ[3], 0, 'f', 1)
                .arg(predictedQ[0], 0, 'f', 1).arg(predictedQ[1], 0, 'f', 1).arg(predictedQ[2], 0, 'f', 1)
                .arg(predictedQ[3], 0, 'f', 1)
        );
    }
}

//-------------------------------------------------------------------------------------------------
// M11: why 2x minification got WORSE. This is the mechanism, isolated.
//
// At exactly 2x minification the correct answer for each output pixel is the 2x2 box of level 0 -
// which is precisely one texel of mip level 1. So on a source that is constant inside every 2x2
// block, a correct renderer picks level 1 and reads the texel whose box it wants: contrast
// preserved, 255. Half a texel of error in the texture coordinate moves the level-1 read a quarter
// of a level-1 texel off that texel's centre, which averages it with its neighbour - a 0.75/0.25
// mix of two blocks that the user can see are different, and the over-blur that the corpus
// measured at 2x.
//
// The two candidates are 64 units of red apart, so which one the render matches is not a judgement
// call: it is a measurement of where the sample lands.
void suiteTwoXMechanism() {
    suiteHeader("M11 2x minification: which texel does the level-1 read land on?");
    const QSize canvas(64, 64);
    const float scale = 0.5f;
    const Img src = makeOpaquePattern(canvas.width(), 3);
    const Img lvl1 = downsamplePremultiplied(src);   // opaque source, so this is the plain box

    const std::vector<uint8_t> out = renderLayer(canvas, src, scale, img::BlendMode_Normal);

    // The layer is centred, so it spans screen [16, 48) in both axes; keep clear of its border.
    const int lo = 20, hi = 44;
    double worstVsBox = 0, worstVsPhase = 0, worstVsFixed = 0;
    int boxContrast = 0, renderContrast = 0;
    int rMin = 255, rMax = 0, bMin = 255, bMax = 0;
    for (int sy = lo; sy < hi; ++sy) {
        for (int sx = lo; sx < hi; ++sx) {
            // image coordinate of this output pixel's centre: the layer covers 32 screen pixels
            // with 64 texels, so one screen pixel is 2 texels. The raw readback's row 0 is the
            // canvas BOTTOM while the source image's row 0 is its top, so the vertical texel
            // coordinate is mirrored - and on a pattern whose value depends on y parity a mirror
            // image is a different pattern, not the same one shifted. The artwork's top edge is
            // screen row 48, so its own row coordinate for buffer row sy is (48 - (sy + 0.5)) * 2.
            const double u = 2.0 * (sx + 0.5) - 32.0;
            const double v = 95.0 - 2.0 * sy;   // mirrored into source row index

            const double box = bilinearAt(asLevel(src), u, v, 0);
            const double phase = bilinearAt(lvl1, (u - 0.5) / 2.0, (v - 0.5) / 2.0, 0);
            const double fixed = bilinearAt(lvl1, u / 2.0, v / 2.0, 0);

            const int got = out[((size_t)sy * canvas.width() + sx) * 4];
            worstVsBox = std::max(worstVsBox, std::fabs(got - box));
            worstVsPhase = std::max(worstVsPhase, std::fabs(got - phase));
            worstVsFixed = std::max(worstVsFixed, std::fabs(got - fixed));

            rMin = std::min(rMin, got);
            rMax = std::max(rMax, got);
            bMin = std::min(bMin, (int)box);
            bMax = std::max(bMax, (int)box);
        }
    }
    renderContrast = rMax - rMin;
    boxContrast = bMax - bMin;

    // The two hypotheses this started from, kept as measurements because neither may be quietly
    // assumed away: a level-1 read a quarter texel off its centre would give this, and a read at
    // the aligned centre would give that. The render matches NEITHER, which is the point - it is
    // the box itself, so the level-1 texel is read at its centre and the chain is being used
    // exactly. (This is the no-margin geometry; with the 1-texel margin the same render lands
    // half a level-1 texel off and does match the quarter-phase mix - see M13.)
    const int caseFails = gFails;
    const QString prefix = "M11 2x minification of a 2x2-block checkerboard";
    expect(worstVsPhase > 30.0,
        QString("%1: the render must NOT be a quarter-texel level-1 mix (that delta is %2)")
            .arg(prefix).arg(worstVsPhase, 0, 'f', 1));
    expect(worstVsFixed <= 6.0,
        QString("%1: the render must be the level-1 texel whose box it wants (delta %2)")
            .arg(prefix).arg(worstVsFixed, 0, 'f', 1));

    // THE ASSERTION: the output pixel's footprint is exactly one level-1 texel, so the correct
    // render is the box - 255 of contrast, matching it pixel for pixel.
    expect(worstVsBox <= 6.0,
        QString("%1: each output pixel must be the exact 2x2 box = one level-1 texel (worst delta %2)")
            .arg(prefix).arg(worstVsBox, 0, 'f', 1));
    expect(renderContrast >= 250,
        QString("%1: the block pattern must survive (contrast %2 of %3, box gives %4)")
            .arg(prefix).arg(renderContrast).arg(255).arg(boxContrast));
    // Non-vacuity: the two candidates really are far apart on this pattern
    expect(boxContrast >= 250,
        QString("%1: the candidates must be distinguishable (box contrast %2)").arg(prefix).arg(boxContrast));

    caseReport(
        prefix, gFails == caseFails,
        QString("render contrast %1 (box %2) | delta vs box %3, vs level-1 quarter-phase %4, "
                "vs aligned level-1 %5")
            .arg(renderContrast).arg(boxContrast)
            .arg(worstVsBox, 0, 'f', 1).arg(worstVsPhase, 0, 'f', 1).arg(worstVsFixed, 0, 'f', 1)
    );

    // A block-uniform pattern cannot see a sub-texel error: every 2x2 block averages to itself, so
    // the check above pins the BLOCK the read lands in, not the position inside it. A 1-texel
    // checkerboard is the complement - its exact 2x minification is uniformly 127.5, so ANY phase
    // error shows up as the pattern reappearing at level 1.
    {
        const Img fine = makeOpaquePattern(canvas.width(), 0);
        const std::vector<uint8_t> out2 = renderLayer(canvas, fine, scale, img::BlendMode_Normal);
        const ChannelStats st = channelStats(out2, canvas, lo, 0);
        const int fails2 = gFails;
        expect(st.contrast() <= 3,
            QString("M11 1-texel checkerboard at 2x: the exact answer is uniform, a phase error "
                    "reappears as pattern (contrast %1, values %2..%3)")
                .arg(st.contrast()).arg(st.minV).arg(st.maxV));
        expect(near(st.mean, 127.5, 2.0),
            QString("M11 1-texel checkerboard at 2x: must average to 127.5 (got %1)").arg(st.mean, 0, 'f', 2));
        caseReport(
            "M11 1-texel checkerboard at 2x", gFails == fails2,
            QString("contrast %1 of 255 (a quarter-texel level-1 phase would give 127), mean %2")
                .arg(st.contrast()).arg(st.mean, 0, 'f', 2)
        );
    }
}

//-------------------------------------------------------------------------------------------------
// M13: the padding parity, isolated. This is a defect the corpus found at 2x, reproduced under
// control, and it is a two-line difference: the same artwork, the same sampler, the same zoom, and
// only the width of a transparent margin padded around the artwork inside its texture changes.
//
// The mip chain is built over the PADDED image, so its texel grids are anchored to the padded
// image's (0,0). Level 1 is a box of 2 base texels, so it is aligned to EVEN base texel indices:
// the artwork's own aligned 2x2 blocks fall inside single level-1 texels only if the artwork's
// origin sits at an even index. With a margin of 1 the origin is at texel 1, every level-1 texel
// is half artwork and half nothing, and a 2x minified pixel - whose footprint IS one level-1 texel
// - is answered by a 0.75/0.25 mix of two of them, i.e. four base texels instead of two. The
// shipped code pads by nothing at all: the artwork sits at the texture origin, which is a multiple
// of every 2^l (see img::Util::prepareForSampling). M14 generalises this to every level.
void suiteMarginParity() {
    suiteHeader("M13 texture margin parity: the 2x over-blur, isolated");
    const QSize canvas(64, 64);
    const int content = 64;
    const int lo = 20, hi = 44;

    const char* kLabels[3] = {"margin 0 (no padding - shipped)", "margin 1 (the old padding)", "margin 8"};
    const int kMargins[3] = {0, 1, 8};

    for (int i = 0; i < 3; ++i) {
        const int margin = kMargins[i];
        // The artwork: a 1-texel cross. One texel wide, so the exact 2x2 box that covers an arm
        // holds one bright texel of four (64) and a straddled pair of level-1 texels spreads that
        // over twice the area at half the value - a difference no rounding can explain away.
        Img src;
        src.w = src.h = content + 2 * margin;
        src.d.assign((size_t)src.w * src.h * 4, 0);
        for (int y = 0; y < src.h; ++y) {
            for (int x = 0; x < src.w; ++x) {
                uint8_t* p = src.at(x, y);
                const int cx = content / 2 + margin, cy = content / 2 + margin;
                const bool on = (x == cx || y == cy);
                const bool inside = (margin <= x && x < src.w - margin && margin <= y && y < src.h - margin);
                p[0] = p[1] = p[2] = (on && inside) ? 255 : 0;
                p[3] = inside ? 255 : 0;   // transparent margin, exactly as the padding produces
            }
        }

        const std::vector<uint8_t> out = renderLayer(canvas, src, 0.5f, img::BlendMode_Normal);

        // The exact answer: the 2x2 box of the artwork, read off the source. The layer is centred
        // and covers 32 screen pixels, so screen pixel sx samples artwork texel coordinate
        // 2 * (sx + 0.5) - 32, and the sampler's coordinate is that plus the margin.
        double worst = 0;
        for (int sy = lo; sy < hi; ++sy) {
            for (int sx = lo; sx < hi; ++sx) {
                const double u = 2.0 * (sx + 0.5) - 32.0 + margin;
                const double v = (95.0 - 2.0 * sy) + margin;
                const double box = bilinearAt(src, u, v, 0);
                const int got = out[((size_t)sy * canvas.width() + sx) * 4];
                worst = std::max(worst, std::fabs(got - box));
            }
        }
        const ChannelStats st = channelStats(out, canvas, lo, 0);

        const int caseFails = gFails;
        const QString label = QString("M13 2x minification, %1").arg(kLabels[i]);
        if (margin % 2 == 0) {
            expect(worst <= 3.0,
                QString("%1: an even margin aligns level 1 with the artwork, so the render is the "
                        "exact 2x2 box (worst delta %2)").arg(label).arg(worst, 0, 'f', 1));
        } else {
            // ...and this is the assertion that would fail the moment the margin is made odd
            // again: the same artwork, rendered by the same code, stops being the box.
            expect(worst > 20.0,
                QString("%1: an ODD margin misaligns level 1 and the render stops being the exact "
                        "box (worst delta %2 - if this is small the mechanism has changed)")
                    .arg(label).arg(worst, 0, 'f', 1));
        }
        caseReport(
            label, gFails == caseFails,
            QString("delta vs the exact 2x2 box %1, contrast %2 of 255").arg(worst, 0, 'f', 1).arg(st.contrast())
        );
    }
}


//-------------------------------------------------------------------------------------------------
// M14: the margin's arithmetic at EVERY level, not just level 1.
//
// M13 pins the level-1 case: a margin of 1 puts the artwork at an odd base texel, and a 2x minified
// pixel - whose footprint IS one level-1 texel - is answered by a mix of two of them instead.
//
// The general statement is that level l is a box of 2^l base texels whose grid is anchored to
// multiples of 2^l IN THE PADDED TEXTURE, so the artwork's origin has to be a multiple of 2^l. It
// is not enough for the margin to be even: a padding of 8 is 0 modulo 2^l only for l <= 3, and
// levels 4 and 5 (16x and 32x minification) were therefore misaligned as well - a read of them
// straddles two boxes exactly like M13's margin-1 case does one level up. That is why the shipped
// code now pads by nothing: the artwork's origin is the texture's origin, a multiple of every 2^l.
//
// Measured here at every level against the exact 2^l x 2^l box of the artwork, which is the answer
// a footprint-aligned screen pixel must get. Level 0 is the control: its footprint is one texel and
// no mip level is involved, so it validates the artwork-to-screen mapping independently of any
// mipmap behaviour - a wrong mapping cannot pass the control and blame the chain.
void suiteMarginDepth() {
    suiteHeader("M14 level-l alignment: the margin must be a multiple of 2^l, not merely even");
    const QSize canvas(64, 64);
    const int content = 512;   // 512 texels: at 128x minification the artwork still covers 4 pixels
    const int kLevels = 8;     // l = 0..7, i.e. 1x to 128x minification
    const int kMargins[4] = {0, 8, 16, 32};
    const char* kMarginLabels[4] = {"margin 0 (shipped)", "margin 8", "margin 16", "margin 32"};
    const char* kPatternLabels[3] = {"hard edge", "1-texel cross", "random"};

    // The three source patterns, all opaque (so premultiplying is the identity and the render's red
    // channel IS the filtered red): a full-scale edge, whose transition is a step the width of one
    // level-l texel and so is displaced by a straddling read at every level; a 1-texel cross, the
    // sharpest instrument at level 1; and pseudo-random, which gives the realistic magnitude.
    std::vector<uint8_t> pat[3];
    pat[0].resize((size_t)content * content);
    pat[1].resize((size_t)content * content);
    pat[2].resize((size_t)content * content);
    uint32_t rng = 12345u;
    for (int y = 0; y < content; ++y) {
        for (int x = 0; x < content; ++x) {
            const size_t i = (size_t)y * content + x;
            pat[0][i] = (x < content / 2) ? 255 : 0;
            pat[1][i] = (x == content / 2 || y == content / 2) ? 255 : 0;
            rng = rng * 1664525u + 1013904223u;
            pat[2][i] = (uint8_t)(rng >> 24);
        }
    }

    for (int l = 0; l < kLevels; ++l) {
        const int step = 1 << l;   // 2^l base texels per screen pixel, i.e. per level-l texel
        const float zoom = 1.0f / (float)step;
        for (int mi = 0; mi < 4; ++mi) {
            const int margin = kMargins[mi];
            const bool aligned = (margin % step) == 0;

            double worst[3] = {0, 0, 0};
            double mean[3] = {0, 0, 0};
            int counts[3] = {0, 0, 0};
            int caseFails = gFails;

            for (int pi = 0; pi < 3; ++pi) {
                Img src;
                src.w = src.h = content + 2 * margin;
                src.d.assign((size_t)src.w * src.h * 4, 0);
                for (int y = 0; y < content; ++y) {
                    for (int x = 0; x < content; ++x) {
                        uint8_t* p = src.at(x + margin, y + margin);
                        p[0] = p[1] = p[2] = pat[pi][(size_t)y * content + x];
                        p[3] = 255;
                    }
                }

                const std::vector<uint8_t> out = renderLayer(canvas, src, zoom, img::BlendMode_Normal);

                // The layer is centred on the canvas centre and the camera maps world (0,0) to the
                // canvas origin, so the screen pixel centre (sx + 0.5, H - 0.5 - sy) sits at artwork
                // coordinate ((p - canvas/2) / zoom + content/2), and its footprint is the 2^l x 2^l
                // block centred there. The readback is bottom-up, hence the flip in y.
                for (int sy = 0; sy < canvas.height(); ++sy) {
                    for (int sx = 0; sx < canvas.width(); ++sx) {
                        const double acx =
                            (sx + 0.5) * step - (canvas.width() * 0.5) * step + content * 0.5;
                        const double acy =
                            (canvas.height() - 0.5 - sy) * step - (canvas.height() * 0.5) * step + content * 0.5;
                        const int bx = (int)std::lround(acx - step * 0.5);
                        const int by = (int)std::lround(acy - step * 0.5);
                        if (bx < 0 || by < 0 || bx + step > content || by + step > content)
                            continue;   // a footprint that reaches outside the artwork has partial
                                        // coverage and is not the property under test
                        double box = 0;
                        for (int dy = 0; dy < step; ++dy)
                            for (int dx = 0; dx < step; ++dx)
                                box += src.at(bx + dx + margin, by + dy + margin)[0];
                        box /= (double)(step * step);

                        const int got = out[((size_t)sy * canvas.width() + sx) * 4];
                        const double d = std::fabs(got - box);
                        worst[pi] = std::max(worst[pi], d);
                        mean[pi] += d;
                        ++counts[pi];
                    }
                }

                if (counts[pi] == 0)
                    expect(false, QString("M14 level %1, %2: no fully covered footprint to measure")
                                      .arg(l).arg(kMarginLabels[mi]));
                else
                    mean[pi] /= counts[pi];
            }

            // The edge pattern is the instrument: a straddling read displaces the edge's transition,
            // so an aligned margin leaves the box exact to rounding and a misaligned one cannot.
            const double edge = worst[0];
            const QString label = QString("M14 level %1 (%2x minification), %3")
                                      .arg(l).arg(step).arg(kMarginLabels[mi]);

            if (aligned || l == 0) {
                expect(edge <= 3.0,
                    QString("%1: the margin is %2 modulo 2^%3, so the read lands on a level-%4 texel "
                            "centre and the render must be the exact %5x%5 box (worst delta %6, %7 "
                            "pixels)")
                        .arg(label).arg(margin).arg(l).arg(l).arg(step)
                        .arg(edge, 0, 'f', 1).arg(counts[0]));
            } else {
                // The negative check: if a misaligned level starts matching the box, this
                // measurement has stopped having power and the arithmetic above is wrong.
                expect(edge > 20.0,
                    QString("%1: the margin is %2 modulo 2^%3, so the read straddles two level-%4 "
                            "boxes and must NOT be the exact box (worst delta %5 - if this is small "
                            "the mechanism has changed)")
                        .arg(label).arg(margin).arg(l).arg(l).arg(edge, 0, 'f', 1));
            }

            caseReport(
                label, gFails == caseFails,
                QString("%1%2 | worst delta vs the exact %3x%3 box: %4 edge, %5 cross, %6 random | "
                        "mean: %7 / %8 / %9 | %10 pixels")
                    .arg(aligned ? "aligned" : "MISALIGNED")
                    .arg(l == 0 ? " (control: no level involved)" : "")
                    .arg(step).arg(worst[0], 0, 'f', 1).arg(worst[1], 0, 'f', 1).arg(worst[2], 0, 'f', 1)
                    .arg(mean[0], 0, 'f', 2).arg(mean[1], 0, 'f', 2).arg(mean[2], 0, 'f', 2)
                    .arg(counts[0])
            );
            (void)kPatternLabels;
        }
    }

    // A second, independent condition on the same grids, measured against the whole expected chain
    // rather than against a hand-picked box. Level l has floor(size/2^l) texels over normalized
    // [0,1], so unless the size is a multiple of 2^l the level grid is STRETCHED over the artwork
    // (each texel covers slightly more than 2^l base texels, the displacement growing to
    // size mod 2^l base texels at the far edge) and the chain drops the last size mod 2^l base
    // texels of that axis entirely, so the artwork's last rows and columns are represented by the
    // neighbouring block instead of by themselves. Both are fixed by uploading a texture whose size
    // divides evenly (img::Util::mipAlignedTextureSize, applied by ImageKey), which is what this
    // measures: the render must equal boxLevel() of the texture the app would upload, sampled
    // bilinearly at the footprint centre, with the artwork's own last texel included.
    {
        const int kSizes[4] = {512, 504, 500, 513};
        for (int l = 2; l <= 5; ++l) {
            const int step = 1 << l;
            const float zoom = 1.0f / (float)step;
            for (int si = 0; si < 4; ++si) {
                const int size = kSizes[si];
                for (int mi = 0; mi < 2; ++mi) {
                    const int margin = (mi == 0) ? 0 : 8;
                    Img src;
                    src.w = src.h = size + 2 * margin;
                    src.d.assign((size_t)src.w * src.h * 4, 0);
                    uint32_t r = 99991u;
                    for (int y = 0; y < size; ++y) {
                        for (int x = 0; x < size; ++x) {
                            uint8_t* p = src.at(x + margin, y + margin);
                            r = r * 1664525u + 1013904223u;
                            p[0] = p[1] = p[2] = (uint8_t)(r >> 24);
                            p[3] = 255;
                        }
                    }

                    // the buffer the app uploads: the image padded up to the aligned texture size,
                    // transparent to the right and bottom
                    const QSize texSize = img::Util::mipAlignedTextureSize(QSize(src.w, src.h));
                    Img texture;
                    texture.w = texSize.width();
                    texture.h = texSize.height();
                    texture.d.assign((size_t)texture.w * texture.h * 4, 0);
                    for (int y = 0; y < src.h; ++y)
                        for (int x = 0; x < src.w; ++x)
                            for (int c = 0; c < 4; ++c)
                                texture.at(x, y)[c] = src.at(x, y)[c];

                    const std::vector<uint8_t> out = renderLayer(canvas, src, zoom, img::BlendMode_Normal);
                    // the expected level, built ONCE: expectedChainSample() rebuilds the whole
                    // pyramid from level 0, which per pixel is quadratic
                    const Img level = boxLevel(texture, l);
                    double worst = 0, sum = 0;
                    int n = 0, worstSx = -1, worstSy = -1;
                    for (int sy = 0; sy < canvas.height(); ++sy) {
                        for (int sx = 0; sx < canvas.width(); ++sx) {
                            const double acx =
                                (sx + 0.5) * step - (canvas.width() * 0.5) * step + size * 0.5;
                            const double acy =
                                (canvas.height() - 0.5 - sy) * step - (canvas.height() * 0.5) * step
                                + size * 0.5;
                            // Fully covered footprints only, i.e. the interior. The outermost row
                            // and column of the layer are excluded because the layer's MESH insets
                            // its border vertices, so the sample position there is not the one the
                            // geometry implies - every suite here excludes that band (M6's crop, the
                            // golden suite's borderCrop). What happens at the artwork's own last
                            // rows and columns is not measured through the render at all but in the
                            // chain, by M16, where no mesh is involved.
                            const int bx = (int)std::lround(acx - step * 0.5);
                            const int by = (int)std::lround(acy - step * 0.5);
                            if (bx < 0 || by < 0 || bx + step > size || by + step > size)
                                continue;
                            const double expected =
                                bilinearAt(level, (acx + margin) / (double)step,
                                           (acy + margin) / (double)step, 0);
                            const double d =
                                std::fabs(out[((size_t)sy * canvas.width() + sx) * 4] - expected);
                            if (d > worst) {
                                worst = d;
                                worstSx = sx;
                                worstSy = sy;
                            }
                            sum += d;
                            ++n;
                        }
                    }
                    if (n)
                        sum /= n;
                    const QString label = QString("M14 size %1, level %2 (%3x), %4")
                                              .arg(size).arg(l).arg(step)
                                              .arg(mi == 0 ? "margin 0" : "margin 8");
                    expect(worst <= 12.0 && n > 100,
                        QString("%1: the render must be the expected chain over the texture the app "
                                "uploads (worst delta %2, %3 pixels)")
                            .arg(label).arg(worst, 0, 'f', 1).arg(n));
                    caseReport(
                        label,
                        worst <= 12.0 && n > 100,
                        QString("size = %1*%2 + %3, uploaded %4x%4 | worst delta vs the expected "
                                "chain %5 (at %6,%7), mean %8, %9 pixels")
                            .arg(size / step).arg(step).arg(size % step).arg(texSize.width())
                            .arg(worst, 0, 'f', 1).arg(worstSx).arg(worstSy)
                            .arg(sum, 0, 'f', 2).arg(n)
                    );
                }
            }
        }
    }
}


//-------------------------------------------------------------------------------------------------
// M15: is the level actually selected lambda = -log2(zoom), at zooms that are not powers of two?
//
// M6/M11 pin the integer zooms, where the level is unambiguous; between them trilinear blends two
// levels, and that is where a level-selection error hides. The instrument is the step response of a
// hard edge: a screen pixel's value is the kernel convolved with the step, sampled at the pixel
// centre, so the edge profile's steepest step measures the kernel's width, and the kernel's width
// IS 2^lambda base texels - one footprint. For the ideal chain the kernel is a blend of the two
// neighbouring box filters, so the whole profile can be predicted from the zoom alone:
//   lambda = -log2(zoom), l = floor(lambda), t = lambda - l
//   K = (1-t)*box(2^l * zoom px) + t*box(2^(l+1) * zoom px), E = step response of K
//   predicted value at pixel centre x = E(x - edge)
// The comparison is against that prediction rather than against lambda directly, because the
// steepest-step statistic is itself biased between levels (it is a discrete sample of a
// piecewise-linear profile); comparing the same statistic on both sides removes the bias instead of
// hiding it in a tolerance. Measured at 15 zooms from 0.9 down to 0.05, i.e. lambda from 0.15 to
// 4.3, including the whole of levels 1, 2, 3 and 4.
void suiteLodSweep() {
    suiteHeader("M15 level selection across a continuous zoom sweep");
    const QSize canvas(64, 64);
    const int content = 1024;   // long enough that the half-plane edge is at the canvas centre at
                                // every zoom in the sweep
    Img src;
    src.w = src.h = content;
    src.d.assign((size_t)content * content * 4, 0);
    for (int y = 0; y < content; ++y) {
        for (int x = 0; x < content; ++x) {
            uint8_t* p = src.at(x, y);
            p[0] = p[1] = p[2] = (x < content / 2) ? 0 : 255;
            p[3] = 255;
        }
    }

    const float kZooms[15] = {0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.42f, 0.35f, 0.3f, 0.25f,
                              0.2f, 0.17f, 0.125f, 0.1f, 0.071f, 0.0625f};

    for (int zi = 0; zi < 15; ++zi) {
        const double zoom = kZooms[zi];
        const double lambda = -std::log2(zoom);
        const int l = (int)std::floor(lambda);
        const double t = lambda - l;
        const double w1 = std::pow(2.0, l) * zoom;       // kernel widths in screen pixels
        const double w2 = std::pow(2.0, l + 1) * zoom;

        // step response of the box blend, per axis, in normalised units
        const auto boxStep = [](double aX, double aW) {
            const double half = aW * 0.5;
            if (aX <= -half)
                return 0.0;
            if (aX >= half)
                return 1.0;
            return (aX + half) / aW;
        };
        const auto profile = [&](double aX) {
            return 255.0 * ((1.0 - t) * boxStep(aX, w1) + t * boxStep(aX, w2));
        };

        const std::vector<uint8_t> out = renderLayer(canvas, src, zoom, img::BlendMode_Normal);

        // the artwork's centre - the edge - lands on the canvas centre, a pixel boundary
        const double edge = canvas.width() * 0.5;
        int measured = 0, predicted = 0;
        for (int sy = 8; sy < canvas.height() - 8; ++sy) {
            for (int sx = 8; sx < canvas.width() - 8; ++sx) {
                const int a = out[((size_t)sy * canvas.width() + sx) * 4];
                const int b = out[((size_t)sy * canvas.width() + sx + 1) * 4];
                const int mp = (int)std::lround(profile(sx + 1.5 - edge) - profile(sx + 0.5 - edge));
                measured = std::max(measured, std::abs(b - a));
                predicted = std::max(predicted, std::abs(mp));
            }
        }

        // the width the measurement implies, for the record: w = 255 / steepest step
        const double wMeasured = measured > 0 ? 255.0 / measured : 0.0;
        const double lambdaFromWidth = wMeasured > 0 ? std::log2(wMeasured / zoom) : -1.0;

        const QString label = QString("M15 zoom %1 (2^-%2)").arg(zoom, 0, 'f', 4).arg(lambda, 0, 'f', 2);
        const int caseFails = gFails;
        expect(std::abs(measured - predicted) <= 12,
            QString("%1: the edge's steepest step must be the one the ideal chain predicts for "
                    "lambda = -log2(zoom) (measured %2, predicted %3 of 255)")
                .arg(label).arg(measured).arg(predicted));
        expect(l == (int)std::floor(-std::log2(zoom)) && measured > 0,
            QString("%1: sanity - the sweep must actually be sampling a level").arg(label));
        caseReport(
            label, gFails == caseFails,
            QString("lambda=%1 level %2 blend %3 | kernel %4..%5 px | steepest step measured %6 "
                    "predicted %7 | width implies lambda %8")
                .arg(lambda, 0, 'f', 3).arg(l).arg(t, 0, 'f', 3)
                .arg(w1, 0, 'f', 3).arg(w2, 0, 'f', 3)
                .arg(measured).arg(predicted).arg(lambdaFromWidth, 0, 'f', 2)
        );
    }
}

//-------------------------------------------------------------------------------------------------
// M16: are the artwork's own last rows and columns represented by the deepest levels?
//
// This is the third condition on the level grids and it is arithmetic, so it is checked as
// arithmetic - and then checked once on the GPU, on the chain itself, where no mesh geometry or
// sampling position can confuse it. Level l has max(1, floor(size / 2^l)) texels, which together
// cover base texels [0, 2^l * floor(size / 2^l)) of that axis. Unless the TEXTURE size is a
// multiple of 2^l that is less than the artwork, so the last (size mod 2^l) base texels of the
// axis are not in the level at all: the chain dropped them, and the renderer represents that strip
// of the artwork with the neighbouring block. The artwork's origin also has to stay at the
// texture's origin (M14), which is why the fix is to pad the texture to the right and bottom
// (img::Util::mipAlignedTextureSize) rather than to add a ring on all four sides.
void suiteDeepCoverage() {
    suiteHeader("M16 deep-level coverage of the artwork's last rows and columns");
    const int kSizes[4] = {512, 513, 500, 504};
    const int kMargins[2] = {0, 8};
    const int kMaxLevel = 7;   // the cap mipAlignedTextureSize documents

    for (int si = 0; si < 4; ++si) {
        const int size = kSizes[si];
        for (int mi = 0; mi < 2; ++mi) {
            const int margin = kMargins[mi];
            const int imageSide = size + 2 * margin;
            const QSize aligned = img::Util::mipAlignedTextureSize(QSize(imageSide, imageSide));

            // what the texture covers at level l, as base texels, for the size actually uploaded
            // and for the size the code used to upload (the image itself)
            int alignedCover[kMaxLevel + 1] = {0};
            int unalignedCover[kMaxLevel + 1] = {0};
            for (int l = 0; l <= kMaxLevel; ++l) {
                const int lev = 1 << l;
                alignedCover[l] = lev * std::max(1, aligned.width() / lev);
                unalignedCover[l] = lev * std::max(1, imageSide / lev);
            }
            const int contentEnd = margin + size;   // one past the artwork's last texel

            const int caseFails = gFails;
            const QString label = QString("M16 artwork %1 texels, margin %2").arg(size).arg(margin);

            expect(aligned.width() >= imageSide && aligned.width() % 32 == 0,
                QString("%1: the uploaded texture (%2) must contain the image (%3) and divide by "
                        "2^l up to the cap")
                    .arg(label).arg(aligned.width()).arg(imageSide));

            // What the unpadded upload would have lost, if anything. The old code's image carried
            // a ring, so its size was larger than the artwork and the floor rule was harmless for
            // the margin cases; with no ring the image IS the artwork, and any size that is not a
            // multiple of 2^l loses the artwork's own last rows and columns to the deepest levels.
            int worstLoss = 0;
            QString lossAt = "nothing (the size divides evenly at every level)";
            for (int l = 1; l <= kMaxLevel; ++l) {
                if (unalignedCover[l] < contentEnd) {
                    const int lost = contentEnd - unalignedCover[l];
                    if (lost > worstLoss) {
                        worstLoss = lost;
                        lossAt = QString("level %1 (%2x) would have covered %3 of %4 texels, dropping "
                                         "the artwork's last %5 rows and columns")
                                     .arg(l).arg(1 << l).arg(unalignedCover[l]).arg(contentEnd).arg(lost);
                    }
                }
            }
            // Only the shipped configuration has power here: the old code's ring made its image
            // larger than the artwork, so it covered the artwork (and paid for it with the
            // misalignment M14 measures) - with no ring the image is the artwork, and an
            // unaligned size drops the artwork's own last rows and columns.
            if (mi == 0 && size % (1 << kMaxLevel) != 0)
                expect(worstLoss > 0,
                    QString("%1: an image side of %2 texels does not divide by the level widths, so "
                            "the unpadded upload had to be dropping artwork texels - if it was not, "
                            "this measurement has no power")
                        .arg(label).arg(imageSide));

            caseReport(
                label, gFails == caseFails,
                QString("uploaded %1x%1, every level 1..%2 covers the artwork whole (%3 texels "
                        "including its last row and column); unpadded it would have been %4")
                    .arg(aligned.width()).arg(kMaxLevel).arg(contentEnd).arg(lossAt)
            );
        }
    }

    // ...and once on the GPU, on the chain the app's upload actually produces, so the arithmetic
    // above is anchored to what glGenerateMipmap is handed. Level l of a correctly sized texture is
    // the exact box of the texels below it, including the artwork's last row and column.
    {
        const int size = 513;   // odd, and 1 modulo every 2^l: the worst case for the floor rule
        const int margin = 0;
        const QSize aligned = img::Util::mipAlignedTextureSize(QSize(size + 2 * margin, size + 2 * margin));
        Img src;
        src.w = aligned.width();
        src.h = aligned.height();
        src.d.assign((size_t)src.w * src.h * 4, 0);
        uint32_t r = 7u;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                uint8_t* p = src.at(x + margin, y + margin);
                r = r * 1664525u + 1013904223u;
                p[0] = p[1] = p[2] = (uint8_t)(r >> 24);
                p[3] = 255;
            }
        }
        // the artwork's very last row and column, marked so that dropping them is visible
        for (int i = 0; i < size; ++i) {
            uint8_t* px = src.at(margin + size - 1, margin + i);
            px[0] = px[1] = px[2] = 255;
            uint8_t* py = src.at(margin + i, margin + size - 1);
            py[0] = py[1] = py[2] = 255;
        }

        const std::vector<Img> levels = readChain(src, true);
        const int checkLevels = std::min<int>(3, (int)levels.size() - 1);
        expect(levels.size() >= 2, QString("M16 the texture must have a chain (got %1 levels)").arg(levels.size()));
        for (int l = 1; l <= checkLevels; ++l) {
            const Img expected = boxLevel(src, l);
            int worst = 0;
            for (int y = 0; y < expected.h; ++y)
                for (int x = 0; x < expected.w; ++x)
                    for (int c = 0; c < 4; ++c)
                        worst = std::max(worst, std::abs((int)levels[l].at(x, y)[c] - (int)expected.at(x, y)[c]));
            expect(worst <= 2,
                QString("M16 level %1 of the uploaded %2x%2 texture must be the exact box of the "
                        "artwork plus its transparent padding (worst delta %3)")
                    .arg(l).arg(aligned.width()).arg(worst));
        }
        caseReport(
            QString("M16 chain of a %1-texel artwork in a %2x%2 texture").arg(size).arg(aligned.width()),
            true,
            QString("%1 levels, levels 1..%2 match boxLevel %3").arg(levels.size()).arg(checkLevels)
                .arg("(the last row and column are inside the chain)")
        );
    }
}

//-------------------------------------------------------------------------------------------------
// M12: the SHAPE of glGenerateMipmap's filter, measured rather than assumed.
//
// M2 checks level 1 against a hand-written 2x2 box - but it does so on a CHECKERBOARD, and a
// checkerboard is the one pattern that cannot tell two symmetric filters apart: every aligned
// 2x2 block of it averages to the same 127.5, and so does every aligned 4x4 block, so a driver
// whose level 1 is a 4x4 box passes a 2x2-box comparison exactly. If the filter is wider than a
// box, minification is softer than the ground truth, which is precisely the 2x over-blur the
// corpus measured - and this is the measurement that can see it.
//
// A single bright texel is the whole kernel: push an impulse through the chain and level l IS the
// filter's footprint at that level, scaled by the normalization. A 2x2 box answers with exactly
// one texel holding a quarter of the impulse; anything wider spreads it.
void suiteMipKernel() {
    suiteHeader("M12 glGenerateMipmap filter shape (impulse response)");
    const int size = 64;
    Img src;
    src.w = src.h = size;
    src.d.assign((size_t)size * size * 4, 0);
    for (int i = 3; i < (int)src.d.size(); i += 4)
        src.d[i] = 255;                       // opaque, so premultiplying is the identity
    uint8_t* imp = src.at(size / 2, size / 2);
    imp[0] = imp[1] = imp[2] = 255;           // one bright texel at (32, 32)

    const std::vector<Img> levels = readChain(src, true);

    for (int l = 1; l <= 3 && l < (int)levels.size(); ++l) {
        const Img& lv = levels[l];
        const int ex = (size / 2) >> l, ey = (size / 2) >> l;   // where the impulse lands
        int nonzero = 0;
        int peak = 0;
        double total = 0;
        for (int i = 0; i < (int)lv.d.size(); i += 4) {
            const int v = lv.d[i];
            total += v;
            if (v > 0) {
                ++nonzero;
                peak = std::max(peak, v);
            }
        }
        // The footprint, printable: a 5x5 neighbourhood around the impulse
        QString foot;
        for (int y = ey + 2; y >= ey - 2; --y) {     // print top row first, as the image reads
            for (int x = ex - 2; x <= ex + 2; ++x) {
                const bool in = x >= 0 && x < lv.w && y >= 0 && y < lv.h;
                foot += QString("%1 ").arg(in ? lv.at(x, y)[0] : 0, 3);
            }
            foot += "| ";
        }

        const int caseFails = gFails;
        const QString label = QString("M12 level %1 impulse response").arg(l);
        // A box filter of width 2 keeps exactly one texel per level, holding 1/4 of the impulse
        // (255/4 = 64 at level 1, 255/16 = 16 at level 2, 255/64 = 4 at level 3).
        const double expected = 255.0 / std::pow(4.0, l);
        expect(nonzero == 1,
            QString("%1: a 2x2 box keeps exactly one texel per level, this one spreads over %2")
                .arg(label).arg(nonzero));
        expect(near(peak, expected, 2.0),
            QString("%1: the single tap must hold %2, got %3").arg(label).arg(expected, 0, 'f', 1).arg(peak));
        expect(near(total, expected, 2.0),
            QString("%1: the filter must be normalized (total %2, expected %3)")
                .arg(label).arg(total, 0, 'f', 1).arg(expected, 0, 'f', 1));
        caseReport(
            label, gFails == caseFails,
            QString("nonzero %1, peak %2 (box would be %3, total %4) | %5")
                .arg(nonzero).arg(peak).arg(expected, 0, 'f', 1).arg(total, 0, 'f', 1).arg(foot)
        );
    }
}

// M12b: the same question at the level the render actually uses. The impulse is a point sample;
// real artwork is broadband, so check the thing that matters - is level 1 of a HIGH-FREQUENCY
// image the 2x2 box, to the last bit? The checkerboard in M2 is invariant to kernel width; this
// pseudo-random image is not, and the maximum deviation over the level says how far the driver's
// filter is from the box on content that has detail at every frequency.
void suiteMipKernelRandom() {
    suiteHeader("M12b level 1 of a broadband image vs the exact 2x2 box");
    const int size = 128;
    Img src;
    src.w = src.h = size;
    src.d.assign((size_t)size * size * 4, 0);
    uint32_t rng = 0x12345678u;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint8_t* p = src.at(x, y);
            rng = rng * 1664525u + 1013904223u;
            p[0] = (uint8_t)(rng >> 24);
            p[1] = (uint8_t)(rng >> 16);
            p[2] = (uint8_t)(rng >> 8);
            p[3] = 255;   // opaque: the box is then also the premultiplied box, so no branch
        }
    }

    const std::vector<Img> levels = readChain(src, true);
    const Img box1 = downsamplePremultiplied(src);
    const Img box2 = downsamplePremultiplied(box1);

    const int d1 = maxAnyChannelDelta(levels[1].d, box1.d);
    const int d2 = maxAnyChannelDelta(levels[2].d, box2.d);
    const double m1 = [&] {
        double s = 0;
        int n = 0;
        for (size_t i = 0; i < levels[1].d.size() && i < box1.d.size(); ++i) {
            s += std::fabs((double)levels[1].d[i] - (double)box1.d[i]);
            ++n;
        }
        return n ? s / n : 0.0;
    }();

    const int caseFails = gFails;
    expect(d1 <= 2,
        QString("M12b level 1 must be the exact 2x2 box of a broadband image (worst channel delta %1)").arg(d1));
    expect(d2 <= 2,
        QString("M12b level 2 must be the exact box of level 1 (worst channel delta %1)").arg(d2));
    caseReport(
        "M12b 128x128 pseudo-random image", gFails == caseFails,
        QString("level 1 vs box: max %1, mean %2 | level 2 vs box: max %3").arg(d1).arg(m1, 0, 'f', 3).arg(d2)
    );
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

// Loads a project from any supported source. A .anie is a saved project (ProjectLoader); a .psd,
// .ora or plain image is imported into a fresh one (ImageFileLoader, with the .ora prompt forced
// off because a harness has nobody to answer it).
bool loadAnyProject(const QString& aPath, core::Project& aProject, gl::DeviceInfo& aDev,
                    util::IProgressReporter& aReporter) {
    if (QFileInfo(aPath).suffix().toLower() == QLatin1String("anie")) {
        ctrl::ProjectLoader loader;
        if (!loader.load(aPath, aProject, aDev, aReporter)) {
            std::fprintf(stderr, "FATAL: %s: %s\n", qPrintable(aPath), qPrintable(loader.log().join("; ")));
            return false;
        }
        return true;
    }
    ctrl::ImageFileLoader loader(aDev);
    loader.setOraImportMode(ctrl::ImageFileLoader::OraImportMode::Layered);
    if (!loader.load(aPath, aProject, aReporter)) {
        std::fprintf(stderr, "FATAL: %s: %s\n", qPrintable(aPath), qPrintable(loader.log()));
        return false;
    }
    return true;
}

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
    if (!loadAnyProject(aAnie, project, dev, reporter))
        return 3;
    const QSize canvas = project.attribute().imageSize();
    {
        // an importer can leave a pending GL error behind; the next error-checked GL call (every
        // gl::Texture::create) then aborts inside XC_ASSERT. Report the code instead of guessing.
        auto& ggl0 = gl::Global::functions();
        GLenum e = GL_NO_ERROR;
        int n = 0;
        while ((e = ggl0.glGetError()) != GL_NO_ERROR && n < 6) {
            std::printf("  pending GL error after load: 0x%04x\n", (unsigned)e);
            ++n;
        }
    }
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
// The camera's leftTopPos that makes a supersampled render's screen 0 coincide with the left edge
// of shipped pixel aX0, so the two pixel grids line up exactly instead of half a pixel apart.
//
// CameraInfo: screen(world) = center + scale * (world - imageSize/2), center = leftTopPos +
// imageSize/2. So the shipped render (scale z, leftTopPos 0) and the supersampled one (scale z*ss,
// leftTopPos L) satisfy screenS = ss*screen + L + (imageSize/2)*(1 - ss). Requiring screenS(aX0) = 0
// gives L = (imageSize/2)*(ss-1) - ss*aX0, which is an integer only when (imageSize/2)*(ss-1) is -
// for an odd canvas dimension that forces an ODD ss. Getting this wrong by rounding is a half-pixel
// shift between the render and the truth it is compared against, which on edge-heavy artwork reads
// as an error of 40+ of 255 in both chains and quietly invalidates the comparison; the winref_delta
// column below is what catches it.
int referenceLeftTop(float aCanvasW, float aCanvasH, int aSS, int aX0, int aY0, QPoint& aOut) {
    const double cx = aCanvasW * 0.5, cy = aCanvasH * 0.5;
    const double lx = cx * (aSS - 1) - (double)aSS * aX0;
    const double ly = cy * (aSS - 1) - (double)aSS * aY0;
    const double rx = std::fabs(lx - std::lround(lx)), ry = std::fabs(ly - std::lround(ly));
    aOut = QPoint((int)std::lround(lx), (int)std::lround(ly));
    return (int)std::lround(std::max(rx, ry) * 255.0);   // the misalignment that is left, in 1/255 px
}

QImage supersampledReference(core::Project& aProject, const QSize& aCanvas, float aZoom, int& aSS,
                             const uint8_t aBackdrop[3]) {
    // The supersampled target is canvas*ss, and canvas*ss can exceed what the device will allocate:
    // a 1938x2531 PSD at ss=8 wants 15504x20248, past the 16384 limit, and the failure surfaces as
    // an XC_ASSERT deep inside the first gl::Texture::create rather than as a clear "too big".
    // Clamp ss to what actually fits and report it, rather than failing or silently under-sampling.
    {
        auto& ggl = gl::Global::functions();
        GLint maxTex = 0, maxRb = 0;
        ggl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        ggl.glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRb);
        const int limit = std::min(maxTex > 0 ? (int)maxTex : 16384, maxRb > 0 ? (int)maxRb : 16384);
        // Step by two, not one: the caller picks ss so that (imageSize/2)*(ss-1) is an integer,
        // which is what lets the supersampled grid align with the shipped one. Decrementing by one
        // would flip that parity and silently shift the reference by half a pixel.
        while (aSS > 1
               && (aCanvas.width() * aSS > limit || aCanvas.height() * aSS > limit)) {
            aSS = std::max(1, aSS - 2);
        }
        if (aSS < 1)
            aSS = 1;
    }
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
    // ss x: keep imageSize at the CANVAS (that is what the artwork is centred on), shift the world
    // so shipped pixel 0 lands on this frame's pixel 0 via leftTopPos, and multiply the scale by
    // ss. Passing the big size as imageSize instead - which is what this did first - halves the
    // artwork into a corner at 1/ss scale and makes the "reference" meaningless.
    QPoint leftTop;
    referenceLeftTop(aCanvas.width(), aCanvas.height(), aSS, 0, 0, leftTop);
    core::CameraInfo camera;
    camera.reset(big, 1.0, aCanvas, leftTop);
    camera.setScale(aZoom * (float)aSS);
    const std::vector<uint8_t> px = fx.render(aProject.objectTree(), camera, 0, aBackdrop);
    return boxDownsample(figures::readbackToImage(px, big), aSS);
}



// The three sampler states a comparison needs. Reached through the public ImageKey::cache() rather
// than a mutation, so every variant comes from ONE binary and only the sampler differs - nothing
// else can account for a difference between the panels.
enum class Sampling {
    Level0,           // what shipped before the change: no chain, level-0 minification, aliases
    MipmapCutoff,     // shipped: mipmapped, GL_TEXTURE_MAX_LEVEL pinned to the chain that exists
    MipmapNoCutoff,   // mipmapped, MAX_LEVEL left at GL's default 1000 - the clamp removed
    // The level-4 read FORCED, whatever the zoom would have picked. At exactly 16x the shipped
    // render reads level 4 anyway, so this panel is the chain's ideal output for that footprint and
    // nothing else: if it matches the shipped render, then whatever separates either of them from
    // the supersampled truth is the box filter itself and not the sampling. Without this there is
    // no way to see the difference between "the chain is wrong" and "the box is not the integral".
    ForceLevel4,
};

const char* samplingName(Sampling aSampling) {
    switch (aSampling) {
    case Sampling::Level0: return "level-0 (pre-change)";
    case Sampling::MipmapCutoff: return "mipmapped (shipped)";
    case Sampling::MipmapNoCutoff: return "mipmapped, no MAX_LEVEL cutoff";
    case Sampling::ForceLevel4: return "level 4 forced (the ideal box chain)";
    }
    return "?";
}

void setTreeSampling(core::ObjectTree& aTree, Sampling aSampling) {
    std::vector<core::LayerNode*> layers;
    figures::collectLayers(aTree.topNode(), layers);
    auto& ggl = gl::Global::functions();
    int n = 0;
    for (auto* layer : layers) {
        if (!layer->timeLine())
            continue;
        auto* key = dynamic_cast<core::ImageKey*>(layer->timeLine()->defaultKey(core::TimeKeyType_Image));
        if (!key)
            continue;
        gl::Texture& tex = key->cache().texture();
        if (tex.id() == 0)
            continue;
        // The chain stays allocated in every state; only the sampler and the sampled level RANGE
        // change, so this is reversible without rebuilding any texture. Every state writes BOTH
        // parameters, because MAX_LEVEL is sticky: a state that only wrote the filter would inherit
        // the previous state's range and produce a variant that is not the one it claims to be.
        switch (aSampling) {
        case Sampling::Level0:
            // the pre-change state exactly: a texture with no chain at all, which Texture.cpp
            // pins to MAX_LEVEL 0 rather than leaving at the default
            tex.setFilter(GL_LINEAR, GL_LINEAR);
            ggl.glBindTexture(GL_TEXTURE_2D, tex.id());
            ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            ggl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, -1000.0f);   // GL's default
            ggl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 1000.0f);
            ggl.glBindTexture(GL_TEXTURE_2D, 0);
            break;
        case Sampling::MipmapCutoff:
            tex.setFilter(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
            ggl.glBindTexture(GL_TEXTURE_2D, tex.id());
            ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, tex.mipmapLevels() - 1);
            ggl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, -1000.0f);
            ggl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 1000.0f);
            ggl.glBindTexture(GL_TEXTURE_2D, 0);
            break;
        case Sampling::ForceLevel4:
            // MIN_LOD and MAX_LOD are both 4, so the sampler reads level 4 no matter what the
            // derivatives say - and MAX_LEVEL must be at least 4 for that level to exist.
            tex.setFilter(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
            ggl.glBindTexture(GL_TEXTURE_2D, tex.id());
            ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, std::max(4, tex.mipmapLevels() - 1));
            ggl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, 4.0f);
            ggl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 4.0f);
            ggl.glBindTexture(GL_TEXTURE_2D, 0);
            break;
        case Sampling::MipmapNoCutoff:
            // The clamp removed, everything else identical: MAX_LEVEL stays at GL's default of
            // 1000, so the sampled range runs past the levels that exist.
            tex.setFilter(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
            ggl.glBindTexture(GL_TEXTURE_2D, tex.id());
            ggl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
            ggl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, -1000.0f);
            ggl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 1000.0f);
            ggl.glBindTexture(GL_TEXTURE_2D, 0);
            break;
        }
        ++n;
    }
    std::printf("  sampler: %s on %d textures\n", samplingName(aSampling), n);
}

void setLayerSampling(core::Project& aProject, Sampling aSampling) {
    setTreeSampling(aProject.objectTree(), aSampling);
}


//-------------------------------------------------------------------------------------------------
// Corpus evaluation.
//
// Sixteen figures, each one artwork, is a set of anecdotes: it cannot support a judgement about
// whether the change is an improvement. This runs the before/after/ground-truth comparison over a
// whole corpus and emits one CSV row per (file, zoom), so the result is a distribution rather than
// an example.
//
// Per row: the error of LEVEL-0 sampling (the state this change replaces - what shipped when no mip
// chain existed) and of the shipped MIPMAPPED render, both measured against the supersampled
// reference. Errors are max-channel over premultiplied RGB, which is the framebuffer representation
// and the one that behaves correctly across alpha edges.
struct DiffStats {
    double mean = 0;
    int p99 = 0, p999 = 0, max = 0, over16 = 0;
};

DiffStats diffVsRef(const QImage& aA, const QImage& aB) {
    DiffStats st;
    const int w = std::min(aA.width(), aB.width()), h = std::min(aA.height(), aB.height());
    std::vector<int> d;
    d.reserve((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        const uchar* ra = aA.constScanLine(y);
        const uchar* rb = aB.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            int worst = 0;
            for (int c = 0; c < 3; ++c) {
                // premultiply so a difference under a transparent pixel cannot outrank a visible one
                const int pa = ra[x * 4 + c] * ra[x * 4 + 3] / 255;
                const int pb = rb[x * 4 + c] * rb[x * 4 + 3] / 255;
                worst = std::max(worst, std::abs(pa - pb));
            }
            d.push_back(worst);
        }
    }
    if (d.empty())
        return st;
    double sum = 0;
    for (int v : d) {
        sum += v;
        if (v > 16)
            ++st.over16;
    }
    st.mean = sum / d.size();
    st.max = *std::max_element(d.begin(), d.end());
    std::sort(d.begin(), d.end());
    st.p99 = d[(size_t)(d.size() * 0.99)];
    st.p999 = d[(size_t)std::min(d.size() - 1, (size_t)(d.size() * 0.999))];
    return st;
}

// aW x aH pixels of an image at (aAt), clamped to the image.
QImage cropAt(const QImage& aImg, const QPoint& aAt, int aW, int aH) {
    const int x = std::max(0, std::min(aImg.width() - 1, aAt.x()));
    const int y = std::max(0, std::min(aImg.height() - 1, aAt.y()));
    return aImg.copy(x, y, std::min(aW, aImg.width() - x), std::min(aH, aImg.height() - y));
}

// The central aWindow x aWindow pixels of an image, or the whole image if it is smaller.
QImage cropCentral(const QImage& aImg, int aWindow) {
    const int w = std::min(aWindow, aImg.width());
    const int h = std::min(aWindow, aImg.height());
    return aImg.copy((aImg.width() - w) / 2, (aImg.height() - h) / 2, w, h);
}

// The supersampled ground truth for zooms too deep for a whole-canvas supersample. At 16x a 1920
// px canvas needs a 30720 px target and the device stops at 16384, so supersampledReference
// silently falls back to ss=8 - HALF a sample per base texel, which is not an integral of anything
// and would flatter whichever chain happens to agree with a point sample. Instead, supersample
// only the window the comparison looks at: a W x W block of screen pixels around the canvas centre
// at ss samples each. imageSize stays at the canvas, so the artwork stays centred and the field of
// view is the central W screen pixels of the shipped render; the cost is (W*ss)^2 instead of
// (canvas*ss)^2. Validated against the full-frame reference wherever that one fits (the
// winref_delta column in the eval CSV).
QImage supersampledWindowReference(core::Project& aProject, const QSize& aCanvas, float aZoom, int& aSS,
                                   const QSize& aWindow, const QPoint& aAt, const uint8_t aBackdrop[3],
                                   int* aAlign255 = nullptr) {
    {
        auto& ggl = gl::Global::functions();
        GLint maxTex = 0, maxRb = 0;
        ggl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        ggl.glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRb);
        const int limit = std::min(maxTex > 0 ? (int)maxTex : 16384, maxRb > 0 ? (int)maxRb : 16384);
        // Step by two: the caller picks ss so that (canvas/2)*(ss-1) is an integer, which is what
        // lets the supersampled grid align with the shipped one (see referenceLeftTop). Decrementing
        // by one would flip that parity and silently shift the reference by half a pixel.
        while (aSS > 1 && std::max(aWindow.width(), aWindow.height()) * aSS > limit)
            aSS = std::max(1, aSS - 2);
        if (aSS < 1)
            aSS = 1;
    }
    QPoint leftTop;
    const int align255 =
        referenceLeftTop(aCanvas.width(), aCanvas.height(), aSS, aAt.x(), aAt.y(), leftTop);
    if (aAlign255)
        *aAlign255 = align255;

    const QSize big(aWindow.width() * aSS, aWindow.height() * aSS);
    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(big.width()).arg(big.height());
    if (fixtures.find(key) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(big);
        fixtures.emplace(key, std::move(fx));
    }
    core::CameraInfo camera;
    camera.reset(big, 1.0, aCanvas, leftTop);
    camera.setScale(aZoom * (float)aSS);
    const std::vector<uint8_t> px = fixtures[key]->render(aProject.objectTree(), camera, 0, aBackdrop);
    return boxDownsample(figures::readbackToImage(px, big), aSS);
}


// A third reference: the integral over the footprint of a LANCZOS reconstruction of the artwork
// (windowed sinc, a = 3). This is the reconstruction a high-quality resampler assumes, so it is the
// natural reference if the question is "how close is this to the best resample" rather than "how
// close is this to what the renderer shows when magnifying" (that one is tentIntegral).
double lanczosIntegral(const Img& aImg, double aX0, double aY0, int aSpan, int aChannel) {
    const int a = 3;
    const auto lanczos = [&](double x) {
        if (x == 0.0)
            return 1.0;
        if (std::fabs(x) >= a)
            return 0.0;
        const double pix = M_PI * x;
        return (a * std::sin(pix) * std::sin(pix / a)) / (pix * pix);
    };
    const int taps = aSpan + 2 * a + 2;
    const int kx0 = (int)std::floor(aX0) - a - 1, ky0 = (int)std::floor(aY0) - a - 1;
    std::vector<double> wx(taps, 0.0), wy(taps, 0.0);
    const int samples = 16 * aSpan;
    for (int t = 0; t < taps; ++t) {
        const double cx = kx0 + t + 0.5;
        double sum = 0.0;
        for (int i = 0; i < samples; ++i)
            sum += lanczos(aX0 + (i + 0.5) * (double)aSpan / samples - cx);
        wx[t] = sum / samples;
        const double cy = ky0 + t + 0.5;
        sum = 0.0;
        for (int i = 0; i < samples; ++i)
            sum += lanczos(aY0 + (i + 0.5) * (double)aSpan / samples - cy);
        wy[t] = sum / samples;
    }
    double total = 0.0;
    for (int j = 0; j < taps; ++j) {
        const int y = ky0 + j;
        for (int i = 0; i < taps; ++i) {
            const int x = kx0 + i;
            if (wx[i] == 0.0 || wy[j] == 0.0)
                continue;
            const double v = (x < 0 || y < 0 || x >= aImg.w || y >= aImg.h) ? 0.0
                                                                            : (double)aImg.at(x, y)[aChannel];
            total += wx[i] * wy[j] * v;
        }
    }
    return total;
}

// The third candidate definition of the right answer: the artwork as a piecewise-CONSTANT array of
// texels, each weighted by how much of the footprint it covers. This is "area downsampling" in the
// image-processing sense and it is what a mip chain approximates; it differs from tentIntegral()
// only in the reconstruction model it integrates, and the two differ by exactly the amount that
// makes the choice of "truth" arguable. Both integrate the same footprint with the same box.
double areaIntegral(const Img& aImg, double aX0, double aY0, int aSpan, int aChannel) {
    const auto weights = [&](double aStart, int aBase, std::vector<double>& aOut) {
        for (int t = 0; t < (int)aOut.size(); ++t) {
            const double lo = std::max(aStart, (double)(aBase + t));
            const double hi = std::min(aStart + aSpan, (double)(aBase + t + 1));
            aOut[t] = std::max(0.0, hi - lo) / aSpan;
        }
    };
    const int taps = aSpan + 2;
    const int kx0 = (int)std::floor(aX0) - 1, ky0 = (int)std::floor(aY0) - 1;
    std::vector<double> wx(taps, 0.0), wy(taps, 0.0);
    weights(aX0, kx0, wx);
    weights(aY0, ky0, wy);
    double total = 0.0;
    for (int j = 0; j < taps; ++j) {
        const int y = ky0 + j;
        for (int i = 0; i < taps; ++i) {
            const int x = kx0 + i;
            if (wx[i] == 0.0 || wy[j] == 0.0)
                continue;
            const double v = (x < 0 || y < 0 || x >= aImg.w || y >= aImg.h) ? 0.0
                                                                            : (double)aImg.at(x, y)[aChannel];
            total += wx[i] * wy[j] * v;
        }
    }
    return total;
}

// The integral of the TENT reconstruction of an image over the footprint [x0, x0+w) x [y0, y0+w),
// which is what a supersampled render converges to: the artwork as a continuous signal is the
// bilinear interpolation of its texels, and the destination pixel's value is that signal's integral
// over the pixel's footprint. Separable, so the 2-D tap weights are the product of two 1-D
// integrals, each computed by fine sampling of the tent. This is the reference the box pyramid is
// being compared against, so it must not be the box.
double tentIntegral(const Img& aImg, double aX0, double aY0, int aW, int aChannel) {
    XC_ASSERT(aW > 0);
    const int taps = aW + 3;
    const int samples = 8 * aW;
    std::vector<double> wx(taps, 0.0), wy(taps, 0.0);
    const int kx0 = (int)std::floor(aX0) - 1, ky0 = (int)std::floor(aY0) - 1;

    const auto weights = [&](double aStart, std::vector<double>& aOut, int aBase) {
        for (int t = 0; t < taps; ++t) {
            const double centre = aBase + t + 0.5;   // the texel's centre, at half-integers
            double sum = 0.0;
            for (int i = 0; i < samples; ++i) {
                const double x = aStart + (i + 0.5) * (double)aW / samples;
                const double d = std::fabs(x - centre);
                if (d < 1.0)
                    sum += 1.0 - d;
            }
            aOut[t] = sum / samples;
        }
    };
    weights(aX0, wx, kx0);
    weights(aY0, wy, ky0);

    double total = 0.0;
    for (int j = 0; j < taps; ++j) {
        if (wy[j] == 0.0)
            continue;
        const int y = ky0 + j;
        for (int i = 0; i < taps; ++i) {
            if (wx[i] == 0.0)
                continue;
            const int x = kx0 + i;
            // outside the texture the artwork does not exist, and the padding this texture carries
            // is transparent, so out-of-range taps contribute nothing
            const double v = (x < 0 || y < 0 || x >= aImg.w || y >= aImg.h)
                                 ? 0.0
                                 : (double)aImg.at(x, y)[aChannel];
            total += wx[i] * wy[j] * v;
        }
    }
    return total;
}

//-------------------------------------------------------------------------------------------------
// --decompose <csv> <file> : what the remaining error IS, term by term, on a real layer.
//
// The claim under test is that what is left at 16x is the difference between a box filter and the
// tent-weighted integral the supersampled truth represents, and not an error in the chain or in how
// it is sampled. One real layer, rendered ALONE at exactly 16x (lambda = 4), gives four quantities:
//
//   render  the shipped render: the GPU's level-4 read of the texture the app uploads
//   truth   the supersampled render of the same scene, box-averaged: the integral
//   box     a CPU box pyramid over those same uploaded bytes, read bilinearly at the footprint
//           centres - the ideal chain, which M14 shows the render matches to a couple of 8-bit
//           levels
//   tent    the integral of the TENT reconstruction of the same artwork over the same footprints
//
// |render - truth| <= |render - box| + |box - tent| + |tent - truth|, so the three terms say which
// part of the error is the filter and which part is the implementation. The last term is also a
// test of the claim that the supersampled truth IS that integral, on real content instead of by
// definition.

//-------------------------------------------------------------------------------------------------
// --bias <csv> <file> : does sampling a FINER level than the footprint buy accuracy?
//
// The level-l texel holds the average over its 2^l block, so nothing in level l records where inside
// that block the detail sat. A footprint that covers a whole block can be answered from it exactly
// (measured: 0.05 to 0.31 of 255 against the supersampled truth), but a footprint that straddles two
// blocks cannot: the chain has to smear each block's content uniformly over the block. Sampling a
// finer level keeps the sub-block structure, at the price of filtering less. This sweeps
// GL_TEXTURE_LOD_BIAS against the supersampled truth, so the trade is measured rather than argued:
// the truth is an antialiased integral, so it penalises both blurring and aliasing, and whichever
// bias has the smallest error there is genuinely the smallest total error by this measure.
void setTreeLodBias(core::ObjectTree& aTree, float aBias) {
    std::vector<core::LayerNode*> layers;
    figures::collectLayers(aTree.topNode(), layers);
    auto& ggl = gl::Global::functions();
    for (auto* layer : layers) {
        if (!layer->timeLine())
            continue;
        auto* key = dynamic_cast<core::ImageKey*>(layer->timeLine()->defaultKey(core::TimeKeyType_Image));
        if (!key)
            continue;
        gl::Texture& tex = key->cache().texture();
        if (tex.id() == 0)
            continue;
        ggl.glBindTexture(GL_TEXTURE_2D, tex.id());
        ggl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, aBias);
        ggl.glBindTexture(GL_TEXTURE_2D, 0);
    }
}

int runBias(const QString& aCsv, const QString& aPath) {
    gl::DeviceInfo dev;
    dev.vender = "harness"; dev.renderer = "harness"; dev.version = "5";
    dev.maxTextureSize = 16384; dev.maxRenderBufferSize = 16384;
    gl::DeviceInfo::setInstance(&dev);

    static std::vector<std::unique_ptr<MipAnimator>> animators;
    animators.emplace_back(new MipAnimator());
    static MipReporter reporter;
    core::Project project(aPath, *animators.back(), nullptr);
    if (!loadAnyProject(aPath, project, dev, reporter))
        return 3;

    const QSize canvas = project.attribute().imageSize();
    if (canvas.isEmpty())
        return 4;
    const uint8_t backdrop[3] = {128, 128, 128};

    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(canvas.width()).arg(canvas.height());
    if (fixtures.find(key) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(canvas);
        fixtures.emplace(key, std::move(fx));
    }
    RenderFixture& fx = *fixtures[key];

    QFile out(aCsv);
    const bool fresh = !out.exists() || out.size() == 0;
    if (!out.open(QIODevice::Append | QIODevice::Text))
        return 5;
    QTextStream ts(&out);
    if (fresh)
        ts << "file,zoom,lod_bias,mean,max,over16\n";

    const float zooms[] = {0.25f, 0.0625f};
    const float biases[] = {-1.5f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    const int window = 64;

    for (float zoom : zooms) {
        int ss = std::max(2, (int)std::lround(2.0f / zoom));
        if (ss % 2 == 0 && (canvas.width() % 2 != 0 || canvas.height() % 2 != 0))
            ++ss;
        const QPoint at((canvas.width() - window) / 2, (canvas.height() - window) / 2);

        // the shipped state first: the truth is rendered with the level bias at its default
        setLayerSampling(project, Sampling::MipmapCutoff);
        setTreeLodBias(project.objectTree(), 0.0f);
        int ssRef = ss;
        const QImage ref =
            supersampledWindowReference(project, canvas, zoom, ssRef, QSize(window, window), at, backdrop);

        core::CameraInfo camera;
        camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
        camera.setScale(zoom);

        std::printf("  %s z=%.4f\n", qPrintable(QFileInfo(aPath).fileName()), zoom);
        for (float bias : biases) {
            setTreeLodBias(project.objectTree(), bias);
            const QImage img = figures::readbackToImage(fx.render(project.objectTree(), camera, 0, backdrop), canvas);
            const DiffStats d = diffVsRef(cropCentral(img, window), ref);
            ts << QFileInfo(aPath).fileName() << "," << zoom << "," << bias << ","
               << QString::number(d.mean, 'f', 4) << "," << d.max << "," << d.over16 << "\n";
            ts.flush();
            std::printf("    lod bias %+4.1f : mean %7.4f  max %3d  pixels>16 %d\n", bias, d.mean, d.max, d.over16);
        }
        setTreeLodBias(project.objectTree(), 0.0f);
    }
    return 0;
}


//-------------------------------------------------------------------------------------------------
// --bias1 <csv> <file> : the LOD-bias optimum against each candidate reference.
//
// The bias sweep on whole projects uses the supersampled render as its reference, which carries the
// same tent reconstruction the magnifier uses. If the reference for tuning ought to be a high-quality
// resample instead - area (piecewise-constant texels) or Lanczos (band-limited) - the optimum could
// move. This renders ONE real layer at 16x under several biases and scores every one of them against
// all four references: the supersampled render, and the CPU area / tent / Lanczos models.
int runBias1(const QString& aCsv, const QString& aPath) {
    gl::DeviceInfo dev;
    dev.vender = "harness"; dev.renderer = "harness"; dev.version = "5";
    dev.maxTextureSize = 16384; dev.maxRenderBufferSize = 16384;
    gl::DeviceInfo::setInstance(&dev);

    static std::vector<std::unique_ptr<MipAnimator>> animators;
    animators.emplace_back(new MipAnimator());
    static MipReporter reporter;
    core::Project project(aPath, *animators.back(), nullptr);
    if (!loadAnyProject(aPath, project, dev, reporter))
        return 3;

    std::vector<core::LayerNode*> layers;
    figures::collectLayers(project.objectTree().topNode(), layers);
    Img bestCopy;
    QString bestName;
    bool have = false;
    for (auto* layer : layers) {
        if (!layer->timeLine())
            continue;
        auto* ikey = dynamic_cast<core::ImageKey*>(layer->timeLine()->defaultKey(core::TimeKeyType_Image));
        if (!ikey || !ikey->data().resource()->hasImage())
            continue;
        const img::Buffer& image = ikey->data().resource()->image();
        const QSize size = image.pixelSize();
        if (size.width() < 256 || size.height() < 256)
            continue;
        if (have && (qint64)size.width() * size.height() <= (qint64)bestCopy.w * bestCopy.h)
            continue;
        bestCopy.w = size.width();
        bestCopy.h = size.height();
        bestCopy.d.assign(image.data(), image.data() + (size_t)size.width() * size.height() * 4);
        bestName = layer->name();
        have = true;
    }
    if (!have)
        return 0;
    const Img artwork = bestCopy;

    const QSize canvas(128, 128);
    const int ss = 32;
    const float zoom = 1.0f / 16.0f;
    const int level = 4;
    const uint8_t backdrop[3] = {0, 0, 0};

    core::ObjectTree tree;
    auto* root = new core::FolderNode("root");
    tree.grabTopNode(root);
    auto* res = makeResource("bias1_src", artwork);
    addScaledLayer(tree, root, res, "layer", QVector2D(canvas.width() * 0.5f, canvas.height() * 0.5f), 1.0f,
                   img::BlendMode_Normal);

    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(canvas.width()).arg(canvas.height());
    if (fixtures.find(key) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(canvas);
        fixtures.emplace(key, std::move(fx));
    }
    core::CameraInfo camera;
    camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
    camera.setScale(zoom);

    fixtures[key]->render(tree, camera, 0, backdrop);
    setTreeSampling(tree, Sampling::MipmapCutoff);
    setTreeLodBias(tree, 0.0f);

    const QSize big(canvas.width() * ss, canvas.height() * ss);
    const QString bigKey = QString("%1x%2").arg(big.width()).arg(big.height());
    if (fixtures.find(bigKey) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(big);
        fixtures.emplace(bigKey, std::move(fx));
    }
    QPoint leftTop;
    referenceLeftTop(canvas.width(), canvas.height(), ss, 0, 0, leftTop);
    core::CameraInfo bigCamera;
    bigCamera.reset(big, 1.0, canvas, leftTop);
    bigCamera.setScale(zoom * (float)ss);
    const QImage truth = boxDownsample(
        figures::readbackToImage(fixtures[bigKey]->render(tree, bigCamera, 0, backdrop), big), ss
    );

    const QSize textureSize = img::Util::mipAlignedTextureSize(QSize(artwork.w, artwork.h));
    const std::vector<uint8> upload =
        img::Util::makeTextureUpload(artwork.d.data(), QSize(artwork.w, artwork.h), textureSize);
    Img texture;
    texture.w = textureSize.width();
    texture.h = textureSize.height();
    texture.d = upload;

    QFile out(aCsv);
    const bool fresh = !out.exists() || out.size() == 0;
    if (!out.open(QIODevice::Append | QIODevice::Text))
        return 5;
    QTextStream ts(&out);
    if (fresh)
        ts << "file,layer,lod_bias,vs_ssaa,vs_area,vs_tent,vs_lanczos\n";

    const double step = 1.0 / zoom;
    const double half = step * 0.5;
    const float biases[] = {-1.5f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    std::printf("  %s  layer %s  %dx%d\n", qPrintable(QFileInfo(aPath).fileName()), qPrintable(bestName),
                artwork.w, artwork.h);

    for (float bias : biases) {
        setTreeLodBias(tree, bias);
        const QImage img = figures::readbackToImage(fixtures[key]->render(tree, camera, 0, backdrop), canvas);
        double sS = 0, sA = 0, sT = 0, sL = 0;
        int n = 0;
        for (int sy = 0; sy < canvas.height(); ++sy) {
            for (int sx = 0; sx < canvas.width(); ++sx) {
                const double ax = (sx + 0.5) * step - (canvas.width() * 0.5) * step + artwork.w * 0.5;
                const double ay = (sy + 0.5) * step - (canvas.height() * 0.5) * step + artwork.h * 0.5;
                if (ax - half < 0.0 || ay - half < 0.0 || ax + half > artwork.w || ay + half > artwork.h)
                    continue;
                const double r = img.pixelColor(sx, sy).red();
                sS += std::fabs(r - truth.pixelColor(sx, sy).red());
                sA += std::fabs(r - areaIntegral(texture, ax - half, ay - half, (int)step, 0));
                sT += std::fabs(r - tentIntegral(texture, ax - half, ay - half, (int)step, 0));
                sL += std::fabs(r - lanczosIntegral(texture, ax - half, ay - half, (int)step, 0));
                ++n;
            }
        }
        const double d = n ? n : 1;
        ts << QFileInfo(aPath).fileName() << "," << bestName << "," << bias << ","
           << QString::number(sS / d, 'f', 4) << "," << QString::number(sA / d, 'f', 4) << ","
           << QString::number(sT / d, 'f', 4) << "," << QString::number(sL / d, 'f', 4) << "\n";
        ts.flush();
        std::printf("    bias %+4.1f : vs ssaa %6.3f | vs area %6.3f | vs tent %6.3f | vs lanczos %6.3f\n",
                    bias, sS / d, sA / d, sT / d, sL / d);
    }
    setTreeLodBias(tree, 0.0f);
    return 0;
}

int runDecompose(const QString& aCsv, const QString& aPath, bool aAligned) {
    gl::DeviceInfo dev;
    dev.vender = "harness"; dev.renderer = "harness"; dev.version = "5";
    dev.maxTextureSize = 16384; dev.maxRenderBufferSize = 16384;
    gl::DeviceInfo::setInstance(&dev);

    static std::vector<std::unique_ptr<MipAnimator>> animators;
    animators.emplace_back(new MipAnimator());
    static MipReporter reporter;
    core::Project project(aPath, *animators.back(), nullptr);
    if (!loadAnyProject(aPath, project, dev, reporter))
        return 3;

    // the largest layer by image area: the one whose minification is deepest and whose content is
    // most likely to be real artwork rather than a flat fill
    std::vector<core::LayerNode*> layers;
    figures::collectLayers(project.objectTree().topNode(), layers);
    const Img* best = nullptr;
    Img bestCopy;
    QString bestName;
    for (auto* layer : layers) {
        if (!layer->timeLine())
            continue;
        auto* ikey = dynamic_cast<core::ImageKey*>(layer->timeLine()->defaultKey(core::TimeKeyType_Image));
        if (!ikey || !ikey->data().resource()->hasImage())
            continue;
        const img::Buffer& image = ikey->data().resource()->image();
        const QSize size = image.pixelSize();
        if (size.width() < 256 || size.height() < 256)
            continue;
        if (best && (qint64)size.width() * size.height() <= (qint64)best->w * best->h)
            continue;
        bestCopy.w = size.width();
        bestCopy.h = size.height();
        bestCopy.d.assign(image.data(), image.data() + (size_t)size.width() * size.height() * 4);
        best = &bestCopy;
        bestName = layer->name();
    }
    if (!best) {
        std::printf("  decompose %s: no layer image large enough\n", qPrintable(QFileInfo(aPath).fileName()));
        return 0;
    }
    const Img artwork = *best;

    // The scene: that one layer, centred, at 1:1, on a canvas small enough that the supersampled
    // reference (canvas * ss) fits the device. 128 * 32 = 4096.
    const QSize canvas(128, 128);
    const int ss = 32;
    const float zoom = 1.0f / 16.0f;
    const uint8_t backdrop[3] = {0, 0, 0};

    // The "aligned" pass shifts the layer by a fraction of a texel so that every footprint centre
    // lands exactly on a level-4 texel centre: then the level-4 read is a plain box and the only
    // thing between it and the truth is the box's ends. "As it falls" leaves the phase wherever
    // centring puts it, which is what happens in real artwork - and there the bilinear read blends
    // two neighbouring boxes, a kernel twice as wide. The difference between the two passes is
    // exactly the question of which filter is being applied.
    // At exactly 16x one screen pixel is 16 texels, i.e. exactly one level-4 texel, so the phase is
    // the same for every pixel in the frame: frac(w/32 + 0.5) in level units. Shifting the layer by
    // 16 * (that - 0.5) texels puts every footprint centre on a level-texel centre.
    const auto alignOffset = [](int aSide) {
        const double frac = std::fmod((double)aSide / 32.0 + 0.5, 1.0);
        return 16.0 * (frac - 0.5);
    };
    const double alignX = alignOffset(artwork.w);
    const double alignY = alignOffset(artwork.h);

    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(canvas.width()).arg(canvas.height());
    if (fixtures.find(key) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(canvas);
        fixtures.emplace(key, std::move(fx));
    }
    core::CameraInfo camera;
    camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
    camera.setScale(zoom);

    core::ObjectTree tree;
    auto* root = new core::FolderNode("root");
    tree.grabTopNode(root);
    auto* res = makeResource("decompose_src", artwork);
    addScaledLayer(
        tree, root, res, "layer",
        QVector2D(
            canvas.width() * 0.5f + (aAligned ? (float)alignX : 0.0f),
            canvas.height() * 0.5f + (aAligned ? (float)alignY : 0.0f)
        ),
        1.0f, img::BlendMode_Normal
    );

    fixtures[key]->render(tree, camera, 0, backdrop);   // build the texture
    setTreeSampling(tree, Sampling::MipmapCutoff);
    const QImage render = figures::readbackToImage(fixtures[key]->render(tree, camera, 0, backdrop), canvas);

    // a second truth at half the sampling: if the error numbers move between the two, they are
    // measuring the quadrature, not the filter
    QImage truthCoarse;
    {
        const int ss2 = ss / 2;
        const QSize big2(canvas.width() * ss2, canvas.height() * ss2);
        const QString key2 = QString("%1x%2").arg(big2.width()).arg(big2.height());
        if (fixtures.find(key2) == fixtures.end()) {
            std::unique_ptr<RenderFixture> fx(new RenderFixture());
            fx->init(big2);
            fixtures.emplace(key2, std::move(fx));
        }
        QPoint lt2;
        referenceLeftTop(canvas.width(), canvas.height(), ss2, 0, 0, lt2);
        core::CameraInfo cam2;
        cam2.reset(big2, 1.0, canvas, lt2);
        cam2.setScale(zoom * (float)ss2);
        truthCoarse = boxDownsample(
            figures::readbackToImage(fixtures[key2]->render(tree, cam2, 0, backdrop), big2), ss2
        );
    }

    // the truth: the same scene at ss x, box-averaged down
    const QSize big(canvas.width() * ss, canvas.height() * ss);
    const QString bigKey = QString("%1x%2").arg(big.width()).arg(big.height());
    if (fixtures.find(bigKey) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(big);
        fixtures.emplace(bigKey, std::move(fx));
    }
    QPoint leftTop;
    referenceLeftTop(canvas.width(), canvas.height(), ss, 0, 0, leftTop);
    core::CameraInfo bigCamera;
    bigCamera.reset(big, 1.0, canvas, leftTop);
    bigCamera.setScale(zoom * (float)ss);
    const QImage truth = boxDownsample(
        figures::readbackToImage(fixtures[bigKey]->render(tree, bigCamera, 0, backdrop), big), ss
    );

    // the bytes the app uploads, and the ideal box pyramid over them
    const QSize textureSize = img::Util::mipAlignedTextureSize(QSize(artwork.w, artwork.h));
    const std::vector<uint8> upload = img::Util::makeTextureUpload(artwork.d.data(), QSize(artwork.w, artwork.h), textureSize);
    Img texture;
    texture.w = textureSize.width();
    texture.h = textureSize.height();
    texture.d = upload;
    const int level = 4;
    const Img boxed = boxLevel(texture, level);
    const Img finer = boxLevel(texture, level - 1);

    // sample positions: the layer is centred, so screen pixel (sx, sy) - the readback is bottom-up -
    // sits at artwork coordinate ((p - canvas/2) / zoom + artwork/2), and the texture's origin is
    // the artwork's origin, so the same number is the texture coordinate.
    const double step = 1.0 / zoom;
    double sumRT = 0, sumBT = 0, sumRB = 0, sumTT = 0, sumTB = 0;
    int n = 0, maxRT = 0, maxRB = 0;
    double worstRB = 0;
    QString worstAt;

    // Every kept sample, so the box model can be re-evaluated at small position offsets without
    // rendering again: the largest render-vs-box differences sit on content edges, and a sub-texel
    // position difference shows up there as a large local error while leaving the mean alone. An
    // offset sweep tells a position difference apart from a wrong kernel.
    struct Sample {
        double r, t, ax, ay;
    };
    double gradXRender = 0, gradYRender = 0, gradXTruth = 0, gradYTruth = 0, gradXBox = 0, gradYBox = 0;
    int gradN = 0;
    const double margin0 = 0.0;   // this texture keeps the artwork at the origin
    double sumRTc = 0, sumBTc = 0, sumTentc = 0, sumRTb = 0, sumBTb = 0, sumTentb = 0;
    double sumAT = 0, sumAB = 0, sumAA = 0, sumOT = 0;
    int nc = 0, nb = 0;
    std::vector<Sample> samples;

    for (int sy = 0; sy < canvas.height(); ++sy) {
        for (int sx = 0; sx < canvas.width(); ++sx) {
            // the model's coordinate frame moves with the layer: shifting the layer right by d puts
            // the artwork at a smaller coordinate for the same screen pixel, so the model samples
            // at (ideal position - d)
            const double ax = (sx + 0.5) * step - (canvas.width() * 0.5) * step + artwork.w * 0.5
                              - (aAligned ? alignX : 0.0);
            // render and truth are read back through readbackToImage, i.e. TOP-DOWN, while the
            // raw framebuffer that other suites index is bottom-up: the row term is (sy + 0.5),
            // not (canvas.height() - 0.5 - sy).
            const double ay = (sy + 0.5) * step - (canvas.height() * 0.5) * step + artwork.h * 0.5
                              - (aAligned ? alignY : 0.0);

            // Footprints that reach outside the artwork are skipped: there the render shows the
            // backdrop (or a partial coverage) while bilinearAt clamps to the edge texel, so the
            // two model different things. What is under test is the filter, and this is where both
            // models are defined.
            const double half = step * 0.5;
            if (ax - half < 0.0 || ay - half < 0.0 || ax + half > artwork.w || ay + half > artwork.h)
                continue;

            const double r = render.pixelColor(sx, sy).red();
            const double t = truth.pixelColor(sx, sy).red();
            const double b = bilinearAt(boxed, ax / (double)step, ay / (double)step, 0);

            // Directional detail, per axis: is the render losing more along x than along y compared
            // with the truth (which would be an anisotropy - a different kernel per axis), or the
            // same in both (which is what a separable box pyramid does)? The neighbours are read
            // from the same images. The box model is evaluated at the neighbouring sample positions
            // too, so "the filter does this" and "the render does this" can be told apart.
            if (sx > 0 && sy > 0 && sx + 1 < canvas.width() && sy + 1 < canvas.height()) {
                const double rxp = render.pixelColor(sx + 1, sy).red();
                const double ryp = render.pixelColor(sx, sy + 1).red();
                const double txp = truth.pixelColor(sx + 1, sy).red();
                const double typ = truth.pixelColor(sx, sy + 1).red();
                const double bxp = bilinearAt(boxed, (ax + step) / (double)step, ay / (double)step, 0);
                const double byp = bilinearAt(boxed, ax / (double)step, (ay + step) / (double)step, 0);
                gradXRender += std::fabs(rxp - r);
                gradYRender += std::fabs(ryp - r);
                gradXTruth += std::fabs(txp - t);
                gradYTruth += std::fabs(typ - t);
                gradXBox += std::fabs(bxp - b);
                gradYBox += std::fabs(byp - b);
                ++gradN;
            }
            const double tn = tentIntegral(texture, ax - half, ay - half, (int)step, 0);
            const double ar = areaIntegral(texture, ax - half, ay - half, (int)step, 0);
            // the shader route: the same 2 taps per axis with overlap weights, but from one level
            // finer than the footprint, so the reconstruction grid is half a block instead of a
            // whole one. Two fetches per axis, no pyramid level matched to the footprint.
            const double o2 = areaIntegral(
                finer, (ax - half) / (step * 0.5), (ay - half) / (step * 0.5), 2, 0
            );
            // Which phase the level-4 read is at: 0.5 means the footprint centre sits on a level
            // texel centre (the footprint is aligned with the artwork's own 16-texel grid, so the
            // read is a plain box), 0.0 or 1.0 means it sits between two texels (so the bilinear
            // read blends two neighbouring boxes, a kernel twice as wide). Splitting the error by
            // this is how "the box's ends are hard" and "bilinear blends two boxes" are told apart:
            // the end taper is a sub-pixel effect, the blend is a whole extra texel of support.
            const double phx = (ax + margin0) / (double)step;
            const double phy = (ay + margin0) / (double)step;
            const double dx = std::fabs(phx - std::floor(phx) - 0.5);
            const double dy = std::fabs(phy - std::floor(phy) - 0.5);
            const bool centred = (dx < 0.25 && dy < 0.25);
            samples.push_back({r, t, ax, ay});
            if (centred) {
                sumRTc += std::fabs(r - t);
                sumBTc += std::fabs(b - t);
                sumTentc += std::fabs(tn - t);
                ++nc;
            } else {
                sumRTb += std::fabs(r - t);
                sumBTb += std::fabs(b - t);
                sumTentb += std::fabs(tn - t);
                ++nb;
            }
            sumRT += std::fabs(r - t);
            sumBT += std::fabs(b - t);
            sumRB += std::fabs(r - b);
            sumTT += std::fabs(tn - t);
            sumTB += std::fabs(tn - b);
            sumAT += std::fabs(ar - t);
            sumOT += std::fabs(o2 - t);
            sumAB += std::fabs(ar - b);
            sumAA += std::fabs(ar - tn);
            maxRT = std::max(maxRT, (int)std::lround(std::fabs(r - t)));
            if (std::fabs(r - b) > worstRB) {
                worstRB = std::fabs(r - b);
                maxRB = (int)std::lround(worstRB);
                // the alpha the truth carries there, and the coverage the box texel holds: a
                // disagreement at very low alpha is the un-premultiply rounding, not the filter
                const int truthA = truth.pixelColor(sx, sy).alpha();
                const int boxA =
                    (int)std::lround(bilinearAt(boxed, ax / (double)step, ay / (double)step, 3));
                worstAt = QString("at (%1,%2): render %3 truth %4 box %5 tent %6 | alpha render %7 "
                                  "truth %8 box %9")
                              .arg(sx).arg(sy).arg(r).arg(t).arg(b, 0, 'f', 1).arg(tn, 0, 'f', 1)
                              .arg(render.pixelColor(sx, sy).alpha()).arg(truthA).arg(boxA);
            }
            ++n;
        }
    }

    // ...and the same difference restricted to footprints whose box texel is FULLY opaque. Where it
    // is not, the pixel's value is decided by the mesh's geometric coverage, which the CPU models
    // do not model at all - so if the large differences live exactly there, that is what they are,
    // and this is the measurement that says so rather than the claim.
    double sumFull = 0, sumFullRT = 0, sumFullBT = 0;
    int maxFull = 0, nFull = 0;
    for (const Sample& sp : samples) {
        const double boxA = bilinearAt(boxed, sp.ax / (double)step, sp.ay / (double)step, 3);
        if (boxA < 254.5)
            continue;
        const double bb = bilinearAt(boxed, sp.ax / (double)step, sp.ay / (double)step, 0);
        const double d = std::fabs(sp.r - bb);
        sumFull += d;
        sumFullRT += std::fabs(sp.r - sp.t);
        sumFullBT += std::fabs(bb - sp.t);
        maxFull = std::max(maxFull, (int)std::lround(d));
        ++nFull;
    }

    // the position sweep: |render - box| at the sample position, and at the one the render behaves
    // as if it used
    double offsetMean[9] = {0};
    int offsetMax[9] = {0};
    const double kOffsets[3] = {-0.5, 0.0, 0.5};
    int bestOffset = 4;
    for (int ox = 0; ox < 3; ++ox) {
        for (int oy = 0; oy < 3; ++oy) {
            const int idx = ox * 3 + oy;
            double sum = 0;
            for (const Sample& sp : samples) {
                const double bb = bilinearAt(
                    boxed, (sp.ax + kOffsets[ox]) / (double)step, (sp.ay + kOffsets[oy]) / (double)step, 0
                );
                const double d = std::fabs(sp.r - bb);
                sum += d;
                offsetMax[idx] = std::max(offsetMax[idx], (int)std::lround(d));
            }
            offsetMean[idx] = samples.empty() ? 0.0 : sum / (double)samples.size();
            if (offsetMean[idx] < offsetMean[bestOffset])
                bestOffset = idx;
        }
    }

    const double dd = n ? (double)n : 1.0;

    QFile out(aCsv);
    const bool fresh = !out.exists() || out.size() == 0;
    if (!out.open(QIODevice::Append | QIODevice::Text))
        return 5;
    QTextStream ts(&out);
    if (fresh) {
        ts << "file,layer,phase,art_w,art_h,tex_w,tex_h,render_vs_truth,box_vs_truth,render_vs_box,"
              "tent_vs_truth,box_vs_tent,render_vs_truth_max,render_vs_box_max\n";
    }
    double sumConv = 0;
    if (!truthCoarse.isNull())
        for (int y = 0; y < canvas.height(); ++y)
            for (int x = 0; x < canvas.width(); ++x)
                sumConv += std::fabs(truth.pixelColor(x, y).red() - truthCoarse.pixelColor(x, y).red());
    std::printf("    reference convergence: |ss=%d truth - ss=%d truth| mean %.4f\n", ss, ss / 2,
                sumConv / ((double)canvas.width() * canvas.height()));
    const QString phase = aAligned ? "aligned" : "as-it-falls";
    ts << QFileInfo(aPath).fileName() << "," << bestName << "," << phase << "," << artwork.w << ","
       << artwork.h << "," << textureSize.width() << "," << textureSize.height()
       << "," << QString::number(sumRT / dd, 'f', 4) << "," << QString::number(sumBT / dd, 'f', 4)
       << "," << QString::number(sumRB / dd, 'f', 4) << "," << QString::number(sumTT / dd, 'f', 4)
       << "," << QString::number(sumTB / dd, 'f', 4) << "," << maxRT << "," << maxRB << "\n";
    ts.flush();

    std::printf(
        "  %-24s %-5s %dx%d -> tex %dx%d | render-vs-truth %.3f | box-vs-truth %.3f | "
        "render-vs-box %.3f | tent-vs-truth %.3f | box-vs-tent %.3f\n",
        qPrintable(QFileInfo(aPath).fileName()), qPrintable(phase), artwork.w, artwork.h,
        textureSize.width(), textureSize.height(), sumRT / dd, sumBT / dd, sumRB / dd, sumTT / dd,
        sumTB / dd
    );
    if (!worstAt.isEmpty())
        std::printf("    worst render-vs-box %s\n", qPrintable(worstAt));
    if (gradN > 0) {
        const double gn = (double)gradN;
        const double xR = gradXRender / gn, yR = gradYRender / gn;
        const double xT = gradXTruth / gn, yT = gradYTruth / gn;
        const double xB = gradXBox / gn, yB = gradYBox / gn;
        std::printf(
            "    directional detail, mean |delta| per axis: truth x %.3f y %.3f | render x %.3f y %.3f "
            "| box model x %.3f y %.3f\n",
            xT, yT, xR, yR, xB, yB
        );
        std::printf(
            "    detail kept vs truth: render x %.3f%% y %.3f%% | box model x %.3f%% y %.3f%%\n",
            100.0 * xR / (xT > 1e-9 ? xT : 1.0), 100.0 * yR / (yT > 1e-9 ? yT : 1.0),
            100.0 * xB / (xT > 1e-9 ? xT : 1.0), 100.0 * yB / (yT > 1e-9 ? yT : 1.0)
        );
    }
    {
        std::printf(
            "    by phase: footprint centred on a level texel (%d px): render-vs-truth %.3f, "
            "box-vs-truth %.3f, tent-vs-truth %.3f\n"
            "              footprint between two level texels (%d px): render-vs-truth %.3f, "
            "box-vs-truth %.3f, tent-vs-truth %.3f\n",
            nc, nc ? sumRTc / nc : 0.0, nc ? sumBTc / nc : 0.0, nc ? sumTentc / nc : 0.0, nb,
            nb ? sumRTb / nb : 0.0, nb ? sumBTb / nb : 0.0, nb ? sumTentb / nb : 0.0
        );
    }
    std::printf(
        "    candidate definitions: area(texels weighted by coverage)-vs-truth %.3f | "
        "tent(integral of the bilinear reconstruction)-vs-truth %.3f | the two definitions differ by %.3f\n",
        sumAT / dd, sumTT / dd, sumAA / dd
    );
    std::printf(
        "    chain (box model) against each definition: vs area %.3f | vs tent %.3f\n", sumAB / dd,
        sumTB / dd
    );
    std::printf(
        "    routes vs the reference: pyramid bias 0 (the render) %.3f | exact area over the footprint "
        "%.3f | 2-tap overlap filter off level %d %.3f\n",
        sumRT / dd, sumAT / dd, level - 1, sumOT / dd
    );
    std::printf(
        "    render-vs-box: all footprints mean %.3f max %d | fully opaque box texel (%d of %d "
        "pixels): mean %.3f max %d\n",
        sumRB / dd, maxRB, nFull, n, nFull ? sumFull / nFull : 0.0, maxFull
    );
    if (nFull > 0)
        std::printf(
            "    fully opaque footprints only: render-vs-truth %.3f | box-vs-truth %.3f "
            "(all footprints: %.3f and %.3f)\n",
            sumFullRT / nFull, sumFullBT / nFull, sumRT / dd, sumBT / dd
        );
    std::printf(
        "    render-vs-box at sample position: mean %.3f max %d | best offset (%.2f, %.2f): mean %.3f "
        "max %d\n",
        offsetMean[4], offsetMax[4], kOffsets[bestOffset / 3], kOffsets[bestOffset % 3], offsetMean[bestOffset],
        offsetMax[bestOffset]
    );
    return 0;
}

int runEval(const QString& aCsv, const QString& aPath, const QString& aSaveDir) {
    gl::DeviceInfo dev;
    dev.vender = "harness"; dev.renderer = "harness"; dev.version = "5";
    dev.maxTextureSize = 16384; dev.maxRenderBufferSize = 16384;
    gl::DeviceInfo::setInstance(&dev);

    static std::vector<std::unique_ptr<MipAnimator>> animators;
    animators.emplace_back(new MipAnimator());
    static MipReporter reporter;
    core::Project project(aPath, *animators.back(), nullptr);
    if (!loadAnyProject(aPath, project, dev, reporter))
        return 3;

    const QSize canvas = project.attribute().imageSize();
    if (canvas.isEmpty())
        return 4;
    const uint8_t backdrop[3] = {128, 128, 128};

    std::vector<core::LayerNode*> layers;
    figures::collectLayers(project.objectTree().topNode(), layers);

    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(canvas.width()).arg(canvas.height());
    if (fixtures.find(key) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(canvas);
        fixtures.emplace(key, std::move(fx));
    }
    RenderFixture& fx = *fixtures[key];

    QFile out(aCsv);
    const bool fresh = !out.exists() || out.size() == 0;
    if (!out.open(QIODevice::Append | QIODevice::Text))
        return 5;
    QTextStream ts(&out);
    if (fresh) {
        ts << "file,type,canvas_w,canvas_h,layers,zoom,ss,window,ref_align255,winref_delta,before_mean,before_p99,before_p999,"
              "before_max,before_over16,after_mean,after_p99,after_p999,after_max,after_over16,"
              "nocut_mean,nocut_p99,nocut_p999,nocut_max,nocut_over16,"
              "change_mean,change_p99,change_over16,box4_mean,box4_max,box4_vs_after_mean,"
              "box4_vs_after_max\n";
    }

    // 2x, 4x, 8x and 16x minification. The deeper two are where a texture padding ring shows: level
    // l is a box of 2^l base texels, so a ring of 8 misaligns every level from 4 (16x) upward.
    const float zooms[] = {0.5f, 0.25f, 0.125f, 0.0625f};
    const int window = 64;
    for (float zoom : zooms) {
        core::CameraInfo camera;
        camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
        if (zoom != 1.0f)
            camera.setScale(zoom);

        setLayerSampling(project, Sampling::MipmapCutoff);   // AFTER: shipped
        const QImage after = figures::readbackToImage(fx.render(project.objectTree(), camera, 0, backdrop), canvas);
        setLayerSampling(project, Sampling::Level0);         // BEFORE: level 0 only
        const QImage before = figures::readbackToImage(fx.render(project.objectTree(), camera, 0, backdrop), canvas);
        setLayerSampling(project, Sampling::MipmapNoCutoff); // the clamp removed
        const QImage nocut = figures::readbackToImage(fx.render(project.objectTree(), camera, 0, backdrop), canvas);
        // The same footprint with level 4 FORCED: the chain's ideal output for this zoom, which at
        // 16x is the level the shipped render picks anyway and at the other zooms is not.
        setLayerSampling(project, Sampling::ForceLevel4);
        const QImage box4 = figures::readbackToImage(fx.render(project.objectTree(), camera, 0, backdrop), canvas);
        // ...and back to the shipped state BEFORE the reference renders: with the LOD clamp in
        // place the supersampled render would be forced to read level 4 as well and would stop
        // being a ground truth at all.
        setLayerSampling(project, Sampling::MipmapCutoff);

        // Two samples per base texel at the shallowest, i.e. over-sampled at every zoom: an
        // integral of the footprint rather than a point sample of it. An odd canvas dimension
        // forces an odd ss, or no integer leftTopPos can align the reference with the render.
        int ss = std::max(2, (int)std::lround(2.0f / zoom));
        if (ss % 2 == 0 && (canvas.width() % 2 != 0 || canvas.height() % 2 != 0))
            ++ss;
        int refAlign = 0;
        const QPoint metricAt((canvas.width() - window) / 2, (canvas.height() - window) / 2);
        const QImage win =
            supersampledWindowReference(project, canvas, zoom, ss, QSize(window, window), metricAt, backdrop, &refAlign);

        // Cross-check the window against the whole-canvas reference wherever that one still fits.
        int winrefDelta = -1;
        {
            int ssFull = ss;
            const QImage full = supersampledReference(project, canvas, zoom, ssFull, backdrop);
            if (ssFull == ss) {
                const QImage fullWin = cropCentral(full, window);
                winrefDelta = (int)std::lround(diffVsRef(fullWin, win).max);
            }
        }

        const QImage beforeW = cropCentral(before, window);
        const QImage afterW = cropCentral(after, window);
        const QImage nocutW = cropCentral(nocut, window);
        const QImage box4W = cropCentral(box4, window);

        // A second window, sized to what the artwork covers on the canvas at this zoom, so the
        // saved panels show the whole artwork at ACTUAL resolution at every zoom - at 16x that is
        // canvas/16 pixels, and a canvas-sized panel of it would be almost all backdrop. The metric
        // above keeps its own 64x64 window, so the CSV stays comparable with earlier runs.
        const int figW = std::max((int)std::lround(canvas.width() * zoom), window);
        const int figH = std::max((int)std::lround(canvas.height() * zoom), window);
        const QPoint figAt((canvas.width() - figW) / 2, (canvas.height() - figH) / 2);
        int ssFig = ss;
        const QImage fig = supersampledWindowReference(
            project, canvas, zoom, ssFig, QSize(figW, figH), figAt, backdrop
        );
        const QImage afterFig = cropAt(after, figAt, figW, figH);
        const QImage beforeFig = cropAt(before, figAt, figW, figH);
        const DiffStats db = diffVsRef(beforeW, win), da = diffVsRef(afterW, win), dn = diffVsRef(nocutW, win);
        const DiffStats d4 = diffVsRef(box4W, win);
        // what separating the shipped render from the ideal box chain actually costs at this zoom:
        // at 16x it is the level the render picks, so this is the implementation's own error
        const DiffStats d4a = diffVsRef(box4W, afterW);
        // |AFTER - BEFORE|: how much the change moved the pixels at all. This is the column the
        // crop picker ranks on, so "which artwork shows the change" is a measured question rather
        // than whoever looked at a figure last.
        const DiffStats dc = diffVsRef(afterW, beforeW);
        const QString type = QFileInfo(aPath).suffix().toLower();
        ts << QFileInfo(aPath).fileName() << "," << type << "," << canvas.width() << "," << canvas.height()
           << "," << (int)layers.size() << "," << zoom << "," << ss << "," << window << "," << refAlign
           << "," << winrefDelta
           << "," << QString::number(db.mean, 'f', 4) << "," << db.p99 << "," << db.p999 << "," << db.max << "," << db.over16
           << "," << QString::number(da.mean, 'f', 4) << "," << da.p99 << "," << da.p999 << "," << da.max << "," << da.over16
           << "," << QString::number(dn.mean, 'f', 4) << "," << dn.p99 << "," << dn.p999 << "," << dn.max << "," << dn.over16
           << "," << QString::number(dc.mean, 'f', 4) << "," << dc.p99 << "," << dc.over16
           << "," << QString::number(d4.mean, 'f', 4) << "," << d4.max
           << "," << QString::number(d4a.mean, 'f', 4) << "," << d4a.max
           << "\n";
        ts.flush();

        if (!aSaveDir.isEmpty() && QString::fromLocal8Bit(qgetenv("AE_EVAL_SAVE")) == QLatin1String("1")) {
            QDir().mkpath(aSaveDir);
            const QString base = QDir(aSaveDir).filePath(QFileInfo(aPath).completeBaseName() + QString("_z%1").arg((int)(zoom * 1000)));
            afterFig.save(base + "_after.png");
            beforeFig.save(base + "_before.png");
            fig.save(base + "_ref.png");
            cropAt(box4, figAt, figW, figH).save(base + "_box4.png");
        }
        std::printf(
            "  %s z=%.4f ss=%d win=%d align=%d refdelta=%d  BEFORE mean=%.3f max=%d | AFTER mean=%.3f max=%d "
            "| LEVEL4-FORCED mean=%.3f max=%d | after-vs-forced mean=%.3f max=%d | change mean=%.3f over16=%d\n",
            qPrintable(QFileInfo(aPath).fileName()), zoom, ss, window, refAlign, winrefDelta,
            db.mean, db.max, da.mean, da.max, d4.mean, d4.max, d4a.mean, d4a.max, dc.mean, dc.over16
        );
    }
    return 0;
}

//-------------------------------------------------------------------------------------------------
// --levels <file> : what the mip chain of a REAL artwork's texture actually contains.
//
// M12b shows that glGenerateMipmap is the exact 2x2 box for a 128x128 power-of-two texture. The
// corpus, however, finds the 2x over-blur on 4500x4500 non-power-of-two PSD layers, where the
// render is measurably softer than the box. So the property that was verified in the lab has to be
// verified on the object it is claimed about: read this layer's own level 0 and level 1 back off
// the GPU and compare level 1 with the box, the 3x3 and the 4x4 average of level 0.
int runLevels(const QString& aPath) {
    gl::DeviceInfo dev;
    dev.vender = "harness"; dev.renderer = "harness"; dev.version = "5";
    dev.maxTextureSize = 16384; dev.maxRenderBufferSize = 16384;
    gl::DeviceInfo::setInstance(&dev);

    static std::vector<std::unique_ptr<MipAnimator>> animators;
    animators.emplace_back(new MipAnimator());
    static MipReporter reporter;
    core::Project project(aPath, *animators.back(), nullptr);
    if (!loadAnyProject(aPath, project, dev, reporter))
        return 3;

    const QSize canvas = project.attribute().imageSize();
    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(canvas.width()).arg(canvas.height());
    if (fixtures.find(key) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(canvas);
        fixtures.emplace(key, std::move(fx));
    }
    core::CameraInfo camera;
    camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
    const uint8_t backdrop[3] = {128, 128, 128};
    fixtures[key]->render(project.objectTree(), camera, 0, backdrop);   // build the textures

    std::vector<core::LayerNode*> layers;
    figures::collectLayers(project.objectTree().topNode(), layers);
    std::printf("levels %s : canvas %dx%d, %d layers\n", qPrintable(aPath), canvas.width(), canvas.height(),
                (int)layers.size());

    for (auto* layer : layers) {
        if (!layer->timeLine())
            continue;
        auto* ikey = dynamic_cast<core::ImageKey*>(layer->timeLine()->defaultKey(core::TimeKeyType_Image));
        if (!ikey)
            continue;
        gl::Texture& tex = ikey->cache().texture();
        if (tex.id() == 0)
            continue;
        const QSize size = tex.size();
        const int levels = tex.mipmapLevels();
        // WHERE the artwork sits inside the texture. The mip grid of level l is aligned to
        // multiples of 2^l in base-texel coordinates, so if the artwork's origin sits at an ODD
        // texel the artwork's own 2x2 blocks straddle level-1 texels and a level-1 read averages
        // four texels where the footprint holds two. This number is the whole difference between
        // the 2x cases that improve and those that get worse.
        const QVector2D off = ikey->data().imageOffset();
        std::printf("  texture '%s' %dx%d, %d levels, imageOffset (%.1f, %.1f), image %dx%d\n",
                    qPrintable(layer->name()), size.width(), size.height(), levels, off.x(), off.y(),
                    ikey->data().resource()->image().pixelSize().width(),
                    ikey->data().resource()->image().pixelSize().height());
        if (levels < 2)
            continue;

        Img l0;
        l0.w = size.width(); l0.h = size.height();
        l0.d = readLevel(tex.id(), 0, size);
        const QSize s1(std::max(1, size.width() / 2), std::max(1, size.height() / 2));
        const std::vector<uint8_t> gl1 = readLevel(tex.id(), 1, s1);

        // the box, on the GPU's own level 0 (so this measures the FILTER, not the upload)
        Img box = downsamplePremultiplied(l0);
        const auto mix = [&](int aTap) {
            // aTap x aTap mean of level 0, rounded to nearest: the shape a wider kernel would have
            Img dst;
            dst.w = s1.width(); dst.h = s1.height();
            dst.d.assign((size_t)dst.w * dst.h * 4, 0);
            const int off = aTap / 2;
            for (int y = 0; y < dst.h; ++y) {
                for (int x = 0; x < dst.w; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        int sum = 0;
                        for (int dy = 0; dy < aTap; ++dy) {
                            for (int dx = 0; dx < aTap; ++dx) {
                                const int sx = std::max(0, std::min(l0.w - 1, 2 * x - off + dx));
                                const int sy = std::max(0, std::min(l0.h - 1, 2 * y - off + dy));
                                sum += l0.at(sx, sy)[c];
                            }
                        }
                        dst.at(x, y)[c] = (uint8_t)((sum + aTap * aTap / 2) / (aTap * aTap));
                    }
                }
            }
            return dst;
        };

        const auto report = [&](const char* aWhat, const Img& aRef) {
            const int mx = maxAnyChannelDelta(gl1, aRef.d);
            double sum = 0;
            int n = 0;
            for (size_t i = 0; i < gl1.size() && i < aRef.d.size(); ++i) {
                sum += std::fabs((double)gl1[i] - (double)aRef.d[i]);
                ++n;
            }
            std::printf("    level 1 vs %-16s max %3d  mean %.4f\n", aWhat, mx, n ? sum / n : 0.0);
        };
        (void)box;
        report("2x2 box", downsamplePremultiplied(l0));
        report("3x3 mean", mix(3));
        report("4x4 mean", mix(4));
        // ...and, for the shape of it: how much energy the level has lost to smoothing
        double var1 = 0, var0 = 0;
        {
            double m1 = 0, m0 = 0;
            int n1 = 0, n0 = 0;
            for (size_t i = 0; i < gl1.size(); i += 4) { m1 += gl1[i]; ++n1; }
            for (size_t i = 0; i < l0.d.size(); i += 4) { m0 += l0.d[i]; ++n0; }
            m1 /= std::max(1, n1); m0 /= std::max(1, n0);
            for (size_t i = 0; i < gl1.size(); i += 4) var1 += (gl1[i] - m1) * (gl1[i] - m1);
            for (size_t i = 0; i < l0.d.size(); i += 4) var0 += (l0.d[i] - m0) * (l0.d[i] - m0);
            var1 /= std::max(1, n1); var0 /= std::max(1, n0);
        }
        std::printf("    red-channel variance: level 0 %.2f, level 1 %.2f\n", var0, var1);
    }
    return 0;
}

//-------------------------------------------------------------------------------------------------
// --parity <file> <zoom> : the same render at two sub-texel parities of the artwork.
//
// At exactly 2x minification the footprint of one output pixel is 2x2 texels, and BOTH sampler
// states are supposed to produce that box: level 0 because bilinear's four taps land exactly on
// it, level 1 because the box IS one texel of level 1. That is only true when the 2x2 footprint
// coincides with a level-1 texel, which needs the footprint to start at an EVEN base-texel index.
// Move the artwork by a single texel and that parity flips: level 0 still returns the box (its
// taps follow the footprint wherever it is), but the level-1 read now straddles two level-1 texels
// and averages four texels where two are wanted.
//
// So this test needs no reference image at all. At the parity where the levels line up, enabling
// mipmaps must change nothing - BEFORE and AFTER are the same box, pixel for pixel. At the other
// parity they must differ, and by the amount of one extra halving of the filter. One integer shift
// of the layer separates the two cases and changes nothing else.
int runParity(const QString& aPath, float aZoom) {
    gl::DeviceInfo dev;
    dev.vender = "harness"; dev.renderer = "harness"; dev.version = "5";
    dev.maxTextureSize = 16384; dev.maxRenderBufferSize = 16384;
    gl::DeviceInfo::setInstance(&dev);

    static std::vector<std::unique_ptr<MipAnimator>> animators;
    animators.emplace_back(new MipAnimator());
    static MipReporter reporter;
    core::Project project(aPath, *animators.back(), nullptr);
    if (!loadAnyProject(aPath, project, dev, reporter))
        return 3;
    const QSize canvas = project.attribute().imageSize();

    std::vector<core::LayerNode*> layers;
    figures::collectLayers(project.objectTree().topNode(), layers);
    std::vector<core::MoveKey*> moves;
    for (auto* layer : layers) {
        if (layer->timeLine()) {
            auto* mv = dynamic_cast<core::MoveKey*>(layer->timeLine()->defaultKey(core::TimeKeyType_Move));
            if (mv)
                moves.push_back(mv);
        }
    }

    auto& fixtures = renderFixtures();
    const QString key = QString("%1x%2").arg(canvas.width()).arg(canvas.height());
    if (fixtures.find(key) == fixtures.end()) {
        std::unique_ptr<RenderFixture> fx(new RenderFixture());
        fx->init(canvas);
        fixtures.emplace(key, std::move(fx));
    }
    RenderFixture& fx = *fixtures[key];
    const uint8_t backdrop[3] = {128, 128, 128};

    core::CameraInfo camera;
    camera.reset(canvas, 1.0, canvas, QPoint(0, 0));
    camera.setScale(aZoom);

    std::printf("parity %s zoom %.2f, %d layers, %d move keys\n", qPrintable(QFileInfo(aPath).fileName()),
                aZoom, (int)layers.size(), (int)moves.size());

    // A shift in WORLD units is a shift in base texels, since the layer's image is drawn 1:1 in
    // world units. One texel is one output pixel at zoom 1 and half of one at zoom 0.5.
    for (int shift = 0; shift <= 1; ++shift) {
        if (shift == 1) {
            for (auto* mv : moves)
                mv->setPos(mv->pos() + QVector2D(1.0f, 0.0f));
        }

        setLayerSampling(project, Sampling::MipmapCutoff);
        const QImage after = figures::readbackToImage(fx.render(project.objectTree(), camera, 0, backdrop), canvas);
        setLayerSampling(project, Sampling::Level0);
        const QImage before = figures::readbackToImage(fx.render(project.objectTree(), camera, 0, backdrop), canvas);

        const DiffStats d = diffVsRef(after, before);
        std::printf("  artwork shifted by %d texel | AFTER vs BEFORE: mean %.4f  p99 %d  p999 %d  max %d  over16 %d\n",
                    shift, d.mean, d.p99, d.p999, d.max, d.over16);
    }
    return 0;
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
    if (!loadAnyProject(aAnie, project, dev, reporter))
        return 3;
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
        // AFTER: shipped (mipmapped minification). Set explicitly before every variant - a sticky
        // one-shot flag here silently made the "mip" render level-0 from the second zoom onward, so
        // before and after came out identical and the comparison proved nothing.
        setLayerSampling(project, Sampling::MipmapCutoff);
        const QImage mip = figures::readbackToImage(
            fixtures[key]->render(project.objectTree(), camera, 0, backdrop), canvas
        );
        // BEFORE: level-0 only, which is what shipped when no mip chain existed at all.
        setLayerSampling(project, Sampling::Level0);
        const QImage before = figures::readbackToImage(
            fixtures[key]->render(project.objectTree(), camera, 0, backdrop), canvas
        );
        int ss = aSS;
        const QImage ref = supersampledReference(project, canvas, zoom, ss, backdrop);
        if (ss != aSS)
            std::printf("  (ss clamped %d -> %d to fit %dx%d in the device limit)\n", aSS, ss,
                        canvas.width() * ss, canvas.height() * ss);

        const QString z = QString::number((int)(zoom * 1000));
        mip.save(QDir(aDir).filePath(QString("%1_mip_zoom%2.png").arg(aTag).arg(z)));
        before.save(QDir(aDir).filePath(QString("%1_level0_zoom%2.png").arg(aTag).arg(z)));
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

int runLodScan(const QString& aAnie, int aFrame, float aZoom) {
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
    if (!loadAnyProject(aAnie, project, dev, reporter))
        return 3;
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
    camera.setScale(aZoom);
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

    // --eval <csv> <file> [saveDir]
    if (argc > 3 && QString(argv[1]) == "--eval") {
        const int rc = runEval(
            QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]),
            argc > 4 ? QString::fromLocal8Bit(argv[4]) : QString()
        );
        clearRenderFixtures();
        return rc;
    }

    // --parity <file> <zoom>
    if (argc > 3 && QString(argv[1]) == "--parity") {
        const int rc = runParity(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]).toFloat());
        clearRenderFixtures();
        return rc;
    }

    // --bias <csv> <file>
    if (argc > 3 && QString(argv[1]) == "--bias") {
        const int rc = runBias(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
        clearRenderFixtures();
        return rc;
    }

    // --bias1 <csv> <file>
    if (argc > 3 && QString(argv[1]) == "--bias1") {
        const int rc = runBias1(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
        clearRenderFixtures();
        return rc;
    }

    // --decompose <csv> <file>
    if (argc > 3 && QString(argv[1]) == "--decompose") {
        const int rc = runDecompose(
            QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]),
            argc > 4 && QString(argv[4]) == QLatin1String("aligned")
        );
        clearRenderFixtures();
        return rc;
    }

    // --levels <file>
    if (argc > 2 && QString(argv[1]) == "--levels") {
        const int rc = runLevels(QString::fromLocal8Bit(argv[2]));
        clearRenderFixtures();
        return rc;
    }

    // --lodscan <anie> <frame>
    if (argc > 3 && QString(argv[1]) == "--lodscan") {
        const int rc = runLodScan(
            QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]).toInt(),
            argc > 4 ? QString::fromLocal8Bit(argv[4]).toFloat() : 0.25f
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
    suiteTexelPhase();
    suiteTwoXMechanism();
    suiteMipKernel();
    suiteMipKernelRandom();
    suiteMarginParity();
    suiteMarginDepth();
    suiteDeepCoverage();
    suiteLodSweep();

    std::printf("\n%d checks, %d failures\n", gChecks, gFails);
    std::fflush(stdout);

    // tear GL objects down while the context is still current
    clearRenderFixtures();
    return gFails ? 1 : 0;
}
