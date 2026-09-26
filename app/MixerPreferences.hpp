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

    static MixerPreferences& instance();
    int channelWidth() const { return m_channelWidth; }
    void setChannelWidth(int width);
    bool masterVisible() const { return m_masterVisible; }
    void setMasterVisible(bool visible);

signals:
    void channelWidthChanged(int width);
    void masterVisibleChanged(bool visible);

private:
    explicit MixerPreferences(QObject* parent);
    int m_channelWidth = kDefaultWidth;
    bool m_masterVisible = true;
};

} // namespace ui
