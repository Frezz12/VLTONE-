#pragma once

#include "ai/MusicGen.hpp"

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QNetworkReply;

namespace ui {

/// Everything the music endpoint needs beyond the brief itself.
struct MusicConfig {
    QString url;        ///< the whole endpoint, not a base — it is the user's
    QString apiKey;     ///< optional: empty sends no Authorization header
    QString model;
    QString format = QStringLiteral("mp3");
    QString folder;     ///< where the finished audio is written
    int sampleRate = 44100;
    int bitrate = 256000;
    int timeoutSeconds = 300;
};

/// One text-to-music request, from the brief to a file on disk.
///
/// The counterpart of `LlmClient`, and deliberately its twin in shape: only the
/// HTTP lives here — the request body, the reply parsing and the brief itself
/// are pure and live in `daw::ai` (`controller/ai/MusicGen`), where a test can
/// drive them with no server at all.
///
/// Two hops, because these APIs answer with a link: POST the brief, then fetch
/// the audio it points at. When the audio comes back inline instead (hex or
/// base64), the second hop is skipped.
class MusicClient : public QObject {
    Q_OBJECT
public:
    /// What one generation produced. `error` set means nothing was written.
    struct Outcome {
        QString filePath;     ///< the audio, saved under `MusicConfig::folder`
        double seconds = 0.0; ///< as the server reported it, when it did
        QString error;
    };
    using Done = std::function<void(Outcome)>;

    explicit MusicClient(QObject* parent = nullptr);
    ~MusicClient() override;

    void setConfig(MusicConfig config) { m_config = std::move(config); }
    const MusicConfig& config() const { return m_config; }

    /// Generate, then save. `onDone` runs on the GUI thread, exactly once,
    /// unless `cancel()` got there first.
    ///
    /// Virtual for the same reason `LlmClient::send` is: a headless check
    /// stands in a client that answers from a file, so the whole path down to a
    /// real clip on a real track is exercised with no network.
    virtual void generate(const daw::ai::MusicBrief& brief, Done onDone);

    /// Abandon whichever hop is in flight. The callback will not run.
    virtual void cancel();

    virtual bool busy() const;

protected:
    /// Write audio into the configured folder, named after `stem`. Returns the
    /// path, or sets `error`. Shared with the scripted stand-in, so both take
    /// the same route onto the disk.
    QString saveAudio(const QByteArray& bytes, const QString& stem,
                      const QString& format, QString& error) const;

    MusicConfig m_config;

private:
    void fetchAudio(const QString& url, const QString& stem, double seconds);
    /// Deliver, taking the callback first so a callback that starts the next
    /// request cannot be clobbered by this one finishing.
    void answer(Outcome outcome);

    QNetworkAccessManager m_net;
    QPointer<QNetworkReply> m_inFlight;
    Done m_onDone;
};

} // namespace ui
