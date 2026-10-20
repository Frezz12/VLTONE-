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
    m_view = QSettings().value(kViewSetting).toString() == QStringLiteral("compact")
        ? View::Compact : View::Classic;
    m_compactWidth = normalizedCompactWidth(
        QSettings().value(kCompactWidthSetting, kDefaultCompactWidth).toInt());
    // Adopt the narrower panel once, then retain any width chosen with its splitter.
    const int initialDetailsWidth =
        QSettings().value("mixer/detailsWidth", 2 * kDefaultDetailsWidth).toInt() / 2;
    m_detailsWidth = std::clamp(QSettings().value(kDetailsWidthSetting, initialDetailsWidth).toInt(),
                              kMinimumDetailsWidth, kMaximumDetailsWidth);
    m_channelWidth = std::clamp(QSettings().value(kWidthSetting, kDefaultWidth).toInt(),
                              kMinimumWidth, kMaximumWidth);
    m_masterVisible = QSettings().value(kMasterVisibleSetting, true).toBool();
    m_colorVisible = QSettings().value(kColorVisibleSetting, true).toBool();
}

int MixerPreferences::normalizedCompactWidth(int width) {
    return std::clamp((std::clamp(width, 24, 48) + 4) / 8 * 8, 24, 48);
}

void MixerPreferences::setView(View view) {
    if (m_view == view) return;
    m_view = view;
    QSettings().setValue(kViewSetting, view == View::Compact ? "compact" : "classic");
    emit viewChanged(view);
}

void MixerPreferences::setCompactWidth(int width) {
    width = normalizedCompactWidth(width);
    if (width == m_compactWidth) return;
    m_compactWidth = width;
    QSettings().setValue(kCompactWidthSetting, width);
    emit compactWidthChanged(width);
}

void MixerPreferences::setDetailsWidth(int width) {
    width = std::clamp(width, kMinimumDetailsWidth, kMaximumDetailsWidth);
    if (width == m_detailsWidth) return;
    m_detailsWidth = width;
    QSettings().setValue(kDetailsWidthSetting, width);
    emit detailsWidthChanged(width);
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
