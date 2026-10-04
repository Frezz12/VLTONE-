#include "MixerPreferences.hpp"

#include <QCoreApplication>
#include <QSettings>
#include <algorithm>

namespace ui {

MixerPreferences& MixerPreferences::instance() {
    static auto* preferences = new MixerPreferences(QCoreApplication::instance());
    return *preferences;
}

MixerPreferences::MixerPreferences(QObject* parent) : QObject(parent) {
    m_channelWidth = std::clamp(QSettings().value(kWidthSetting, kDefaultWidth).toInt(),
                              kMinimumWidth, kMaximumWidth);
    m_masterVisible = QSettings().value(kMasterVisibleSetting, true).toBool();
    m_colorVisible = QSettings().value(kColorVisibleSetting, true).toBool();
}

void MixerPreferences::setChannelWidth(int width) {
    width = std::clamp(width, kMinimumWidth, kMaximumWidth);
    if (width == m_channelWidth) return;
    m_channelWidth = width;
    QSettings().setValue(kWidthSetting, width);
    emit channelWidthChanged(width);
}

void MixerPreferences::setMasterVisible(bool visible) {
    if (visible == m_masterVisible) return;
    m_masterVisible = visible;
    QSettings().setValue(kMasterVisibleSetting, visible);
    emit masterVisibleChanged(visible);
}

void MixerPreferences::setColorVisible(bool visible) {
    if (visible == m_colorVisible) return;
    m_colorVisible = visible;
    QSettings().setValue(kColorVisibleSetting, visible);
    emit colorVisibleChanged(visible);
}

} // namespace ui
