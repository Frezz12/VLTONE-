#pragma once

#include <QString>
#include <QWidget>

namespace daw { class EngineController; }

class QLabel;
class QLineEdit;
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
    void edited();
    /// The embedded channel's slots changed and peer views need one structural
    /// refresh. Value changes are handled by the cheaper `syncFromModel()`.
    void structureChanged();
    void collapsedChanged(bool collapsed);
    void pluginEditorRequested(const QString& channelId, const QString& insertId);
    void automateControlRequested(const QString& trackId, bool pan);
    void automateMuteRequested(const QString& trackId);
    void automateSendRequested(const QString& trackId, const QString& sendId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void applyTheme();
    void pickColor();
    void loadProperties();

    daw::EngineController* m_controller = nullptr;
    QString m_trackId;
    bool m_collapsed = false;

    QWidget* m_content = nullptr;
    QWidget* m_rail = nullptr;
    QWidget* m_header = nullptr;
    ui::IconButton* m_collapseButton = nullptr;

    QLineEdit* m_nameEdit = nullptr;
    QWidget* m_colorSwatch = nullptr;
    QLabel* m_kindLabel = nullptr;
    QLabel* m_clipsLabel = nullptr;
    QVBoxLayout* m_stripSlot = nullptr;
    ChannelStrip* m_strip = nullptr;
};
