// Packed bone-influence attribute storage.
//
// The skinning attributes are 4 x uint8 indices (bound with
// glVertexAttribIPointer, read as ivec4) plus 4 x float32 weights per set:
// 40 B/vertex from 64. A uint16 normalized weight variant was measured and
// rejected (it exceeded the 1-LSB pixel gate on 2.anie); this gate pins the
// shipped representation so a future packing change cannot land silently.
// The GL binding itself needs a project with bones and is exercised by
// render_probe; this is the CPU-side contract and the default values.
#include <QApplication>
#include <QJsonObject>
#include <cstdio>
#include <vector>
#include "XCAssert.h"
#include "core/BoneInfluenceMap.h"
#include "gui/AudioPlaybackWidget.h"

// Project.cpp references this GUI entry point; the test never opens a .aemus.
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

namespace {
class HarnessAssertHandler: public XCAssertHandler {
public:
    void failure() const override { std::abort(); }
};
class HarnessErrorHandler: public XCErrorHandler {
public:
    void critical(const QString&, const QString&, const QString&) const override { std::abort(); }
};
HarnessAssertHandler gAssertHandler;
HarnessErrorHandler gErrorHandler;
int failures = 0;

void check(bool aCondition, const char* aWhat) {
    if (aCondition) {
        std::printf("PASS  %s\n", aWhat);
    } else {
        std::printf("FAIL  %s\n", aWhat);
        ++failures;
    }
}
} // namespace

XCAssertHandler* gXCAssertHandler = &gAssertHandler;
XCErrorHandler* gXCErrorHandler = &gErrorHandler;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    (void)argv;

    // 4 indices + 4 weights per set, two sets. The shader reads one ivec4/vec4
    // per set; changing any component size changes both the upload and the
    // resident bytes, so the total is part of the contract.
    static_assert(
        2 * (core::BoneInfluenceMap::kBonePerVtxMaxEach * sizeof(core::BoneInfluenceMap::IndicesType) +
             core::BoneInfluenceMap::kBonePerVtxMaxEach * sizeof(core::BoneInfluenceMap::WeightsType)) == 40,
        "packed bone-influence attributes must be 40 B/vertex (from 64)"
    );
    static_assert(sizeof(core::BoneInfluenceMap::IndicesType) == 1, "bone indices are uint8");
    static_assert(sizeof(core::BoneInfluenceMap::WeightsType) == 4, "bone weights are float32");

    const int kVertexCount = 3;
    core::BoneInfluenceMap map;
    map.setMaxBoneCount(32);
    map.allocate(kVertexCount, true);
    check(map.vertexCount() == kVertexCount, "vertex count is retained");

    const core::BoneInfluenceMap::Accessor accessor = map.accessor();
    const core::BoneInfluenceMap::IndicesType* indices[2] = {accessor.indices0(), accessor.indices1()};
    const core::BoneInfluenceMap::WeightsType* weights[2] = {accessor.weights0(), accessor.weights1()};
    constexpr int kTuple = core::BoneInfluenceMap::kBonePerVtxMaxEach;

    bool indicesClear = true;
    bool weightsDefault = true;
    for (int set = 0; set < 2; ++set) {
        if (!indices[set] || !weights[set]) {
            indicesClear = false;
            weightsDefault = false;
            continue;
        }
        for (int vertex = 0; vertex < kVertexCount; ++vertex) {
            for (int component = 0; component < kTuple; ++component) {
                const int at = vertex * kTuple + component;
                if (indices[set][at] != 0)
                    indicesClear = false;
                const float expected = component == 0 ? 1.0f : 0.0f;
                if (weights[set][at] != expected)
                    weightsDefault = false;
            }
        }
    }
    check(indicesClear, "every default index is bone 0");
    check(weightsDefault, "every default weight is 1 on the first slot and 0 elsewhere");

    if (failures == 0) {
        std::printf("PASS: packed influence storage\n");
    } else {
        std::printf("FAILED %d check(s)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
