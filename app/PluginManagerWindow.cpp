#include "PluginManagerWindow.hpp"

#include "Controls.hpp"
#include "EngineController.hpp"
#include "PluginFormatPreference.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using daw::plugins::Format;
using daw::plugins::PluginDescriptor;

namespace {

/// How often the scan state is read while the window is open. The scan itself
/// is a background thread publishing atomics; 100 ms is fast enough that the
/// bar looks continuous and slow enough to cost nothing.
constexpr int kPollMs = 100;

QString formatLabel(Format format) {
    switch (format) {
    case Format::Clap:      return QStringLiteral("CLAP");
    case Format::Vst3:      return QStringLiteral("VST3");
    case Format::Vst:       return QStringLiteral("VST");
    case Format::AudioUnit: return QStringLiteral("Audio Unit");
    case Format::Internal:  return QStringLiteral("Built-in");
    case Format::Unknown:   break;
    }
    return QObject::tr("Unknown");
}

/// The formats this build can actually scan. Driven by the factories rather
/// than by the enum: offering a VST3 path list before the VST3 factory exists
/// would let the user add folders that nothing ever reads.
std::vector<Format> buildFormats() {
    std::vector<Format> formats;
    for (daw::plugins::PluginFactory* factory : daw::plugins::availableFactories()) {
        formats.push_back(factory->format());
    }
    return formats;
}

QTableWidgetItem* readOnlyItem(const QString& text) {
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

} // namespace

PluginManagerWindow::PluginManagerWindow(daw::EngineController* controller,
                                         QWidget* parent)
    : QDialog(parent, Qt::Widget), m_controller(controller) {
    setWindowTitle(tr("Plugin Manager — %1").arg(QApplication::applicationName()));
    resize(880, 580);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // ── Scan header, shared by every tab ──
    auto* header = new QHBoxLayout();
    header->setSpacing(8);
    m_rescanButton = new QPushButton(tr("Scan"), this);
    m_rescanButton->setToolTip(
        tr("Scan the search paths. Files that have not changed since the last "
           "scan are taken from the cache."));
    m_rescanAllButton = new QPushButton(tr("Rescan All"), this);
    m_rescanAllButton->setToolTip(
        tr("Re-open every plugin, ignoring the cache. Slow, but the way to pick "
           "up a plugin that was replaced without its timestamp changing."));
    m_cancelButton = new QPushButton(tr("Cancel"), this);
    connect(m_rescanButton, &QPushButton::clicked, this, [this] { startScan(false); });
    connect(m_rescanAllButton, &QPushButton::clicked, this, [this] { startScan(true); });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        if (m_controller) m_controller->pluginManager().cancelScan();
    });

    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("PluginScanStatus"));
    // The path is the long part; let it elide rather than stretch the window.
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    header->addWidget(m_rescanButton);
    header->addWidget(m_rescanAllButton);
    header->addWidget(m_cancelButton);
    header->addSpacing(8);
    header->addWidget(m_progress, 1);
    root->addLayout(header);
    root->addWidget(m_status);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildPluginsTab(), tr("Plugins"));
    m_tabs->addTab(buildPathsTab(), tr("Search Paths"));
    m_tabs->addTab(buildBlacklistTab(), tr("Blacklist"));
    root->addWidget(m_tabs, 1);

    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this, &PluginManagerWindow::refreshScanState);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &PluginManagerWindow::applyTheme);
    applyTheme();

    refreshPaths();
    refreshPlugins();
    refreshBlacklist();
    refreshScanState();
}

void PluginManagerWindow::showTab(int index) {
    if (m_tabs && index >= 0 && index < m_tabs->count()) m_tabs->setCurrentIndex(index);
}

void PluginManagerWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // Re-read on every open: a scan may have run, or a project load may have
    // touched the cache, since the window was last visible.
    refreshPaths();
    refreshPlugins();
    refreshBlacklist();
    refreshScanState();
    m_poll->start();
}

void PluginManagerWindow::hideEvent(QHideEvent* event) {
    // A scan started here keeps running; only the polling stops. Nothing on
    // screen needs updating while the window is hidden.
    m_poll->stop();
    QDialog::hideEvent(event);
}

// ─────────────────────────────── Plugins ────────────────────────────────

