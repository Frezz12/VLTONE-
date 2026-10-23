#include "ScriptManagerWindow.hpp"
#include "CreatorStyle.hpp"
#include "ScriptFocusFrame.hpp"
#include "ScriptService.hpp"
#include "ShortcutManager.hpp"
#include "Theme.hpp"
#include <QBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>

namespace ui {
ScriptManagerWindow::ScriptManagerWindow(ScriptService *service, QWidget *parent)
    : QWidget(parent, Qt::Tool), m_service(service) {
    setWindowTitle(tr("Script manager"));
    resize(1000, 700);
    setMinimumSize(760, 520);
    restoreGeometry(QSettings().value("scripts/manager/geometry").toByteArray());
    ScriptFocusFrame::install(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);
    auto *heading = new QLabel(tr("Your extensions"), this);
    heading->setProperty("creatorRole", "heading");
    layout->addWidget(heading);
    auto *intro = new QLabel(tr("Install tools, assign commands and check their activity."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_pages = new QTabWidget(this);
    m_pages->setObjectName("ScriptManager.Pages");
    layout->addWidget(m_pages, 1);
    auto *library = new QWidget(m_pages);
    auto *libraryLayout = new QVBoxLayout(library);
    libraryLayout->setContentsMargins(12, 14, 12, 12);
    libraryLayout->setSpacing(12);
    m_pages->addTab(library, tr("Library"));
    auto *runsPage = new QWidget(m_pages);
    auto *runsLayout = new QVBoxLayout(runsPage);
    runsLayout->setContentsMargins(12, 14, 12, 12);
    m_pages->addTab(runsPage, tr("Activity"));
    auto *help = new QWidget(m_pages);
    auto *helpLayout = new QVBoxLayout(help);
    helpLayout->setContentsMargins(24, 24, 24, 24);
    auto *helpText = new QLabel(
        tr("Getting started\n\n1. Install a .lua file or a .vltscript package.\n2. Select a "
           "command in the library.\n3. Assign a shortcut, context menu or toolbar "
           "button.\n\nProject media is available for reading automatically. Review editing and "
           "external access in Permissions. Activity shows running tools, errors and logs."),
        help);
    helpText->setWordWrap(true);
    helpText->setTextFormat(Qt::PlainText);
    helpLayout->addWidget(helpText);
    auto *guide = new QPushButton(tr("User guide"), help);
    auto *reference = new QPushButton(tr("SDK reference"), help);
    auto *create = new QPushButton(tr("Create in Creator…"), help);
    helpLayout->addWidget(guide);
    helpLayout->addWidget(reference);
    helpLayout->addWidget(create);
    helpLayout->addStretch();
    connect(guide, &QPushButton::clicked, m_service, &ScriptService::showGuide);
    connect(reference, &QPushButton::clicked, m_service, &ScriptService::showSdk);
    connect(create, &QPushButton::clicked, this, [this] { m_service->createScript(); });
    m_pages->addTab(help, tr("Help"));
    auto *buttons = new QHBoxLayout;
    libraryLayout->addLayout(buttons);
    const auto button = [&](icons::Glyph glyph, const QString &label, const char *name,
                            auto callback) {
        auto *control = new IconButton(glyph, label, this);
        control->setObjectName(QString("ScriptManager.") + name);
        control->setButtonSize(32, 32);
        control->setGlyphSize(18);
        control->setFocusPolicy(Qt::StrongFocus);
        control->setAccessibleName(label);
        buttons->addWidget(control);
        m_packageActions.append(control);
        connect(control, &QAbstractButton::clicked, this, callback);
    };
    m_search = new QLineEdit(this);
    m_search->setObjectName("ScriptManager.Search");
    m_search->setPlaceholderText(tr("Search extensions and commands"));
    m_search->setAccessibleName(tr("Search extensions and commands"));
    m_search->setClearButtonEnabled(true);
    buttons->addWidget(m_search, 1);
    auto *install = new QPushButton(tr("Install…"), this);
    install->setProperty("creatorPrimary", true);
    buttons->addWidget(install);
    connect(install, &QPushButton::clicked, this, [this] {
        m_service->install(QFileDialog::getOpenFileName(
            this, tr("Install extension"), {}, "Extensions (*.lua *.vltscript *.vltcreator)"));
    });
    button(icons::Glyph::Reload, tr("Update from file…"), "Update", [this] {
        const auto id = extension();
        if (!id.isEmpty())
            m_service->install(
                QFileDialog::getOpenFileName(this, tr("Update extension"), {},
                                             "Extensions (*.lua *.vltscript *.vltcreator)"),
                id);
    });
    button(icons::Glyph::Export, tr("Export…"), "Export", [this] {
        for (const auto &p : m_service->packages())
            if (p.id() == extension()) {
                const auto path = QFileDialog::getSaveFileName(this, tr("Export extension"),
                                                               p.name() + ".vltscript",
                                                               "VLTONE scripts (*.vltscript)");
                QString error;
                if (!path.isEmpty() && !ScriptLibrary::exportPackage(p, path, error))
                    QMessageBox::warning(this, tr("Export"), error);
            }
    });
    button(icons::Glyph::Edit, tr("Open in Creator"), "Creator", [this] {
        for (const auto &p : m_service->packages())
            if (p.id() == extension())
                m_service->createScript(&p);
    });
    button(icons::Glyph::Trash, tr("Remove"), "Remove", [this] {
        const auto id = extension();
        if (id.isEmpty())
            return;
        if (QMessageBox::question(this, tr("Remove extension"),
                                  tr("Remove this extension from the local library?")) !=
            QMessageBox::Yes)
            return;
        auto preferences = m_service->settings(id);
        preferences["enabled"] = false;
        preferences["autostart"] = false;
        m_service->saveSettings(id, preferences);
        QString error;
        if (!ScriptLibrary::remove(id, error))
            QMessageBox::warning(this, tr("Remove"), error);
        m_service->reload();
    });
    auto *split = new QSplitter(this);
    libraryLayout->addWidget(split, 1);
    m_packages = new QTreeWidget(split);
    m_packages->setHeaderLabels({tr("Extension / command"), tr("Version")});
    m_packages->setMinimumWidth(260);
    m_packages->setAccessibleName(tr("Installed extensions"));
    m_packages->header()->setStretchLastSection(false);
    m_packages->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_packages->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_packages->header()->setStyleSheet(
        "QHeaderView::section { background-color: palette(button); color: palette(button-text); "
        "padding: 4px 6px; border: none; }");
    connect(m_search, &QLineEdit::textChanged, this, &ScriptManagerWindow::filterPackages);
    auto *detailPanel = new QWidget(split);
    auto *detailLayout = new QVBoxLayout(detailPanel);
    detailLayout->setContentsMargins(16, 0, 0, 0);
    m_selectionHeading = new QLabel(detailPanel);
    m_selectionHeading->setTextFormat(Qt::PlainText);
    m_selectionHeading->setWordWrap(true);
    m_selectionHeading->setProperty("creatorRole", "heading");
    m_selectionHint = new QLabel(detailPanel);
    m_selectionHint->setTextFormat(Qt::PlainText);
    m_selectionHint->setWordWrap(true);
    detailLayout->addWidget(m_selectionHeading);
    detailLayout->addWidget(m_selectionHint);
    auto *tabs = new QTabWidget(detailPanel);
    detailLayout->addWidget(tabs, 1);
    auto *scroll = new QScrollArea(tabs);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tabs->addTab(scroll, tr("Command"));
    auto *details = new QWidget(scroll);
    scroll->setWidget(details);
    auto *form = new QFormLayout(details);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *permissionsScroll = new QScrollArea(tabs);
    permissionsScroll->setFrameShape(QFrame::NoFrame);
    permissionsScroll->setWidgetResizable(true);
    permissionsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tabs->addTab(permissionsScroll, tr("Permissions"));
    auto *permissionsPage = new QWidget(permissionsScroll);
    permissionsScroll->setWidget(permissionsPage);
    auto *permissionsForm = new QFormLayout(permissionsPage);
    permissionsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    split->setSizes({340, 620});
    m_enabled = new QCheckBox(tr("Enabled"), details);
    m_autostart = new QCheckBox(tr("Start automatically"), details);
    m_pinned = new QCheckBox(tr("Pin directly in context menus"), details);
    m_shortcut = new QKeySequenceEdit(details);
    m_shortcut->setAccessibleName(tr("Shortcut"));
    m_shortcut->setMaximumSequenceLength(1);
    m_permissions = new QLabel(permissionsPage);
    m_permissions->setWordWrap(true);
    m_permissions->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *general = new QWidget(tabs);
    auto *generalForm = new QFormLayout(general);
    generalForm->setContentsMargins(16, 16, 16, 16);
    generalForm->addRow(m_enabled);
    generalForm->addRow(m_autostart);
    auto *generalHint = new QLabel(tr("Autostart is optional. If a tool fails during startup, it "
                                      "is suspended until you enable it again."),
                                   general);
    generalHint->setWordWrap(true);
    generalForm->addRow(generalHint);
    tabs->insertTab(1, general, tr("Extension"));
    form->addRow(m_pinned);
    form->addRow(tr("Shortcut"), m_shortcut);
    permissionsForm->addRow(m_permissions);
    m_contexts = new QListWidget(details);
    m_contexts->setMinimumHeight(140);
    m_contexts->setMaximumHeight(260);
    m_contexts->setAccessibleName(tr("Context menus"));
    form->addRow(tr("Context menus"), m_contexts);
    connect(m_contexts, &QListWidget::itemChanged, this, [this] {
        if (m_refreshing || command().isEmpty())
            return;
        auto prefs = m_service->settings(extension());
        auto contexts = ScriptService::Json::array();
        for (int i = 0; i < m_contexts->count(); ++i)
            if (m_contexts->item(i)->checkState() == Qt::Checked)
                contexts.push_back(
                    m_contexts->item(i)->data(Qt::UserRole).toString().toStdString());
        prefs["commands"][command().section('/', 1).toStdString()]["contexts"] = contexts;
        m_service->saveSettings(extension(), prefs);
    });
    auto *accessActions = new QHBoxLayout;
    permissionsForm->addRow(tr("Resource access"), accessActions);
    auto *grant = new IconButton(icons::Glyph::Folder, tr("Grant folder…"), permissionsPage);
    accessActions->addWidget(grant);
    m_packageActions.append(grant);
    connect(grant, &QAbstractButton::clicked, this, [this] {
        const auto id = extension();
        if (id.isEmpty())
            return;
        const auto path = QFileDialog::getExistingDirectory(this, tr("Grant folder"));
        if (path.isEmpty())
            return;
        const bool write = QMessageBox::question(
                               this, tr("Folder access"),
                               tr("Allow writing new files in this folder as well as reading?")) ==
                           QMessageBox::Yes;
        auto prefs = m_service->settings(id);
        if (!prefs.contains("paths"))
            prefs["paths"] = ScriptService::Json::array();
        prefs["paths"].push_back({{"path", QFileInfo(path).canonicalFilePath().toStdString()},
                                  {"folder", true},
                                  {"write", write}});
        m_service->saveSettings(id, prefs);
    });
    auto *executable =
        new IconButton(icons::Glyph::Play, tr("Grant external program…"), permissionsPage);
    accessActions->addWidget(executable);
    m_packageActions.append(executable);
    connect(executable, &QAbstractButton::clicked, this, [this] {
        const auto id = extension();
        if (id.isEmpty())
            return;
        const auto path = QFileDialog::getOpenFileName(this, tr("Grant executable"));
        if (path.isEmpty())
            return;
        if (QMessageBox::question(this, tr("External program"),
                                  tr("This program will run with your operating-system account's "
                                     "permissions. Allow this extension to launch it?")) !=
            QMessageBox::Yes)
            return;
        auto prefs = m_service->settings(id);
        if (!prefs.contains("executables"))
            prefs["executables"] = ScriptService::Json::array();
        prefs["executables"].push_back(QFileInfo(path).canonicalFilePath().toStdString());
        m_service->saveSettings(id, prefs);
    });
    auto *revoke = new IconButton(icons::Glyph::Lock, tr("Revoke permissions"), permissionsPage);
    accessActions->addWidget(revoke);
    accessActions->addStretch();
    for (auto *control : {grant, executable, revoke}) {
        control->setButtonSize(32, 32);
        control->setGlyphSize(18);
        control->setFocusPolicy(Qt::StrongFocus);
        control->setAccessibleName(control->toolTip());
    }
    m_packageActions.append(revoke);
    connect(revoke, &QAbstractButton::clicked, this, [this] {
        auto prefs = m_service->settings(extension());
        prefs["permissions"] = ScriptService::Json::array();
        prefs["automaticProjectRead"] = false;
        prefs["paths"] = ScriptService::Json::array();
        prefs["executables"] = ScriptService::Json::array();
        prefs["permissionEpoch"] = prefs.value("permissionEpoch", 0) + 1;
        m_service->saveSettings(extension(), prefs);
        selection();
    });
    auto *approve = new QPushButton(tr("Review requested permissions…"), permissionsPage);
    permissionsForm->addRow(approve);
    m_packageActions.append(approve);
    connect(approve, &QPushButton::clicked, this, [this] {
        for (const auto &package : m_service->packages())
            if (package.id() == extension())
                m_service->approvePermissions(package);
        selection();
    });
    auto *commandActions = new QHBoxLayout;
    auto *run = new QPushButton(tr("Run command"), details);
    run->setObjectName("ScriptManager.Run");
    run->setProperty("creatorPrimary", true);
    m_runButton = run;
    commandActions->addWidget(run);
    commandActions->addStretch();
    connect(run, &QPushButton::clicked, this, [this] {
        m_service->run(command());
        m_pages->setCurrentIndex(1);
    });
    auto *toolbar = new IconButton(icons::Glyph::Gear, tr("Configure script toolbar…"), details);
    toolbar->setButtonSize(32, 32);
    toolbar->setGlyphSize(18);
    toolbar->setFocusPolicy(Qt::StrongFocus);
    toolbar->setAccessibleName(toolbar->toolTip());
    commandActions->addWidget(toolbar);
    form->addRow(commandActions);
    connect(toolbar, &QAbstractButton::clicked, m_service, &ScriptService::configureToolbar);
    auto *actions = new QHBoxLayout;
    runsLayout->addLayout(actions);
    auto *runHeading = new QLabel(tr("Runs and log"), this);
    runHeading->setProperty("creatorRole", "heading");
    actions->addWidget(runHeading, 1);
    m_runs = new QTreeWidget(this);
    m_runs->setHeaderLabels({tr("Run"), tr("Project"), tr("Status")});
    auto *runSplit = new QSplitter(Qt::Vertical, runsPage);
    runsLayout->addWidget(runSplit, 1);
    runSplit->setChildrenCollapsible(false);
    runSplit->addWidget(m_runs);
    m_runs->setMinimumHeight(140);
    m_runs->setAccessibleName(tr("Script runs"));
    m_runs->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_runs->setColumnWidth(0, 330);
    m_runs->setColumnWidth(1, 220);
    m_runs->header()->setStyleSheet(m_packages->header()->styleSheet());
    auto *stop = new IconButton(icons::Glyph::Stop, tr("Stop selected"), this);
    m_stopButton = stop;
    actions->addWidget(stop);
    connect(stop, &QAbstractButton::clicked, this, [this] {
        if (auto *item = m_runs->currentItem())
            m_service->stop(item->data(0, Qt::UserRole).toString());
    });
    auto *error = new IconButton(icons::Glyph::Edit, tr("Go to error in Creator"), this);
    m_errorButton = error;
    actions->addWidget(error);
    connect(error, &QAbstractButton::clicked, this, [this] {
        if (auto *item = m_runs->currentItem()) {
            const auto status = m_service->status(item->data(0, Qt::UserRole).toString());
            if (!status.is_object())
                return;
            const auto command = QString::fromStdString(status.value("command", ""));
            const auto message = QString::fromStdString(status.value("error", ""));
            const QRegularExpression expression("([^\\r\\n:]+\\.lua):(\\d+):");
            const auto match = expression.match(message);
            for (const auto &package : m_service->packages())
                if (package.id() == command.section('/', 0, 0))
                    m_service->createScript(&package, match.captured(1).trimmed(),
                                            match.captured(2).toInt());
        }
    });
    for (auto *control : {stop, error}) {
        control->setButtonSize(32, 32);
        control->setGlyphSize(18);
        control->setFocusPolicy(Qt::StrongFocus);
        control->setAccessibleName(control->toolTip());
    }
    m_log = new QPlainTextEdit(this);
    m_log->setAccessibleName(tr("Script log"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(512);
    m_log->setMinimumHeight(120);
    m_log->setPlaceholderText(tr("Select a run to view its log"));
    runSplit->addWidget(m_log);
    runSplit->setSizes({240, 260});
    connect(m_runs, &QTreeWidget::itemSelectionChanged, this, &ScriptManagerWindow::refreshLog);
    connect(m_packages, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!m_refreshing)
            selection();
    });
    for (auto *box : {m_enabled, m_autostart, m_pinned})
        connect(box, &QCheckBox::toggled, this, [this] {
            if (m_refreshing || extension().isEmpty())
                return;
            auto prefs = m_service->settings(extension());
            if (sender() == m_enabled) {
                prefs["enabled"] = m_enabled->isChecked();
                if (m_enabled->isChecked())
                    prefs["autostartSuspended"] = false;
            }
            if (sender() == m_autostart) {
                prefs["autostart"] = m_autostart->isChecked();
                prefs["autostartSuspended"] = false;
            }
            if (sender() == m_pinned)
                prefs["commands"][command().section('/', 1).toStdString()]["pinned"] =
                    m_pinned->isChecked();
            m_service->saveSettings(extension(), prefs);
            m_service->reload();
        });
    connect(m_shortcut, &QKeySequenceEdit::editingFinished, this, [this] {
        if (m_refreshing || command().isEmpty())
            return;
        auto *shortcuts = m_service->parent()->findChild<ShortcutManager *>();
        if (!shortcuts)
            return;
        const auto id = "script." + command();
        const auto conflict = shortcuts->conflict(id, m_shortcut->keySequence());
        if (!conflict.isEmpty()) {
            QMessageBox::warning(this, tr("Shortcut conflict"), shortcuts->label(conflict));
            selection();
            return;
        }
        shortcuts->setShortcut(id, m_shortcut->keySequence());
    });
    connect(m_service, &ScriptService::changed, this, &ScriptManagerWindow::refresh);
    connect(m_service, &ScriptService::runChanged, this,
            [this](const QString &) { refreshRuns(); });
    styleCreator(this);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            [this] { styleCreator(this); });
    refresh();
}
void ScriptManagerWindow::closeEvent(QCloseEvent *event) {
    QSettings().setValue("scripts/manager/geometry", saveGeometry());
    QWidget::closeEvent(event);
}
QString ScriptManagerWindow::extension() const {
    auto *item = m_packages->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}
QString ScriptManagerWindow::command() const {
    auto *item = m_packages->currentItem();
    return item ? item->data(1, Qt::UserRole).toString() : QString();
}
void ScriptManagerWindow::refresh() {
    if (m_refreshing)
        return;
    m_refreshing = true;
    const auto selected = command(), selectedExtension = extension();
    m_packages->clear();
    for (const auto &package : m_service->packages()) {
        auto *root = new QTreeWidgetItem(
            m_packages,
            {package.name(), QString::fromStdString(package.manifest.value("version", ""))});
        root->setData(0, Qt::UserRole, package.id());
        root->setExpanded(true);
        if (selected.isEmpty() && selectedExtension == package.id())
            m_packages->setCurrentItem(root);
        for (const auto &command : package.manifest["commands"]) {
            auto *item =
                new QTreeWidgetItem(root, {QString::fromStdString(command.value("label", ""))});
            item->setData(0, Qt::UserRole, package.id());
            item->setData(1, Qt::UserRole,
                          package.id() + "/" + QString::fromStdString(command.value("id", "")));
            if (item->data(1, Qt::UserRole).toString() == selected)
                m_packages->setCurrentItem(item);
        }
    }
    if (!m_packages->currentItem() && m_packages->topLevelItemCount()) {
        auto *first = m_packages->topLevelItem(0);
        m_packages->setCurrentItem(first->childCount() ? first->child(0) : first);
    }
    m_refreshing = false;
    filterPackages();
    selection();
    refreshRuns();
}
void ScriptManagerWindow::filterPackages() {
    const auto query = m_search->text().trimmed();
    QTreeWidgetItem *firstVisible = nullptr;
    for (int i = 0; i < m_packages->topLevelItemCount(); ++i) {
        auto *root = m_packages->topLevelItem(i);
        const bool extensionMatches =
            root->text(0).contains(query, Qt::CaseInsensitive) ||
            root->data(0, Qt::UserRole).toString().contains(query, Qt::CaseInsensitive);
        bool visible = extensionMatches;
        for (int j = 0; j < root->childCount(); ++j) {
            auto *command = root->child(j);
            const bool matches =
                extensionMatches || command->text(0).contains(query, Qt::CaseInsensitive);
            command->setHidden(!matches);
            visible |= matches;
            if (matches && !firstVisible)
                firstVisible = command;
        }
        root->setHidden(!visible);
        if (!query.isEmpty() && visible)
            root->setExpanded(true);
    }
    if (auto *current = m_packages->currentItem();
        !current || current->isHidden() || (current->parent() && current->parent()->isHidden()))
        m_packages->setCurrentItem(firstVisible);
}
void ScriptManagerWindow::refreshRuns() {
    QHash<QString, QTreeWidgetItem *> existing;
    for (int i = 0; i < m_runs->topLevelItemCount(); ++i) {
        auto *item = m_runs->topLevelItem(i);
        existing.insert(item->data(0, Qt::UserRole).toString(), item);
    }
    QHash<QString, QString> labels;
    for (const auto &command : m_service->describe())
        labels.insert(QString::fromStdString(command["id"]),
                      QString::fromStdString(command["label"]));
    for (const auto &run : m_service->status({})) {
        const auto id = QString::fromStdString(run.value("id", ""));
        const auto error = QString::fromStdString(run.value("error", ""));
        auto *item = existing.take(id);
        if (!item)
            item = new QTreeWidgetItem(m_runs);
        const auto command = QString::fromStdString(run.value("command", ""));
        item->setText(0, labels.value("script." + command, command));
        item->setToolTip(0, command);
        item->setText(1, QString::fromStdString(run.value("project", "")));
        item->setText(2, error.isEmpty()
                             ? (run.value("complete", false) ? tr("Finished") : tr("Running"))
                             : tr("Error"));
        item->setToolTip(2, error);
        item->setToolTip(2, item->text(2));
        item->setData(0, Qt::UserRole, id);
    }
    for (auto *item : existing)
        delete item;
    refreshLog();
}
void ScriptManagerWindow::refreshLog() {
    m_stopButton->setEnabled(false);
    m_errorButton->setEnabled(false);
    QStringList lines;
    if (auto *item = m_runs->currentItem()) {
        const auto status = m_service->status(item->data(0, Qt::UserRole).toString());
        if (status.is_object()) {
            m_stopButton->setEnabled(!status.value("complete", true));
            const auto command = QString::fromStdString(status.value("command", ""));
            const auto message = QString::fromStdString(status.value("error", ""));
            const bool sourceLocation =
                QRegularExpression("[^\\r\\n:]+\\.lua:\\d+:").match(message).hasMatch();
            const auto packages = m_service->packages();
            const bool installed =
                std::any_of(packages.begin(), packages.end(), [&](const auto &package) {
                    return package.id() == command.section('/', 0, 0);
                });
            m_errorButton->setEnabled(sourceLocation && installed);
            for (const auto &line : status.value("log", ScriptService::Json::array()))
                lines.append(QString::fromStdString(line.get<std::string>()));
        }
    }
    const auto text = lines.join('\n');
    if (m_log->toPlainText() == text)
        return;
    const auto position = m_log->verticalScrollBar()->value();
    const bool atEnd = position == m_log->verticalScrollBar()->maximum();
    m_log->setPlainText(text);
    m_log->verticalScrollBar()->setValue(atEnd ? m_log->verticalScrollBar()->maximum() : position);
}
void ScriptManagerWindow::selection() {
    const auto *selectedItem = m_packages->currentItem();
    m_selectionHeading->setText(selectedItem ? selectedItem->text(0) : tr("Select an extension"));
    m_selectionHint->setText(
        command().isEmpty()
            ? tr("Select a command below an extension to assign its shortcut and menus.")
            : tr("Run this command directly or assign it to your workflow."));
    m_refreshing = true;
    const auto prefs = m_service->settings(extension());
    m_enabled->setChecked(prefs.value("enabled", true));
    m_autostart->setChecked(prefs.value("autostart", false) &&
                            !prefs.value("autostartSuspended", false));
    const auto actions = prefs.value("commands", ScriptService::Json::object());
    const auto key = command().section('/', 1).toStdString();
    m_contexts->clear();
    const QHash<QString, QString> labels = {{"clip", tr("Clips")},
                                            {"track", tr("Tracks")},
                                            {"arrangement", tr("Arrangement")},
                                            {"notes", tr("MIDI notes")},
                                            {"automation", tr("Automation")},
                                            {"mixer", tr("Mixer channels")},
                                            {"device", tr("Devices")},
                                            {"parameter", tr("Parameters")},
                                            {"file", tr("Browser files")}};
    for (const auto &package : m_service->packages())
        if (package.id() == extension())
            for (const auto &action : package.manifest["commands"])
                if (action["id"] == key) {
                    const auto declared = action.value("contexts", ScriptService::Json::array());
                    const auto chosen =
                        actions.contains(key) ? actions[key].value("contexts", declared) : declared;
                    for (const auto &context : declared)
                        if (context != "global") {
                            const auto key = QString::fromStdString(context.get<std::string>());
                            auto *item = new QListWidgetItem(labels.value(key, key), m_contexts);
                            item->setData(Qt::UserRole, key);
                            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                            item->setCheckState(std::find(chosen.begin(), chosen.end(), context) !=
                                                        chosen.end()
                                                    ? Qt::Checked
                                                    : Qt::Unchecked);
                        }
                }
    m_pinned->setChecked(actions.contains(key) && actions[key].value("pinned", false));
    if (auto *shortcuts = m_service->parent()->findChild<ShortcutManager *>())
        m_shortcut->setKeySequence(shortcuts->shortcut("script." + command()));
    m_shortcut->setEnabled(!command().isEmpty());
    m_pinned->setEnabled(!command().isEmpty());
    m_runButton->setEnabled(!command().isEmpty() && prefs.value("enabled", true));
    for (auto *button : m_packageActions)
        button->setEnabled(!extension().isEmpty());
    m_enabled->setEnabled(!extension().isEmpty());
    m_autostart->setEnabled(!extension().isEmpty());
    QStringList permissions, requested;
    for (const auto &permission : prefs.value("permissions", ScriptService::Json::array()))
        permissions.append(ScriptService::permissionLabel(permission.get<std::string>()));
    for (const auto &package : m_service->packages())
        if (package.id() == extension())
            for (const auto &permission :
                 package.manifest.value("permissions", ScriptService::Json::array()))
                requested.append(ScriptService::permissionLabel(permission.get<std::string>()));
    auto summary =
        tr("Requested: %1\nGranted: %2")
            .arg(requested.join(", "), permissions.isEmpty() ? tr("None") : permissions.join(", "));
    if (prefs.value("autostartSuspended", false))
        summary += "\n" + tr("Autostart is paused after a failure. Enable it again to retry.");
    for (const auto &path : prefs.value("paths", ScriptService::Json::array()))
        summary += "\n\n" + tr("%1 — %2").arg(QString::fromStdString(path.value("path", "")),
                                              path.value("write", false) ? tr("Read and write")
                                                                         : tr("Read only"));
    for (const auto &program : prefs.value("executables", ScriptService::Json::array()))
        summary +=
            "\n\n" + tr("Program: %1").arg(QString::fromStdString(program.get<std::string>()));
    if (prefs.value("automaticProjectRead", true))
        summary += tr("\nProject media is available for reading automatically. Original files are "
                      "preserved; new results can use the script's temporary folder.");
    else
        summary += tr("\nAutomatic project access was revoked. Review requested permissions to "
                      "grant access again.");
    m_permissions->setText(summary);
    m_refreshing = false;
}
} // namespace ui
