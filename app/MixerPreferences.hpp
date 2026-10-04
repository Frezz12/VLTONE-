#pragma once

#include <QObject>

namespace ui {

/// Application-wide console layout, independent of project state and Undo.
class MixerPreferences final : public QObject {
    Q_OBJECT
public:
    static constexpr int kMinimumWidth = 75;
    static constexpr int kDefaultWidth = 100;
    static constexpr int kMaximumWidth = 180;
    static constexpr auto kWidthSetting = "mixer/channelWidth";
    static constexpr auto kMasterVisibleSetting = "mixer/masterVisible";
    /// Section visibility applies to the whole console to retain alignment.
    /// The legacy COLOR setting name is kept for existing preferences.
    static constexpr auto kColorVisibleSetting = "mixer/colorVisible";

    static MixerPreferences& instance();
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
    void channelWidthChanged(int width);
    void masterVisibleChanged(bool visible);
    void colorVisibleChanged(bool visible);

private:
    explicit MixerPreferences(QObject* parent);
    int m_channelWidth = kDefaultWidth;
    bool m_masterVisible = true;
    bool m_colorVisible = true;
};

} // namespace ui
