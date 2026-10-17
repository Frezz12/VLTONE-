#pragma once
#include <QThreadPool>

namespace ui {
// Interactive previews must not wait behind project analysis or export jobs.
// Audio and MIDI share a small pool; each loader coalesces pending requests.
inline QThreadPool& previewThreadPool() {
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(2); return true; }();
    (void)configured;
    return pool;
}
}
