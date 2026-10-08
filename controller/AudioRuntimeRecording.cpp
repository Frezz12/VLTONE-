#include "AudioRuntime.hpp"

#include <algorithm>
#include <limits>

namespace daw {

audio::AudioRecorder* AudioRuntime::captureRecorder(AudioCaptureId id) const {
    const auto found = recordings.find(id);
    return found == recordings.end() ? nullptr : found->second.recorder.get();
}

bool AudioRuntime::hasActiveCaptures() const {
    // A stopped writer may still need file repair. Keep the session alive
    // until stopCapture acknowledges a durable, finalized take.
    return !recordings.empty();
}

audio::Result AudioRuntime::startCapture(const AudioCaptureSpec& spec, AudioCaptureStarted& out) {
    out = {};
    if (spec.channelCount < 1 || spec.channelCount > 2 || spec.inputChannel >= engine::kMaxChannels ||
        nextCaptureId == std::numeric_limits<AudioCaptureId>::max())
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Invalid audio capture configuration.");
    auto recorder = std::make_shared<audio::AudioRecorder>();
    if (auto result = recorder->initialize(sampleRate, spec.channelCount); !result) return result;
    recorder->setRecordPath(spec.directory);
    recorder->setInputChannels(spec.inputChannel, spec.channelCount, spec.inputEnabled);
    if (auto result = recorder->startRecording(0, spec.startSample); !result) return result;
    out = {++nextCaptureId, recorder->session().filePath, recorder->peakBucketFrames()};
    recordings.emplace(out.id, RecordingCapture{std::move(recorder), sampleRate});
    return audio::Result::ok();
}

bool AudioRuntime::publishCaptures(std::span<const AudioCaptureId> ids) {
    if (ids.empty()) { activeRecorders.publish({}); return true; }
    auto active = std::make_shared<RecorderList>();
    active->reserve(ids.size());
    std::unordered_set<AudioCaptureId> seen;
    for (auto id : ids) {
        const auto found = recordings.find(id);
        if (found == recordings.end() || !seen.insert(id).second) return false;
        active->push_back(found->second.recorder);
    }
    activeRecorders.publish(std::move(active));
    return true;
}

AudioCaptureStatus AudioRuntime::captureStatus(AudioCaptureId id) const {
    const auto found = recordings.find(id);
    if (found == recordings.end()) return {};
    const auto& recorder = *found->second.recorder;
    return {true, recorder.isRecording(), recorder.recordedFrames(), recorder.startSample(),
            recorder.peakBucketFrames(), found->second.sampleRate};
}

AudioCapturePeaks AudioRuntime::capturePeaks(AudioCaptureId id, std::uint64_t fromBucket) const {
    AudioCapturePeaks out;
    out.status = captureStatus(id);
    const auto* recorder = captureRecorder(id);
    if (!recorder || out.status.recordedFrames <= 0 || !out.status.peakBucketFrames) return out;
    const auto latest = (std::uint64_t(out.status.recordedFrames) - 1) / out.status.peakBucketFrames;
    const auto oldest = latest >= audio::AudioRecorder::kPeakHistoryBuckets
        ? latest - audio::AudioRecorder::kPeakHistoryBuckets + 1 : 0;
    out.firstBucket = std::max(fromBucket, oldest);
    if (out.firstBucket > latest) return out;
    out.values.resize(std::size_t(latest - out.firstBucket + 1), 0);
    for (std::size_t i = 0; i < out.values.size(); ++i)
        (void)recorder->readPeakBucket(out.firstBucket + i, out.values[i]);
    return out;
}

bool AudioRuntime::setCaptureInput(AudioCaptureId id, std::uint32_t first,
    std::uint32_t count, bool enabled) {
    if (first >= engine::kMaxChannels || count < 1 || count > 2) return false;
    auto* recorder = captureRecorder(id);
    if (!recorder) return false;
    recorder->setInputChannels(first, count, enabled);
    return true;
}

audio::Result AudioRuntime::stopCapture(AudioCaptureId id, audio::RecordingSession& closed) {
    closed = {};
    auto* recorder = captureRecorder(id);
    if (!recorder) return audio::Result::fail(audio::EngineError::InvalidArgument, "Audio capture is no longer available.");
    // Detach first. Old callback snapshots retain ownership while stopRecording
    // drains an in-flight producer and joins the WAV writer.
    if (auto active = activeRecorders.controlCopy(); active &&
        std::any_of(active->begin(), active->end(), [&](const auto& value) { return value.get() == recorder; })) {
        auto next = std::make_shared<RecorderList>(*active);
        std::erase_if(*next, [&](const auto& value) { return value.get() == recorder; });
        activeRecorders.publish(std::move(next));
    }
    auto result = recorder->isRecording() ? recorder->stopRecording() : audio::Result::ok();
    closed = recorder->session();
    // AudioRecorder resets its reusable local state to Idle after joining the
    // writer. The session service acknowledges a permanently closed capture.
    closed.state = audio::RecordingSession::State::Stopped;
    if (!closed.fileWriteSucceeded) {
        audio::RecordingSession repaired;
        result = audio::AudioRecorder::recoverInterruptedFile(closed, repaired);
        if (!result) return result; // Keep the stopped owner/path for a retry.
        closed = std::move(repaired);
    }
    recordings.erase(id);
    return result;
}

void AudioRuntime::interruptCapture(AudioCaptureId id) {
    if (auto* recorder = captureRecorder(id)) recorder->markInterrupted();
}

bool AudioRuntime::feedCaptureForTest(AudioCaptureId id, const audio::AudioBuffer& input, audio::BufferSize frames) {
    auto* recorder = captureRecorder(id);
    if (liveDeviceAllowed || !recorder || !recorder->isRecording() || frames > input.numFrames()) return false;
    recorder->process(input, frames);
    return true;
}

} // namespace daw
