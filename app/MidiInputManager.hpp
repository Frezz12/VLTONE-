#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QTimer;

namespace daw { class EngineController; }

/// Plug-and-play MIDI input for the desktop application.
///
/// RtMidi calls from backend-owned threads. Messages are copied there and
/// queued onto this QObject's thread before they touch EngineController: the
/// live-event ring deliberately has one producer, the UI thread.
class MidiInputManager final : public QObject {
    Q_OBJECT
public:
    explicit MidiInputManager(daw::EngineController* controller,
                              QObject* parent = nullptr);
    ~MidiInputManager() override;

    void setTargetProvider(std::function<std::string()> provider) {
        m_target = std::move(provider);
    }

    int heldCount() const { return int(m_held.size()); }
    int openPortCount() const;
    /// Re-resolve the shared destination and silence the old one if it changed.
    void refreshTarget();
    void allNotesOff();

    /// Hardware-free path used by the deterministic UI self-test.
    bool injectMessageForTest(const QByteArray& message,
                              quint64 source = 0xFFFFFFFFu);

signals:
    /// One physical key changed state after its live event was accepted.
    void noteStateChanged(const QString& trackId, int pitch, bool down);

private:
    struct Port;
    struct Held {
        quint64 source = 0;
        int channel = 0;
        int pitch = 0;
        std::string trackId;
    };
    struct Route {
        quint64 source = 0;
        int channel = 0;
        std::string trackId;
    };

    void refreshPorts();
    void closePorts();
    void postMessage(quint64 generation, quint64 source,
                     const std::vector<unsigned char>& message);
    bool handleMessage(quint64 source, const QByteArray& message);
    void rememberRoute(quint64 source, int channel,
                       const std::string& trackId);
    void releaseHeld(quint64 source, int channel);

    daw::EngineController* m_controller = nullptr;
    std::function<std::string()> m_target;
    QTimer* m_scanTimer = nullptr;
    QStringList m_portNames;
    std::vector<std::unique_ptr<Port>> m_ports;
    std::vector<Held> m_held;
    std::vector<Route> m_routes;
    std::string m_lastTarget;
    quint64 m_generation = 1;
};
