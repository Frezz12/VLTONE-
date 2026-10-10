#pragma once

#include "model/AudioEditDocument.hpp"
#include "Audio/SampleBuffer.hpp"
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>

namespace daw::audioedit {

using Decoder = std::function<std::shared_ptr<const engine::SampleBuffer>(const AudioEditSource&)>;
using Continue = std::function<bool()>;

AudioEditDocument fromSource(AudioEditSource source);
bool valid(const AudioEditDocument& document);
bool silence(AudioEditDocument&, AudioEditFrame first, AudioEditFrame last);
bool split(AudioEditDocument&, AudioEditFrame at);
AudioEditDocument copy(const AudioEditDocument&, AudioEditFrame first, AudioEditFrame last);
bool paste(AudioEditDocument&, const AudioEditDocument& clipboard, AudioEditFrame at,
           AudioEditFrame replaceEnd);
bool move(AudioEditDocument&, const std::string& regionId, AudioEditFrame destination);
bool reverse(AudioEditDocument&, AudioEditFrame first, AudioEditFrame last);
bool gain(AudioEditDocument&, AudioEditFrame first, AudioEditFrame last, double db);
bool fade(AudioEditDocument&, AudioEditFrame first, AudioEditFrame last,
          bool fadeIn, AudioEditFrame length, double curve = 0);
std::shared_ptr<const engine::SampleBuffer> render(const AudioEditDocument&, const Decoder&,
                                                 const Continue& keepGoing = {});
double peak(const engine::SampleBuffer&, AudioEditFrame first, AudioEditFrame last);
nlohmann::json toJson(const AudioEditDocument&);
bool fromJson(const nlohmann::json&, AudioEditDocument&);

} // namespace daw::audioedit
