#pragma once

#include <QString>
#include <QStringList>
#include <QList>

/// Everything the AI assistant remembers, in one place.
///
/// Free functions over QSettings, the same shape as `ui::browserprefs`: the
/// panel, the AI page in Settings and the shell all read these, and the shell
/// needs the visibility before the panel exists. Every key lives under "ai/".
namespace ui::aiprefs {

/// Which request/response protocol a model endpoint speaks.
enum class Provider {
    Anthropic,
    OpenAi,
};
/// The stable string a settings file stores, and its inverse.
QString providerId(Provider provider);
Provider providerFromId(const QString& id);

enum class ModelSource { Managed, Custom };

/// One selectable chat model. Managed entries contain only public metadata;
/// their direct endpoint and key are issued from the account server for each
/// checked request and never stored here. Custom entries keep their endpoint
/// locally and their API key in the operating-system credential vault.
struct ModelConnection {
    QString id;
    QString displayName;
    Provider provider = Provider::OpenAi;
    QString model;
    QString endpoint;
    ModelSource source = ModelSource::Custom;
    bool hasApiKey = false;
};

QList<ModelConnection> managedModels();
void setManagedModels(const QList<ModelConnection>& models);
QList<ModelConnection> customModels();
QList<ModelConnection> availableModels();
bool modelById(const QString& id, ModelConnection* model);

/// Empty `apiKey` preserves an existing key. A new model may deliberately have
/// no key for a local endpoint. False means the native credential vault
/// rejected a non-empty key and no metadata was changed.
bool saveCustomModel(ModelConnection model, const QString& apiKey,
                     QString* savedId = nullptr);
bool removeCustomModel(const QString& id);
QString customApiKey(const QString& id);

QString activeModelId();
void setActiveModelId(const QString& id);

/// How many rounds of tool calls one request may take before it is cut off —
/// the guard against a loop that spends money forever. 1 … 200.
int maxIterations();
void setMaxIterations(int iterations);

/// Prompts the user keeps around — "my house drum template", "mix check".
/// Application-wide rather than per-project, because a way of working follows
/// the person; the per-project instructions live in the document instead.
QStringList savedPrompts();
void setSavedPrompts(const QStringList& prompts);

/// How many turns back a request carries. 0 sends the whole conversation,
/// which gets expensive; the default keeps the recent past and drops the rest.
int historyLimit();
void setHistoryLimit(int turns);

/// Show the answer as it is written. Off means waiting for the whole reply,
/// which some OpenAI-compatible servers need.
bool streaming();
void setStreaming(bool on);

// ── Music generation ────────────────────────────────────────────────────────
//
// The panel's second mode. It talks to a music model (MiniMax Music and
// anything that speaks its shape), not to a chat provider, so it has its own
// endpoint, key and model — and none of the chat settings apply to it.

/// Which mode the panel is in. Remembered, because a session spent writing
/// music should still be writing music tomorrow.
enum class Mode { Assistant, Music };
Mode mode();
void setMode(Mode mode);

/// The full URL of the music endpoint. Defaults to MiniMax's own; the point of
/// the field is that the model is usually running somewhere else.
QString musicUrl();
void setMusicUrl(const QString& url);

QString musicModel();
void setMusicModel(const QString& model);

/// Optional, unlike the chat key: a server of one's own may want no
/// authorization at all, and then no Authorization header is sent.
/// `MINIMAX_API_KEY` in the environment wins over anything stored.
bool hasMusicApiKey();
QString musicApiKey();
void setMusicApiKey(const QString& key);

/// What the server is asked to produce. Both are decodable by the engine.
QString musicFormat();          ///< "mp3" or "wav"
void setMusicFormat(const QString& format);
int musicSampleRate();
void setMusicSampleRate(int rate);
int musicBitrate();
void setMusicBitrate(int bitrate);

/// Where generated audio is written. Defaults inside the user's Music folder,
/// which is also where the file browser looks, so results show up there.
QString musicFolder();
void setMusicFolder(const QString& folder);

/// How long one generation may take before it is given up on. Generating a
/// four-minute song is not fast.
int musicTimeoutSeconds();
void setMusicTimeoutSeconds(int seconds);

/// Whether the composer's instrumental switch is on.
bool musicInstrumental();
void setMusicInstrumental(bool instrumental);

/// The panel starts hidden: the assistant is opt-in, and it costs money.
bool visible();
void setVisible(bool visible);

int width();
void setWidth(int width);

} // namespace ui::aiprefs
