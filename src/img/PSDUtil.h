#ifndef IMG_PSDUTIL_H
#define IMG_PSDUTIL_H

#include <memory>
#include <QRect>
#include "XC.h"
#include "img/PSDFormat.h"

namespace img {

// CSP marks layers with proprietary settings via a "tsly" additional-info block whose
// 4-byte value is 0 (CSP-specific) instead of 1 (plain): CSP's Add (Glow)/Glow Dodge are
// exported with the plain "lddg"/"div " blend keys plus this flag, so importers need it
// to tell them apart from LinearDodge/ColorDodge. Adobe never writes "tsly", so files
// without the block always read as plain. See getBlendModeFromPSD.
//
// The dichotomy (0 = CSP glow mode, 1 = plain, identical 4CCs) was verified against a
// pair of CSP-authored PSDs exported from the same document - one with the glow modes,
// one with their non-glow equivalents. Those exports live outside the repo (verified on
// the reverse-engineering author's machine); nothing in tests/golden covers AddGlow/
// GlowDodge because neither Krita nor GIMP can write the "tsly" block.
inline bool psdHasCspTslyFlag(const PSDFormat::Layer& aLayer) {
    for (const auto& info : aLayer.additionalInfos) {
        if (info->key == "tsly" && info->dataLength >= 4)
            return info->data[0] == 0 && info->data[1] == 0 && info->data[2] == 0 && info->data[3] == 0;
    }
    return false;
}

// Photoshop's "lyid" layer id: a stable identity that survives renames, unique within its
// document (layers and groups both carry one). It is what a source reload should match on,
// because the layer NAME is a display label the artist may change and may even repeat. An
// empty result means the file has no id for this layer (an older Photoshop, or another
// producer), and matching falls back to the name.
inline QString psdLayerStableId(const PSDFormat::Layer& aLayer) {
    for (const auto& info : aLayer.additionalInfos) {
        if (info->key != "lyid" || info->dataLength < 4 || !info->data) {
            continue;
        }
        const uint32 id = ((uint32)info->data[0] << 24) | ((uint32)info->data[1] << 16)
            | ((uint32)info->data[2] << 8) | (uint32)info->data[3];
        if (id != 0) {
            return QStringLiteral("lyid:%1").arg(id);
        }
    }
    return QString();
}

class PSDUtil {
public:
    enum ColorFormat { ColorFormat_RGB8, ColorFormat_RGBA8 };

    static XCMemBlock
    makeClippedImage(const uint8* aTarget, const QRect& aRectT, const uint8* aBase, const QRect& aRectB);

    static size_t encodePackBits(const uint8* aSrc, uint8* aDst, size_t aLength);

    static XCMemBlock
    encodePlanePackBits(const uint8* aSrc, size_t aSrcLength, int aWidth, int aHeight, int aSrcStride);

    static bool makeChanneledImage(
        PSDFormat::Layer& aDst, const PSDFormat::Header& aHeader, const XCMemBlock& aSrc, ColorFormat aSrcFormat
    );

    static bool makeChanneledImage(
        PSDFormat::ImageData& aDst, const PSDFormat::Header& aHeader, const XCMemBlock& aSrc, ColorFormat aSrcFormat
    );

    static bool makeChanneledImage(
        PSDFormat::ChannelList& aDst,
        const PSDFormat::Header& aHeader,
        const XCMemBlock& aSrc,
        ColorFormat aSrcFormat,
        int aSrcWidth,
        int aSrcHeight
    );

    static bool decodePlanePackBits(
        uint8* aDst, size_t aDstLength, const uint8* aSrc, size_t aSrcLength, int aWidth, int aHeight, int aDstStride
    );

    static XCMemBlock
    makeInterleavedImage(const PSDFormat::Header& aHeader, const PSDFormat::Layer& aLayer, ColorFormat aFormat);

    static XCMemBlock
    makeInterleavedImage(const PSDFormat::Header& aHeader, const PSDFormat::ImageData& aImageData, ColorFormat aFormat);

    static XCMemBlock makeInterleavedImage(
        const PSDFormat::Header& aHeader,
        const PSDFormat::ChannelList& aChannels,
        ColorFormat aFormat,
        int aWidth,
        int aHeight
    );
};

} // namespace img

#endif // IMG_PSDUTIL_H
