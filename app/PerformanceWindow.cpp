#include "PerformanceWindow.hpp"
#include "EngineController.hpp"
#include "PlatformDiagnostics.hpp"
#include "Theme.hpp"
#include "graphics/GraphicsPreferences.hpp"

#include <QCheckBox>
#include <QClipboard>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QSet>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <numeric>
#include <cstdio>

namespace {
constexpr int kNumeric = Qt::UserRole + 1;
constexpr int kIssue = Qt::UserRole + 2;
enum Column { Name, Kind, Load, Mean, Peak, Delay, Path, Overruns, State };
QString number(double value, int precision = 2) { return QString::number(value, 'f', precision); }
QString ms(double value) { return number(value) + QStringLiteral(" ms"); }
quint64 delta(quint64 now, quint64 before) { return now >= before ? now - before : 0; }
class Item : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem& other) const override {
        const int column = treeWidget()->sortColumn();
        const auto a = data(column, kNumeric), b = other.data(column, kNumeric);
        if (a.isValid() && b.isValid()) return a.toDouble() < b.toDouble();
        return text(column).localeAwareCompare(other.text(column)) < 0;
    }
};
void value(QTreeWidgetItem* item, int column, const QString& text, double numeric) {
    item->setText(column, text);
    item->setData(column, kNumeric, numeric);
    item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
}
QString format(daw::plugins::Format f) {
    using F = daw::plugins::Format;
    switch (f) {
    case F::Vst: return QStringLiteral("VST");
    case F::Vst3: return QStringLiteral("VST3");
    case F::Clap: return QStringLiteral("CLAP");
    case F::AudioUnit: return QStringLiteral("AU");
    default: return {};
    }
}
}

class PerformanceHistory final : public QWidget {
public:
    explicit PerformanceHistory(QWidget* parent) : QWidget(parent) { setMinimumHeight(64); }
    void add(double value) {
        m_values[m_cursor++ % m_values.size()] = value;
        m_count = std::min(m_count + 1, m_values.size());
        update();
    }
    void clear() { m_cursor = m_count = 0; m_values.fill(0); update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& t = ThemeManager::instance().theme();
        const QRectF area = QRectF(rect()).adjusted(0, 20, -35, -5);
        p.setPen(t.textSecondary);
        p.drawText(QRect(0, 0, width(), 20), PerformanceWindow::tr("Audio DSP · last 60 seconds"));
        double top = 100;
        for (auto v : m_values) top = std::max(top, v);
        p.drawText(QRectF(area.right() + 4, area.top() - 6, 32, 18), number(top, 0));
        p.setPen(t.separator());
        p.drawLine(area.topLeft(), area.topRight());
        p.drawLine(area.bottomLeft(), area.bottomRight());
        QPainterPath line;
        for (std::size_t i = 0; i < m_count; ++i) {
            const auto v = m_values[(m_cursor - m_count + i) % m_values.size()];
            QPointF point(area.right() - double(m_count - 1 - i) * area.width() / (m_values.size() - 1),
                area.bottom() - area.height() * v / top);
            if (!i) line.moveTo(point); else line.lineTo(point);
        }
        p.setPen(QPen(t.accent, 1.5));
        p.drawPath(line);
    }
private:
    std::array<double, 120> m_values{};
    std::size_t m_cursor = 0, m_count = 0;
};

