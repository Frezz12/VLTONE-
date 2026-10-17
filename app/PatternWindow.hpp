#pragma once

#include <QWidget>
#include <QList>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <string>
#include <optional>
#include <vector>

namespace daw { class EngineController; struct NoteModel; struct TrackModel; }
class QVBoxLayout;
class QMenu;
class QComboBox;
class QLabel;
class QMimeData;
namespace daw::plugins { struct PluginDescriptor; }

/// Rack module for a Pattern container.
///
/// Every row is still a real instrument track — with its own mixer channel,
/// clips and plugin chain. Audio files dropped on the module become new sounds.
class PatternWindow final : public QWidget {
    Q_OBJECT
public:
    explicit PatternWindow(daw::EngineController* controller,
                           QWidget* parent = nullptr);

    void setPattern(const QString& patternId, const QString& clipId = {});
    const QString& patternId() const { return m_patternId; }
    const QString& patternClipId() const { return m_patternClipId; }
    bool command(const QString& action);
    bool addInstrument(const daw::plugins::PluginDescriptor& descriptor);
    const QStringList& selectedSourceTrackIds() const { return m_selectedIds; }
    const QString& selectedSourceTrackId() const { return m_primaryId; }
    /// Replace the selected layer's audio while retaining an existing
    /// Sampler instance and all of its controls.
    bool replaceSelectedSample(const QString& path);
    void refresh();
    bool populateActionsMenu(QMenu& menu);
    bool checkInteractionGesturesForTest();
    static bool checkEditingForTest();

signals:
    void projectEdited(bool localFileDirty = true);
    void sourceSelectionChanged();
    void sourceRackRequested(const QString& trackId);
    void openPianoRollRequested(const QString& trackId, const QString& clipId);
    void openPluginEditorRequested(const QString& trackId,
                                   const QString& slotId);
    void automateControlRequested(const QString& trackId, bool pan);
    void automateMuteRequested(const QString& trackId);

protected:
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void applyTheme();
    void syncClipSelector();
    std::optional<daw::plugins::PluginDescriptor> droppedInstrument(const QMimeData*) const;
    void rebuildRows();
    bool rowStructureMatches(const QStringList& ids) const;
    bool syncRowsFromModel();
    void showInstrumentMenu();
    void addSampleFiles(const QStringList& paths, double startSeconds = 0.0,
                        int insertionIndex = -1);
    bool replaceSample(const QString& trackId, const QString& path);
    void chooseReplacementSample(const QString& trackId);
    void openInstrument(const QString& trackId);
    void renameSource(const QString& trackId);
    void duplicateSource(const QString& trackId);
    void removeSource(const QString& trackId);
    void deleteSelectedSources();
    void transposeSelectedSources();
    void transposeSelectedSourcesBy(int semitones);
    void moveSelectedSources(int direction);
    void reorderSelectedSources(int dropIndex);
    void openRoll(const QString& trackId);
    void populateRhythmMenu(QMenu* menu, const QString& trackId);
    void populateSourceMenu(QMenu* menu, const QString& trackId);
    void fillRhythm(const QString& trackId, int divisionsPerBar);
    bool applyMidiFile(const QString& trackId, const QString& path,
                      QString* error = nullptr);
    bool replaceSourceNotes(const QString& trackId,
                            std::vector<daw::NoteModel> notes,
                            double lengthBeats, const std::string& label);
    void populateSelectionMenu(QMenu& menu, const QString& trackId);
    void showSelectionMenu(const QString& trackId, const QPoint& globalPos);
    void beginRowGesture(const QString& trackId, const QPoint& globalPos,
                         Qt::KeyboardModifiers modifiers);
    void updateRowGesture(const QPoint& globalPos);
    void endRowGesture(const QString& trackId, const QPoint& globalPos);
    int rowIndexAtGlobal(const QPoint& globalPos) const;
    int insertionIndexAtGlobal(const QPoint& globalPos) const;
    void setSelectedSources(const QStringList& ids, const QString& primary = {});
    void selectRange(int first, int last);
    void updateSelectionVisuals();
    void updateDropIndicator(int insertionIndex);
    int replacementRowAtGlobal(const QPoint& globalPos) const;
    void updateExternalDropFeedback(const QPoint& globalPos, bool replaceMidi);
    void clearExternalDropFeedback();
    void endRowGestureState();
    QStringList childTrackIds() const;
    bool ownsTrack(const daw::TrackModel* track) const;

    daw::EngineController* m_controller = nullptr;
    QString m_patternId;
    QString m_patternClipId;
    mutable QStringList m_childIds;
    mutable QString m_childPattern, m_childClip;
    mutable quint64 m_childRevision = ~quint64(0), m_childGeneration = 0;
    mutable quint64 m_childGeometry = 0, m_childStructure = 0;
    QWidget* m_rowsHost = nullptr;
    QLabel* m_title = nullptr;
    QComboBox* m_clipSelector = nullptr;
    QVBoxLayout* m_rowsLayout = nullptr;
    QWidget* m_dropIndicator = nullptr;
    QList<QWidget*> m_rowWidgets;
    QStringList m_selectedIds;
    QString m_primaryId;
    QString m_selectionAnchorId;
    QPoint m_gesturePressGlobal;
    int m_gestureAnchorIndex = -1;
    int m_dropIndex = -1;
    int m_externalReplaceIndex = -1;
    Qt::KeyboardModifiers m_gestureModifiers = Qt::NoModifier;
    bool m_rowGestureActive = false;
    bool m_rangeSelecting = false;
    bool m_reorderCandidate = false;
    bool m_reordering = false;
};
