// Contract tests for the EDIT half of proportional keys (src/ctrl/TimeLineUtil.cpp):
// a frame move, a value edit and a proportional toggle each stay ONE undo step
// while every key they touch - the selected ones AND the proportional keys
// between them - comes back exactly.
//
// What is pinned, and why:
//   * a frame move of an absolute key carries the proportional keys between its
//     bounds: Move keys 0 abs / 2 prop / 4 abs / 6 prop / 8 abs moved +2 at frame
//     4 land at 0,3,6,7,8. ONE undo restores 2/4/6, ONE redo rebuilds 3/6/7, and
//     repeated +/- moves of the same absolute key return the proportional keys to
//     the same frames - the weights are stored and re-applied, never re-derived
//     from the rounded destination, which is the drift this test would otherwise
//     only measure later;
//   * moving a selected PROPORTIONAL key directly keeps it proportional and
//     re-bases its weight from the frame it now owns (2 -> 3 between 0 and 4 is
//     0.75), and undo restores both the frame and the weight;
//   * a value edit through the timeline's own assign path (assignMoveKeyData)
//     makes a proportional sibling follow per its stored weight (a 40 -> 60 edit
//     of the right bound moves a 0.5-weighted key from 20 to 30) in the SAME undo
//     step, and undo restores the values AND the weights;
//   * setProportionalKeys enables/disables a selection as ONE step announcing
//     core::TimeLineEvent::Type_ChangeKeyProportional, with undo/redo restoring
//     the previous state (weights included).
//
// Headless: QApplication for the project/timeline internals; no widgets, no GL,
// no fixture. Every key is installed through the production
// core::TimeLine::createPusher path, and the frame/value commands are the ones the
// timeline and the graph use.
#include <QApplication>
#include <QJsonObject>
#include <QVector2D>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include "XCAssert.h"
#include "cmnd/ScopedMacro.h"
#include "cmnd/Stack.h"
#include "core/MoveKey.h"
#include "core/ObjectNode.h"
#include "core/Project.h"
#include "core/TimeKey.h"
#include "core/TimeKeyType.h"
#include "core/TimeLine.h"
#include "core/TimeLineEvent.h"
#include "ctrl/TimeLineUtil.h"
#include "ctrl/proportional/prop_Keys.h"
#include "util/Range.h"

// core::Project.cpp references the GUI AudioPlaybackWidget::deserialize only to
// load a .aemus sidecar, which this test never has; ctrl's timeline editor
// references two GUI symbols that this test does not drive. Stub them (the same
// three stubs proportional_keys / graph_drag use) so the link succeeds without
// pulling the whole gui library in.
#include "gui/AudioPlaybackWidget.h"
bool AudioPlaybackWidget::deserialize(const QJsonObject&, std::vector<audioConfig>*) { return false; }

#include "gui/ObjectTreeWidget.h"
bool gui::obj::Item::isTopNode() const { return false; }
std::pair<core::TimeKey*, core::LayerMesh*> gui::ObjectTreeWidget::getAreaMeshImpl(core::ObjectNode&, int) {
    return { nullptr, nullptr };
}

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

namespace prop = ctrl::proportional;
using core::TimeKeyType;
using core::TimeKeyType_Move;

int failures = 0;

void check(bool aOk, const char* aWhat) {
    if (!aOk) {
        std::printf("FAIL  %s\n", aWhat);
        ++failures;
    }
}

bool nearly(float a, float b, float aEps = 1e-4f) { return std::fabs(a - b) <= aEps; }

// The frames the map holds, in frame order (QMap order).
QVector<int> framesOf(const core::TimeLine& aLine, TimeKeyType aType) {
    QVector<int> frames;
    const core::TimeLine::MapType& map = aLine.map(aType);
    for (auto itr = map.begin(); itr != map.end(); ++itr) {
        frames.append(itr.key());
    }
    return frames;
}

bool framesAre(const QVector<int>& aFrames, std::initializer_list<int> aExpected) {
    if (aFrames.size() != static_cast<int>(aExpected.size())) {
        return false;
    }
    int i = 0;
    for (int expected : aExpected) {
        if (aFrames.at(i) != expected) {
            return false;
        }
        ++i;
    }
    return true;
}

// Push one key through the production path, which is where a real edit installs
// keys.
core::MoveKey* pushMove(core::TimeLine& aLine, cmnd::Stack& aStack, int aFrame, float aX) {
    auto* key = new core::MoveKey();
    key->setPos(QVector2D(aX, 0.0f));
    aStack.push(aLine.createPusher(TimeKeyType_Move, aFrame, key));
    return key;
}

