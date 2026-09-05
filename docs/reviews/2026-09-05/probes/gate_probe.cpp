#include "Engine/RealtimeEngine.hpp"
#include <cstdio>
#include <algorithm>
using namespace daw::engine;
struct Probe : Node {
    int live = 0;
    int offline = 0;
    std::string_view name() const noexcept override { return "probe"; }
    bool isSource() const noexcept override { return true; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }
    void process(const ProcessContext& ctx) override {
        ctx.offline ? ++offline : ++live;
        for (ChannelCount ch = 0; ch < ctx.output.numChannels(); ++ch)
            std::fill_n(ctx.output.data(ch), ctx.frames, 1.0f);
    }
};
int main() {
    RealtimeEngine engine(1);
    auto probe = std::make_shared<Probe>();
    engine.graph().setSink(engine.graph().adoptNode(probe));
    if (!engine.prepare(48000, 64, 2)) return 2;
    float l[64]{}, r[64]{}; float* ptrs[]{l,r};
    AudioBlock block(ptrs, 2, 64);
    {
        RealtimeEngine::RenderGate outer(engine);
        engine.renderBlock(block, nullptr, 0, 64);
        std::printf("outer gate, before inner: live calls=%d\n", probe->live);
        { RealtimeEngine::RenderGate inner(engine); }
        engine.renderBlock(block, nullptr, 0, 64);
        std::printf("outer gate, after inner: live calls=%d (expected 0)\n", probe->live);
    }
    const int before = probe->live;
    auto status = engine.renderOffline(0, 128, 64,
        [&](const AudioBlock&, FrameCount) {
            { RealtimeEngine::RenderGate callbackGate(engine); }
            engine.renderBlock(block, nullptr, 0, 64);
            return true;
        });
    std::printf("offline render status=%d, unexpected live calls=%d, offline calls=%d\n",
        bool(status), probe->live-before, probe->offline);
}
