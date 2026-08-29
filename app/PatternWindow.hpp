#pragma once

#include <QDialog>
#include <QList>
#include <QPoint>
#include <QString>
#include <QStringList>

namespace daw { class EngineController; }
class QVBoxLayout;

/// Compact editor for a Pattern container.
///
/// Every row is still a real instrument track — with its own mixer channel,
/// clips and plugin chain — while this window presents the rows as one musical
/// object. Audio files dropped anywhere on it become new Sampler rows.
class PatternWindow final : public QDialog {
    Q_OBJECT
public:
    explicit PatternWindow(daw::EngineController* controller,
                           QWidget* parent = nullptr);

    void setPattern(const QString& patternId);
    const QString& patternId() const { return m_patternId; }
    void refresh();
    bool checkInteractionGesturesForTest();

signals:
    void projectEdited();
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
    void closeEvent(QCloseEvent* event) override;

private:
    void applyTheme();
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
    void updateExternalDropFeedback(const QPoint& globalPos);
    void clearExternalDropFeedback();
    void endRowGestureState();
    QStringList childTrackIds() const;

    daw::EngineController* m_controller = nullptr;
    QString m_patternId;
    QWidget* m_rowsHost = nullptr;
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
