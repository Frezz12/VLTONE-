#include "MidiInputManager.hpp"

#include "EngineController.hpp"
#include "Midi/MidiEvent.hpp"

#include <rtmidi/RtMidi.h>

#include <QMetaObject>
#include <QTimer>
#include <QtLogging>

#include <algorithm>
#include <cstdint>

#if defined(Q_OS_MACOS)
#include <CoreMIDI/CoreMIDI.h>

#include <thread>
#endif

namespace {

constexpr int kPortScanMs = 1000;

#if defined(Q_OS_MACOS)
// Scan ticks (one second each) between two attempts after CoreMIDI answered
// with an error, and before an unanswered probe is reported as a wedged
// MIDIServer.
constexpr int kCoreMidiRetryTicks = 5;
constexpr int kCoreMidiStallTicks = 8;
#endif

int messageBytes(std::uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0:
        case 0xD0:
            return 2;
        case 0x80:
        case 0x90:
        case 0xA0:
        case 0xB0:
        case 0xE0:
            return 3;
        default:
            return 0;
    }
}

} // namespace

struct MidiInputManager::Port {
    MidiInputManager* owner = nullptr;
    quint64 generation = 0;
    quint64 source = 0;
    std::unique_ptr<RtMidiIn> input;

    static void callback(double, std::vector<unsigned char>* message,
                         void* context) {
        auto* port = static_cast<Port*>(context);
        if (!port || !port->owner || !message) return;
        port->owner->postMessage(port->generation, port->source, *message);
    }

    ~Port() {
        if (!input) return;
        try {
            input->cancelCallback();
            if (input->isPortOpen()) input->closePort();
        } catch (...) {
            // Destruction is the stuck-note safety path; an unavailable device
            // must not turn application shutdown into an exception.
        }
    }
};

MidiInputManager::MidiInputManager(daw::EngineController* controller,
                                   QObject* parent)
    : QObject(parent), m_controller(controller) {
    m_scanTimer = new QTimer(this);
    m_scanTimer->setInterval(kPortScanMs);
    connect(m_scanTimer, &QTimer::timeout, this,
            &MidiInputManager::refreshPorts);
    m_scanTimer->start();
    QTimer::singleShot(0, this, &MidiInputManager::refreshPorts);
}

MidiInputManager::~MidiInputManager() {
    if (m_scanTimer) m_scanTimer->stop();
    ++m_generation; // queued callbacks from the old ports become harmless
    closePorts();
    allNotesOff();
}

int MidiInputManager::openPortCount() const {
    return int(std::count_if(m_ports.begin(), m_ports.end(),
                             [](const auto& port) { return bool(port); }));
}

void MidiInputManager::closePorts() {
    m_ports.clear();
}

#if defined(Q_OS_MACOS)
bool MidiInputManager::coreMidiReady() {
    if (m_coreMidiReadyLatched) return true;

    const int state = m_coreMidiProbe
        ? m_coreMidiProbe->state.load(std::memory_order_acquire)
        : int(CoreMidiProbe::Idle);
    switch (state) {
        case CoreMidiProbe::Ready:
            // The XPC handshake is per process and done: RtMidi may now be
            // entered from this thread without risking a stall.
            m_coreMidiReadyLatched = true;
            return true;
        case CoreMidiProbe::Running:
            // MIDIServer has not answered. Say so once and carry on; the probe
            // keeps waiting, so a server that recovers later still brings MIDI
            // up without a restart.
            if (++m_coreMidiWaitTicks >= kCoreMidiStallTicks &&
                !m_coreMidiStallReported) {
                m_coreMidiStallReported = true;
                qWarning("the system MIDI server is not answering; MIDI input "
                         "stays off until it does. A faulty driver in "
                         "/Library/Audio/MIDI Drivers can hang it for every "
                         "application on the machine.");
            }
            return false;
        case CoreMidiProbe::Failed: {
            const int status = m_coreMidiProbe->status.load(
                std::memory_order_relaxed);
            if (!m_coreMidiFailureReported ||
                status != m_coreMidiReportedStatus) {
                m_coreMidiFailureReported = true;
                m_coreMidiReportedStatus = status;
                qWarning("CoreMIDI is not ready yet (status %d); MIDI scan "
                         "deferred", status);
            }
            if (++m_coreMidiRetryTicks < kCoreMidiRetryTicks) return false;
            m_coreMidiRetryTicks = 0;
            break;
        }
        case CoreMidiProbe::Idle:
        default:
            break;
    }

    // A probe that never returns can never be joined, so every attempt gets
    // its own state, held by a shared_ptr the thread keeps alive: this manager
    // may be destroyed, and the process may exit, while a thread is still
    // parked in the kernel waiting for MIDIServer.
    m_coreMidiProbe = std::make_shared<CoreMidiProbe>();
    m_coreMidiProbe->state.store(CoreMidiProbe::Running,
                                 std::memory_order_release);
    std::thread([probe = m_coreMidiProbe] {
        MIDIClientRef client = 0;
        const OSStatus status = MIDIClientCreate(
            CFSTR("VLT Studio Pro MIDI readiness check"), nullptr, nullptr,
            &client);
        if (status == noErr && client != 0) {
            MIDIClientDispose(client);
            probe->state.store(CoreMidiProbe::Ready, std::memory_order_release);
            return;
        }
        if (client != 0) MIDIClientDispose(client);
        probe->status.store(int(status), std::memory_order_relaxed);
        probe->state.store(CoreMidiProbe::Failed, std::memory_order_release);
    }).detach();
    return false;
}
#endif

