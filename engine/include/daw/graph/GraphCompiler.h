#pragma once

#include <memory>
#include <vector>

#include "daw/graph/AudioFileSource.h"
#include "daw/graph/AudioOutputNode.h"
#include "daw/graph/GainNode.h"
#include "daw/graph/ProcessGraph.h"
#include "daw/graph/SummingBus.h"
#include "daw/model/Session.h"

namespace daw::graph {

struct NodeConnection {
    int sourceNodeIndex = -1;
    int targetNodeIndex = -1;
};

class GraphCompiler {
public:
    GraphCompiler() = default;

    void addNode(std::unique_ptr<ProcessNode> node);

    void addConnection(int sourceIndex, int targetIndex);

    std::unique_ptr<ProcessGraph> compile(double sampleRate, int maxBlockSize);

    static std::unique_ptr<ProcessGraph> compileSession(
        const daw::model::Session& session,
        double sampleRate,
        int maxBlockSize);

    int nodeCount() const noexcept { return static_cast<int>(nodes_.size()); }

    void clear()
    {
        nodes_.clear();
        connections_.clear();
    }

private:
    struct NodeEntry {
        std::unique_ptr<ProcessNode> node;
        int inDegree = 0;
        int outputBufferIndex = -1;
    };

    std::vector<NodeEntry> nodes_;
    std::vector<NodeConnection> connections_;
};

} // namespace daw::graph
