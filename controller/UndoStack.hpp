#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace daw {

/// Coarse-grained closure-pair undo/redo, mirroring the Swift UndoStack. Each
/// entry is an (undo, redo) pair with a label. The `isApplying` guard stops an
/// undo/redo from recording itself as a new entry.
class UndoStack {
public:
    struct Entry {
        std::string label;
        std::function<void()> undo;
        std::function<void()> redo;
        std::uint64_t sequence = 0;
        std::size_t bytes = 256;
    };

    /// Stable token for a compound operation. Unlike a depth marker it remains
    /// meaningful when the stack is already at its limit: eviction is deferred
    /// while at least one group is open, then applied once to the collapsed
    /// result. Tokens are intentionally opaque and cheap to keep in UI state.
    struct Group {
        std::uint64_t id = 0;
        explicit operator bool() const noexcept { return id != 0; }
    };

    explicit UndoStack(size_t limit = 100,
                       size_t byteBudget = 128u * 1024u * 1024u)
        : m_limit(limit), m_byteBudget(byteBudget) {}

    /// Record an operation that has *already* been performed.
    void push(std::string label,
              std::function<void()> undo,
              std::function<void()> redo, std::size_t bytes = 256) {
        if (m_applying) return;
        m_redoStack.clear();
        // Transactional copies of history share immutable closures. Captured
        // note arrays and project snapshots are never deep-copied with a stack.
        struct Actions { std::function<void()> undo, redo; };
        auto actions = std::make_shared<Actions>(Actions{std::move(undo), std::move(redo)});
        m_undoStack.push_back({std::move(label),
            [actions] { if (actions->undo) actions->undo(); },
            [actions] { if (actions->redo) actions->redo(); }, m_nextSequence++, bytes});
        ++m_revision;
        trimToLimit();
    }

    /// The cap. Past it the oldest entry is dropped for every new one, so the
    /// depth stops growing — which a caller that counts entries has to know.
    std::size_t limit() const noexcept { return m_limit; }
    std::size_t estimatedBytes() const noexcept {
        std::size_t total = 0;
        for (const auto& e : m_undoStack) total += e.bytes;
        for (const auto& e : m_redoStack) total += e.bytes;
        return total;
    }

    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }

    /// How many entries are on the undo stack. Paired with `collapse` it lets a
    /// caller mark where a long operation began and fold everything since.
    size_t depth() const { return m_undoStack.size(); }

    /// Monotonic document-history activity. Unlike depth it changes at a full
    /// stack and on undo/redo, so a long-running assistant can tell that the
    /// user edited the project while a model request was in flight.
    std::uint64_t revision() const noexcept { return m_revision; }

    /// Start a compound operation that may push ordinary entries over several
    /// event-loop turns. While it is open, entries are allowed to exceed the
    /// steady-state limit so none of the operation's closures is evicted before
    /// it can be folded into the single action the user performed.
    Group beginGroup() {
        const Group group{m_nextGroupId++};
        m_activeGroups.push_back({group.id, m_nextSequence});
        return group;
    }

    /// Fold everything pushed since `group` began, close the token, and only
    /// then enforce the stack limit. A one-entry group is relabelled; an empty
    /// group simply releases its eviction protection. Closing an outer group
    /// also closes any still-open groups nested inside the entries it consumes.
    bool collapseGroup(Group group, std::string label) {
        const auto active = std::find_if(
            m_activeGroups.begin(), m_activeGroups.end(),
            [&](const ActiveGroup& candidate) {
                return candidate.id == group.id;
            });
        if (active == m_activeGroups.end()) return false;
        const std::uint64_t firstSequence = active->firstSequence;

        // If an outer transaction closes before a nested one, its entry range
        // necessarily contains the nested transaction too. Invalidate those
        // later tokens now rather than leaving eviction protection stranded.
        std::erase_if(m_activeGroups, [&](const ActiveGroup& candidate) {
            return candidate.id == group.id ||
                   (candidate.id > group.id &&
                    candidate.firstSequence >= firstSequence);
        });

        const auto first = std::find_if(
            m_undoStack.begin(), m_undoStack.end(),
            [&](const Entry& entry) { return entry.sequence >= firstSequence; });
        if (first == m_undoStack.end()) {
            trimToLimit();
            return true;
        }

        const std::size_t count =
            std::size_t(std::distance(first, m_undoStack.end()));
        if (count == 1) {
            first->label = std::move(label);
        } else {
            collapseRange(first, std::move(label));
        }
        trimToLimit();
        return true;
    }

    /// Close a group without folding its entries. Used by an interrupted or
    /// no-op UI gesture; individual commands, if any, remain individually
    /// undoable and normal eviction resumes once no other group is active.
    void releaseGroup(Group group) {
        std::erase_if(m_activeGroups, [&](const ActiveGroup& candidate) {
            return candidate.id == group.id;
        });
        trimToLimit();
    }

    /// Replace the last `count` entries with one, so an operation the user
    /// thinks of as a single action undoes as a single action. Unlike
    /// `Suspend`, this needs no guard held across the work, which is what makes
    /// it usable for something spanning network waits (the AI assistant): the
    /// steps record normally and are folded once they are all done.
    ///
    /// Refuses when `count` does not fit — the stack's own limit may have
    /// discarded the start of the group — leaving the entries individual rather
    /// than building one that only undoes half of what it claims.
    void collapse(size_t count, std::string label) {
        if (count < 2 || count > m_undoStack.size()) return;
        auto first = m_undoStack.end() - static_cast<std::ptrdiff_t>(count);
        collapseRange(first, std::move(label));
        trimToLimit();
    }

    /// Forget entries recorded after a caller's depth marker. This is the
    /// companion to a snapshot-based gesture: a complex live edit may need to
    /// call ordinary undoable primitives while it is in flight, then replace
    /// those implementation details with one before/after entry on release.
    void discardSince(size_t depth) {
        if (depth >= m_undoStack.size()) return;
        m_undoStack.erase(m_undoStack.begin() +
                              static_cast<std::ptrdiff_t>(depth),
                          m_undoStack.end());
        m_redoStack.clear();
    }

    std::string undoLabel() const {
        return m_undoStack.empty() ? "" : m_undoStack.back().label;
    }
    std::string redoLabel() const {
        return m_redoStack.empty() ? "" : m_redoStack.back().label;
    }

    void undo() {
        if (m_undoStack.empty()) return;
        Entry e = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        m_applying = true;
        if (e.undo) e.undo();
        m_applying = false;
        m_redoStack.push_back(std::move(e));
        ++m_revision;
    }

    void redo() {
        if (m_redoStack.empty()) return;
        Entry e = std::move(m_redoStack.back());
        m_redoStack.pop_back();
        m_applying = true;
        if (e.redo) e.redo();
        m_applying = false;
        // Redo is a new top-of-history event for any group currently open.
        e.sequence = m_nextSequence++;
        m_undoStack.push_back(std::move(e));
        ++m_revision;
        trimToLimit();
    }

    void clear() {
        ++m_revision;
        m_undoStack.clear();
        m_redoStack.clear();
        m_activeGroups.clear();
    }

    bool isApplying() const { return m_applying; }

    /// RAII guard that suppresses recording while a compound edit runs, so a
    /// multi-step operation (packing tracks into a folder, say) lands on the
    /// stack as the single entry its caller pushes afterwards.
    class Suspend {
    public:
        explicit Suspend(UndoStack& stack)
            : m_stack(stack), m_previous(stack.m_applying) {
            stack.m_applying = true;
        }
        ~Suspend() { m_stack.m_applying = m_previous; }
        Suspend(const Suspend&) = delete;
        Suspend& operator=(const Suspend&) = delete;

    private:
        UndoStack& m_stack;
        bool m_previous;
    };