QWidget* PluginManagerWindow::buildPluginsTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* filters = new QHBoxLayout();
    filters->setSpacing(8);
    m_filter = new QLineEdit(page);
    m_filter->setPlaceholderText(tr("Filter by name, vendor or path…"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this,
            &PluginManagerWindow::refreshPlugins);

    m_formatFilter = new QComboBox(page);
    m_formatFilter->addItem(tr("Preferred variants"),
                            static_cast<int>(Format::Unknown));
    for (Format format : buildFormats()) {
        m_formatFilter->addItem(formatLabel(format), static_cast<int>(format));
    }
    connect(m_formatFilter, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshPlugins(); });

    m_typeFilter = new QComboBox(page);
    m_typeFilter->addItem(tr("All types"));
    m_typeFilter->addItem(tr("Effects"));
    m_typeFilter->addItem(tr("Instruments"));
    connect(m_typeFilter, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshPlugins(); });

    filters->addWidget(m_filter, 1);
    filters->addWidget(m_formatFilter);
    filters->addWidget(m_typeFilter);
    layout->addLayout(filters);

    m_pluginTable = new QTableWidget(0, 6, page);
    m_pluginTable->setHorizontalHeaderLabels(
        {tr("Name"), tr("Vendor"), tr("Format"), tr("Type"), tr("Version"), tr("Path")});
    m_pluginTable->verticalHeader()->setVisible(false);
    m_pluginTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pluginTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pluginTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pluginTable->setShowGrid(false);
    m_pluginTable->setSortingEnabled(true);
    // Ascending by name from the start; without this the first click on the
    // header sorts descending and the table opens in that order.
    m_pluginTable->sortByColumn(0, Qt::AscendingOrder);
    m_pluginTable->setAlternatingRowColors(true);
    m_pluginTable->horizontalHeader()->setStretchLastSection(true);
    m_pluginTable->horizontalHeader()->setHighlightSections(false);
    m_pluginTable->setWordWrap(false);
    layout->addWidget(m_pluginTable, 1);

    m_pluginCount = ui::sectionLabel(QString(), page);
    layout->addWidget(m_pluginCount);
    return page;
}

void PluginManagerWindow::refreshPlugins() {
    if (!m_controller || !m_pluginTable) return;
    const auto formatWanted =
        static_cast<Format>(m_formatFilter ? m_formatFilter->currentData().toInt() : 0);
    m_plugins = m_controller->pluginManager().plugins();
    if (formatWanted == Format::Unknown) {
        m_plugins = daw::preferredPluginVariants(std::move(m_plugins),
                                                 ui::preferredPluginFormat());
    }
    std::sort(m_plugins.begin(), m_plugins.end(),
              [](const PluginDescriptor& a, const PluginDescriptor& b) {
                  if (a.name != b.name) return a.name < b.name;
                  return a.uid < b.uid;
              });

    const QString needle = m_filter ? m_filter->text().trimmed() : QString();
    const int typeWanted = m_typeFilter ? m_typeFilter->currentIndex() : 0;

    // Sorting has to be off while rows are filled, or each insertion re-sorts
    // the table under the column being written and the cells scatter.
    m_pluginTable->setSortingEnabled(false);
    m_pluginTable->setRowCount(0);

    int shown = 0;
    for (const PluginDescriptor& descriptor : m_plugins) {
        if (formatWanted != Format::Unknown && descriptor.format != formatWanted) continue;
        if (typeWanted == 1 && descriptor.isInstrument) continue;
        if (typeWanted == 2 && !descriptor.isInstrument) continue;

        const QString name = QString::fromStdString(descriptor.name);
        const QString vendor = QString::fromStdString(descriptor.vendor);
        const QString path = QString::fromStdString(descriptor.path);
        if (!needle.isEmpty() &&
            !name.contains(needle, Qt::CaseInsensitive) &&
            !vendor.contains(needle, Qt::CaseInsensitive) &&
            !path.contains(needle, Qt::CaseInsensitive)) {
            continue;
        }

        const int row = m_pluginTable->rowCount();
        m_pluginTable->insertRow(row);
        m_pluginTable->setItem(row, 0, readOnlyItem(name));
        m_pluginTable->setItem(row, 1, readOnlyItem(vendor));
        m_pluginTable->setItem(row, 2, readOnlyItem(formatLabel(descriptor.format)));
        m_pluginTable->setItem(
            row, 3, readOnlyItem(descriptor.isInstrument ? tr("Instrument") : tr("Effect")));
        m_pluginTable->setItem(row, 4,
                               readOnlyItem(QString::fromStdString(descriptor.version)));
        auto* pathItem = readOnlyItem(path);
        pathItem->setToolTip(path);
        m_pluginTable->setItem(row, 5, pathItem);
        ++shown;
    }
    m_pluginTable->setSortingEnabled(true);
    m_pluginTable->resizeColumnsToContents();

    const int total = static_cast<int>(m_plugins.size());
    if (total == 0) {
        m_pluginCount->setText(
            tr("No plugins yet — check the search paths, then press Scan."));
    } else if (shown == total) {
        // Written out rather than tr("%n …"): with no translator loaded Qt
        // substitutes the count but leaves the "(s)" literal on screen.
        m_pluginCount->setText(total == 1 ? tr("1 plugin")
                                          : tr("%1 plugins").arg(total));
    } else {
        m_pluginCount->setText(tr("%1 of %2 plugins").arg(shown).arg(total));
    }
}

