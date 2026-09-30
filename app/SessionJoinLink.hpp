#pragma once

#include <QRegularExpression>
#include <QString>
#include <QUrl>

namespace collab {

inline bool validSessionJoinCode(const QString& code) {
    // The server currently issues 12 digits. Keep the desktop's bounded
    // compatibility range so changing that width does not break old clients.
    static const QRegularExpression digits(QStringLiteral("\\A[0-9]{6,32}\\z"));
    return digits.match(code).hasMatch();
}

inline QString canonicalSessionJoinLink(const QString& code) {
    return validSessionJoinCode(code) ? QStringLiteral("vlt://join/") + code : QString();
}

inline QString sessionJoinCodeFromLink(const QString& value) {
    if (value.size() > 256 || value != value.trimmed()) return {};
    const QUrl url(value, QUrl::StrictMode);
    if (!url.isValid() || !url.userInfo().isEmpty()) return {};
    QString code;
    if (url.scheme() == QLatin1String("vlt")) {
        if (url.host() != QLatin1String("join") || url.port() != -1 || url.hasQuery() || url.hasFragment()) return {};
        const QString path = url.path();
        if (!path.startsWith(QLatin1Char('/'))) return {};
        code = path.mid(1);
    } else if (url.scheme() == QLatin1String("https") && url.host() == QLatin1String("vltstudio.ru")) {
        if ((url.port() != -1 && url.port() != 443) ||
            (url.path() != QLatin1String("/join") && url.path() != QLatin1String("/ru/join") && url.path() != QLatin1String("/en/join")) ||
            (url.hasQuery() && url.query() != QLatin1String("lang=ru") && url.query() != QLatin1String("lang=en"))) return {};
        code = url.fragment();
    }
    return validSessionJoinCode(code) ? code : QString();
}

inline bool checkSessionJoinLinkForTest() {
    const QString code = QStringLiteral("123456789012");
    for (const QString& value : {QStringLiteral("vlt://join/") + code,
            QStringLiteral("https://vltstudio.ru/en/join#") + code,
            QStringLiteral("https://vltstudio.ru/join?lang=ru#") + code})
        if (sessionJoinCodeFromLink(value) != code) return false;
    for (const QString& value : {QStringLiteral("vlt://join/123"), QStringLiteral("vlt://join/123456?file=x"),
            QStringLiteral("vlt://user@join/123456"), QStringLiteral("vlt://join/123456/other"),
            QStringLiteral("https://attacker.invalid/join#123456"), QStringLiteral("http://vltstudio.ru/join#123456"),
            QStringLiteral("https://vltstudio.ru/login#123456"), QStringLiteral("vlt://join/１２３４５６"),
            QStringLiteral("vlt://join/123456%0A"), QStringLiteral("https://vltstudio.ru/join#123456%0A"),
            QStringLiteral("vlt://join/123456#extra"), QStringLiteral("C:/project.vlt")})
        if (!sessionJoinCodeFromLink(value).isEmpty()) return false;
    return canonicalSessionJoinLink(code) == QStringLiteral("vlt://join/") + code;
}

} // namespace collab