private:
    struct ActiveGroup {
        std::uint64_t id = 0;
        std::uint64_t firstSequence = 0;
    };

    using EntryIterator = std::vector<Entry>::iterator;

    void collapseRange(EntryIterator first, std::string label) {
        const std::uint64_t sequence = first->sequence;
        std::size_t bytes = 0;
        for (auto it = first; it != m_undoStack.end(); ++it) bytes += it->bytes;
        auto entries = std::make_shared<std::vector<Entry>>(
            std::make_move_iterator(first),
            std::make_move_iterator(m_undoStack.end()));
        m_undoStack.erase(first, m_undoStack.end());
        m_undoStack.push_back(
            {std::move(label),
             [entries] {
                 for (auto it = entries->rbegin(); it != entries->rend(); ++it)
                     if (it->undo) it->undo();
             },
             [entries] {
                 for (const Entry& entry : *entries)
                     if (entry.redo) entry.redo();
             },
             sequence, bytes});
        // A collapse only ever describes work just done, so the redo branch is
        // already dead; clearing it keeps that explicit.
        m_redoStack.clear();
    }

    void trimToLimit() {
        if (!m_activeGroups.empty()) return;
        std::size_t excess = m_undoStack.size() > m_limit ? m_undoStack.size() - m_limit : 0;
        std::size_t bytes = estimatedBytes();
        for (std::size_t i = 0; i < excess; ++i) bytes -= m_undoStack[i].bytes;
        // Keep the most recent action even if that one snapshot alone exceeds
        // the budget. Large projects retain useful Undo without keeping 100
        // full-document copies alive.
        while (bytes > m_byteBudget && excess + 1 < m_undoStack.size())
            bytes -= m_undoStack[excess++].bytes;
        m_undoStack.erase(
            m_undoStack.begin(),
            m_undoStack.begin() + static_cast<std::ptrdiff_t>(excess));
    }

    size_t m_limit;
    size_t m_byteBudget;
    bool m_applying = false;
    std::uint64_t m_nextSequence = 1;
    std::uint64_t m_nextGroupId = 1;
    std::uint64_t m_revision = 0;
    std::vector<Entry> m_undoStack;
    std::vector<Entry> m_redoStack;
    std::vector<ActiveGroup> m_activeGroups;
};

} // namespace daw
