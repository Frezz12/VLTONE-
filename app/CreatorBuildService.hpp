#pragma once
#include "Creator/CreatorCompilerClient.hpp"
#include "CreatorProject.hpp"
#include "MiniModuleUpdate.hpp"
#include <QByteArray>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <functional>
#include <future>
namespace daw {
class EngineController;
}

namespace daw {
class EngineController;
}

namespace ui {
struct CreatorFileState {
  bool exists = false;
  QByteArray bytes;
  bool operator==(const CreatorFileState &) const = default;
  static bool read(const QString &, CreatorFileState &, QString &error);
};
struct CreatorBuildResult {
  CreatorProject project;
  std::string revision;
  QString error, path;
  QStringList warnings;
  std::vector<daw::plugins::mini::CodeDiagnostic> diagnostics;
  std::shared_ptr<daw::MiniModuleUpdate> update;
  CreatorFileState beforeFile, afterFile;
  bool restore = false, fading = false;
  std::chrono::steady_clock::time_point fadeStart;
};
/// Single compilation/publication path for both the toolbar and assistant.
/// Workers own snapshots only. All guards, file publication and host changes
/// run on the owner thread; no modal UI or project saving occurs here.
class CreatorBuildService final : public QObject {
  Q_OBJECT
public:
  using Result = std::shared_ptr<CreatorBuildResult>;
  using Done = std::function<void(Result)>;
  using Guard = std::function<bool()>;
  explicit CreatorBuildService(daw::EngineController *,
                               QObject *parent = nullptr);
  ~CreatorBuildService() override;
  bool start(const CreatorProject &, const QString &directory, Guard, Done);
  bool restore(const CreatorProject &, const QString &path,
               CreatorFileState expected, CreatorFileState previous,
               std::shared_ptr<daw::MiniModuleUpdate>, Guard, Done);
  void cancel();
  bool busy() const { return m_future.valid() || bool(m_pending); }
  std::function<void(const QString &)> progress;

private:
  static Result prepare(Result, double, const std::atomic<bool> *);
  void poll();
  void finish(Result);
  daw::EngineController *m_controller;
  QTimer m_timer;
  std::atomic<bool> m_cancel{false};
  std::future<Result> m_future;
  Result m_pending;
  Guard m_guard;
  Done m_done;
};
} // namespace ui
