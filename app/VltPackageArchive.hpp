#pragma once
#include <QString>
#include <functional>

namespace ui::vltarchive {
using KeepGoing = std::function<bool()>;
inline constexpr qint64 kMaximumExpandedBytes = 4'000'000'000LL;
inline constexpr int kMaximumEntries = 10'000;
// Destination must be a new empty staging directory. No links, special files,
// duplicate names, absolute paths or parent traversals are accepted.
bool extract(const QString& archivePath, const QString& destination,
             QString& error, KeepGoing keepGoing = {});
// Packages the contents of a saved project/template (manifest, Content, State
// and other relative resources), without changing the active project.
bool pack(const QString& sourceDirectory, const QString& destination,
          QString& error, KeepGoing keepGoing = {});
bool safeEntryPath(const QString& relative);
}
