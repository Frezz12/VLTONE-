#include "EngineController.hpp"
#include "SlideNotes.hpp"
namespace daw {
EngineController::InstrumentSlideStatus
EngineController::instrumentSlideStatus(const std::string &trackId) {
    auto *track = m_project.findTrack(trackId);
    if (!track)
        return {};
    const auto status = m_runtime->pluginSlideStatus({trackId, track->instrument.id});
    return {plugins::SlideDelivery(status.mode), status.overloaded, status.clipped};
}
void EngineController::setInstrumentSlideSettings(const std::string &trackId, int mode,
                                                  double range, double reserve, bool undo) {
    if (!canEditSlideNotes())
        return;
    auto *track = m_project.findTrack(trackId);
    if (!track)
        return;
    mode = std::clamp(mode, 0, 4);
    range = std::clamp(range, 1., 96.);
    reserve = std::clamp(reserve, 0., 20.);
    auto &slot = track->instrument;
    const int oldMode = slot.slideDelivery;
    const double oldRange = slot.slideBendRange, oldReserve = slot.slideReleaseReserve;
    if (mode == oldMode && range == oldRange && reserve == oldReserve)
        return;
    if (undo && cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        const collab::PluginLocation loc{collab::PluginChain::Instrument, trackId, {}};
        auto add = [&](collab::PluginProperty field, collab::PluginPropertyValue value) {
            collab::ProjectCommand c;
            c.body = collab::SetPluginProperty{loc, slot.id, field, std::move(value)};
            batch->commands.push_back(std::move(c));
        };
        add(collab::PluginProperty::SlideDelivery, std::int64_t(mode));
        add(collab::PluginProperty::SlideBendRange, range);
        add(collab::PluginProperty::SlideReleaseReserve, reserve);
        if (submitSharedMutation(batch, "Set Slide Delivery") !=
            collab::SharedMutationResult::LocalFallback)
            return;
    }
    unfreezeTrack(trackId, false);
    slot.slideDelivery = mode;
    slot.slideBendRange = range;
    slot.slideReleaseReserve = reserve;
    m_runtime->setPluginSlide({trackId, slot.id}, mode, range, reserve);
    if (undo)
        m_undo.push(
            "Set Slide Delivery",
            [=, this] {
                setInstrumentSlideSettings(trackId, oldMode, oldRange, oldReserve, false);
            },
            [=, this] { setInstrumentSlideSettings(trackId, mode, range, reserve, false); });
}
void EngineController::setClipMidiObjects(const std::string &trackId, const std::string &clipId,
                                          std::vector<NoteModel> notes,
                                          std::vector<SlideNoteModel> gestures,
                                          const std::string &label, bool undo,
                                          std::optional<std::string> takeId) {
    auto *track = m_project.findTrack(trackId);
    if (!track)
        return;
    auto clip = std::find_if(track->clips.begin(), track->clips.end(),
                             [&](const auto &c) { return c.id == clipId; });
    if (clip == track->clips.end() || clip->kind != ClipKind::Midi)
        return;
    if (!takeId) {
        takeId = std::string{};
        for (const auto &t : clip->takes)
            if (&slides::editable(*clip) == &t.slideNotes)
                takeId = t.id;
    }
    auto *take = takeId->empty() ? nullptr : findTake(*clip, *takeId);
    if (!takeId->empty() && !take)
        return;
    auto &storedNotes = take ? take->notes : clip->notes;
    auto &storedSlides = take ? take->slideNotes : clip->slideNotes;
    for (auto &n : notes) {
        if (n.id.empty())
            n.id = newUuid();
        n.startBeats = std::max(0., n.startBeats);
        n.lengthBeats = std::max(1. / 960., n.lengthBeats);
        n.pitch = std::clamp(n.pitch, 0, 127);
        n.velocity = std::clamp(n.velocity, 1, 127);
        n.pan = std::clamp(n.pan, -1.f, 1.f);
    }
    const auto beforeNotes = storedNotes;
    const auto beforeSlides = storedSlides;
    if (beforeNotes == notes && beforeSlides == gestures)
        return;
    for (auto &s : gestures) {
        slides::normalize(s);
        if (!slides::valid(s))
            return;
    }
    if (undo && cloudProjectBound()) {
        if (!canEditSlideNotes())
            return;
        auto batch = std::make_shared<collab::BatchCommand>();
        auto add = [&](collab::CommandBody body) {
            collab::ProjectCommand c;
            c.body = std::move(body);
            batch->commands.push_back(std::move(c));
        };
        for (const auto &n : beforeNotes)
            if (std::none_of(notes.begin(), notes.end(),
                             [&](const auto &a) { return a.id == n.id; }))
                add(collab::DeleteMidiNote{trackId, clipId, n.id});
        for (std::size_t i = 0; i < notes.size(); ++i) {
            auto old = std::find_if(beforeNotes.begin(), beforeNotes.end(),
                                    [&](const auto &n) { return n.id == notes[i].id; });
            if (old == beforeNotes.end() || *old != notes[i])
                add(collab::UpsertMidiNote{trackId, clipId, notes[i],
                                           i ? notes[i - 1].id : std::string{}});
        }
        for (const auto &s : beforeSlides)
            if (std::none_of(gestures.begin(), gestures.end(),
                             [&](const auto &a) { return a.id == s.id; }))
                add(collab::SetSlideNote{trackId, clipId, *takeId, s.id, {}});
        for (const auto &s : gestures) {
            auto old = std::find_if(beforeSlides.begin(), beforeSlides.end(),
                                    [&](const auto &a) { return a.id == s.id; });
            if (old == beforeSlides.end() || *old != s)
                add(collab::SetSlideNote{trackId, clipId, *takeId, s.id, s});
        }
        if (submitSharedMutation(batch, label) != collab::SharedMutationResult::LocalFallback)
            return;
    }
    storedNotes = notes;
    storedSlides = gestures;
    syncTrackNotes(*track);
    if (undo)
        m_undo.push(
            label,
            [=, this] {
                setClipMidiObjects(trackId, clipId, beforeNotes, beforeSlides, label, false,
                                   takeId);
            },
            [=, this] {
                setClipMidiObjects(trackId, clipId, notes, gestures, label, false, takeId);
            });
}

void EngineController::setClipSlideNotes(const std::string &trackId, const std::string &clipId,
                                         std::vector<SlideNoteModel> value,
                                         const std::string &label, bool undo,
                                         std::optional<std::string> takeId) {
    if (!canEditSlideNotes())
        return;
    auto *track = m_project.findTrack(trackId);
    if (!track)
        return;
    auto clip = std::find_if(track->clips.begin(), track->clips.end(),
                             [&](const auto &c) { return c.id == clipId; });
    if (clip == track->clips.end() || clip->kind != ClipKind::Midi)
        return;
    for (auto &s : value) {
        slides::normalize(s);
        if (!slides::valid(s))
            return;
    }
    if (!takeId) {
        takeId = std::string{};
        for (const auto &t : clip->takes)
            if (&slides::editable(*clip) == &t.slideNotes)
                takeId = t.id;
    }
    auto *take = takeId->empty() ? nullptr : findTake(*clip, *takeId);
    if (!takeId->empty() && !take)
        return;
    auto &stored = take ? take->slideNotes : clip->slideNotes;
    const auto before = stored;
    if (before == value)
        return;
    if (undo && cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        const auto add = [&](const std::string &id, std::optional<SlideNoteModel> slide) {
            collab::ProjectCommand c;
            c.body = collab::SetSlideNote{trackId, clipId, *takeId, id, std::move(slide)};
            batch->commands.push_back(std::move(c));
        };
        for (const auto &old : before)
            if (std::none_of(value.begin(), value.end(),
                             [&](const auto &n) { return n.id == old.id; }))
                add(old.id, {});
        for (const auto &n : value) {
            const auto old = std::find_if(before.begin(), before.end(),
                                          [&](const auto &v) { return v.id == n.id; });
            if (old == before.end() || *old != n)
                add(n.id, n);
        }
        if (submitSharedMutation(batch, label) != collab::SharedMutationResult::LocalFallback)
            return;
    }
    stored = value;
    bumpMidiNotesRevision(trackId);
    syncTrackNotes(*track, false);
    if (undo)
        m_undo.push(
            label,
            [this, trackId, clipId, before, label, takeId] {
                setClipSlideNotes(trackId, clipId, before, label, false, takeId);
            },
            [this, trackId, clipId, value, label, takeId] {
                setClipSlideNotes(trackId, clipId, value, label, false, takeId);
            });
}
} // namespace daw
