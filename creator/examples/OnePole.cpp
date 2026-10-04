#include <vlt/creator.hpp>

// State is private to each graph node / callable binding. Never use a mutable
// global or static variable for audio history.
struct State {
  vlt::AudioFrame history{};
};

void reset(State &state) { state.history = {}; }

VLT_NODE vlt::AudioFrame process(State &state, const vlt::Context &context,
                                 VLT_PORT("input") vlt::AudioFrame input,
                                 VLT_PORT("cutoff") float cutoff = 1500.f) {
  cutoff = vlt::clamp(cutoff, 20.f, float(context.sampleRate * .45));
  const auto coefficient =
      1.f - vlt::exp(-6.28318530718f * cutoff / float(context.sampleRate));
  state.history = state.history + (input - state.history) * coefficient;
  return state.history;
}