PerformanceWindow::PerformanceWindow(daw::EngineController* controller, QWidget* parent)
    : QDialog(parent), m_controller(controller) {
    setObjectName(QStringLiteral("PerformanceWindow"));
    setWindowTitle(tr("Performance & diagnostics"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(1180, 760);
    setMinimumSize(760, 540);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(tr("Performance & diagnostics"), this);
    title->setObjectName(QStringLiteral("PerformanceTitle"));
    heading->addWidget(title); heading->addStretch();
    m_pause = new QPushButton(tr("Pause display"), this);
    m_pause->setCheckable(true);
    m_pause->setToolTip(tr("Freezes these readings. Audio continues playing."));
    heading->addWidget(m_pause);
    auto* copy = new QPushButton(tr("Copy report"), this); heading->addWidget(copy);
    layout->addLayout(heading);
    auto* summary = new QHBoxLayout;
    for (auto& card : m_cards) {
        card = new QLabel(this);
        card->setObjectName(QStringLiteral("PerformanceCard"));
        card->setTextFormat(Qt::PlainText);
        card->setMinimumHeight(62);
        card->setTextInteractionFlags(Qt::TextSelectableByMouse);
        summary->addWidget(card, 1);
    }
    layout->addLayout(summary);
    m_history = new PerformanceHistory(this);
    m_history->setAccessibleName(tr("Audio DSP history"));
    layout->addWidget(m_history);
    m_banner = new QLabel(this);
    m_banner->setWordWrap(true);
    m_banner->setTextFormat(Qt::PlainText);
    layout->addWidget(m_banner);
    auto* tabs = new QTabWidget(this);
    layout->addWidget(tabs, 1);
    auto* tracks = new QWidget(tabs);
    auto* tracksLayout = new QVBoxLayout(tracks);
    tracksLayout->setContentsMargins(0, 10, 0, 0);
    auto* filters = new QHBoxLayout;
    auto* searchLabel = new QLabel(tr("Find:"), tracks);
    m_search = new QLineEdit(tracks);
    m_search->setClearButtonEnabled(true);
    m_search->setPlaceholderText(tr("Track, plugin or clip"));
    searchLabel->setBuddy(m_search);
    m_search->setAccessibleName(tr("Find a track or plugin"));
    filters->addWidget(searchLabel); filters->addWidget(m_search, 1);
    m_problems = new QCheckBox(tr("Problems only"), tracks);
    m_internal = new QCheckBox(tr("Engine nodes"), tracks);
    filters->addWidget(m_problems); filters->addWidget(m_internal);
    tracksLayout->addLayout(filters);
    m_tree = new QTreeWidget(tracks);
    m_tree->setObjectName(QStringLiteral("PerformanceTree"));
    m_tree->setAccessibleName(tr("Track and plugin measurements"));
    m_tree->setColumnCount(9);
    m_tree->setHeaderLabels({tr("Track / plugin"), tr("Stage"), tr("DSP %"), tr("Mean / block"),
        tr("Peak / block"), tr("Delay"), tr("Path delay"), tr("Over budget"), tr("State")});
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    const int widths[] = {240, 115, 70, 100, 100, 90, 90, 90, 165};
    for (int i = 0; i < 9; ++i) m_tree->setColumnWidth(i, widths[i]);
    m_tree->header()->setSectionsClickable(true);
    m_tree->headerItem()->setToolTip(Load, tr("Processing time divided by one audio block's time budget. Parallel tracks can sum to more than 100%. This is not total system CPU."));
    m_tree->headerItem()->setToolTip(Peak, tr("Maximum observed block time since this processor instance was created. Track peaks are sums of individual maxima, not simultaneous peaks."));
    m_tree->headerItem()->setToolTip(Delay, tr("Algorithmic latency reported by the plugin. Bypass retains delay compensation."));
    m_tree->headerItem()->setToolTip(Path, tr("Cumulative latency arriving at this output, including upstream buses and delay compensation. Parallel paths are not added together."));
    m_tree->headerItem()->setToolTip(Overruns, tr("Number of calls that exceeded an entire audio block's budget. This is separate from device underruns."));
    tracksLayout->addWidget(m_tree, 1);
    auto* explanation = new QLabel(tr("DSP is processing time, not signal delay. Track totals include engine nodes. Click a heading to sort; rows stay in place during updates."), tracks);
    explanation->setWordWrap(true);
    tracksLayout->addWidget(explanation);
    m_detail = new QLabel(tr("Select a plugin to inspect it or change its state."), tracks);
    m_detail->setWordWrap(true);
    m_detail->setTextFormat(Qt::PlainText);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detail->setMinimumHeight(50);
    tracksLayout->addWidget(m_detail);
    auto* actions = new QHBoxLayout;
    m_bypass = new QPushButton(tr("Bypass"), tracks);
    m_restart = new QPushButton(tr("Restart with settings"), tracks);
    m_remove = new QPushButton(tr("Remove plugin…"), tracks);
    actions->addWidget(m_bypass); actions->addWidget(m_restart); actions->addStretch(); actions->addWidget(m_remove);
    tracksLayout->addLayout(actions);
    tabs->addTab(tracks, tr("Tracks & plugins"));
    m_engine = new QTreeWidget(tabs);
    m_engine->setHeaderLabels({tr("Measurement"), tr("Current value")});
    m_engine->setColumnWidth(0, 310);
    m_engine->setRootIsDecorated(false);
    m_engine->setUniformRowHeights(true);
    m_engine->setAlternatingRowColors(true);
    m_engine->setAccessibleName(tr("Audio engine and process statistics"));
    tabs->addTab(m_engine, tr("Audio & system"));
    m_events = new QPlainTextEdit(tabs);
    m_events->setReadOnly(true);
    m_events->setMaximumBlockCount(300);
    m_events->setPlaceholderText(tr("Audio problems and recovery actions appear here. The log is kept for this project while the app is open."));
    m_events->setAccessibleName(tr("Diagnostics event log"));
    tabs->addTab(m_events, tr("Events"));
    auto* footer = new QHBoxLayout;
    auto* settings = new QPushButton(tr("Audio settings…"), this);
    footer->addWidget(settings); footer->addStretch();
    auto* close = new QPushButton(tr("Close"), this);
    footer->addWidget(close); layout->addLayout(footer);
    for (auto* button : findChildren<QPushButton*>()) button->setAutoDefault(false);
    m_timer = new QTimer(this);
    m_timer->setInterval(500);
    connect(m_timer, &QTimer::timeout, this, &PerformanceWindow::refresh);
    connect(m_search, &QLineEdit::textChanged, this, &PerformanceWindow::filterRows);
    connect(m_problems, &QCheckBox::toggled, this, &PerformanceWindow::filterRows);
    connect(m_internal, &QCheckBox::toggled, this, &PerformanceWindow::filterRows);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &PerformanceWindow::updateSelection);
    connect(m_tree->header(), &QHeaderView::sectionClicked, this, [this](int column) {
        const auto order = m_tree->header()->sortIndicatorSection() == column &&
            m_tree->header()->sortIndicatorOrder() == Qt::DescendingOrder ? Qt::AscendingOrder : Qt::DescendingOrder;
        m_tree->header()->setSortIndicatorShown(true);
        m_tree->sortItems(column, order);
    });
    connect(m_pause, &QPushButton::toggled, this, [this](bool paused) {
        m_pause->setText(paused ? tr("Resume display") : tr("Pause display"));
        m_previous.clear(); m_lastPoll = 0;
        if (!paused) m_history->clear();
        refresh(); updateSelection();
    });
    connect(m_bypass, &QPushButton::clicked, this, [this] { requestAction(0); });
    connect(m_restart, &QPushButton::clicked, this, [this] { requestAction(1); });
    connect(m_remove, &QPushButton::clicked, this, [this] { requestAction(2); });
    connect(settings, &QPushButton::clicked, this, &PerformanceWindow::audioSettingsRequested);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(copy, &QPushButton::clicked, this, [this] {
        QString report = windowTitle() + QStringLiteral("\n") + QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("\n");
        for (int i = 0; i < m_engine->topLevelItemCount(); ++i) {
            const auto* item = m_engine->topLevelItem(i);
            report += item->text(0) + QStringLiteral(": ") + item->text(1) + QLatin1Char('\n');
        }
        for (int i = 0; i < m_tree->columnCount(); ++i) report += m_tree->headerItem()->text(i) + QLatin1Char('\t');
        report += QLatin1Char('\n');
        QTreeWidgetItemIterator it(m_tree);
        while (*it) {
            for (int c = 0; c < m_tree->columnCount(); ++c) report += (*it)->text(c) + QLatin1Char('\t');
            report += QLatin1Char('\n'); ++it;
        }
        report += QLatin1Char('\n') + m_events->toPlainText();
        QApplication::clipboard()->setText(report);
    });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &PerformanceWindow::applyTheme);
    applyTheme(); updateSelection();
}

