#include "TimelineWidget.hpp"
#include "TrackListWidget.hpp"
#include "MixerWidget.hpp"
#include "InspectorWidget.hpp"
#include "ContextPanel.hpp"
#include "MasterPanel.hpp"
#include "FileBrowserPanel.hpp"
#include "TypingKeyboard.hpp"
#include "ChannelStrip.hpp"
#include "RackWidget.hpp"
#include "RackDrag.hpp"
#include "ClipLibraryView.hpp"
#include "PreviewLoader.hpp"
#include "MidiPreviewLoader.hpp"
#include "Controls.hpp"
#include "PluginQuickAdder.hpp"
#include <QAbstractAnimation>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <cmath>

void TimelineWidget::setProjectController(daw::EngineController* controller) {
    if (m_controller == controller) return;
    finishInterruptedPointerGesture();
    finishProjectGesture();
    clearRegion();
    m_selection.clear();
    m_selectedClipId.clear();
    m_selectedTrackId.clear();
    m_controller = controller;
    m_patternSources.clear();
    m_patternSourcesStamp = {};
    m_folderPreviewStamp = {};
    m_folderPreviewRows.clear();
    m_clipIntervals.clear(); m_clipIntervalEntries = 0;
    m_midiPreviewCache.clear(); m_midiPreviewCachedNotes = m_midiPreviewCachedOwners = 0;
    m_laneGeometryValid = false;
    m_labelSelectionId.clear(); m_labelSelectionRow = m_labelDragRow = -1;
    m_labelProjectRevision = ~std::uint64_t(0);
    m_linkCounts.clear();
    m_linkCountRevision = ~std::uint64_t(0);
    m_layoutStructureRevision = ~std::uint64_t(0);
    m_staticFrameValid = false;
    m_staticDirty = {}; m_staticViewportDirty = true;
    update();
}

void TimelineWidget::restoreProjectSelection(const QVector<ui::ClipSel>& clips) {
    m_selection.clear();
    for (const auto& clip : clips) m_selection.push_back({clip.trackId, clip.clipId});
    m_selectedClipId = m_selection.isEmpty() ? QString() : m_selection.front().clipId;
    publishSelection();
    update();
}

void TimelineWidget::restoreProjectRegion(double start, double end, const QStringList& tracks) {
    clearRegion();
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0. || end <= start || tracks.isEmpty()) return;
    const auto& rows = visibleRows();
    for (int lane = 0; lane < int(rows.size()); ++lane) {
        if (!tracks.contains(trackIdForLane(lane))) continue;
        if (m_regionLaneA < 0) m_regionLaneA = lane;
        m_regionLaneB = lane;
    }
    m_regionStart = start; m_regionEnd = end;
    m_regionActive = m_regionLaneA >= 0;
    update();
}

bool TimelineWidget::pasteProjectTransfer(const QString& track, double seconds) {
    std::optional<daw::AutomationTarget> automation;
    if (m_selection.size() == 1) {
        if (const auto* clip = findClipModel(m_selection.front().trackId, m_selection.front().clipId);
            clip && clip->kind == daw::ClipKind::Automation) automation = clip->automation.target;
    }
    std::vector<std::pair<std::string, std::string>> inserted;
    std::string notice;
    const auto pasted = m_controller->pasteProjectObjects(daw::projectTransferClipboard(), track.toStdString(), seconds,
        inserted, notice, automation ? &*automation : nullptr);
    if (!pasted) { emit operationStatus(QString::fromStdString(pasted.message())); return false; }
    clearRegion();
    QVector<ui::ClipSel> selected;
    for (const auto& [channel, clip] : inserted) selected.push_back({QString::fromStdString(channel), QString::fromStdString(clip)});
    restoreProjectSelection(selected);
    if (!selected.isEmpty()) emit clipSelected(selected.front().trackId, selected.front().clipId);
    if (!notice.empty()) emit operationStatus(QString::fromStdString(notice));
    emit projectEdited(); update(); return true;
}

