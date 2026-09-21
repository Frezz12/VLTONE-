#pragma once
#include "collaboration/ProjectCommand.hpp"
#include <nlohmann/json_fwd.hpp>
namespace daw::collab {
nlohmann::json midiContentToJson(const ClipModel &content);
bool midiContentFromJson(const nlohmann::json &value, ClipModel &out);
std::vector<PrepareMidiPart> prepareMidiContent(const std::string &recordingId,
                                                const std::string &contentId,
                                                const ClipModel &clip);
bool validMidiContentIdentities(const ClipModel &);
ClipModel joinMidiContent(const std::vector<PrepareMidiPart> &parts);
} // namespace daw::collab
