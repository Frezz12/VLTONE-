#pragma once

#include <QWidget>

namespace ui { class IconButton; }
class QLabel;
/// Defined in the .cpp so their panel-specific accessible labels stay local.
class AiChatButton;
class WebBrowserButton;

/// The thin strip under the arrangement: icon-only toggles for the mixer, the
/// inspector and the mixer window, plus a hint of where the mixer currently
/// lives. Labels are kept to the minimum — the icons carry the meaning.
class BottomBar : public QWidget {
    Q_OBJECT
public:
    explicit BottomBar(QWidget* parent = nullptr);

    void setMixerVisible(bool visible);
    void setInspectorVisible(bool visible);
    void setBrowserVisible(bool visible);
    void setWebVisible(bool visible);
    void setAiVisible(bool visible);
    void setMixerDetached(bool detached);

signals:
    void mixerToggled(bool visible);
    void inspectorToggled(bool visible);
    void browserToggled(bool visible);
    void webToggled(bool visible);
    void aiToggled(bool visible);
    void detachMixerRequested();
    void addTrackRequested();
    void settingsRequested();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void applyTheme();

    ui::IconButton* m_mixerButton = nullptr;
    ui::IconButton* m_inspectorButton = nullptr;
    ui::IconButton* m_browserButton = nullptr;
    ui::IconButton* m_detachButton = nullptr;
    WebBrowserButton* m_webButton = nullptr;
    AiChatButton* m_aiButton = nullptr;
    QLabel* m_hint = nullptr;
};
