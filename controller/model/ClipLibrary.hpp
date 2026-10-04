#pragma once
#include "model/Document.hpp"

namespace daw {
template <class Track, class Visitor>
void visitStoredPlugins(Track& track, Visitor visit) {
    if (track.instrument.isLoaded()) visit(track.instrument);
    if (track.channelColor) visit(*track.channelColor);
    for(auto& module:track.miniModules) visit(module);
    for (auto& slot : track.inserts) visit(slot);
    for (auto& slot : track.samplerFx.inserts) visit(slot);
    for (auto& clip : track.clips) {
        for (auto& slot : clip.inserts) visit(slot);
        for (auto& slot : clip.offlineProcess.chain) visit(slot);
    }
}
template <class Project, class Visitor>
void visitLibraryPlugins(Project& project, Visitor visit) {
    for (auto& entry : project.clipLibrary)
        for (auto& track : entry.tracks) visitStoredPlugins(track, visit);
}
} // namespace daw
