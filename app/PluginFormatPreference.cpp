#include "PluginFormatPreference.hpp"

#include <QSettings>

namespace ui {

daw::plugins::Format defaultPreferredPluginFormat() noexcept {
#if defined(__APPLE__)
    return daw::plugins::Format::AudioUnit;
#else
    return daw::plugins::Format::Vst3;
#endif
}

QString pluginFormatSettingValue(daw::plugins::Format format) {
    const std::string_view value = daw::plugins::toString(format);
    return QString::fromLatin1(value.data(), int(value.size()));
}

daw::plugins::Format preferredPluginFormat() {
    const QString fallback = pluginFormatSettingValue(defaultPreferredPluginFormat());
    const QByteArray saved =
        QSettings().value(QLatin1String(kPreferredPluginFormatSetting), fallback)
            .toString().toLatin1();
    const daw::plugins::Format format =
        daw::plugins::formatFromString(std::string_view(saved.constData(), saved.size()));
    switch (format) {
        case daw::plugins::Format::Clap:
        case daw::plugins::Format::Vst3:
        case daw::plugins::Format::Vst:
        case daw::plugins::Format::AudioUnit:
            return format;
        case daw::plugins::Format::Internal:
        case daw::plugins::Format::Unknown:
            break;
    }
    return defaultPreferredPluginFormat();
}

} // namespace ui