void PerformanceWindow::applyTheme() {
    const auto& t = ThemeManager::instance().theme();
    setStyleSheet(QStringLiteral(
        "QLabel#PerformanceTitle { font-size: 18px; font-weight: 600; }"
        "QLabel#PerformanceCard { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 8px; }"
        "QHeaderView::section { background: %1; color: %2; padding: 6px 4px; border: none; border-right: 1px solid %3; border-bottom: 1px solid %3; }"
        "QTreeWidget::item { min-height: 24px; }"
        "QTreeWidget { selection-color: %4; }")
        .arg(t.surfaceElevated.name(), t.textPrimary.name(), t.separator().name(), t.accentText().name()));
    m_history->update();
}
void PerformanceWindow::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    m_controller->setAudioDiagnosticsEnabled(true);
    m_previous.clear(); m_lastPoll = 0;
    m_timer->start(); refresh();
}
void PerformanceWindow::hideEvent(QHideEvent* e) {
    m_timer->stop();
    m_controller->setAudioDiagnosticsEnabled(false);
    QDialog::hideEvent(e);
}
QString PerformanceWindow::channelName(const std::string& channel) const {
    if (channel == "master") return tr("Master");
    if (const auto* track = m_controller->project().findTrack(channel)) return QString::fromStdString(track->name);
    return tr("Engine / preview");
}
QString PerformanceWindow::kindName(daw::AudioPerformanceRow::Kind kind) const {
    using K = daw::AudioPerformanceRow::Kind;
    switch (kind) {
    case K::Instrument: return tr("Instrument");
    case K::Insert: return tr("Insert FX");
    case K::SamplerFx: return tr("Sampler FX");
    case K::ClipFx: return tr("Clip FX");
    case K::MiniModule: return tr("Module");
    default: return tr("Engine node");
    }
}
QString PerformanceWindow::rowStatus(const daw::AudioPerformanceRow& row, quint64 now) const {
    using H = daw::AudioPluginRuntimeState;
    if (daw::processingUnresponsive(row.timing, now)) return tr("Not responding (>2 s)");
    if (row.kind != daw::AudioPerformanceRow::Kind::Processing) {
        if (row.health.state == H::Missing) return tr("Unavailable");
        if (row.health.state != H::Local) return tr("Processing failed");
    }
    if (!row.inGraph) return tr("Inactive / frozen");
    if (row.bypassed) return tr("Bypassed");
    if (!row.timing.completedAtNs || delta(now, row.timing.completedAtNs) > 2'000'000'000ull) return tr("No recent blocks");
    return tr("Running");
}

void PerformanceWindow::refresh() {
    const auto project = m_controller->projectGeneration();
    if (m_project != project) {
        m_project = project; m_tree->clear(); m_items.clear(); m_channels.clear();
        m_previous.clear(); m_previousStatus.clear(); m_rows.clear(); m_events->clear(); m_history->clear();
        m_lastXruns = m_lastOverruns = m_lastPoll = 0;
        const QSignalBlocker blocked(m_pause);
        m_pause->setChecked(false); m_pause->setText(tr("Pause display"));
    }
    if (m_pause->isChecked()) { updateSelection(); return; }
    m_controller->setAudioDiagnosticsEnabled(isVisible()); // also follows a runtime replacement
    const auto snapshot = m_controller->audioPerformanceSnapshot();
    const auto device = m_controller->audioDeviceDiagnostics();
    const auto callback = m_controller->callbackMetrics().counters;
    const auto graph = m_controller->graphMetrics().counters;
    const auto process = PlatformDiagnostics::processSample();
    const double rate = m_controller->sampleRate();
    const double budget = rate > 0 ? 1000.0 * m_controller->bufferSizeFrames() / rate : 0;
    const double load = device.running ? m_controller->dspLoad() * 100.0 : 0;
    const auto xruns = std::accumulate(device.xruns.begin(), device.xruns.end(), quint64(0));
    m_cards[0]->setText(tr("Audio DSP\n%1%  ·  %2 / block").arg(number(load, 1), ms(budget)));
    m_cards[1]->setText(tr("Graph delay (PDC)\n%1  ·  %2 samples").arg(ms(rate > 0 ? snapshot.totalLatencySamples * 1000.0 / rate : 0)).arg(snapshot.totalLatencySamples));
    m_cards[2]->setText(tr("Process memory\n%1 MiB").arg(number(process.value("resident_bytes").toDouble() / 1048576.0, 1)));
    m_cards[3]->setText(tr("Audio dropouts\n%1  ·  %2 late blocks").arg(xruns).arg(callback.overruns));
    m_history->add(load);
    m_unresponsive = false;
    int problems = 0, plugins = 0;
    QSet<quint64> present;
    struct Total {
        double load = 0, mean = 0, peak = 0;
        quint64 overruns = 0;
        std::uint32_t path = 0;
        int issues = 0;
        bool sampled = false, observed = false, recent = false;
    };
    QHash<QString, Total> totals;
    m_tree->setUpdatesEnabled(false);
    for (const auto& row : snapshot.rows) {
        const bool plugin = row.kind != daw::AudioPerformanceRow::Kind::Processing;
        plugins += plugin;
        const auto key = QString::fromStdString(row.address.channelId);
        auto*& parent = m_channels[key];
        if (!parent) {
            parent = new Item(m_tree);
            parent->setExpanded(true);
            auto bold = parent->font(0); bold.setBold(true); parent->setFont(0, bold);
        }
        parent->setText(Name, channelName(row.address.channelId));
        parent->setText(Kind, tr("Channel total"));
        auto*& item = m_items[row.id];
        if (!item) { item = new Item(parent); item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(row.id)); }
        else if (item->parent() != parent) {
            const bool selected = item == m_tree->currentItem();
            item->parent()->takeChild(item->parent()->indexOfChild(item));
            parent->addChild(item);
            if (selected) m_tree->setCurrentItem(item);
        }
        present.insert(row.id);
        QString label = QString::fromStdString(row.name);
        if (row.address.right) label += tr(" · Right");
        if (!row.clipId.empty()) {
            if (const auto* clip = m_controller->audioClip(row.address.channelId, row.clipId)) label += QStringLiteral(" · ") + QString::fromStdString(clip->name);
        }
        item->setText(Name, label);
        item->setText(Kind, kindName(row.kind) + (format(row.format).isEmpty() ? QString{} : QStringLiteral(" · ") + format(row.format)));
        const auto before = m_previous.value(row.id, row.timing);
        const auto calls = delta(row.timing.calls, before.calls);
        const auto elapsed = delta(row.timing.totalNs, before.totalNs);
        const auto timeBudget = delta(row.timing.budgetNs, before.budgetNs);
        const double percent = row.inGraph && timeBudget && calls ? 100.0 * elapsed / timeBudget : 0;
        const double mean = row.inGraph && calls ? double(elapsed) / calls / 1e6 : 0;
        auto status = rowStatus(row, snapshot.timestampNs);
        const bool stuck = daw::processingUnresponsive(row.timing, snapshot.timestampNs);
        const bool issue = stuck || (plugin && row.health.state != daw::AudioPluginRuntimeState::Local) ||
            delta(row.timing.overruns, before.overruns) > 0;
        if (!stuck && row.health.state == daw::AudioPluginRuntimeState::Local &&
            delta(row.timing.overruns, before.overruns) > 0) status = tr("Over budget");
        if (issue) ++problems;
        m_unresponsive |= stuck;
        item->setData(0, kIssue, issue);
        item->setText(State, status);
        if (plugin && issue && m_previousStatus.value(row.id) != status)
            appendEvent(channelName(row.address.channelId) + QStringLiteral(" / ") + label + QStringLiteral(": ") + status);
        m_previousStatus[row.id] = status;
        value(item, Load, calls ? number(percent, 1) : QStringLiteral("—"), percent);
        value(item, Mean, calls ? ms(mean) : QStringLiteral("—"), mean);
        value(item, Peak, row.timing.calls ? ms(row.timing.peakNs / 1e6) : QStringLiteral("—"), double(row.timing.peakNs));
        value(item, Delay, ms(rate > 0 ? row.latencySamples * 1000.0 / rate : 0), row.latencySamples);
        value(item, Path, row.inGraph ? ms(rate > 0 ? row.pathLatencySamples * 1000.0 / rate : 0) : QStringLiteral("—"), row.pathLatencySamples);
        value(item, Overruns, QString::number(row.timing.overruns), double(row.timing.overruns));
        const QString tooltip = label + QStringLiteral("\n") + status + QStringLiteral("\n") + QString::fromStdString(row.health.detail);
        item->setToolTip(0, tooltip); item->setToolTip(State, tooltip);
        item->setToolTip(Delay, tr("%1 samples").arg(row.latencySamples));
        auto& total = totals[key];
        total.sampled |= row.inGraph && calls > 0;
        total.observed |= row.inGraph && row.timing.calls > 0;
        total.recent |= row.inGraph && row.timing.completedAtNs && delta(snapshot.timestampNs, row.timing.completedAtNs) <= 2'000'000'000ull;
        total.load += percent; total.mean += mean;
        if (row.inGraph) total.peak += row.timing.peakNs / 1e6;
        total.overruns += row.timing.overruns; total.issues += issue;
        if (row.inGraph) total.path = std::max(total.path, row.pathLatencySamples);
        m_previous[row.id] = row.timing; m_rows[row.id] = row;
    }
    for (auto it = m_items.begin(); it != m_items.end();) {
        if (present.contains(it.key())) { ++it; continue; }
        m_previous.remove(it.key()); m_previousStatus.remove(it.key()); m_rows.remove(it.key());
        delete it.value(); it = m_items.erase(it);
    }
    // Empty tracks still appear, including folders with no executable nodes.
    for (const auto& track : m_controller->project().tracks) {
        const auto key = QString::fromStdString(track.id);
        if (!m_channels.contains(key)) m_channels[key] = new Item(m_tree);
        m_channels[key]->setText(0, QString::fromStdString(track.name));
        if (!totals.contains(key)) totals.insert(key, Total{});
    }
    for (auto it = m_channels.begin(); it != m_channels.end();) {
        if (!totals.contains(it.key())) { delete it.value(); it = m_channels.erase(it); continue; }
        const auto total = totals.value(it.key()); auto* item = it.value();
        value(item, Load, total.sampled ? number(total.load, 1) : QStringLiteral("—"), total.load);
        value(item, Mean, total.sampled ? ms(total.mean) : QStringLiteral("—"), total.mean);
        value(item, Peak, total.observed ? ms(total.peak) : QStringLiteral("—"), total.peak * 1e6);
        item->setText(Delay, QStringLiteral("—"));
        value(item, Path, ms(rate > 0 ? total.path * 1000.0 / rate : 0), total.path);
        value(item, Overruns, QString::number(total.overruns), double(total.overruns));
        item->setText(State, total.issues ? tr("%1 problems").arg(total.issues) : total.recent ? tr("OK") : tr("No recent blocks"));
        item->setToolTip(Path, tr("Longest compensated path within this channel, including its upstream routing."));
        ++it;
    }
    m_banner->setText(m_unresponsive
        ? tr("A processor has not returned for over 2 seconds. Restart and removal are unavailable while its call is still running. Native plugins share the app process; a permanent hang requires reopening the app from recovery.")
        : problems ? tr("%1 processors need attention. Select a row for details and recovery actions.").arg(problems)
        : m_controller->offlineRenderInProgress() ? tr("Offline rendering is in progress. Processor timings below cover live audio only; editing is temporarily unavailable.")
        : !device.running ? tr("Audio device is stopped. Start audio to collect live measurements; unavailable values are shown as —.")
        : tr("Live · updated every 500 ms · %1 plugins").arg(plugins));
    const auto stat = [&](const QString& name, const QString& reading) {
        auto*& item = m_engineItems[name];
        if (!item) { item = new QTreeWidgetItem(m_engine); item->setText(0, name); }
        item->setText(1, reading);
        item->setToolTip(0, name); item->setToolTip(1, reading);
    };
    stat(tr("Audio device"), device.running ? tr("Running") : tr("Stopped / unavailable"));
    stat(tr("Sample rate"), number(rate, 0) + QStringLiteral(" Hz"));
    stat(tr("Buffer / deadline"), tr("%1 samples / %2").arg(m_controller->bufferSizeFrames()).arg(ms(budget)));
    stat(tr("Driver input latency"), device.hasStream ? ms(device.inputLatencySeconds * 1000) : QStringLiteral("—"));
    stat(tr("Driver output latency"), device.hasStream ? ms(device.outputLatencySeconds * 1000) : QStringLiteral("—"));
    stat(tr("Graph compensation"), tr("%1 samples / %2").arg(snapshot.totalLatencySamples).arg(ms(rate > 0 ? snapshot.totalLatencySamples * 1000.0 / rate : 0)));
    stat(tr("Estimated monitoring latency"), device.hasStream ? ms((device.inputLatencySeconds + device.outputLatencySeconds) * 1000 + (rate > 0 ? snapshot.totalLatencySamples * 1000.0 / rate : 0)) : QStringLiteral("—"));
    stat(tr("Latency measurement"), tr("Driver reports + graph PDC; not a measured physical round trip."));
    stat(tr("Audio workers / realtime workers"), QStringLiteral("%1 / %2").arg(m_controller->audioWorkerCount()).arg(m_controller->realtimeAudioWorkerCount()));
    stat(tr("Graph nodes / edges"), QStringLiteral("%1 / %2").arg(snapshot.graphNodes).arg(snapshot.graphEdges));
    stat(tr("Graph generation"), QString::number(snapshot.generation));
    stat(tr("Device callbacks / render calls"), QStringLiteral("%1 / %2").arg(device.callbacks).arg(device.renderCalls));
    stat(tr("Callback peak / late blocks"), QStringLiteral("%1 / %2").arg(ms(callback.maximumNs / 1e6)).arg(callback.overruns));
    stat(tr("Graph peak / late blocks"), QStringLiteral("%1 / %2").arg(ms(graph.maximumNs / 1e6)).arg(graph.overruns));
    stat(tr("Failed / gated blocks"), QStringLiteral("%1 / %2").arg(m_controller->failedAudioBlocks()).arg(m_controller->gatedAudioBlocks()));
    stat(tr("Input underflow / overflow"), QStringLiteral("%1 / %2").arg(device.xruns[0]).arg(device.xruns[1]));
    stat(tr("Output underflow / overflow"), QStringLiteral("%1 / %2").arg(device.xruns[2]).arg(device.xruns[3]));
    stat(tr("Last render error"), QString::number(m_controller->lastAudioRenderError()));
    stat(tr("Dropped profiling events"), QString::number(m_controller->droppedAudioProfileEvents()));
    stat(tr("Process CPU (100% = one core)"), number(process.value("process_cpu").toDouble(), 1) + QLatin1Char('%'));
    stat(tr("System CPU"), number(process.value("system_cpu").toDouble(), 1) + QLatin1Char('%'));
    stat(tr("Resident memory"), number(process.value("resident_bytes").toDouble() / 1048576.0, 1) + QStringLiteral(" MiB"));
    stat(tr("Per-plugin memory"), tr("Unavailable: plugins share the process address space."));
    stat(tr("UI polling delay"), m_lastPoll ? ms(std::max(0.0, delta(snapshot.timestampNs, m_lastPoll) / 1e6 - 500.0)) : QStringLiteral("—"));
    const auto& graphics = ui::graphics::GraphicsPreferences::instance();
    const auto frames = graphics.recentFrames();
    double guiMs = -1, gpuMs = -1, presentMs = -1;
    for (const auto& frame : frames) {
        guiMs = std::max(guiMs, frame.preparationMs + frame.synchronizationMs + frame.renderCpuMs);
        gpuMs = std::max(gpuMs, frame.gpuMs);
        presentMs = std::max(presentMs, frame.presentIntervalMs);
    }
    stat(tr("Recently rendering windows"), QString::number(frames.size()));
    stat(tr("UI frame work / GPU frame"), QStringLiteral("%1 / %2")
        .arg(guiMs >= 0 ? ms(guiMs) : QStringLiteral("—"), gpuMs >= 0 ? ms(gpuMs) : QStringLiteral("—")));
    stat(tr("Presentation interval / rate"), presentMs > 0 ? QStringLiteral("%1 / %2 FPS").arg(ms(presentMs), number(1000.0 / presentMs, 1)) : QStringLiteral("—"));
    stat(tr("Graphics timing scope"), tr("Latest frame from each active window; largest value. Idle windows and unavailable GPU timestamps show —."));
    stat(tr("Effective graphics quality"), graphics.level() == ui::graphics::QualityLevel::Maximum ? tr("Maximum") :
        graphics.level() == ui::graphics::QualityLevel::Medium ? tr("Medium") : tr("Low"));
    stat(tr("Transport"), m_controller->isRecording() ? tr("Recording") : m_controller->isPlaying() ? tr("Playing") : tr("Stopped"));
    stat(tr("Offline rendering"), m_controller->offlineRenderInProgress() ? tr("Running") : tr("Inactive"));
    int clips = 0; for (const auto& track : m_controller->project().tracks) clips += int(track.clips.size());
    stat(tr("Tracks / clips / plugins"), QStringLiteral("%1 / %2 / %3").arg(m_controller->project().tracks.size()).arg(clips).arg(plugins));
    stat(tr("Update period"), tr("500 ms; detailed engine timing stops when this window is hidden."));
    if (m_lastPoll && xruns > m_lastXruns) appendEvent(tr("Device dropouts: +%1").arg(xruns - m_lastXruns));
    if (m_lastPoll && callback.overruns > m_lastOverruns) appendEvent(tr("Late audio blocks: +%1").arg(callback.overruns - m_lastOverruns));
    m_lastPoll = snapshot.timestampNs; m_lastXruns = xruns; m_lastOverruns = callback.overruns;
    filterRows(); updateSelection(); m_tree->setUpdatesEnabled(true);
}