// Proportional keys are authored through the same helpers the application uses:
// the flag, then the weights from the CURRENT position against the absolute
// neighbours.
void makeProportional(core::TimeLine& aLine, core::MoveKey* aKey) {
    aKey->setProportional(true);
    prop::rebase(*aKey, prop::findBounds(aLine.map(TimeKeyType_Move), *aKey));
}

util::Range allFrames() {
    return util::Range(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
}

// The command the timeline's own move gesture pushes: one MoveFrameOfKey in one
// macro (exec runs on push, exactly as the editor's stack does it).
ctrl::TimeLineUtil::MoveFrameOfKey* pushFrameCommand(cmnd::Stack& aStack, const core::TimeLineEvent& aSource) {
    auto* command = new ctrl::TimeLineUtil::MoveFrameOfKey(aSource);
    cmnd::ScopedMacro macro(aStack, "Move keys");
    aStack.push(command);
    return command;
}

} // namespace

XCAssertHandler* gXCAssertHandler = &gAssertHandler;
XCErrorHandler* gXCErrorHandler = &gErrorHandler;

namespace core {
// Minimal ObjectNode subclass owning a real TimeLine (ObjectNode::timeLine() is
// virtual and null by default). No meshes, no resources - just key maps.
class TestNode: public ObjectNode {
    TimeLine mTimeLine;

public:
    ObjectType type() const override { return ObjectType_Layer; }
    void setName(const QString&) override {}
    const QString& name() const override {
        static QString s;
        return s;
    }
    float initialDepth() const override { return 0; }
    void setVisibility(bool) override {}
    bool isVisible() const override { return true; }
    void setSlimDown(bool) override {}
    bool isSlimmedDown() const override { return false; }
    bool canHoldChild() const override { return false; }
    void setInitialRect(const QRect&) override {}
    QRect initialRect() const override { return QRect(); }
    bool serialize(Serializer&) const override { return true; }
    bool deserialize(Deserializer&) override { return true; }
    TimeLine* timeLine() override { return &mTimeLine; }
    const TimeLine* timeLine() const override { return &mTimeLine; }
};
} // namespace core

namespace {

class NullAnimator: public core::Animator {
public:
    core::Frame currentFrame() const override { return core::Frame(); }
    void stop() override {}
    void suspend() override {}
    void resume() override {}
    bool isSuspended() const override { return false; }
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // --- 1. A frame move carries the proportional keys between the bounds ---
    {
        const int before = failures;
        core::TestNode node;
        core::TimeLine& line = *node.timeLine();
        cmnd::Stack setup;

        core::MoveKey* k0 = pushMove(line, setup, 0, 0.0f);
        core::MoveKey* k2 = pushMove(line, setup, 2, 10.0f);
        core::MoveKey* k4 = pushMove(line, setup, 4, 20.0f);
        core::MoveKey* k6 = pushMove(line, setup, 6, 30.0f);
        core::MoveKey* k8 = pushMove(line, setup, 8, 40.0f);
        (void)k0;
        (void)k8;

        makeProportional(line, k2);
        makeProportional(line, k6);
        check(nearly(k2->proportionalState()->timeWeight, 0.5f), "the 2-key starts halfway between 0 and 4");
        check(nearly(k6->proportionalState()->timeWeight, 0.5f), "the 6-key starts halfway between 4 and 8");

        core::TimeLineEvent source;
        source.setType(core::TimeLineEvent::Type_MoveKey);
        source.pushTarget(node, TimeKeyType_Move, 4);

        cmnd::Stack stack;
        ctrl::TimeLineUtil::MoveFrameOfKey* move = pushFrameCommand(stack, source);

        int clamped = 0;
        core::TimeLineEvent mod;
        check(
            move->modifyMove(mod, 2, allFrames(), &clamped),
            "the +2 move of the absolute key is accepted"
        );
        check(clamped == 2, "nothing is clamped away");
        check(
            framesAre(framesOf(line, TimeKeyType_Move), {0, 3, 6, 7, 8}),
            "moving 4 to 6 carries the 2-key to 3 and the 6-key to 7"
        );
        check(k4->frame() == 6, "the selected key is at its destination");
        check(
            mod.targets().size() == 1 && mod.targets().at(0).pos.index() == 4
                && mod.targets().at(0).subIndex == 6,
            "the modifier event names the selected key's source frame and destination"
        );

        stack.undo();
        check(
            framesAre(framesOf(line, TimeKeyType_Move), {0, 2, 4, 6, 8}),
            "ONE undo restores the absolute key and both projected keys"
        );
        check(nearly(k2->proportionalState()->timeWeight, 0.5f), "undo restores the 2-key's weight");
        check(nearly(k6->proportionalState()->timeWeight, 0.5f), "undo restores the 6-key's weight");

        stack.redo();
        check(
            framesAre(framesOf(line, TimeKeyType_Move), {0, 3, 6, 7, 8}),
            "ONE redo rebuilds the whole move"
        );

        // Squeeze and restore the same bound repeatedly: the stored weights must
        // answer the SAME frames, not walk a frame per step.
        check(move->modifyMove(mod, -2, allFrames(), &clamped), "the -2 move back is accepted");
        check(
            framesAre(framesOf(line, TimeKeyType_Move), {0, 2, 4, 6, 8}),
            "moving the bound back puts both proportional keys back"
        );
        check(move->modifyMove(mod, +2, allFrames(), &clamped), "the +2 move again is accepted");
        check(
            framesAre(framesOf(line, TimeKeyType_Move), {0, 3, 6, 7, 8}),
            "the second +2 move lands on the same frames (no drift)"
        );
        check(move->modifyMove(mod, -2, allFrames(), &clamped), "the second -2 move is accepted");
        check(
            framesAre(framesOf(line, TimeKeyType_Move), {0, 2, 4, 6, 8}),
            "and the proportional keys return to 2 and 6 again"
        );

        if (failures == before)
            std::printf("[1] MoveFrameOfKey: absolute move carries projected proportional keys\n");
    }