void MixerWidget::setProjectController(daw::EngineController* controller) {
    if (m_controller == controller) return;
    finishChannelEdits();
    m_rack->finishEdits();
    disconnect(m_rack, nullptr, this, nullptr);
    ui::rack::retireView(m_rack);
    m_rack = nullptr;
    for (auto* strip : findChildren<ChannelStrip*>()) { disconnect(strip, nullptr, this, nullptr); ui::rack::retireView(strip); }
    m_details = nullptr;
    m_slots.clear(); m_residentSlots.clear(); m_recycledStrips.clear();
    m_strips.clear(); m_channels.clear(); m_selectedTrackId.clear();
    m_colorHeightRevision = ~quint64(0);
    m_controller = controller;
    m_rack = new RackWidget(controller, m_views);
    m_views->addWidget(m_rack);
    m_rack->editorIsOpen = [this](const QString& channel, const QString& slot) {
        return pluginEditorIsOpen && pluginEditorIsOpen(channel, slot);
    };
    connect(m_rack, &RackWidget::editorToggleRequested, this, &MixerWidget::pluginEditorToggleRequested);
    connect(m_rack, &RackWidget::editorRequested, this, &MixerWidget::pluginEditorRequested);
    connect(m_rack, &RackWidget::automationRequested, this, &MixerWidget::automatePluginRequested);
    connect(m_rack, &RackWidget::sendAutomationRequested, this, &MixerWidget::automateSendRequested);
    connect(m_rack, &RackWidget::channelRequested, this, &MixerWidget::trackSelected);
    connect(m_rack, &RackWidget::trackCreated, this, &MixerWidget::trackCreated);
    connect(m_rack, &RackWidget::edited, this, [this](bool structural) {
        emit channelEdited(m_rack->channelId(), true);
        if (structural) {
            QTimer::singleShot(0, this, &MixerWidget::rebuild);
            emit structureChanged();
        }
    });
    rebuild();
    setRackMode(m_rackMode);
}

void InspectorWidget::setProjectController(daw::EngineController* controller) {
    if (m_controller == controller) return;
    finishClipEdits();
    for (auto* strip : std::as_const(m_strips)) { strip->finishEdits(); disconnect(strip, nullptr, this, nullptr); ui::rack::retireView(strip); }
    m_strips.clear(); m_strip = nullptr; m_recentStrips.clear(); m_stripProjectId.clear();
    m_trackId.clear(); m_clipId.clear();
    m_controller = controller;
    rebuild();
}

void TrackListWidget::finishProjectEdits() {
    finishTrackButtonPaint();
    for (const auto& row : m_rows) {
        if (row.fader) row.fader->finishEditing();
        if (row.pan) row.pan->finishEditing();
        if (row.container) row.container->unsetCursor();
    }
    if (m_resizingHeight) {
        m_resizingHeight = false;
        m_controller->commitTrackHeightEdit(m_resizeUndoStart);
        m_resizeStart.clear(); m_resizeUndoStart.clear();
        emit trackHeightChanged();
    }
    cancelDrag();
}

void TrackListWidget::setProjectController(daw::EngineController* controller) {
    if (m_controller == controller) return;
    finishProjectEdits();
    m_controller = controller;
    rebuild();
}

void ContextPanel::setProjectController(daw::EngineController* controller) {
    if (m_controller == controller) return;
    if (m_finishEdits) m_finishEdits();
    if (m_transition) m_transition->stop();
    if (m_drift) m_drift->stop();
    delete m_quickAdder.data();
    m_applyValues = {}; m_retargetTrack = {}; m_finishEdits = {};
    ui::rack::retireView(m_outgoing); m_outgoing = nullptr;
    ui::rack::retireView(m_content); m_content = nullptr;
    m_context = Context::None; m_contextKey.clear();
    m_controller = controller;
    onSelectionChanged(true);
}

void MasterPanel::setProjectController(daw::EngineController* controller) {
    if (m_controller == controller) return;
    m_fader->finishEditing();
    if (m_panBefore) { m_controller->commitMasterPanEdit(*m_panBefore); m_panBefore.reset(); }
    m_gainBefore.reset();
    m_controller = controller;
    m_chainState.clear();
    sync(); refreshMeters();
}

void FileBrowserPanel::setProjectController(daw::EngineController* controller) {
    m_loader->cancel(); m_midiLoader->cancel();
    m_playOnLoad = false;
    m_selectedPath.clear();
    m_playheadTimer->stop();
    m_controller = controller;
    if (m_clipLibrary) m_clipLibrary->setProjectController(controller);
}

void TypingKeyboard::setProjectController(daw::EngineController* controller) {
    if (m_controller == controller) return;
    allNotesOff();
    m_controller = controller;
}