void PerformanceWindow::filterRows() {
    const auto query = m_search->text().trimmed();
    for (auto it = m_channels.begin(); it != m_channels.end(); ++it) {
        auto* parent = it.value(); bool visible = parent->childCount() == 0 && !m_problems->isChecked() &&
            (query.isEmpty() || parent->text(0).contains(query, Qt::CaseInsensitive));
        for (int i = 0; i < parent->childCount(); ++i) {
            auto* item = parent->child(i); const auto row = m_rows.value(item->data(0, Qt::UserRole).toULongLong());
            const bool match = (query.isEmpty() || (parent->text(0) + item->text(0)).contains(query, Qt::CaseInsensitive)) &&
                (!m_problems->isChecked() || item->data(0, kIssue).toBool()) &&
                (m_internal->isChecked() || row.kind != daw::AudioPerformanceRow::Kind::Processing);
            item->setHidden(!match); visible |= match;
        }
        // Channels without plugins are still useful in the normal overview.
        if (query.isEmpty() && !m_problems->isChecked()) visible = true;
        parent->setHidden(!visible);
    }
    updateSelection();
}
void PerformanceWindow::updateSelection() {
    auto* selected = m_tree->currentItem();
    const auto found = selected ? m_rows.constFind(selected->data(0, Qt::UserRole).toULongLong()) : m_rows.cend();
    const bool plugin = found != m_rows.cend() && !selected->isHidden() &&
        (!selected->parent() || !selected->parent()->isHidden()) && found->kind != daw::AudioPerformanceRow::Kind::Processing;
    const bool editable = plugin && !m_pause->isChecked() && !m_unresponsive && !m_controller->offlineRenderInProgress() &&
        !m_controller->isRecording() && !m_controller->isCountingIn();
    m_bypass->setEnabled(editable && m_controller->insertModel(found->address.channelId, found->address.slotId));
    m_remove->setEnabled(editable);
    m_restart->setEnabled(editable && found->health.state != daw::AudioPluginRuntimeState::Missing &&
        (found->health.state == daw::AudioPluginRuntimeState::Local || found->health.canRetry));
    m_bypass->setText(plugin && found->bypassed ? tr("Enable") : tr("Bypass"));
    if (!plugin) { m_detail->setText(tr("Select a plugin to inspect it or change its state.")); return; }
    m_detail->setText(tr("%1 · %2\nDelay: %3 samples. Processed blocks: %4. %5")
        .arg(channelName(found->address.channelId), QString::fromStdString(found->name))
        .arg(found->latencySamples).arg(found->timing.calls)
        .arg(QString::fromStdString(found->health.detail)));
    m_restart->setToolTip(found->health.state == daw::AudioPluginRuntimeState::Local
        ? tr("Stops playback and recreates the plugin with its current state. Recording must finish first.")
        : tr("Restores the last successfully saved state; changes after that checkpoint may be lost."));
}
void PerformanceWindow::requestAction(int action) {
    if (m_pause->isChecked() || m_unresponsive || m_project != m_controller->projectGeneration()) return;
    const auto* selected = m_tree->currentItem();
    if (!selected) return;
    const auto found = m_rows.constFind(selected->data(0, Qt::UserRole).toULongLong());
    if (found == m_rows.cend() || found->kind == daw::AudioPerformanceRow::Kind::Processing) return;
    const auto address = found->address;
    emit pluginActionRequested(address, m_project, action);
    refresh();
}
void PerformanceWindow::appendEvent(const QString& text) {
    m_events->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("  ") + text);
}
void PerformanceWindow::recordNotice(const daw::AudioFailureNotice& notice) {
    if (notice.project != m_controller->projectGeneration()) return;
    if (m_project != notice.project) { m_events->clear(); m_project = 0; refresh(); }
    if (notice.state == daw::AudioFailureNotice::State::Retired) return;
    appendEvent(QString::fromStdString(notice.name.empty() ? notice.operation : notice.name) + QStringLiteral(" · ") +
        (notice.state == daw::AudioFailureNotice::State::Recovered ? tr("Recovered") : QString::fromStdString(notice.detail)));
}

