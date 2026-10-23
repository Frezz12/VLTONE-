#pragma once

#include "EngineController.hpp"
#include "SelectionModel.hpp"
#include "WorkspaceAudioHost.hpp"

#include <QHash>
#include <QPointer>
#include <QString>
#include <QUuid>
#include <QVariantMap>
#include <QWidget>
#include <array>
#include <memory>
#include <vector>

using ProjectId = QString;

struct ProjectSession final {
    ProjectId id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    daw::EngineController controller;
    QString path;
    bool dirty = false;
    quint64 revision = 0;
    QVariantMap view;
    QString selectedTrack;
    std::string midiTarget;
    QHash<QString, std::array<unsigned int, 128>> livePitchCounts;
    QStringList selectedTracks;
    QVector<ui::ClipSel> selectedClips;
    QStringList recordPins;
    bool recordEngaged = false;
    QList<QPointer<QWidget>> editors;
    QHash<QString, QPointer<QWidget>> pluginEditors, sampleEditors, automationEditors;
    QPointer<QWidget> piano, pianoFrame, noteContext, warp, notebook;
    QPointer<QWidget> activeEditor;
    QList<QPointer<QWidget>> visibleEditors;
    std::shared_ptr<const daw::recovery::WorkspaceRecoveryDocument> recoveryDocument;
    quint64 recoveryControllerRevision = 0, recoveryUiRevision = 0;
};

/// Owns documents, not windows. Only the selected document is bound to the
/// shared arrangement/mixer; the audio owner is an independent identity.
class ProjectWorkspace final {
public:
    ProjectWorkspace() { append(); }
    ProjectSession& append() {
        auto session = std::make_unique<ProjectSession>();
        session->controller.useWorkspacePluginManager(plugins);
        auto& result = *session;
        documents.push_back(std::move(session));
        if (selected.isEmpty()) selected = result.id;
        return result;
    }
    ProjectSession* find(const ProjectId& id) const {
        for (const auto& session : documents) if (session->id == id) return session.get();
        return nullptr;
    }
    ProjectSession& current() const { return *find(selected); }
    ProjectSession* audible() const {
        for (const auto& session : documents) if (&session->controller == audio.active()) return session.get();
        return nullptr;
    }
    daw::PluginManager plugins;
    std::vector<std::unique_ptr<ProjectSession>> documents;
    ProjectId selected;
    daw::WorkspaceAudioHost audio;
};
