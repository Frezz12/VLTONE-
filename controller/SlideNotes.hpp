#pragma once
#include "DSP/Curve.hpp"
#include "model/Document.hpp"
#include <unordered_map>

namespace daw::slides {
inline constexpr std::size_t maxPoints = 256;
using PitchCurves = std::unordered_map<std::string, std::vector<engine::curve::Point>>;
void reidentify(std::vector<NoteModel> &, std::vector<SlideNoteModel> &);
std::vector<SlideNoteModel> crop(const std::vector<NoteModel> &before,
                                 const std::vector<SlideNoteModel> &slides,
                                 const std::vector<NoteModel> &after, double from, double to,
                                 const PitchCurves *sourceCurves = nullptr);
void followNotes(std::vector<SlideNoteModel> &, const std::vector<NoteModel> &before,
                 const std::vector<NoteModel> &after, bool stretch = false);
bool valid(const SlideNoteModel &slide) noexcept;
void normalize(SlideNoteModel &slide);
SlideNoteModel create(const std::vector<NoteModel> &notes, double start, double length,
                      double pitch, const std::vector<std::string> &selected = {},
                      bool chord = false);
PitchCurves compile(const std::vector<NoteModel> &notes, const std::vector<SlideNoteModel> &slides);
void preset(SlideNoteModel &slide, double from, double to, int preset);
std::vector<SlideNoteModel> &editable(ClipModel &clip);
const std::vector<SlideNoteModel> &editable(const ClipModel &clip);
} // namespace daw::slides
