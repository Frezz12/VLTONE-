#include "CreatorBuildService.hpp"
#include "Creator/CodeUtilities.hpp"
#include "EngineController.hpp"
#include "Internal/MiniModuleInstance.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "MiniModuleLibrary.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <nlohmann/json.hpp>
namespace ui {
using namespace daw::plugins::mini;
CreatorBuildService::Result
CreatorBuildService::prepare(Result result, double rate,
                             const std::atomic<bool> *cancel) {
  const auto &project = result->project;
  auto update = result->update;
  CreatorCancellationScope cancellation(cancel);
  result->error = QString::fromStdString(validate(project.definition));
  if (!result->error.isEmpty())
    return result;
  auto &definition = result->project.definition;
  const auto compileGraph = [&](auto &graph) {
    std::string error;
    if (!compileCppGraph(graph, error, result->diagnostics, cancel)) {
      result->error = QString::fromStdString(error);
      return false;
    }
    return true;
  };
  if (definition.modes.empty()) {
    if (!compileGraph(definition))
      return result;
  } else {
    for (auto &mode : definition.modes) {
      auto graph = resolved(definition, mode.id);
      if (!compileGraph(graph))
        return result;
      mode.code = graph.code;
      if (mode.id == definition.defaultMode)
        definition.code = graph.code;
    }
  }
  if (update) {
    update->definition = definition;
    for (auto &target : update->targets) {
      target.after.miniModule = definition;
      target.audioChanged = !sameAudioGraph(
          *target.before.miniModule, target.before.miniModuleMode, definition,
          target.after.miniModuleMode);
    }
  }
  std::vector<std::string> modes;
  if (project.definition.modes.empty())
    modes.push_back({});
  else
    for (const auto &m : project.definition.modes)
      modes.push_back(m.id);
  for (const auto &mode : modes) {
    const auto graph = resolved(project.definition, mode);
    auto used = reachableNodes(graph);
    for (unsigned pass = 0; pass < graph.nodes.size(); ++pass)
      for (const auto &edge : graph.connections)
        if (edge.fromPort == "function") {
          const auto from =
              std::find_if(graph.nodes.begin(), graph.nodes.end(),
                           [&](const auto &n) { return n.id == edge.from; });
          const auto to =
              std::find_if(graph.nodes.begin(), graph.nodes.end(),
                           [&](const auto &n) { return n.id == edge.to; });
          if (from != graph.nodes.end() && to != graph.nodes.end() &&
              used[to - graph.nodes.begin()])
            used[from - graph.nodes.begin()] = true;
        }
    for (unsigned i = 0; i < graph.nodes.size(); ++i)
      if (!used[i] && graph.nodes[i].type != "interface")
        result->warnings << QString::fromStdString(
            graph.nodes[i].id + ": unused node will not be processed");
    for (unsigned channels : {1u, 2u}) {
      MiniModuleInstance instance;
      daw::plugins::PluginBusLayout accepted;
      if (!instance.configure(definition, 0, mode) ||
          !instance.setBusLayout(
              {{std::uint16_t(channels)}, {std::uint16_t(channels)}},
              accepted) ||
          !instance.activate({rate, 1024})) {
        result->error = QString::fromStdString(instance.error());
        if (result->error.isEmpty())
          result->error = "DSP preparation failed";
        return result;
      }
    }
  }
  if (update && !update->prepare())
    result->error = QString::fromStdString(update->error);
  return result;
}
bool CreatorFileState::read(const QString &path, CreatorFileState &out,
                            QString &error) {
  out = {};
  out.exists = QFileInfo::exists(path);
  if (!out.exists)
    return true;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    error = f.errorString();
    return false;
  }
  if (f.size() > 64 * 1024 * 1024) {
    error = "Module file exceeds 64 MiB";
    return false;
  }
  out.bytes = f.readAll();
  if (f.error() != QFileDevice::NoError) {
    error = f.errorString();
    return false;
  }
  return true;
}
namespace {
bool writeState(const QString &path, const CreatorFileState &state,
                QString &error) {
  if (!state.exists) {
    if (!QFileInfo::exists(path) || QFile::remove(path))
      return true;
    error = "Cannot remove the installed module";
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(state.bytes) != state.bytes.size() || !file.commit()) {
    error = file.errorString();
    return false;
  }
  return true;
}
} // namespace
CreatorBuildService::CreatorBuildService(daw::EngineController *c,
                                         QObject *parent)
    : QObject(parent), m_controller(c) {
  m_timer.setInterval(25);
  connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
}
CreatorBuildService::~CreatorBuildService() {
  cancel();
  if (m_future.valid())
    m_future.wait();
  if (m_pending && m_pending->fading && m_controller)
    m_controller->cancelMiniModuleUpdateFade(*m_pending->update);
}
bool CreatorBuildService::start(const CreatorProject &project,
                                const QString &directory, Guard guard,
                                Done done) {
  if (busy())
    return false;
  auto r = std::make_shared<CreatorBuildResult>();
  r->project = project;
  r->revision = codeHash(toJson(project.definition).dump());
  const auto id = QString::fromStdString(project.definition.id);
  if (!QRegularExpression("^[A-Za-z0-9_.-]{1,128}$").match(id).hasMatch() ||
      id == "." || id == "..")
    r->error = "Invalid module identity";
  r->path = directory + "/" + id + ".vltmini";
  if (r->error.isEmpty() && !QDir().mkpath(directory))
    r->error = "Cannot create mini-module folder";
  if (r->error.isEmpty())
    CreatorFileState::read(r->path, r->beforeFile, r->error);
  if (r->error.isEmpty() && r->beforeFile.exists) {
    const auto existing = MiniModuleLibrary::read(r->path);
    if (!existing.error.isEmpty() ||
        existing.definition.id != project.definition.id)
      r->error = "Destination belongs to another or unreadable module";
  }
  m_guard = std::move(guard);
  m_done = std::move(done);
  m_cancel.store(false);
  r->update = m_controller
                  ? m_controller->planMiniModuleUpdate(project.definition)
                  : nullptr;
  const double rate = m_controller ? m_controller->sampleRate() : 48000;
  m_future = std::async(std::launch::async, [this, r, rate] {
    if (!r->error.isEmpty())
      return r;
    return prepare(r, rate, &m_cancel);
  });
  if (progress)
    progress(tr("Compiling…"));
  m_timer.start();
  return true;
}
bool CreatorBuildService::restore(const CreatorProject &project,
                                  const QString &path,
                                  CreatorFileState expected,
                                  CreatorFileState previous,
                                  std::shared_ptr<daw::MiniModuleUpdate> update,
                                  Guard guard, Done done) {
  if (busy())
    return false;
  auto r = std::make_shared<CreatorBuildResult>();
  r->restore = true;
  r->project = project;
  r->path = path;
  r->beforeFile = std::move(expected);
  r->afterFile = std::move(previous);
  r->update = std::move(update);
  m_guard = std::move(guard);
  m_done = std::move(done);
  m_cancel.store(false);
  m_future = std::async(std::launch::async, [this, r] {
    CreatorCancellationScope cancellation(&m_cancel);
    if (r->update && !r->update->prepare())
      r->error = QString::fromStdString(r->update->error);
    return r;
  });
  if (progress)
    progress(tr("Preparing previous DSP…"));
  m_timer.start();
  return true;
}
void CreatorBuildService::cancel() { m_cancel.store(true); }
void CreatorBuildService::finish(Result r) {
  if (r->fading && m_controller)
    m_controller->cancelMiniModuleUpdateFade(*r->update);
  m_timer.stop();
  m_pending.reset();
  m_guard = {};
  auto done = std::move(m_done);
  if (done)
    done(std::move(r));
}
void CreatorBuildService::poll() {
  if (!m_pending) {
    if (!m_future.valid() || m_future.wait_for(std::chrono::milliseconds(0)) !=
                                 std::future_status::ready)
      return;
    try {
      m_pending = m_future.get();
    } catch (const std::exception &e) {
      m_pending = std::make_shared<CreatorBuildResult>();
      m_pending->error = QString::fromUtf8(e.what());
    }
  }
  auto r = m_pending;
  if (m_cancel.load() || (m_guard && !m_guard()))
    r->error = tr(
        "The draft changed or the build was cancelled. Nothing was installed.");
  if (!r->error.isEmpty()) {
    finish(r);
    return;
  }
  CreatorFileState current;
  if (!CreatorFileState::read(r->path, current, r->error) ||
      current != r->beforeFile) {
    if (r->error.isEmpty())
      r->error = tr("The installed file changed. Nothing was installed.");
    finish(r);
    return;
  }
  if (m_controller && m_controller->offlineRenderInProgress()) {
    if (progress)
      progress(tr("Waiting for export or Freeze to finish…"));
    return;
  }
  if (m_controller && !m_controller->miniModuleUpdateCurrent(*r->update)) {
    r->error = tr("Module instances changed during preparation. Read the "
                  "current context and retry.");
    finish(r);
    return;
  }
  if (m_controller) {
    if (!r->fading) {
      m_controller->fadeMiniModuleUpdate(*r->update);
      r->fading = true;
      r->fadeStart = std::chrono::steady_clock::now();
    }
    if (!m_controller->miniModuleUpdateFaded(*r->update) &&
        std::chrono::steady_clock::now() - r->fadeStart <
            std::chrono::milliseconds(120))
      return;
  }
  // Knobs, bypass and placement may change while a worker prepares the DSP.
  // Publication preserves these edits; its inverse must preserve them too.
  // Capture the live pre-publication state after the last guard, on the GUI
  // thread, rather than restoring older values from the worker snapshot.
  if (m_controller && !r->update->exactRestore) {
    for (auto &target : r->update->targets)
      for (const auto &slot : m_controller->miniModules(target.channel))
        if (slot.id == target.before.id) {
          target.before = slot;
          break;
        }
  }
  if (progress)
    progress(tr("Installing…"));
  bool written =
      r->restore
          ? writeState(r->path, r->afterFile, r->error)
          : MiniModuleLibrary::write(r->path, r->project.definition, r->error);
  if (!written) {
    finish(r);
    return;
  }
  std::string error;
  if (m_controller && !m_controller->applyMiniModuleUpdate(r->update, error)) {
    QString rollbackError;
    if (!writeState(r->path, r->beforeFile, rollbackError))
      error += "; file restoration failed: " + rollbackError.toStdString();
    r->error = QString::fromStdString(error);
    finish(r);
    return;
  }
  r->fading = false;
  if (!r->restore)
    CreatorFileState::read(r->path, r->afterFile, r->error);
  finish(r);
}
} // namespace ui
