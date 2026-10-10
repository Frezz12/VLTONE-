#include "AudioContentValidation.hpp"
#include <stdexcept>

namespace daw {
namespace {
[[noreturn]] void invalid(const char* message) { throw std::invalid_argument(message); }
template<class Enum> void range(Enum value, Enum last) {
    if (std::uint32_t(value) > std::uint32_t(last)) invalid("unknown content enum");
}
}

void validateAudioContent(const AudioContentSpec& content) {
    if (content.clips) {
        const auto check = [](const auto& clips) { if (clips) for (const auto& clip : *clips)
            if (clip.warp && !validWarp(*clip.warp)) invalid("invalid clip warp"); };
        check(content.clips->shared);
        for (const auto& [id, clips] : content.clips->individual) check(clips);
    }
    if (content.midi && content.midi->notes)
        for (const auto& note : *content.midi->notes) {
            if (note.key > 127 || note.velocity > 127 || note.channel > 15 || note.releaseVelocity > 127)
                invalid("invalid MIDI note");
            for (const auto& point : note.pitch) range(point.shape, engine::curve::Shape::SCurve);
        }
    if (content.midi && content.midi->controllers)
        for (const auto& controller : *content.midi->controllers) {
            if (controller.channel < 0 || controller.channel > 15 || controller.key < 0 || controller.key > 127 ||
                controller.cc < 0 || controller.cc > 131) invalid("invalid MIDI controller");
            for (const auto& point : controller.points) range(point.shape, engine::curve::Shape::SCurve);
        }
}
} // namespace daw
