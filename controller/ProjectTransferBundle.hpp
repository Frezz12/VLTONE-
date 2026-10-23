#pragma once
#include "recovery/RecoverySnapshot.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace daw {
/// An owned package, never a reference into the source controller. Its lifetime
/// can be shared with undo records; normal project saves use the existing .vlt
/// serializer to put its media into the recipient's Content directory.
struct ProjectTransferResources {
    explicit ProjectTransferResources(std::filesystem::path root) : root(std::move(root)) {}
    ~ProjectTransferResources() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    std::filesystem::path root;
};
struct ProjectTransferBundle {
    std::shared_ptr<ProjectTransferResources> resources;
    recovery::RecoverySnapshot snapshot;
    bool wholeTracks = false;
    bool region = false;
    std::string sourceDocument;
    // A parameter number alone cannot establish compatibility across plugins.
    std::unordered_map<std::string, std::pair<PluginFormat, std::string>> automationPlugins;
    double originSeconds = 0.;
    bool empty() const { return snapshot.project.tracks.empty(); }
};
/// The application has one clipboard; its snapshot survives closing any tab.
inline ProjectTransferBundle& projectTransferClipboard() {
    static ProjectTransferBundle clipboard;
    return clipboard;
}
} // namespace daw
