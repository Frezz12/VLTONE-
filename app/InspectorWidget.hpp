#pragma once

#include <QString>
#include <QWidget>
#include <QHash>

namespace daw { class EngineController; }

class QLabel;
class QLineEdit;
class QAbstractButton;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QVBoxLayout;
class ChannelStrip;
namespace ui { class IconButton; }

/// The left panel, Logic-style: track properties on top and the selected
/// track's channel strip underneath, so the fader you reach for is always next
/// to the arrangement. Collapses to a narrow rail with a single expand button.
class InspectorWidget : public QWidget {
    Q_OBJECT
public:
    /// The two widths this panel ever has. Public because the tool strip above
    /// it lines its zones up with this column and has to know them — they used
    /// to be duplicated there as bare numbers.
    static constexpr int kExpandedWidth = 152;
    static constexpr int kRailWidth = 30;

    explicit InspectorWidget(daw::EngineController* controller,
                             QWidget* parent = nullptr);

    void setTrack(const QString& trackId);
    /// Follow one selected clip without rebuilding the track channel strip.
    /// An empty clip id restores the ordinary track-only inspector.
    void setSelection(const QString& trackId, const QString& clipId);
    /// Change identity and rebuild exactly once. Bulk shell refreshes use this
    /// instead of `setTrack()` followed by a second `rebuild()`.
    void rebuildForTrack(const QString& trackId);
    /// Rebuild the embedded strip and re-read the properties.
    void rebuild();
    /// Re-read values without reconstructing the embedded channel strip.
    void syncFromModel();
    void refreshMeters();
    void refreshAutomationValues();

    bool isCollapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed);
    void toggleCollapsed() { setCollapsed(!m_collapsed); }

signals:
    void edited(bool localFileDirty = true);
    /// The embedded channel's slots changed and peer views need one structural
    /// refresh. Value changes are handled by the cheaper `syncFromModel()`.
    void structureChanged();
    void collapsedChanged(bool collapsed);
    void pluginEditorRequested(const QString& channelId, const QString& insertId);
    void automateControlRequested(const QString& trackId, bool pan);
    void automateMuteRequested(const QString& trackId);
    void automateSendRequested(const QString& trackId, const QString& sendId);
    void stretchToolRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void applyTheme();
    void pickColor();
    void loadProperties();
    QWidget* buildClipSection();
    QDoubleSpinBox* addClipSpin(QFormLayout* form, const QString& label,
                               const QString& parameterId, double minimum,
                               double maximum, double step, int decimals,
                               const QString& suffix = {}, double scale = 1.0);
    void applyClipParameter(const QString& parameterId, double value);
    void commitClipParameter(const QString& parameterId, double before);

    daw::EngineController* m_controller = nullptr;
    QString m_trackId;
    QString m_clipId;
    bool m_collapsed = false;
    bool m_loadingClipControls = false;

    QWidget* m_content = nullptr;
    QWidget* m_rail = nullptr;
    QWidget* m_header = nullptr;
    ui::IconButton* m_collapseButton = nullptr;

    QLineEdit* m_nameEdit = nullptr;
    QWidget* m_colorSwatch = nullptr;
    QLabel* m_kindLabel = nullptr;
    QLabel* m_clipsLabel = nullptr;
    QWidget* m_clipSection = nullptr;
    QLabel* m_clipNameLabel = nullptr;
    QWidget* m_clipAdvanced = nullptr;
    QHash<QString, QDoubleSpinBox*> m_clipSpins;
    QHash<QString, QComboBox*> m_clipCombos;
    QHash<QString, QAbstractButton*> m_clipToggles;
    QHash<QString, double> m_clipGestureStarts;
    QVBoxLayout* m_stripSlot = nullptr;
    ChannelStrip* m_strip = nullptr;
};
