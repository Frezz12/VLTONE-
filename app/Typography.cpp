#include "Typography.hpp"

#include <QApplication>
#include <QDebug>
#include <QFontDatabase>
#include <QFile>
#include <QRawFont>
#include <QtEndian>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>

#include <array>

namespace ui {
namespace {

struct Face {
    const char* name;
    QFont::Weight weight;
};

constexpr std::array kFaces{
    Face{"Thin", QFont::Thin}, Face{"ExtraLight", QFont::ExtraLight},
    Face{"Light", QFont::Light}, Face{"Regular", QFont::Normal},
    Face{"Medium", QFont::Medium}, Face{"SemiBold", QFont::DemiBold},
    Face{"Bold", QFont::Bold}, Face{"ExtraBold", QFont::ExtraBold},
    Face{"Black", QFont::Black},
};

QString fileName(const Face& face, bool italic) {
    const QString style = italic && face.weight == QFont::Normal
        ? QStringLiteral("Italic")
        : QString::fromLatin1(face.name) + (italic ? QStringLiteral("Italic")
                                                 : QString());
    return QStringLiteral("Inter-%1.ttf").arg(style);
}

class FontUrlHandler final : public QWebEngineUrlSchemeHandler {
public:
    using QWebEngineUrlSchemeHandler::QWebEngineUrlSchemeHandler;

    void requestStarted(QWebEngineUrlRequestJob* job) override {
        const QUrl url = job->requestUrl();
        const QString name = url.path().mid(1);
        // Only public, embedded font assets are served. There is no network
        // access and no path into the user's filesystem through this handler.
        if (job->requestMethod() != "GET" || url.host() != QLatin1String("inter") ||
            !name.startsWith(QLatin1String("Inter-")) || !name.endsWith(QLatin1String(".ttf")) ||
            name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
            job->fail(QWebEngineUrlRequestJob::RequestDenied);
            return;
        }
        auto* file = new QFile(QStringLiteral(":/fonts/") + name, job);
        if (!file->open(QIODevice::ReadOnly)) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        job->reply("font/ttf", file);
    }
};

} // namespace

void registerFontUrlScheme() {
    // qrc: is not CORS enabled, so Chromium rejects font-face loads from the
    // notebook's file: page and the browser's about:blank start page.
    QWebEngineUrlScheme scheme("vlt-font");
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled);
    QWebEngineUrlScheme::registerScheme(scheme);
}

void installFontUrlHandler(QWebEngineProfile* profile) {
    if (profile && !profile->urlSchemeHandler("vlt-font"))
        profile->installUrlSchemeHandler("vlt-font", new FontUrlHandler(profile));
}

void initializeApplicationFonts() {
    // Keep every application font registered for the lifetime of the process.
    // In particular, resetting an imported font must not unregister Inter.
    static const bool registered = [] {
        bool ok = true;
        for (const Face& face : kFaces) {
            for (bool italic : {false, true}) {
                const QString path = QStringLiteral(":/fonts/") + fileName(face, italic);
                if (QFontDatabase::addApplicationFont(path) < 0) {
                    qWarning() << "Could not load bundled font" << path;
                    ok = false;
                }
            }
        }
        return ok;
    }();
    Q_UNUSED(registered);

    QFont font = QApplication::font();
    font.setFamily(QStringLiteral("Inter"));
    font.setStyleName(QString());
    font.setStyle(QFont::StyleNormal);
    font.setWeight(QFont::Normal);
    font.setPixelSize(12);
    QApplication::setFont(font);
}

QString bundledFontFaceCss() {
    static const QString css = [] {
        QString result;
        for (const Face& face : kFaces) {
            for (bool italic : {false, true}) {
                result += QStringLiteral(
                    "@font-face{font-family:'Inter';src:url('vlt-font://inter/%1') "
                    "format('truetype');font-weight:%2;font-style:%3;font-display:swap;}\n")
                    .arg(fileName(face, italic))
                    .arg(int(face.weight))
                    .arg(italic ? QStringLiteral("italic") : QStringLiteral("normal"));
            }
        }
        return result;
    }();
    return css;
}

bool checkBundledFontsForTest(QString* error) {
    for (const Face& face : kFaces) {
        for (bool italic : {false, true}) {
            QFont requested(QStringLiteral("Inter"));
            requested.setPixelSize(12);
            requested.setWeight(face.weight);
            requested.setItalic(italic);
            // CoreText maps the extreme OS/2 weights onto its own scale.
            // Resolve those by their named face; the four UI weights below
            // must also resolve correctly from weight alone, as QSS uses them.
            if (face.weight < QFont::Normal || face.weight > QFont::Bold)
                requested.setStyleName(QString::fromLatin1(face.name) +
                    (italic ? QStringLiteral(" Italic") : QString()));
            const QRawFont actual = QRawFont::fromFont(requested);
            const QByteArray os2 = actual.fontTable("OS/2");
            const int actualWeight = os2.size() >= 6
                ? qFromBigEndian<quint16>(os2.constData() + 4) : 0;
            if (!actual.isValid() || !actual.familyName().startsWith(QLatin1String("Inter")) ||
                actualWeight != face.weight ||
                (actual.style() != QFont::StyleNormal) != italic ||
                !actual.supportsCharacter(QChar(u'A')) ||
                !actual.supportsCharacter(QChar(u'Я')) ||
                !actual.supportsCharacter(QChar(u'ё'))) {
                if (error) *error = QStringLiteral("bundled %1 resolved to %2 / %3 (weight %4)")
                    .arg(fileName(face, italic), actual.familyName(), actual.styleName())
                    .arg(actualWeight);
                return false;
            }
        }
    }
    return true;
}

} // namespace ui