void MidiInputManager::refreshPorts() {
    refreshTarget();

#if defined(Q_OS_MACOS)
    // Every RtMidi entry point drags in CoreMIDI's one-time XPC handshake with
    // the system MIDIServer, and a wedged server makes that handshake block
    // forever -- on the GUI thread that is a frozen application, splash screen
    // and all. It also cannot be caught: RtMidi 6.0.0 declares its CoreMIDI
    // client helper throw() while throwing RtMidiError from it, so an error
    // reaches std::terminate rather than the catch below. Wait for the probe
    // thread instead; the one-second scan only polls its result.
    if (!coreMidiReady()) return;
#endif

    QStringList names;
    try {
        RtMidiIn probe(RtMidi::UNSPECIFIED, "VLT Studio Pro MIDI probe");
        const unsigned count = probe.getPortCount();
        names.reserve(int(count));
        for (unsigned i = 0; i < count; ++i)
            names.append(QString::fromStdString(probe.getPortName(i)));
    } catch (const RtMidiError& error) {
        qWarning("MIDI input scan failed: %s", error.getMessage().c_str());
        return;
    }

    if (names != m_portNames) {
        ++m_generation;
        closePorts();
        allNotesOff();
        m_portNames = names;
        m_ports.resize(std::size_t(names.size()));
    }

    // A port that was temporarily busy is retried without disturbing the
    // working ports beside it.
    for (int index = 0; index < names.size(); ++index) {
        if (m_ports[std::size_t(index)]) continue;
        try {
            auto port = std::make_unique<Port>();
            port->owner = this;
            port->generation = m_generation;
            port->source = (m_generation << 32) | quint64(index + 1);
            port->input = std::make_unique<RtMidiIn>(
                RtMidi::UNSPECIFIED, "VLT Studio Pro");
            port->input->ignoreTypes(true, true, true);
            port->input->openPort(unsigned(index), "VLT Studio Pro input");
            port->input->setCallback(&Port::callback, port.get());
            m_ports[std::size_t(index)] = std::move(port);
        } catch (const RtMidiError& error) {
            qWarning("MIDI input '%s' could not be opened: %s",
                     names[index].toUtf8().constData(),
                     error.getMessage().c_str());
        }
    }
}

void MidiInputManager::postMessage(
    quint64 generation, quint64 source,
    const std::vector<unsigned char>& message) {
    const QByteArray bytes(reinterpret_cast<const char*>(message.data()),
                           qsizetype(message.size()));
    QMetaObject::invokeMethod(
        this,
        [this, generation, source, bytes] {
            if (generation == m_generation) handleMessage(source, bytes);
        },
        Qt::QueuedConnection);
}

void MidiInputManager::rememberRoute(quint64 source, int channel,
                                     const std::string& trackId) {
    const auto found = std::find_if(
        m_routes.begin(), m_routes.end(), [&](const Route& route) {
            return route.source == source && route.channel == channel &&
                   route.trackId == trackId;
        });
    if (found == m_routes.end())
        m_routes.push_back(Route{source, channel, trackId});
}