// ───────────────────────────── Search paths ─────────────────────────────

QWidget* PluginManagerWindow::buildPathsTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* top = new QHBoxLayout();
    top->setSpacing(8);
    top->addWidget(ui::sectionLabel(tr("FORMAT"), page));
    m_pathFormat = new QComboBox(page);
    for (Format format : buildFormats()) {
        m_pathFormat->addItem(formatLabel(format), static_cast<int>(format));
    }
    connect(m_pathFormat, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshPaths(); });
    top->addWidget(m_pathFormat);
    top->addStretch(1);
    layout->addLayout(top);

    m_pathList = new QListWidget(page);
    m_pathList->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_pathList, &QListWidget::currentRowChanged, this, [this](int row) {
        m_removePathButton->setEnabled(row >= 0);
    });
    layout->addWidget(m_pathList, 1);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(8);
    auto* add = new QPushButton(tr("Add Folder…"), page);
    m_removePathButton = new QPushButton(tr("Remove"), page);
    m_removePathButton->setEnabled(false);
    auto* defaults = new QPushButton(tr("Restore Defaults"), page);

    connect(add, &QPushButton::clicked, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(
            this, tr("Add plugin folder"), QString());
        if (directory.isEmpty() || !m_controller) return;
        m_controller->pluginManager().addSearchPath(selectedPathFormat(),
                                                    directory.toStdString());
        m_controller->pluginManager().save();
        refreshPaths();
    });
    connect(m_removePathButton, &QPushButton::clicked, this, [this] {
        QListWidgetItem* item = m_pathList->currentItem();
        if (!item || !m_controller) return;
        m_controller->pluginManager().removeSearchPath(selectedPathFormat(),
                                                       item->text().toStdString());
        m_controller->pluginManager().save();
        refreshPaths();
    });
    connect(defaults, &QPushButton::clicked, this, [this] {
        if (!m_controller) return;
        // Every format at once: the defaults come from the factories as a set,
        // and restoring one format from a shared dialog would be a lie.
        m_controller->pluginManager().resetSearchPathsToDefaults();
        m_controller->pluginManager().save();
        refreshPaths();
    });

    buttons->addWidget(add);
    buttons->addWidget(m_removePathButton);
    buttons->addStretch(1);
    buttons->addWidget(defaults);
    layout->addLayout(buttons);

    auto* hint = new QLabel(
        tr("Folders are searched recursively. Changes take effect on the next scan."),
        page);
    hint->setObjectName(QStringLiteral("PluginHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    return page;
}

Format PluginManagerWindow::selectedPathFormat() const {
    if (!m_pathFormat) return Format::Unknown;
    return static_cast<Format>(m_pathFormat->currentData().toInt());
}

void PluginManagerWindow::refreshPaths() {
    if (!m_controller || !m_pathList) return;
    m_pathList->clear();
    for (const std::string& path :
         m_controller->pluginManager().searchPaths(selectedPathFormat())) {
        m_pathList->addItem(QString::fromStdString(path));
    }
    m_removePathButton->setEnabled(m_pathList->currentRow() >= 0);
}

// ────────────────────────────── Blacklist ───────────────────────────────

QWidget* PluginManagerWindow::buildBlacklistTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* hint = new QLabel(
        tr("A plugin that crashed, hung or produced nothing usable during the scan. "
           "It is skipped by later scans, because a plugin that crashes the scanner "
           "would crash VLT Studio Pro."),
        page);
    hint->setObjectName(QStringLiteral("PluginHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_blacklistTable = new QTableWidget(0, 4, page);
    m_blacklistTable->setHorizontalHeaderLabels(
        {tr("Plugin"), tr("Format"), tr("Reason"), tr("Attempts")});
    m_blacklistTable->verticalHeader()->setVisible(false);
    m_blacklistTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_blacklistTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_blacklistTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_blacklistTable->setShowGrid(false);
    m_blacklistTable->setAlternatingRowColors(true);
    m_blacklistTable->horizontalHeader()->setHighlightSections(false);
    // The reason takes the slack, not the attempt count: "Attempts" is two
    // digits, and stretching it would leave the header ending mid-table. Set
    // per column rather than calling resizeColumnsToContents() on refresh,
    // which would put every section back to Interactive.
    QHeaderView* blacklistHeader = m_blacklistTable->horizontalHeader();
    blacklistHeader->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    blacklistHeader->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    blacklistHeader->setSectionResizeMode(2, QHeaderView::Stretch);
    blacklistHeader->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    connect(m_blacklistTable, &QTableWidget::itemSelectionChanged, this, [this] {
        m_unblacklistButton->setEnabled(m_blacklistTable->currentRow() >= 0);
    });
    layout->addWidget(m_blacklistTable, 1);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(8);
    m_unblacklistButton = new QPushButton(tr("Try Again on Next Scan"), page);
    m_unblacklistButton->setEnabled(false);
    m_clearBlacklistButton = new QPushButton(tr("Clear Blacklist"), page);

    connect(m_unblacklistButton, &QPushButton::clicked, this, [this] {
        const int row = m_blacklistTable->currentRow();
        if (row < 0 || row >= static_cast<int>(m_blacklistRows.size()) || !m_controller) {
            return;
        }
        const auto& entry = m_blacklistRows[static_cast<std::size_t>(row)];
        m_controller->pluginManager().unblacklist(entry.format, entry.path);
        m_controller->pluginManager().save();
        refreshBlacklist();
    });
    connect(m_clearBlacklistButton, &QPushButton::clicked, this, [this] {
        if (!m_controller) return;
        if (QMessageBox::question(
                this, tr("Clear blacklist"),
                tr("Every blacklisted plugin will be opened again on the next scan. "
                   "One of them may crash the scanner. Continue?")) !=
            QMessageBox::Yes) {
            return;
        }
        m_controller->pluginManager().clearBlacklist();
        m_controller->pluginManager().save();
        refreshBlacklist();
    });

    buttons->addWidget(m_unblacklistButton);
    buttons->addStretch(1);
    buttons->addWidget(m_clearBlacklistButton);
    layout->addLayout(buttons);
    return page;
}

void PluginManagerWindow::refreshBlacklist() {
    if (!m_controller || !m_blacklistTable) return;
    m_blacklistRows = m_controller->pluginManager().blacklist();
    // The row index addresses `m_blacklistRows`, so this table must not sort.
    m_blacklistTable->setRowCount(0);
    for (const auto& entry : m_blacklistRows) {
        const int row = m_blacklistTable->rowCount();
        m_blacklistTable->insertRow(row);
        const QString path = QString::fromStdString(entry.path);
        auto* nameItem = readOnlyItem(QFileInfo(path).fileName());
        nameItem->setToolTip(path);
        m_blacklistTable->setItem(row, 0, nameItem);
        m_blacklistTable->setItem(row, 1, readOnlyItem(formatLabel(entry.format)));
        m_blacklistTable->setItem(row, 2,
                                  readOnlyItem(QString::fromStdString(entry.reason)));
        m_blacklistTable->setItem(row, 3, readOnlyItem(QString::number(entry.attempts)));
    }
    m_unblacklistButton->setEnabled(false);
    m_clearBlacklistButton->setEnabled(!m_blacklistRows.empty());
}

// ──────────────────────────────── Scan ──────────────────────────────────

void PluginManagerWindow::startScan(bool rescanAll) {
    if (!m_controller) return;
    daw::PluginManager& manager = m_controller->pluginManager();
    if (manager.isScanning()) return;
    manager.startScan(rescanAll);
    m_wasScanning = true;
    refreshScanState();
}

void PluginManagerWindow::refreshScanState() {
    if (!m_controller) return;
    daw::PluginManager& manager = m_controller->pluginManager();
    const bool scanning = manager.isScanning();

    m_rescanButton->setEnabled(!scanning);
    m_rescanAllButton->setEnabled(!scanning);
    m_cancelButton->setEnabled(scanning);

    if (scanning) {
        const std::uint32_t total = manager.scanTotal();
        const std::uint32_t done = manager.scanned();
        // A scan that has not counted its candidates yet gets a busy bar rather
        // than a bar sitting at zero, which reads as "stuck".
        m_progress->setRange(0, total == 0 ? 0 : static_cast<int>(total));
        if (total != 0) m_progress->setValue(static_cast<int>(done));

        const QString path = QString::fromStdString(manager.currentScanPath());
        const QString name = path.isEmpty() ? QString() : QFileInfo(path).fileName();
        m_status->setText(total == 0
                              ? tr("Collecting plugins…")
                              : tr("Scanning %1 of %2 — %3").arg(done).arg(total).arg(name));
        m_status->setToolTip(path);
        m_wasScanning = true;
        return;
    }

    m_progress->setRange(0, 1);
    m_progress->setValue(m_wasScanning ? 1 : 0);

    // `takeScanFinished` is a one-shot flag, so the refresh happens exactly
    // once per scan even though this runs ten times a second.
    if (manager.takeScanFinished()) {
        manager.save();
        refreshPlugins();
        refreshBlacklist();
        emit pluginsChanged();
    }
    if (m_wasScanning) {
        m_wasScanning = false;
        const int found = static_cast<int>(m_plugins.size());
        m_status->setText(found == 1 ? tr("Scan finished — 1 plugin found.")
                                     : tr("Scan finished — %1 plugins found.").arg(found));
        m_status->setToolTip(QString());
    } else if (m_status->text().isEmpty()) {
        m_status->setText(tr("Ready."));
    }
}

// ──────────────────────────────── Theme ─────────────────────────────────

void PluginManagerWindow::applyTheme() {
    const Theme& t = th();
    // Tables, lists, tab bars and the progress bar are not covered by the
    // global stylesheet (Theme.cpp), so without this they fall back to raw
    // Fusion and read as a different application.
    setStyleSheet(QString(R"(
QTabWidget::pane { background: %SURFACE%; border: 1px solid %SEP%;
                   border-radius: 8px; top: -1px; }
QTabBar::tab { background: transparent; color: %TEXT2%; padding: 5px 14px;
               border: 1px solid transparent; border-top-left-radius: 7px;
               border-top-right-radius: 7px; }
QTabBar::tab:hover { color: %TEXT%; }
QTabBar::tab:selected { background: %SURFACE%; color: %TEXT%;
                        border-color: %SEP%; border-bottom-color: %SURFACE%; }

QTableWidget, QListWidget {
    background: %WELL%; border: 1px solid %SEP%; border-radius: 7px;
    alternate-background-color: %ALT%; outline: none;
}
QTableWidget::item, QListWidget::item { padding: 3px 6px; border: none; }
QTableWidget::item:selected, QListWidget::item:selected {
    background: %ACCENT%; color: white;
}
QHeaderView::section {
    background: %ELEV%; color: %TEXT2%; padding: 4px 6px;
    border: none; border-right: 1px solid %SEP%; border-bottom: 1px solid %SEP%;
    font-size: 10px; font-weight: 700;
}
QHeaderView::section:last { border-right: none; }
QTableCornerButton::section { background: %ELEV%; border: none; }

QProgressBar { background: %WELL%; border: none; border-radius: 3px; }
QProgressBar::chunk { background: %ACCENT%; border-radius: 3px; }

#PluginScanStatus, #PluginHint { color: %TEXT2%; font-size: 11px; }
)")
        .replace("%SURFACE%", t.surface.name())
        .replace("%WELL%", t.well().name())
        .replace("%ELEV%", t.surfaceElevated.name())
        .replace("%ALT%", mixColors(t.well(), t.surface, 0.45).name())
        .replace("%SEP%", t.separator().name())
        .replace("%ACCENT%", t.accent.name())
        .replace("%TEXT2%", t.textSecondary.name())
        .replace("%TEXT%", t.textPrimary.name()));
}
