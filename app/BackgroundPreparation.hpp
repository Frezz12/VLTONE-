#pragma once
#include <QEventLoop>
#include <QProgressDialog>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <exception>
#include <memory>
#include <optional>

namespace ui {
// The worker owns its inputs and result. Application shutdown may end the
// nested loop early; no worker callback holds a QWidget or a stack reference.
// Call only before mutating the live model, never from graph reconciliation.
template<class T, class Work>
std::optional<T> prepareInBackground(QWidget* parent, const QString& label, Work work, bool canCancel = true) {
    struct State {
        std::atomic_bool cancel{false}, done{false};
        std::optional<T> result;
        std::exception_ptr exception;
    };
    auto state = std::make_shared<State>();
    QProgressDialog progress(label, QObject::tr("Cancel"), 0, 0, parent);
    progress.setWindowModality(Qt::ApplicationModal);
    if (!canCancel) progress.setCancelButton(nullptr);
    progress.setMinimumDuration(150);
    progress.setAutoClose(false); progress.setAutoReset(false);
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&progress, &QProgressDialog::canceled, &loop,
                     [state] { state->cancel.store(true, std::memory_order_relaxed); });
    QObject::connect(&poll, &QTimer::timeout, &loop, [state, &loop] {
        if (state->done.load(std::memory_order_acquire)) loop.quit();
    });
    QThreadPool::globalInstance()->start([state, work = std::move(work)]() mutable {
        try { state->result.emplace(work([state] { return !state->cancel.load(std::memory_order_relaxed); })); }
        catch (...) { state->exception = std::current_exception(); }
        state->done.store(true, std::memory_order_release);
    });
    poll.start(10);
    progress.setValue(0);
    loop.exec();
    const bool done = state->done.load(std::memory_order_acquire);
    const bool cancelled = state->cancel.exchange(true, std::memory_order_relaxed);
    if (!done || cancelled) return std::nullopt;
    if (state->exception) std::rethrow_exception(state->exception);
    return std::move(state->result);
}
} // namespace ui
