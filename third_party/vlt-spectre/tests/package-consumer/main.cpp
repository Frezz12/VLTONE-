// SPDX-License-Identifier: MIT
#include <vlt/spectre/Analyzer.hpp>
#ifdef SPECTRE_CONSUME_WIDGETS
#include <vlt/spectre/SpectrumWidget.hpp>
#include <QApplication>
#endif
int main(int argc, char** argv) {
    vlt::spectre::Analyzer analyzer;
    if (!analyzer.prepare(48000)) return 1;
    analyzer.pushStereo(nullptr, nullptr, 8192);
    if (!analyzer.read() || !analyzer.frame().ready) return 2;
#ifdef SPECTRE_CONSUME_WIDGETS
    QApplication app(argc, argv);
    vlt::spectre::SpectrumWidget widget;
    if (!widget.setTrace(QStringLiteral("input"), analyzer.frame())) return 3;
#else
    (void)argc; (void)argv;
#endif
    return 0;
}