void MidiInputManager::releaseHeld(quint64 source, int channel) {
    for (auto it = m_held.begin(); it != m_held.end();) {
        if (it->source != source || it->channel != channel) {
            ++it;
            continue;
        }
        m_controller->liveMidiEvent(
            it->trackId, daw::engine::MidiEvent::kNoteOff | it->channel,
            it->pitch, 0);
        emit noteStateChanged(QString::fromStdString(it->trackId),
                              it->pitch, false);
        it = m_held.erase(it);
    }
}

void MidiInputManager::allNotesOff() {
    for (const Held& held : m_held) {
        m_controller->liveMidiEvent(
            held.trackId, daw::engine::MidiEvent::kNoteOff | held.channel,
            held.pitch, 0);
        emit noteStateChanged(QString::fromStdString(held.trackId),
                              held.pitch, false);
    }
    m_held.clear();

    // Pedals and wheels can outlive the last key. Return every channel that
    // accepted input to a neutral state before forgetting its route.
    for (const Route& route : m_routes) {
        m_controller->liveMidiEvent(
            route.trackId, daw::engine::MidiEvent::kControlChange | route.channel,
            64, 0); // sustain up
        m_controller->liveMidiEvent(
            route.trackId, daw::engine::MidiEvent::kControlChange | route.channel,
            121, 0); // reset all controllers
        m_controller->liveMidiEvent(
            route.trackId, daw::engine::MidiEvent::kControlChange | route.channel,
            123, 0); // all notes off
        m_controller->liveMidiEvent(
            route.trackId, daw::engine::MidiEvent::kPitchBend | route.channel,
            0, 64); // centre (8192)
    }
    m_routes.clear();
}

void MidiInputManager::refreshTarget() {
    const std::string target = m_target ? m_target() : std::string();
    if (target == m_lastTarget) return;
    allNotesOff();
    m_lastTarget = target;
}

bool MidiInputManager::handleMessage(quint64 source,
                                     const QByteArray& message) {
    if (message.isEmpty()) return false;
    const auto status = std::uint8_t(message[0]);
    const int bytes = messageBytes(status);
    if (bytes == 0 || message.size() != bytes) return false;

    const int data1 = std::uint8_t(message[1]);
    const int data2 = bytes == 3 ? std::uint8_t(message[2]) : 0;
    if (data1 > 127 || data2 > 127) return false;

    refreshTarget();
    const std::string& target = m_lastTarget;
    if (target.empty()) return false;

    const int type = status & 0xF0;
    const int channel = status & 0x0F;
    const bool noteOn = type == daw::engine::MidiEvent::kNoteOn && data2 > 0;
    const bool noteOff = type == daw::engine::MidiEvent::kNoteOff ||
                         (type == daw::engine::MidiEvent::kNoteOn && data2 == 0);
    auto held = std::find_if(m_held.begin(), m_held.end(),
                             [&](const Held& candidate) {
        return candidate.source == source && candidate.channel == channel &&
               candidate.pitch == data1;
    });

    if (noteOn && held != m_held.end()) {
        m_controller->liveMidiEvent(
            held->trackId, daw::engine::MidiEvent::kNoteOff | held->channel,
            held->pitch, 0);
        emit noteStateChanged(QString::fromStdString(held->trackId),
                              held->pitch, false);
        m_held.erase(held);
        held = m_held.end();
    }

    if (noteOff && held != m_held.end()) {
        const std::string noteTarget = held->trackId;
        const int pitch = held->pitch;
        const bool accepted = m_controller->liveMidiEvent(
            noteTarget, status, data1, data2);
        emit noteStateChanged(QString::fromStdString(noteTarget), pitch, false);
        m_held.erase(held);
        return accepted;
    }

    const bool accepted =
        m_controller->liveMidiEvent(target, status, data1, data2);
    if (!accepted) return false;
    rememberRoute(source, channel, target);

    if (noteOn) {
        m_held.push_back(Held{source, channel, data1, target});
        emit noteStateChanged(QString::fromStdString(target), data1, true);
    } else if (type == daw::engine::MidiEvent::kControlChange &&
               (data1 == 120 || data1 == 123)) {
        releaseHeld(source, channel);
    }
    return true;
}

bool MidiInputManager::injectMessageForTest(const QByteArray& message,
                                            quint64 source) {
    return handleMessage(source, message);
}
