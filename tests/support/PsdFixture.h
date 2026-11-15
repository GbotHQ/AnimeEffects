#ifndef TESTS_SUPPORT_PSDFIXTURE_H
#define TESTS_SUPPORT_PSDFIXTURE_H

// Fixture writers for the PSD layer blocks the tests and the harnesses hand to the readers: the
// big-endian descriptor layout Photoshop writes (img/PSDDescriptor), and the layer-effects block
// ("lfx2") built out of it.
//
// Why ONE copy of this, shared: a fixture is a claim about the file format, and two harnesses
// writing the same block from two copies of the same code is how a fixture starts confirming a
// reader's bug instead of catching it. The Stroke's paint type is the worked example, and
// tools/render_verify.cpp carries its history: the reader tested "SCol", which no producer writes,
// so a solid stroke was misfiled and dropped - and the harness's own helper wrote "SCol" too, so
// the suite agreed with the bug for as long as nobody wrote a file with the other tooling. One
// place to be wrong is also one place to fix.
//
// The layout is written from the format, not from the reader: keys are 4-byte terms or counted
// strings, values carry their OSType, opaque unit floats are "UntF" + unit + double, and the
// descriptor header is a version pair.
//
// `XCMemBlock`-friendly: makeLfx2 returns the bytes of the block's data area, i.e. exactly what
// PSDFormat::AdditionalLayerInfo::data holds and dataLength counts.

#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "XCType.h"

namespace psdfx {

// a minimal big-endian descriptor writer, mirroring img/PSDDescriptor's documented layout
class DescWriter {
public:
    std::vector<uint8> data;

