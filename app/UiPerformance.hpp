#pragma once
#include <QElapsedTimer>
namespace ui::perf {
bool enabled();
void sample(const char* name, double value);
void flush();
void reset(); // diagnostic harness: exclude loading/warmup from measured frames
class Scope {
public:
    explicit Scope(const char* name) : m_name(enabled() ? name : nullptr) {
        if (m_name) m_time.start();
    }
    ~Scope() { if (m_name) sample(m_name, double(m_time.nsecsElapsed()) / 1e6); }
private:
    const char* m_name;
    QElapsedTimer m_time;
};
}
