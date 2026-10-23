#include "AiChatPanel.hpp"
#include "ContextPanel.hpp"
#include "FileBrowserPanel.hpp"
#include "InspectorWidget.hpp"
#include "InternalEditorFrame.hpp"
#include "MainWindow.hpp"
#include "MasterPanel.hpp"
#include "MixerWidget.hpp"
#include "PianoRollWindow.hpp"
#include "ScriptService.hpp"
#include "ShortcutManager.hpp"
#include "TimelineWidget.hpp"
#include "TrackListWidget.hpp"
#include "TrackOverviewPanel.hpp"
#include "scripting/ScriptProject.hpp"

void MainWindow::setupScripts() {
    using Json = vlt::script::Json;
    ui::ScriptService::Bindings bindings;
    bindings.current = [this] {
        auto &session = m_projects.current();
        return ui::ScriptService::Project{session.id,
                                          QString::fromStdString(session.controller.projectName()),
                                          &session.controller};
    };
    bindings.resolve = [this](const QString &id) {
        auto *session = m_projects.find(id);
        return session ? ui::ScriptService::Project{session->id,
                                                    QString::fromStdString(
                                                        session->controller.projectName()),
                                                    &session->controller}
                       : ui::ScriptService::Project{};
    };
    bindings.context = [this](const QString &id) {
        auto *session = m_projects.find(id);
        if (!session)
            return Json::object();
        const auto &tracks =
            id == m_projects.selected ? m_selection.tracks() : session->selectedTracks;
        const auto &clips =
            id == m_projects.selected ? m_selection.clips() : session->selectedClips;
        Json selectedTracks = Json::array(), selectedClips = Json::array();
        for (const auto &track : tracks)
            selectedTracks.push_back(track.toStdString());
        for (const auto &clip : clips)
            selectedClips.push_back(
                {{"track", clip.trackId.toStdString()}, {"clip", clip.clipId.toStdString()}});
        const auto &c = session->controller;
        Json context{{"type", "global"},
                     {"selection", {{"tracks", selectedTracks}, {"clips", selectedClips}}},
                     {"range",
                      {{"start", c.loopStartSeconds() * c.tempo() / 60},
                       {"end", c.loopEndSeconds() * c.tempo() / 60}}}};
        if (id == m_projects.selected && m_timeline && m_timeline->hasRegionSelection())
            context["range"] = {{"start", m_timeline->regionStartSeconds() * c.tempo() / 60},
                                {"end", m_timeline->regionEndSeconds() * c.tempo() / 60}};
        else if (id != m_projects.selected &&
                 !session->view.value("regionTracks").toStringList().isEmpty())
            context["range"] = {
                {"start", session->view.value("regionStart").toDouble() * c.tempo() / 60},
                {"end", session->view.value("regionEnd").toDouble() * c.tempo() / 60}};
        if (!clips.isEmpty())
            context["target"] = {{"track", clips.front().trackId.toStdString()},
                                 {"clip", clips.front().clipId.toStdString()}};
        else if (!tracks.isEmpty())
            context["target"] = {{"track", tracks.front().toStdString()}};
        if (id == m_projects.selected && m_pianoRoll && m_pianoRollFrame &&
            m_pianoRollFrame->isVisible() && m_pianoRollFrame->isEditorActive()) {
            context["type"] = "notes";
            context["target"] = {{"track", m_pianoRoll->trackId().toStdString()},
                                 {"clip", m_pianoRoll->clipId().toStdString()}};
            context["notes"] = Json::array();
            for (const auto &note : m_pianoRoll->selectedNotesForAssistant())
                context["notes"].push_back(note.id);
        }
        return context;
    };
    bindings.edited = [this](const QString &id) {
        auto *session = m_projects.find(id);
        if (!session)
            return;
        session->dirty = true;
        ++session->revision;
        if (id == m_projects.selected) {
            markDirty();
            m_selection.refresh();
            if (m_timeline)
                m_timeline->update();
            if (m_mixer)
                m_mixer->rebuild();
            if (m_inspector)
                m_inspector->rebuild();
        }
        refreshProjectTabs();
    };
    bindings.command = [this](const QString &id, const QString &project) {
        return project == m_projects.selected && m_shortcuts->invoke(id);
    };
    bindings.workspace = [this](const QString &project, const Json &request) -> Json {
        auto *session = m_projects.find(project);
        if (!session)
            throw std::runtime_error("Project closed");
        const auto method = request.at("method").get<std::string>();
        const bool active = project == m_projects.selected;
        if (method == "workspace.get") {
            Json panels = Json::object();
            if (active) {
                panels = {{"arrangement", m_timeline && m_timeline->isVisible()},
                          {"mixer", m_mixer && m_mixer->isVisible()},
                          {"inspector", m_inspector && m_inspector->isVisible()},
                          {"browser", m_browser && m_browser->isVisible()},
                          {"master", m_masterPanel && m_masterPanel->isVisible()},
                          {"assistant", m_aiPanel && m_aiPanel->isVisible()},
                          {"notebook", m_notebookContainer && m_notebookContainer->isVisible()},
                          {"trackOverview", m_trackOverview && m_trackOverview->isVisible()},
                          {"context", m_contextPanel && m_contextPanel->isVisible()}};
            }
            Json tracks = Json::array(), clips = Json::array();
            for (const auto &id : active ? m_selection.tracks() : session->selectedTracks)
                tracks.push_back(id.toStdString());
            for (const auto &clip : active ? m_selection.clips() : session->selectedClips)
                clips.push_back(
                    {{"track", clip.trackId.toStdString()}, {"clip", clip.clipId.toStdString()}});
            return {{"active", active},
                    {"project", project.toStdString()},
                    {"panels", panels},
                    {"selection", {{"tracks", tracks}, {"clips", clips}}}};
        }
        if (!active)
            throw std::runtime_error("Activate the pinned project first");
        const auto trackModel = [&](const std::string &id) {
            const auto *track = session->controller.project().findTrack(id);
            if (!track)
                throw std::runtime_error("Track no longer exists");
            return track;
        };
        const auto clipModel = [&](const std::string &track, const std::string &id) {
            const auto *model = trackModel(track);
            const auto found = std::find_if(model->clips.begin(), model->clips.end(),
                                            [&](const auto &clip) { return clip.id == id; });
            if (found == model->clips.end())
                throw std::runtime_error("Clip no longer exists");
            return &*found;
        };
        if (method == "workspace.select") {
            const auto tracks = request.value("tracks", Json::array());
            const auto clips = request.value("clips", Json::array());
            if (tracks.size() + clips.size() > 4096)
                throw std::runtime_error("Selection limit exceeded");
            QStringList selectedTracks;
            QVector<ui::ClipSel> selectedClips;
            // Validate the whole selection before changing any view.
            for (const auto &id : tracks) {
                trackModel(id.get<std::string>());
                const auto text = QString::fromStdString(id.get<std::string>());
                if (!selectedTracks.contains(text))
                    selectedTracks.append(text);
            }
            for (const auto &item : clips) {
                const auto track = item.at("track").get<std::string>(),
                           clip = item.at("clip").get<std::string>();
                clipModel(track, clip);
                selectedClips.append({QString::fromStdString(track), QString::fromStdString(clip)});
                if (!selectedTracks.contains(QString::fromStdString(track)))
                    selectedTracks.append(QString::fromStdString(track));
            }
            m_trackList->setSelectedTracks(selectedTracks, selectedTracks.value(0));
            m_timeline->selectClips(selectedClips);
            if (selectedTracks.isEmpty() && selectedClips.isEmpty())
                m_selection.clear();
            else if (selectedClips.isEmpty())
                m_selection.setTracks(selectedTracks);
            else
                m_selection.setClips(selectedClips);
            onSelectionChanged(selectedTracks.value(0));
            refreshRecordChips();
            return true;
        }
        if (method != "workspace.open")
            throw std::runtime_error("Unknown workspace method");
        const auto view = request.at("view").get<std::string>();
        const bool visible = request.value("visible", true);
        if (view == "mixer")
            setMixerVisible(visible);
        else if (view == "inspector")
            setInspectorVisible(visible);
        else if (view == "browser")
            setBrowserVisible(visible);
        else if (view == "master")
            setMasterVisible(visible);
        else if (view == "assistant")
            setAiVisible(visible);
        else if (view == "notebook")
            setNotebookVisible(visible);
        else if (view == "trackOverview")
            setTrackOverviewVisible(visible);
        else if (view == "context")
            setContextPanelVisible(visible);
        else if (view == "arrangement") {
            if (!visible)
                throw std::runtime_error("The arrangement cannot be hidden");
            m_timeline->setFocus(Qt::OtherFocusReason);
        } else {
            if (!visible)
                throw std::runtime_error("Editor views support opening only");
            const auto track = request.at("track").get<std::string>();
            const auto trackId = QString::fromStdString(track);
            if (view == "plugin") {
                const auto device = request.at("device").get<std::string>();
                const auto identity = request.at("identity").get<std::string>();
                if (!session->controller.insertModel(track, device))
                    throw std::runtime_error("Device no longer exists");
                if (identity !=
                    daw::scripting::parameterIdentity(session->controller, track, device))
                    throw std::runtime_error("Device was replaced");
                openPluginEditor(trackId, QString::fromStdString(device));
            } else {
                const auto clip = request.at("clip").get<std::string>();
                const auto *model = clipModel(track, clip);
                const auto clipId = QString::fromStdString(clip);
                if (view == "pianoRoll" && model->kind == daw::ClipKind::Midi)
                    openPianoRoll(trackId, clipId);
                else if (view == "pattern" && model->kind == daw::ClipKind::Pattern)
                    openPatternClip(trackId, clipId);
                else if (view == "sample" && model->kind == daw::ClipKind::Audio)
                    openSampleEditor(trackId, clipId);
                else if (view == "automation" && model->kind == daw::ClipKind::Automation)
                    openAutomationEditor(trackId, clipId);
                else
                    throw std::runtime_error("Unknown editor or incompatible clip kind");
            }
        }
        return true;
    };
    m_scripts = new ui::ScriptService(this, m_shortcuts, std::move(bindings));
    m_scripts->setupMenu();
}
