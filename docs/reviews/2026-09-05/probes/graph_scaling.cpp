#include "Graph/AudioGraph.hpp"
#include <chrono>
#include <cstdio>
using namespace daw::engine;
struct CheapNode : Node {
    std::string_view name() const noexcept override { return "probe"; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }
    void process(const ProcessContext&) override {}
};
int main() {
    for (int tracks : {500,1000,2000,4000}) {
        AudioGraph graph;
        const auto sink = graph.adoptNode(std::make_shared<CheapNode>());
        graph.setSink(sink);
        for (int i=0;i<tracks;++i) {
            auto a=graph.adoptNode(std::make_shared<CheapNode>());
            auto b=graph.adoptNode(std::make_shared<CheapNode>());
            auto c=graph.adoptNode(std::make_shared<CheapNode>());
            graph.connect(a,b); graph.connect(b,c); graph.connect(c,sink);
        }
        auto start=std::chrono::steady_clock::now();
        auto result=graph.compile(PrepareInfo{48000,64,2});
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        const std::size_t nodes=tracks*3+1;
        std::printf("tracks=%d nodes=%zu compile=%.2f ms ancestors=%.1f MiB status=%d\n",tracks,nodes,ms,((nodes+63)/64)*nodes*8/1048576.,bool(result));
    }
}
