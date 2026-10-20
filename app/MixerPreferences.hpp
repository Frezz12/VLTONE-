#pragma once

#include <QObject>

namespace ui {

/// Application-wide console layout, independent of project state and Undo.
class MixerPreferences final : public QObject {
    Q_OBJECT
public:
    enum class View { Classic, Compact };
    Q_ENUM(View)
    static constexpr int kMinimumWidth = 75;
    static constexpr int kDefaultWidth = 100;
    static constexpr int kMaximumWidth = 180;
    static constexpr auto kWidthSetting = "mixer/channelWidth";
    static constexpr auto kViewSetting = "mixer/view";
    static constexpr auto kCompactWidthSetting = "mixer/compactWidth";
    static constexpr auto kDetailsWidthSetting = "mixer/detailsPanelWidth";
    static constexpr int kDefaultCompactWidth = 32;
    static constexpr int kDefaultDetailsWidth = 120;
    static constexpr int kMinimumDetailsWidth = 100;
    static constexpr int kMaximumDetailsWidth = 400;
    static constexpr auto kMasterVisibleSetting = "master/panelVisible";
    /// Section visibility applies to the whole console to retain alignment.
    /// The legacy COLOR setting name is kept for existing preferences.
    static constexpr auto kColorVisibleSetting = "mixer/colorVisible";

    static MixerPreferences& instance();
    View view() const { return m_view; }
    void setView(View view);
    int compactWidth() const { return m_compactWidth; }
    void setCompactWidth(int width);
    static int normalizedCompactWidth(int width);
    int detailsWidth() const { return m_detailsWidth; }
    void setDetailsWidth(int width);
    int channelWidth() const { return m_channelWidth; }
    void setChannelWidth(int width);
    bool masterVisible() const { return m_masterVisible; }
    void setMasterVisible(bool visible);
    /// False takes the COLOR row off every strip. The stage keeps running and
    /// everything already saved in the project stays saved — only the controls
    /// go away.
    bool colorVisible() const { return m_colorVisible; }
    void setColorVisible(bool visible);

signals:
    void viewChanged(ui::MixerPreferences::View view);
    void compactWidthChanged(int width);
    void detailsWidthChanged(int width);
    void channelWidthChanged(int width);
    void masterVisibleChanged(bool visible);
    void colorVisibleChanged(bool visible);

private:
    explicit MixerPreferences(QObject* parent);
    View m_view = View::Classic;
    int m_compactWidth = kDefaultCompactWidth;
    int m_detailsWidth = kDefaultDetailsWidth;
    int m_channelWidth = kDefaultWidth;
    bool m_masterVisible = true;
    bool m_colorVisible = true;
};

} // namespace ui
