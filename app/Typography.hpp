#pragma once

#include <QString>

class QWebEngineProfile;

namespace ui {

/// Register before QApplication; install a read-only font handler per profile.
void registerFontUrlScheme();
void installFontUrlHandler(QWebEngineProfile* profile);

/// Register the bundled Inter family and set the default before creating UI.
/// User font overrides are applied afterwards by ThemeManager.
void initializeApplicationFonts();

/// Font faces for app-owned WebEngine pages. External websites keep their CSS.
QString bundledFontFaceCss();

/// Verify actual font resolution, including weights, italics and Cyrillic.
bool checkBundledFontsForTest(QString* error);

} // namespace ui
