#pragma once

#include "Host/PluginTypes.hpp"

#include <string>
#include <vector>

namespace daw::plugins {

/// The wire format between the DAW and the `daw_scan` helper process, and the
/// on-disk shape of the plugin cache.
///
/// Defined here, in the library both sides link, so the writer and the reader
/// cannot drift apart. It is JSON because the cache is something a user may
/// reasonably want to open and read when a plugin refuses to appear.
namespace scan {

/// Bumped when the descriptor schema or compatibility validation changes.
/// Version 2 retries entries from before the VST3 connection-order fix,
/// including plugins blacklisted after three failed initialization attempts.
inline constexpr int kSchemaVersion = 4;

/// Serialise one descriptor. Returns a JSON object as text.
std::string descriptorToJson(const PluginDescriptor& descriptor);

/// The scanner's stdout: `{"schema":N,"plugins":[…]}` on success.
std::string encodeResult(const std::vector<PluginDescriptor>& plugins);

/// Parse what the scanner printed. Returns false on malformed input or on a
/// schema this build does not understand.
bool decodeResult(const std::string& text, std::vector<PluginDescriptor>& out);

} // namespace scan
} // namespace daw::plugins
