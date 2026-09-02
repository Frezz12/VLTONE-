#pragma once

#include "model/Document.hpp"
#include "Core/Result.hpp"
#include <string>
#include <string_view>

namespace daw {

/// Reads and writes projects in the cross-platform VLT format: a package
/// directory (extension `.vlt`) containing a clickable `Project.vlt` manifest,
/// a `Content/` folder with copies of every referenced audio file, and a
/// `State/` folder holding each loaded plugin's opaque state chunk — so a
/// project is self-contained and portable across machines.
///
/// Plugin state is kept in files rather than inline in the JSON because
/// `Project.vlt` is written pretty-printed: a couple of megabytes of preset
/// data per instrument would turn it into a slow, unreadable single-line blob.
/// The JSON carries only the basename, resolved against the package on load,
/// exactly as clip media already is.
///
/// This is a clean-start format — it deliberately does not read the old
/// Swift `.dawproj` files.
/// How a document's media is referenced in the JSON.
enum class MediaPaths {
    /// Basenames resolved against `<package>/Content` — the portable project
    /// format, where every referenced file has been copied into the package.
    Basenames,
    /// The user's own files, referenced where they actually are. Used by the
    /// crash-recovery journal, which must be cheap enough to write every few
    /// seconds and so cannot copy gigabytes of audio.
    Absolute,
};

class ProjectSerializer {
public:
    static constexpr const char* kProjectFile = "Project.vlt";
    static constexpr const char* kMediaDir = "Content";
    static constexpr const char* kStateDir = "State";
    static constexpr const char* kExtension = "vlt";
    /// A project template uses the same portable package layout and manifest
    /// schema as a project, but is opened as a new document and carries no
    /// arrangement clips.
    static constexpr const char* kTemplateExtension = "vltt";
    /// 2 added plugin inserts, 3 added Sampler FX, 4 per-audio-clip Sample
    /// Editor state, 5 the persisted BPM/key analysis, and 6 content-addressed
    /// assets, plugin compatibility fields, and stable automation/comp ids;
    /// 7 names the durable render sample rate separately from device state.
    /// Never enforced on
    /// load and deliberately so: the
    /// reader defaults every field, which makes the format additive-tolerant in
    /// both directions — v1-v6 files load here, while additive fields remain
    /// ignorable by older readers.
    static constexpr int kFormatVersion = 7;

    /// Write `project` into the package directory `packageDir` (created if
    /// needed). Referenced audio is copied into `<packageDir>/Content/`.
    static audio::Result save(const ProjectModel& project,
                              const std::string& packageDir);

    /// Read a project from `packageDir`. Clip file paths in `out` are resolved
    /// to absolute paths inside the package's media folder.
    static audio::Result load(ProjectModel& out,
                              const std::string& packageDir);

    /// Write just the document to `filePath`, with no media copying and no
    /// package layout — the whole of `save` minus the expensive half.
    ///
    /// Atomic: written to a temporary sibling and renamed over the target, so
    /// a reader never sees a half-written file and a failed write leaves the
    /// previous one intact.
    static audio::Result saveDocument(const ProjectModel& project,
                                      const std::string& filePath,
                                      MediaPaths media);

    /// Read a document written by `saveDocument`. Basenames resolve against
    /// `mediaDir`; absolute paths ignore it and pass through unchanged, which
    /// is what lets one reader serve both media modes.
    static audio::Result loadDocument(ProjectModel& out,
                                      const std::string& filePath,
                                      const std::string& mediaDir);

    /// Canonical in-memory v7 document codec used by cloud snapshots. Compact
    /// output has deterministic object-key ordering and performs no filesystem
    /// access; cloud callers must project away local paths before invoking it.
    static audio::Result serializeDocument(const ProjectModel& project,
                                           std::string& out,
                                           MediaPaths media = MediaPaths::Absolute);
    static audio::Result deserializeDocument(ProjectModel& out,
                                             std::string_view bytes,
                                             const std::string& mediaDir = {});

    /// Copy one additional project-owned asset (for example the built-in
    /// Sampler's source file) into `Content/`. `outFileName` is the portable
    /// basename to store in plugin state. Content collisions are resolved by
    /// hash, using the same policy as clip media.
    static audio::Result copyContentFile(const std::string& sourcePath,
                                         const std::string& packageDir,
                                         std::string& outFileName);

    static std::string mediaPath(const std::string& packageDir);
    /// `<packageDir>/State` — where plugin state chunks live.
    static std::string statePath(const std::string& packageDir);
};

} // namespace daw
