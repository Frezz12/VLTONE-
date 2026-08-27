#pragma once

#include "Audio/SampleBuffer.hpp"
#include "WaveformCache.hpp"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>

/// Decodes a file for the browser's audition, off the UI thread.
///
/// One decode produces both things the preview needs — the audio to play and
/// the envelope to draw — so a file is read once. It deliberately does **not**
/// go through `WaveformCache`: that cache is unsynchronised and read from paint
/// handlers, so a worker touching it would be a data race, and its `peaks()` is
/// a synchronous full-file decode, which is the stall this class exists to
/// avoid.
///
/// Requests are generation-counted, so arrowing down a folder leaves only the
/// newest decode standing.
class PreviewLoader : public QObject {
    Q_OBJECT
public:
    explicit PreviewLoader(QObject* parent = nullptr);
    ~PreviewLoader() override;

    /// Decode `path` and report back on the UI thread.
    void request(const QString& path);
    /// Abandon whatever is in flight; its result is dropped when it lands.
    void cancel();

signals:
    void loaded(const QString& path,
                std::shared_ptr<const daw::engine::SampleBuffer> audio,
                daw::WaveformPeaks peaks);
    void failed(const QString& path, const QString& reason);

private:
    std::shared_ptr<std::atomic<quint64>> m_generation;
};