    void u32(uint32 aValue) {
        data.push_back((uint8)(aValue >> 24));
        data.push_back((uint8)(aValue >> 16));
        data.push_back((uint8)(aValue >> 8));
        data.push_back((uint8)aValue);
    }
    void f64(double aValue) {
        uint64 bits = 0;
        std::memcpy(&bits, &aValue, sizeof(bits));
        for (int i = 7; i >= 0; --i)
            data.push_back((uint8)(bits >> (i * 8)));
    }
    void key(const std::string& aKey) {
        if (aKey.size() == 4) {
            u32(0); // a 4-character key is written as a term
        } else {
            u32((uint32)aKey.size());
        }
        data.insert(data.end(), aKey.begin(), aKey.end());
    }
    void text(const std::string& aText) {
        u32((uint32)aText.size());
        for (char c : aText) {
            data.push_back(0);
            data.push_back((uint8)c);
        }
    }
    void ostype(const char* aType) { data.insert(data.end(), aType, aType + 4); }
    void enumValue(const char* aType, const char* aValue) {
        ostype("enum");
        key(aType);
        key(aValue);
    }
    void unitFloat(const char* aUnit, double aValue) {
        ostype("UntF");
        data.insert(data.end(), aUnit, aUnit + 4);
        f64(aValue);
    }
    void boolean(bool aValue) {
        ostype("bool");
        data.push_back(aValue ? 1 : 0);
    }
    void number(double aValue) {
        ostype("doub");
        f64(aValue);
    }
    void color(double aR, double aG, double aB) {
        ostype("Objc");
        text("");
        key("null");
        u32(3);
        key("Rd  ");
        number(aR);
        key("Grn ");
        number(aG);
        key("Bl  ");
        number(aB);
    }
    void startDescriptor(const std::string& aClass, uint32 aCount) {
        ostype("Objc");
        text("");
        key(aClass);
        u32(aCount);
    }
};

// one effect entry, i.e. an lfx2 member: its key, then a descriptor
using EffectWriter = std::function<void(DescWriter&)>;

// Photoshop's Stroke ("FrFX") as this repository's test file writes it: 4 px inside, a
// two-stop gradient (251,250,158) -> (250,230,83), Screen, 100 %, at -78 degrees scaled to
// 81 % of the layer extent
inline void addStroke(DescWriter& w, const char* aStyle, const char* aPaintType, double aSize) {
    w.key("FrFX");
    // keys written below: enab, Md, Opct, Styl, PntT, Sz, Clr [+ Grad, Scl, Angl, Rvrs]
    w.startDescriptor("null", aPaintType && std::strcmp(aPaintType, "GrFl") == 0 ? 11 : 7);
    w.key("enab");
    w.boolean(true);
    w.key("Md  ");
    w.enumValue("BlnM", "Scrn");
    w.key("Opct");
    w.unitFloat("#Prc", 100.0);
    w.key("Styl");
    w.enumValue("FStl", aStyle);
    w.key("PntT");
    w.enumValue("FrFl", aPaintType);
    w.key("Sz  ");
    w.unitFloat("#Pxl", aSize);
    w.key("Clr ");
    w.color(250.0, 235.0, 66.0);
    if (aPaintType && std::strcmp(aPaintType, "GrFl") == 0) {
        w.key("Grad");
        w.startDescriptor("Grdn", 3);
        w.key("GrdF");
        w.enumValue("GrdF", "CstS");
        w.key("Clrs");
        w.ostype("VlLs");
        w.u32(2);
        const struct { double loc, r, g, b; } stops[2] = {{0.0, 251.0, 250.0, 158.0}, {4096.0, 250.0, 230.0, 83.0}};
        for (const auto& st : stops) {
            w.startDescriptor("Clrt", 2);
            w.key("Clr ");
            w.color(st.r, st.g, st.b);
            w.key("Lctn");
            w.number(st.loc);
        }
        w.key("Trns");
        w.ostype("VlLs");
        w.u32(2);
        for (double loc : {0.0, 4096.0}) {
            w.startDescriptor("TrnS", 2);
            w.key("Opct");
            w.unitFloat("#Prc", 100.0);
            w.key("Lctn");
            w.number(loc);
        }
        w.key("Scl ");
        w.unitFloat("#Prc", 81.0);
        w.key("Angl");
        w.unitFloat("#Ang", -78.0);
        w.key("Rvrs");
        w.boolean(false);
    }
}

// the two-stop gold gradient this repository's test file uses on its title layer
inline void addGradientOverlay(DescWriter& w, bool aEnabled, const char* aStyle, double aAngle, double aScale) {
    w.key("GrFl");
    w.startDescriptor("null", 6);
    w.key("enab");
    w.boolean(aEnabled);
    w.key("Md  ");
    w.enumValue("BlnM", "Nrml");
    w.key("Type");
    w.enumValue("GrdT", aStyle);
    w.key("Angl");
    w.unitFloat("#Ang", aAngle);
    w.key("Scl ");
    w.unitFloat("#Prc", aScale);
    w.key("Grad");
    {
        w.startDescriptor("Grdn", 3);
        w.key("GrdF");
        w.enumValue("GrdF", "CstS");
        w.key("Clrs");
        w.ostype("VlLs");
        w.u32(2);
        const struct { double loc, r, g, b; } stops[2] = {{0.0, 228.0, 161.0, 30.0}, {4096.0, 248.0, 213.0, 79.0}};
        for (const auto& s : stops) {
            w.startDescriptor("Clrt", 2);
            w.key("Clr ");
            w.color(s.r, s.g, s.b);
            w.key("Lctn");
            w.number(s.loc);
        }
        w.key("Trns");
        w.ostype("VlLs");
        w.u32(2);
        for (double loc : {0.0, 4096.0}) {
            w.startDescriptor("TrnS", 2);
            w.key("Opct");
            w.unitFloat("#Prc", 100.0);
            w.key("Lctn");
            w.number(loc);
        }
    }
}

// Chrome ("ChFX"): the inner lobe of Bevel & Emboss, drawn as a blurred silhouette of the layer
inline void addChrome(DescWriter& w, double aBlur, double aOpacityPercent, double aDistance,
                      double aAngleDeg, const char* aModeKey) {
    w.key("ChFX");
    w.startDescriptor("null", aBlur > 0.0 ? 8 : 1);
    w.key("enab");
    w.boolean(true);
    if (aBlur > 0.0) {
        w.key("Md  ");
        w.enumValue("BlnM", aModeKey);
        w.key("Opct");
        w.unitFloat("#Prc", aOpacityPercent);
        w.key("blur");
        w.unitFloat("#Pxl", aBlur);
        w.key("Dstn");
        w.unitFloat("#Pxl", aDistance);
        w.key("lagl");
        w.unitFloat("#Ang", aAngleDeg);
        w.key("Clr ");
        w.color(243.0, 203.0, 72.0);
        w.key("Invr");
        w.boolean(true);
    }
}

// A drop shadow ("DrSh"): a blurred silhouette composited UNDER the layer, i.e. the family the
// renderer draws through a real Gaussian pass (see core/LayerEffects.h). It is one of the two
// effect kinds a source reload can add to a layer, which is what the reload tests need it for.
inline void addDropShadow(DescWriter& w, double aBlur, double aOpacityPercent, double aDistance,
                          double aAngleDeg, const char* aModeKey) {
    w.key("DrSh");
    w.startDescriptor("null", 7);
    w.key("enab");
    w.boolean(true);
    w.key("Md  ");
    w.enumValue("BlnM", aModeKey);
    w.key("Opct");
    w.unitFloat("#Prc", aOpacityPercent);
    w.key("blur");
    w.unitFloat("#Pxl", aBlur);
    w.key("Dstn");
    w.unitFloat("#Pxl", aDistance);
    w.key("lagl");
    w.unitFloat("#Ang", aAngleDeg);
    w.key("Clr ");
    w.color(0.0, 0.0, 0.0);
}

// Bevel & Emboss ("ebbl"): parsed and REPORTED, never drawn by this build (it needs a lighting
// pass nothing else here has). A fixture that switches it on is how "the import says so" is
// pinned - the reader has nothing else to check it against.
inline void addBevel(DescWriter& w, bool aEnabled) {
    w.key("ebbl");
    w.startDescriptor("null", 1);
    w.key("enab");
    w.boolean(aEnabled);
}

// the whole lfx2 block: the descriptor-block header (version + data version) followed by one
// descriptor whose members are the effects
inline std::vector<uint8> makeLfx2(const std::vector<EffectWriter>& aEffects) {
    DescWriter w;
    {
        DescWriter head;
        head.u32(0);  // version
        head.u32(16); // data version
        w.data = head.data;
    }
    DescWriter body;
    // descriptor name (unicode string, empty name is one NUL character) + class "null"
    body.u32(1);
    body.data.push_back(0);
    body.data.push_back(0);
    body.key("null");
    body.u32((uint32)aEffects.size());
    for (const auto& effect : aEffects) {
        effect(body);
    }
    w.data.insert(w.data.end(), body.data.begin(), body.data.end());
    return w.data;
}

// ------------------------------------------------ the shapes the harnesses ask for by name

inline std::vector<uint8> makeLfx2Bevel(bool aEnabled) {
    return makeLfx2({[aEnabled](DescWriter& w) { addBevel(w, aEnabled); }});
}

inline std::vector<uint8> makeLfx2Stroke(const char* aStyle, const char* aPaintType, double aSize) {
    return makeLfx2({[=](DescWriter& w) { addStroke(w, aStyle, aPaintType, aSize); }});
}

// a drop shadow the renderer draws: the blurred silhouette needs a radius above zero, and the
// tint/opacity/distance decide what it looks like
inline std::vector<uint8> makeLfx2DropShadow(
    double aBlur, double aOpacityPercent, double aDistance = 11.0, double aAngleDeg = 120.0,
    const char* aModeKey = "Nrml"
) {
    return makeLfx2({[=](DescWriter& w) {
        addDropShadow(w, aBlur, aOpacityPercent, aDistance, aAngleDeg, aModeKey);
    }});
}

} // namespace psdfx

#endif // TESTS_SUPPORT_PSDFIXTURE_H