    // --- 2. A selected proportional key's direct move stays proportional ---
    {
        const int before = failures;
        core::TestNode node;
        core::TimeLine& line = *node.timeLine();
        cmnd::Stack setup;

        core::MoveKey* k0 = pushMove(line, setup, 0, 0.0f);
        core::MoveKey* k2 = pushMove(line, setup, 2, 10.0f);
        core::MoveKey* k4 = pushMove(line, setup, 4, 20.0f);
        core::MoveKey* k6 = pushMove(line, setup, 6, 30.0f);
        core::MoveKey* k8 = pushMove(line, setup, 8, 40.0f);
        (void)k0;
        (void)k4;
        (void)k8;

        makeProportional(line, k2);
        makeProportional(line, k6);

        core::TimeLineEvent source;
        source.setType(core::TimeLineEvent::Type_MoveKey);
        source.pushTarget(node, TimeKeyType_Move, 2);

        cmnd::Stack stack;
        ctrl::TimeLineUtil::MoveFrameOfKey* move = pushFrameCommand(stack, source);

        int clamped = 0;
        core::TimeLineEvent mod;
        check(move->modifyMove(mod, 1, allFrames(), &clamped), "the proportional key moves by +1");
        check(
            framesAre(framesOf(line, TimeKeyType_Move), {0, 3, 4, 6, 8}),
            "only the selected proportional key changes frame"
        );
        check(k2->isProportional(), "a moved proportional key stays proportional");
        check(
            nearly(k2->proportionalState()->timeWeight, 0.75f),
            "its weight is re-based from the frame it now owns (3 of 0..4)"
        );

        stack.undo();
        check(
            framesAre(framesOf(line, TimeKeyType_Move), {0, 2, 4, 6, 8}),
            "undo puts the proportional key back"
        );
        check(k2->isProportional(), "undo keeps it proportional");
        check(nearly(k2->proportionalState()->timeWeight, 0.5f), "undo restores its old weight");

        stack.redo();
        check(
            framesAre(framesOf(line, TimeKeyType_Move), {0, 3, 4, 6, 8}),
            "redo moves it again"
        );
        check(
            nearly(k2->proportionalState()->timeWeight, 0.75f),
            "redo installs the re-based weight"
        );

        if (failures == before)
            std::printf("[2] MoveFrameOfKey: a selected proportional key keeps its flag and weight\n");
    }

