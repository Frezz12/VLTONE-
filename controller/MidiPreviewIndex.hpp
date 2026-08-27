#pragma once

#include "model/Document.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

namespace daw {

/// Paint-time index for the compact note overview inside an arrangement clip.
///
/// Notes are ordered once when their owning track changes. Queries then skip
/// groups whose latest note end is left of the viewport, so horizontal scroll
/// and partial repaints touch visible notes rather than the whole clip. The
/// group maximum also keeps a long note that begins off-screen discoverable.
class MidiPreviewIndex {
public:
    static constexpr std::size_t kNotesPerBlock = 128;

    void rebuild(std::span<const NoteModel> notes) {
        m_sourceSize = notes.size();
        m_order.resize(notes.size());
        std::iota(m_order.begin(), m_order.end(), std::uint32_t{0});
        const auto startsBefore = [&notes](std::uint32_t a, std::uint32_t b) {
            return notes[a].startBeats < notes[b].startBeats;
        };
        if (!std::is_sorted(m_order.begin(), m_order.end(), startsBefore))
            std::stable_sort(m_order.begin(), m_order.end(), startsBefore);

        m_lowestPitch = 127;
        m_highestPitch = 0;
        for (const NoteModel& note : notes) {
            m_lowestPitch = std::min(m_lowestPitch, note.pitch);
            m_highestPitch = std::max(m_highestPitch, note.pitch);
        }
        if (notes.empty()) {
            m_lowestPitch = 60;
            m_highestPitch = 60;
        }

        m_starts.resize(m_order.size());
        m_ends.resize(m_order.size());
        const std::size_t blocks =
            (m_order.size() + kNotesPerBlock - 1) / kNotesPerBlock;
        m_blockMaxEnd.assign(blocks, -std::numeric_limits<double>::infinity());
        for (std::size_t ordered = 0; ordered < m_order.size(); ++ordered) {
            const NoteModel& note = notes[m_order[ordered]];
            // NoteModel normally guarantees a positive length. Keeping a
            // minimal extent here also makes a malformed zero-length note
            // agree with the preview's minimum two-pixel rectangle.
            const double end = note.startBeats +
                               std::max(note.lengthBeats,
                                        std::numeric_limits<double>::epsilon());
            m_starts[ordered] = note.startBeats;
            m_ends[ordered] = end;
            double& maximum = m_blockMaxEnd[ordered / kNotesPerBlock];
            maximum = std::max(maximum, end);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_order.size(); }
    [[nodiscard]] int lowestPitch() const noexcept { return m_lowestPitch; }
    [[nodiscard]] int highestPitch() const noexcept { return m_highestPitch; }

    /// Visit notes overlapping [fromBeat, toBeat). `inspected` is optional
    /// instrumentation: it counts note records examined after block pruning.
    template <typename Visitor>
    void forEachVisible(std::span<const NoteModel> notes, double fromBeat,
                        double toBeat, Visitor&& visitor,
                        std::size_t* inspected = nullptr) const {
        // An editor may append one live-drawn note while deliberately retaining
        // the previous index until mouse-up. Existing indices remain valid;
        // shrinking below the indexed source size would not be safe.
        if (toBeat <= fromBeat || notes.size() < m_sourceSize) return;

        const auto stop = std::lower_bound(
            m_starts.begin(), m_starts.end(), toBeat);
        const std::size_t stopAt = std::size_t(stop - m_starts.begin());
        const std::size_t blocks =
            (stopAt + kNotesPerBlock - 1) / kNotesPerBlock;

        for (std::size_t block = 0; block < blocks; ++block) {
            if (m_blockMaxEnd[block] <= fromBeat) continue;
            const std::size_t begin = block * kNotesPerBlock;
            const std::size_t end =
                std::min(stopAt, begin + kNotesPerBlock);
            for (std::size_t ordered = begin; ordered < end; ++ordered) {
                if (inspected) ++*inspected;
                const std::uint32_t index = m_order[ordered];
                if (m_ends[ordered] > fromBeat)
                    visitor(notes[index], std::size_t(index));
            }
        }
    }

private:
    // A project cannot have UINT32_MAX notes in one clip within a practical
    // address space; using 32-bit indices halves the persistent preview cache.
    std::vector<std::uint32_t> m_order;
    std::vector<double> m_starts;
    std::vector<double> m_ends;
    std::vector<double> m_blockMaxEnd;
    std::size_t m_sourceSize = 0;
    int m_lowestPitch = 60;
    int m_highestPitch = 60;
};

} // namespace daw
