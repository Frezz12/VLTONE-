#pragma once

#include "Host/PluginTypes.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace daw::plugins::morph {

enum Param : unsigned {
  ChorusOn,
  ChorusRate,
  ChorusDepth,
  ChorusVoices,
  ChorusSpread,
  ChorusDetune,
  ChorusTone,
  ChorusLink,
  ChorusSync,
  FlangerOn,
  FlangerRate,
  FlangerDepth,
  FlangerFeedback,
  FlangerDelay,
  FlangerColor,
  FlangerStereo,
  FlangerLink,
  FlangerSync,
  PhaserOn,
  PhaserRate,
  PhaserDepth,
  PhaserStages,
  PhaserResonance,
  PhaserSpread,
  PhaserMix,
  PhaserLink,
  PhaserSync,
  DoublerOn,
  DoublerTime,
  DoublerDetune,
  DoublerVoices,
  DoublerSpread,
  DoublerHumanize,
  DoublerLevel,
  DoublerLink,
  MorphX,
  MorphY,
  Routing,
  Interaction,
  Lfo1Rate,
  Lfo1Shape,
  Lfo1Phase,
  Lfo1Smooth,
  Lfo1Sync,
  Lfo2Rate,
  Lfo2Shape,
  Lfo2Phase,
  Lfo2Smooth,
  Lfo2Sync,
  EnvSensitivity,
  EnvAttack,
  EnvRelease,
  EnvSource,
  Breathing,
  Movement,
  Texture,
  Character,
  Space,
  HighPass,
  LowPass,
  Drive,
  DriveMode,
  DrivePosition,
  EchoLevel,
  EchoTime,
  EchoFeedback,
  EchoDamping,
  EchoPingPong,
  EchoSync,
  BloomLevel,
  BloomDecay,
  BloomDensity,
  BloomTone,
  Width,
  Mix,
  Output,
  AutoGain,
  SoftClip,
  MotionOn,
  MotionBars,
  MotionDepth,
  MotionReverse,
  MotionSmooth,
  CanvasOn,
  CanvasInfluence,
  CanvasLocks,
  MatrixStart,
  Count = MatrixStart + 16 * 3
};
constexpr unsigned matrixSource(unsigned slot) {
  return MatrixStart + slot * 3;
}
constexpr unsigned matrixTarget(unsigned slot) {
  return matrixSource(slot) + 1;
}
constexpr unsigned matrixAmount(unsigned slot) {
  return matrixSource(slot) + 2;
}
inline constexpr std::array<unsigned, 4> moduleOffsets{ChorusOn, FlangerOn,
                                                       PhaserOn, DoublerOn};
inline constexpr std::array<unsigned, 4> linkParameters{
    ChorusLink, FlangerLink, PhaserLink, DoublerLink};
inline constexpr std::array<const char *, 16> sourceNames{
    "Off",        "LFO 1",     "LFO 2",    "Envelope", "Random",   "Movement",
    "Texture",    "Character", "Space",    "Motion X", "Motion Y", "Brightness",
    "Saturation", "Edges",     "Canvas X", "Canvas Y"};
using Values = std::array<double, Count>;
std::span<const ParameterInfo> parameterTable();
Values defaults();
bool modulationTarget(unsigned index) noexcept;

struct CanvasCell {
  std::array<float, 4> weights{.25f, .25f, .25f, .25f};
  float light = .5f, saturation = 0, edge = 0;
  bool operator==(const CanvasCell &) const = default;
};
struct PathPoint {
  float x = 0, y = 0, time = 0;
  bool operator==(const PathPoint &) const = default;
};
// Fixed, trivially-copyable audio program. No images or dynamically owned data
// ever cross into process(). Image analysis and serialization are control work.
struct MorphCanvasProgram {
  static constexpr unsigned side = 32, maxPoints = 256, previewSide = 128;
  std::array<CanvasCell, side * side> cells{};
  std::array<PathPoint, maxPoints> path{};
  // Fixed row stride (previewSide), including portrait and narrow previews.
  std::array<std::uint32_t, previewSide * previewSide> rgba{};
  unsigned pointCount = 0, previewWidth = 0, previewHeight = 0;
  std::uint32_t seed = 1;
  bool painted = false, recorded = false;
  bool operator==(const MorphCanvasProgram &) const = default;
};
bool validProgram(const MorphCanvasProgram &) noexcept;
CanvasCell sampleCanvas(const MorphCanvasProgram &, double x,
                        double y) noexcept;
void analyzeCanvas(MorphCanvasProgram &) noexcept;
std::uint32_t canvasFingerprint(const MorphCanvasProgram &) noexcept;
void makeOrbit(MorphCanvasProgram &, unsigned variant = 0) noexcept;
void smoothPath(MorphCanvasProgram &) noexcept;
Values interpretCanvas(const MorphCanvasProgram &, Values values);

struct State {
  Values values{};
  MorphCanvasProgram canvas;
  std::string presetKind = "factory", presetName = "Init", category;
};
struct FactoryPreset {
  const char *name;
  const char *category;
};
std::span<const FactoryPreset> factoryPresets() noexcept;
State factoryState(unsigned index);
State randomState(const State &base, std::uint32_t seed);
bool encodeState(const State &, std::vector<std::uint8_t> &);
std::optional<State> decodeState(std::span<const std::uint8_t>);

struct Telemetry {
  float left = 0, right = 0, envelope = 0, x = 0, y = 0, phase = 0;
  float autoGain = 1;
  std::array<float, 4> weights{}, motion{};
  std::array<float, MatrixStart> effective{};
};
struct MorphSnapshot {
  State state;
  Telemetry telemetry;
};

} // namespace daw::plugins::morph
