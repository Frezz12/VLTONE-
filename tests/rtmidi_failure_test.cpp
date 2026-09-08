#include <rtmidi/RtMidi.h>
#include <CoreMIDI/CoreMIDI.h>
#include <cstdio>
#include <string>

namespace {
bool failClient = true;
unsigned clientCalls = 0;
constexpr OSStatus kTestError = -10830;
}

// Link-time substitute for the one system entry point the constructor uses.
// No real MIDI server/device is contacted, including the recovery path.
extern "C" OSStatus MIDIClientCreate(CFStringRef, MIDINotifyProc, void*,
                                      MIDIClientRef* client) {
    ++clientCalls;
    *client = failClient ? 0 : 123;
    return failClient ? kTestError : noErr;
}

int main() {
    unsigned caught = 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        try { RtMidiIn input(RtMidi::MACOSX_CORE, "failure test"); }
        catch (const RtMidiError& error) {
            caught += error.getType() == RtMidiError::DRIVER_ERROR &&
                      error.getMessage().find("-10830") != std::string::npos;
        }
        try { RtMidiOut output(RtMidi::MACOSX_CORE, "failure test"); }
        catch (const RtMidiError& error) {
            caught += error.getType() == RtMidiError::DRIVER_ERROR &&
                      error.getMessage().find("-10830") != std::string::npos;
        }
    }
    if (caught != 6 || clientCalls != 6) return 1;
    failClient = false;
    try {
        RtMidiIn input(RtMidi::MACOSX_CORE, "recovered client");
        RtMidiOut output(RtMidi::MACOSX_CORE, "same recovered client");
        if (clientCalls != 7 || input.getCurrentApi() != RtMidi::MACOSX_CORE ||
            output.getCurrentApi() != RtMidi::MACOSX_CORE) return 1;
    } catch (...) { return 1; }
    std::puts("PASS CoreMIDI failures remain catchable; retries recover without terminating the host");
}
