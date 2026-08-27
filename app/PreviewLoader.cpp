#include "PreviewLoader.hpp"

#include "platform/AudioFileDecoder.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

PreviewLoader::PreviewLoader(QObject* parent)
    : QObject(parent),
      m_generation(std::make_shared<std::atomic<quint64>>(0)) {}

PreviewLoader::~PreviewLoader() { cancel(); }

void PreviewLoader::cancel() {
    m_generation->fetch_add(1, std::memory_order_release);
}

void PreviewLoader::request(const QString& path) {
    const quint64 generation =
        m_generation->fetch_add(1, std::memory_order_release) + 1;
    if (path.isEmpty()) return;

    auto counter = m_generation;
    QPointer<PreviewLoader> self(this);

    QThreadPool::globalInstance()->start([path, generation, counter, self] {
        audio::platform::DecodedAudio decoded;
        const auto result =
            audio::platform::decodeAudioFile(path.toStdString(), decoded);
        // Cheap to check before doing the work of building peaks: by now the
        // user may well have moved on to another file.
        if (counter->load(std::memory_order_acquire) != generation) return;

        if (!result || decoded.frames == 0) {
            const QString reason = QString::fromStdString(result.message());
            QMetaObject::invokeMethod(
                self, [self, path, reason, generation, counter] {
                    if (!self) return;
                    if (counter->load(std::memory_order_acquire) != generation) return;
                    emit self->failed(path, reason);
                },
                Qt::QueuedConnection);
            return;
        }

        daw::WaveformPeaks peaks;
        daw::buildPeaks(decoded, peaks);
        auto audio = daw::engine::SampleBuffer::fromInterleaved(
            decoded.interleaved, daw::engine::ChannelCount(decoded.channels),
            daw::engine::FrameCount(decoded.frames), decoded.sampleRate);

        if (counter->load(std::memory_order_acquire) != generation) return;
        QMetaObject::invokeMethod(
            self,
            [self, path, audio, peaks, generation, counter] {
                if (!self) return;
                if (counter->load(std::memory_order_acquire) != generation) return;
                emit self->loaded(path, audio, peaks);
            },
            Qt::QueuedConnection);
    });
}
