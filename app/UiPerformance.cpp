#include "UiPerformance.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

namespace ui::perf {
namespace {
struct Samples { std::array<double, 4096> values{}; size_t count = 0; double maximum = 0; double sum = 0; };
std::map<std::string, Samples>& samples() { static std::map<std::string, Samples> result; return result; }
}
bool enabled() {
    static const bool active = !qEnvironmentVariableIsEmpty("VLT_UI_PROFILE");
    return active;
}
void sample(const char* name, double value) {
    if (!enabled()) return;
    static const bool connected = [] {
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, &flush);
        return true;
    }();
    (void)connected;
    auto& entry = samples()[name];
    entry.values[entry.count++ % entry.values.size()] = value;
    entry.maximum = std::max(entry.maximum, value); entry.sum += value;
}
void flush() {
    if (!enabled()) return;
    QJsonObject report;
    for (const auto& [name, entry] : samples()) {
        const size_t size = std::min(entry.count, entry.values.size());
        if (!size) continue;
        std::vector<double> sorted(entry.values.begin(), entry.values.begin() + size);
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&](double q) { return sorted[std::min(size - 1, size_t(q * size))]; };
        report[QString::fromStdString(name)] = QJsonObject{
            {"count", double(entry.count)}, {"mean", entry.sum / entry.count},
            {"p50", percentile(.50)}, {"p95", percentile(.95)}, {"p99", percentile(.99)},
            {"max", entry.maximum}};
    }
    QSaveFile file(qEnvironmentVariable("VLT_UI_PROFILE"));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(report).toJson()); file.commit();
    }
}
}