    // --- 3. A value edit reflows the proportional sibling in the same undo step ---
    {
        const int before = failures;
        NullAnimator animator;
        core::Project project("proportional_edits.anie", animator, nullptr);
        core::TestNode node;
        core::TimeLine& line = *node.timeLine();
        cmnd::Stack setup;

        core::MoveKey* k0 = pushMove(line, setup, 0, 0.0f);
        core::MoveKey* p2 = pushMove(line, setup, 2, 20.0f);
        core::MoveKey* k4 = pushMove(line, setup, 4, 40.0f);
        core::MoveKey* k8 = pushMove(line, setup, 8, 80.0f);
        (void)k0;
        (void)k8;

        makeProportional(line, p2);
        check(nearly(p2->proportionalState()->valueWeight[0], 0.5f), "the 2-key starts halfway in value");

        core::MoveKey::Data data = k4->data();
        data.setPos(QVector2D(60.0f, 0.0f));
        ctrl::TimeLineUtil::assignMoveKeyData(project, node, 4, data);

        check(nearly(k4->pos().x(), 60.0f), "the edited key takes the new value");
        check(
            nearly(p2->pos().x(), 30.0f),
            "the proportional key follows the edited bound per its 0.5 weight"
        );
        check(
            nearly(p2->proportionalState()->valueWeight[0], 0.5f),
            "following the edit does not re-derive the stored weight"
        );

        // ONE undo for the value write and the reflow together.
        project.commandStack().undo();
        check(nearly(k4->pos().x(), 40.0f), "undo restores the edited value");
        check(nearly(p2->pos().x(), 20.0f), "undo restores the proportional key's value");
        check(
            nearly(p2->proportionalState()->valueWeight[0], 0.5f),
            "undo restores the proportional key's weight"
        );

        project.commandStack().redo();
        check(nearly(k4->pos().x(), 60.0f), "redo re-applies the edited value");
        check(nearly(p2->pos().x(), 30.0f), "redo re-applies the reflow");

        if (failures == before)
            std::printf("[3] assignMoveKeyData: value reflow is one undo step with the edit\n");
    }

    // --- 4. setProportionalKeys: one step, announced, undo/redo restore state ---
    {
        const int before = failures;
        NullAnimator animator;
        core::Project project("proportional_edits.anie", animator, nullptr);
        core::TestNode node;
        core::TimeLine& line = *node.timeLine();
        cmnd::Stack setup;

        core::MoveKey* k0 = pushMove(line, setup, 0, 0.0f);
        core::MoveKey* k4 = pushMove(line, setup, 4, 40.0f);
        core::MoveKey* k8 = pushMove(line, setup, 8, 80.0f);
        (void)k0;
        (void)k8;

        core::TimeLineEvent::Type lastType = core::TimeLineEvent::Type_TERM;
        int events = 0;
        project.onTimeLineModified.connect([&](core::TimeLineEvent& aEvent, bool) {
            lastType = aEvent.type();
            ++events;
        });

        core::TimeLineEvent source;
        source.setType(core::TimeLineEvent::Type_ChangeKeyProportional);
        source.pushTarget(node, TimeKeyType_Move, 4);

        ctrl::TimeLineUtil::setProportionalKeys(project, source, true);
        check(k4->isProportional(), "enabling the selection makes the key proportional");
        check(
            lastType == core::TimeLineEvent::Type_ChangeKeyProportional,
            "the toggle announces Type_ChangeKeyProportional"
        );
        check(events == 1, "the toggle commits ONE notification");
        check(nearly(k4->proportionalState()->timeWeight, 0.5f), "enabling derives the frame weight from 0..8");
        check(nearly(k4->proportionalState()->valueWeight[0], 0.5f), "enabling derives the value weight too");

        project.commandStack().undo();
        check(!k4->isProportional(), "ONE undo makes the key absolute again");
        project.commandStack().redo();
        check(k4->isProportional(), "redo restores the proportional flag");
        check(nearly(k4->proportionalState()->timeWeight, 0.5f), "redo restores the derived weight");

        // The weights are the ones the previous state held, so disabling and
        // undoing must bring the SAME state back, not a re-derived one.
        ctrl::TimeLineUtil::setProportionalKeys(project, source, false);
        check(!k4->isProportional(), "disabling the selection makes the key absolute");
        check(
            lastType == core::TimeLineEvent::Type_ChangeKeyProportional,
            "the disable announces the same event type"
        );

        project.commandStack().undo();
        check(k4->isProportional(), "undo restores the key as proportional");
        check(nearly(k4->proportionalState()->timeWeight, 0.5f), "undo restores the weights it had");
        project.commandStack().redo();
        check(!k4->isProportional(), "redo disables it again");

        if (failures == before)
            std::printf("[4] setProportionalKeys: one announced step with undo/redo\n");
    }

    if (failures == 0)
        std::printf("proportional_edits: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
