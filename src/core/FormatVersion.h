#ifndef CORE_FORMAT_VERSION_H
#define CORE_FORMAT_VERSION_H

// Single source of truth for the ANIMFX on-disk format version.
//
// The four literals that used to be duplicated ~9x across src/common.pri and
// every src/*/CMakeLists.txt target_compile_definitions() block now live here.
// Keep the // version bump here and add an include to any new consumer.
//
// Bump history (minor):
//   0.4  oldest minor version the loader still reads (AE_PROJECT_FORMAT_OLDEST_*)
//   0.8 -> 0.9   Blur filter introduced (BlurKey layout, src/core/BlurKey.h)
//   0.9 -> 0.10  folder blend mode 4CC in the FolderNd object block
//                (src/core/FolderNode.cpp, read under minor >= 10)
//   0.10 -> 0.11 the global block carries the frame range start (the first 4
//                bytes of its reserve region; older files default to 0, so
//                they load as [0, maxFrame])
//   0.11 -> 0.12 every object block (LayerNd_/FolderNd) carries the node's
//                Photoshop "Blend If" ranges (8 bytes: destination then source
//                trapezoid); older files load with the ranges inactive
//   0.13 -> 0.14 the layer block (LayerNd_) carries the imported Photoshop layer
//                effects this build draws (colour overlay, linear gradient
//                overlay) plus the keys of the ones it only recognises
//   0.12 -> 0.13 the resource node block (ResNode_) carries the layer's mask:
//                a presence flag, an enabled/inverted flags byte and the
//                single-channel A8 plane; older files load without a mask
//   0.14 -> 0.15 the resource node block also carries the layer's outline
//                distance field (an A8 plane, baked at import), which
//                Photoshop's Stroke and Bevel/Emboss are functions of; older
//                files load without one, and the effects that need it are then
//                reported as not drawn instead of being drawn wrong
//   0.15 -> 0.16 the resource node block also carries the resource's STABLE identity
//                (Photoshop's "lyid" layer id when the source had one, else empty),
//                the key a source reload matches on, plus its DERIVED CHANNELS' state
//                (which channels a consumer wants and the recipe each carried plane
//                was built with). Older files load with no id - falling back to
//                matching by layer name, which is what they were written under - and
//                with no derived state, which is derived on demand instead
#define AE_PROJECT_FORMAT_MAJOR_VERSION 0
#define AE_PROJECT_FORMAT_MINOR_VERSION 16

#define AE_PROJECT_FORMAT_OLDEST_MAJOR_VERSION 0
#define AE_PROJECT_FORMAT_OLDEST_MINOR_VERSION 4

static_assert(
    AE_PROJECT_FORMAT_MAJOR_VERSION >= AE_PROJECT_FORMAT_OLDEST_MAJOR_VERSION,
    "format major version regressed below the oldest readable version");
static_assert(
    AE_PROJECT_FORMAT_MINOR_VERSION >= AE_PROJECT_FORMAT_OLDEST_MINOR_VERSION,
    "format minor version regressed below the oldest readable version");

#endif // CORE_FORMAT_VERSION_H