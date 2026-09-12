// Measures where a source reload's time actually goes, so the reload design is decided by
// numbers rather than by assumption (BLEND_IF_ASSESSMENT.md section 18).
//
// It answers the question the reload path raises: "why is change detection a byte compare per
// layer?" The compare is not the expensive part - having something to compare is, because a
// reload today asks for every layer's pixels. This tool times the four stages a reload could
// choose between, on any PSD:
//
//   1. parse              - read the file and its records; every layer's COMPRESSED channel bytes
//                           end up in memory, none of them decoded (no GL, no project)
//   2. structure only     - ctrl::describePsd(..., wantPixels = false): names, ids, rects,
//                           blend/opacity/flags, masks' geometry, Blend If, layer effects
//   3. with pixels        - the same description with every layer and mask decoded (what a
//                           reload asks for today)
//   4. digest             - FNV-1a over each layer's compressed channel bytes. Lossless
//                           compression means equal compressed bytes imply equal pixels, so a
//                           digest match is a SOUND "unchanged" and a mismatch can only be a
//                           false positive (a re-encode that decodes the same), which costs one
//                           decode and ends in a no-op. This is the cheap change signal a reload
//                           should use in place of decoding everything.
//
// Build and run through the probe's script: tools/psd_composite_probe/build_and_run.sh --cost
// [<psd>]. No GL context and no Qt application object are needed; the shaders are never touched.
#include <QElapsedTimer>
#include <cstdio>
#include <fstream>
#include "XC.h"
#include "gui/AudioPlaybackWidget.h"
#include "img/PSDReader.h"
#include "ctrl/SourceDocument.h"

// normally installed by the application (src/gui/Main.cpp); the hooks are unused here
XCAssertHandler* gXCAssertHandler = nullptr;
XCErrorHandler* gXCErrorHandler = nullptr;
// core::Project's constructor references this; headless tools stub it (see the probe)
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.psd>\n", argv[0]);
        return 64;
    }
    QElapsedTimer timer;

    // 1. read + parse
    timer.start();
    std::ifstream file(argv[1], std::ios::binary);
    if (file.fail()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    img::PSDReader reader(file);
    if (reader.resultCode() != img::PSDReader::ResultCode_Success) {
        std::fprintf(stderr, "parse failed: %s\n", reader.resultMessage().c_str());
        return 1;
    }
    const img::PSDFormat& format = *reader.format();
    const qint64 parseMs = timer.elapsed();

    // 2. structure-only description (the cheapest complete description of the document)
    timer.restart();
    auto structure = ctrl::describePsd(format, "cost", false, nullptr);
    const qint64 structureMs = timer.elapsed();

    // 3. the same description with every layer's pixels and mask decoded - what a reload asks
    //    for today, and the stage this tool exists to weigh against 4.
    timer.restart();
    auto full = ctrl::describePsd(format, "cost", true, nullptr);
    const qint64 fullMs = timer.elapsed();

    // 4. the cheap, sound change signal
    timer.restart();
    size_t compressed = 0, layers = 0;
    uint64 lastHash = 0;
    for (const auto& layerPtr : format.layerAndMaskInfo().layers) {
        const img::PSDFormat::Layer& layer = *layerPtr;
        if (layer.entryType != img::PSDFormat::LayerEntryType_Layer) {
            continue;
        }
        ++layers;
        uint64 hash = 1469598103934665603ull; // FNV-1a offset basis
        for (const auto& channelPtr : layer.channels) {
            const img::PSDFormat::Channel& channel = *channelPtr;
            hash ^= (uint64)channel.compressionId; // ZIP and RLE of the same pixels differ
            hash *= 1099511628211ull;
            for (uint32 i = 0; i < channel.dataLength; ++i) {
                hash ^= channel.data[i];
                hash *= 1099511628211ull;
            }
            compressed += channel.dataLength;
        }
        lastHash = hash; // printed so the loop is not dead code
    }
    const qint64 digestMs = timer.elapsed();

    std::printf("%s\n", argv[1]);
    std::printf("  layers in records                %8zu  (%zu compressed channel bytes)\n", layers, compressed);
    std::printf("  parse (read + records, no decode) %7lld ms\n", (long long)parseMs);
    std::printf("  describePsd(wantPixels=false)     %7lld ms  (%zu top-level children, error='%s')\n",
        (long long)structureMs, structure.top.children.size(), structure.error.toUtf8().constData());
    std::printf("  describePsd(wantPixels=true)      %7lld ms  (error='%s')\n",
        (long long)fullMs, full.error.toUtf8().constData());
    std::printf("  digest of the compressed bytes    %7lld ms  (last hash %016llx)\n",
        (long long)digestMs, (unsigned long long)lastHash);
    std::printf("  => a reload that compares digests instead of decoding costs ~%lld ms,\n",
        (long long)(parseMs + structureMs + digestMs));
    std::printf("     against ~%lld ms for one that decodes every layer\n", (long long)(parseMs + fullMs));
    return 0;
}