bool PerformanceWindow::checkForTest(const QString& shots) {
    daw::EngineController controller;
    if (!controller.initialize(48000, 64, false)) return false;
    const auto track = controller.addTrack(daw::TrackKind::Audio, "Lead vocal / Вокал");
    daw::plugins::PluginDescriptor descriptor;
    descriptor.uid = "daw.equalizer"; descriptor.name = "Diagnostic EQ"; descriptor.format = daw::plugins::Format::Internal;
    const auto slot = controller.addInsert(track, descriptor);
    if (slot.empty()) return false;
    const auto empty = controller.addTrack(daw::TrackKind::Audio, "Empty track / Пустая дорожка");
    PerformanceWindow window(&controller);
    window.show(); QApplication::processEvents();
    audio::AudioBuffer input(2, 64), output(2, 64);
    for (int i = 0; i < 8; ++i) controller.processDeviceBlockForTest(input, output, 64);
    window.refresh();
    bool ok = window.m_timer->isActive() && window.m_rows.size() > 0 && window.m_channels.contains(QString::fromStdString(empty));
    QTreeWidgetItem* selected = nullptr;
    for (auto it = window.m_rows.begin(); it != window.m_rows.end(); ++it)
        if (it->address.slotId == slot) selected = window.m_items.value(it.key());
    if (!selected) return false;
    window.m_tree->setCurrentItem(selected);
    ok &= window.m_restart->isEnabled() && window.m_remove->isEnabled();
    window.m_search->setText(QStringLiteral("Diagnostic EQ"));
    ok &= !selected->isHidden();
    window.m_search->setText(QStringLiteral("No match"));
    ok &= selected->isHidden() && !window.m_remove->isEnabled() && !window.m_restart->isEnabled();
    window.m_search->clear();
    window.m_pause->setChecked(true);
    ok &= !window.m_restart->isEnabled() && !window.m_remove->isEnabled();
    window.m_pause->setChecked(false);
    for (int i = 0; i < 8; ++i) controller.processDeviceBlockForTest(input, output, 64);
    window.refresh();
    ok &= window.m_tree->currentItem() == selected;
    if (!shots.isEmpty()) {
        QDir().mkpath(shots);
        QApplication::processEvents();
        ok &= window.grab().save(QDir(shots).filePath(QStringLiteral("performance.png")));
        window.findChild<QTabWidget*>()->setCurrentIndex(1);
        QApplication::processEvents();
        ok &= window.grab().save(QDir(shots).filePath(QStringLiteral("system.png")));
        window.findChild<QTabWidget*>()->setCurrentIndex(0);
        window.resize(760, 560); QApplication::processEvents();
        ok &= window.grab().save(QDir(shots).filePath(QStringLiteral("compact.png")));
    }
    window.hide();
    ok &= !window.m_timer->isActive();
    window.m_pause->setChecked(true);
    controller.newProject(); window.show(); window.refresh();
    ok &= window.m_project == controller.projectGeneration() && !window.m_channels.contains(QString::fromStdString(track)) && !window.m_pause->isChecked();
    window.hide();
    std::fprintf(stderr, "%s performance window interaction check\n", ok ? "PASS" : "FAIL");
    return ok;
}
