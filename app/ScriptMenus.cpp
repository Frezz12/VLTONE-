#include "CreatorStyle.hpp"
#include "LuaCreatorWindow.hpp"
#include "ScriptFocusFrame.hpp"
#include "ScriptManagerWindow.hpp"
#include "ScriptRun.hpp"
#include "ScriptService.hpp"
#include "ShortcutManager.hpp"
#include "Theme.hpp"
#include "scripting/ScriptProject.hpp"
#include <QBoxLayout>
#include <QBuffer>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>

namespace ui {
namespace {
class ScriptToolBar final : public QToolBar {
    QSize withThickness(QSize size) const {
        if (orientation() == Qt::Horizontal)
            size.setHeight(std::max(44, size.height()));
        else
            size.setWidth(std::max(44, size.width()));
        return size;
    }

  public:
    using QToolBar::QToolBar;
    QSize sizeHint() const override { return withThickness(QToolBar::sizeHint()); }
    QSize minimumSizeHint() const override { return withThickness(QToolBar::minimumSizeHint()); }
};
vlt::script::Json toolbarEntries() {
    using Json = vlt::script::Json;
    Json result = Json::array();
    try {
        const auto text = QSettings().value("scripts/toolbar/buttons", "[]").toString();
        if (text.size() > 4 * 1024 * 1024)
            return result;
        const auto entries = Json::parse(text.toStdString());
        if (!entries.is_array())
            return result;
        for (const auto &entry : entries) {
            if (result.size() >= 256)
                break;
            if (!entry.is_object())
                continue;
            if (entry.contains("separator") && !entry["separator"].is_boolean())
                continue;
            if (entry.value("separator", false)) {
                result.push_back({{"separator", true}});
                continue;
            }
            if (!entry.contains("command") || !entry["command"].is_string() ||
                (entry.contains("label") && !entry["label"].is_string()) ||
                (entry.contains("icon") && !entry["icon"].is_string()) ||
                (entry.contains("appearance") &&
                 (entry["appearance"] != "text" && entry["appearance"] != "image" &&
                  entry["appearance"] != "both")) ||
                (entry.contains("arguments") &&
                 (!entry["arguments"].is_object() || entry["arguments"].dump().size() > 65536)))
                continue;
            result.push_back(entry);
        }
    } catch (const std::exception &) {
        // A damaged preference must not prevent the DAW or the editor from opening.
    }
    return result;
}
Qt::ToolButtonStyle buttonStyle(const QString &appearance, bool hasImage) {
    if (appearance == "text" || !hasImage)
        return Qt::ToolButtonTextOnly;
    return appearance == "image" ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon;
}
QImage buttonImage(const QString &path) {
    if (path.isEmpty() || QFileInfo(path).size() > 8 * 1024 * 1024)
        return {};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 64 * 1024 * 1024)
        return {};
    reader.setScaledSize(size.scaled(256, 256, Qt::KeepAspectRatio));
    return reader.read();
}
QString storeButtonImage(const QString &path) {
    const auto image = buttonImage(path);
    if (image.isNull())
        throw std::runtime_error("Cannot read button image (maximum 8 MiB / 64 megapixels)");
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        throw std::runtime_error("Cannot prepare button image");
    const auto folder = ScriptLibrary::directory() + "/.toolbar-images";
    const auto name =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const auto destination = folder + "/" + name + ".png";
    if (!QFileInfo::exists(destination)) {
        if (!QDir().mkpath(folder))
            throw std::runtime_error("Cannot create toolbar image folder");
        QSaveFile file(destination);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw std::runtime_error("Cannot save button image");
    }
    return destination;
}

// Own the draft and callbacks for the entire modeless window lifetime.
class ScriptToolbarEditor final : public QDialog {
    using Json = vlt::script::Json;
    Json m_commands;
    std::function<void(const Json &)> m_apply;
    QListWidget *m_list;
    QLineEdit *m_label, *m_image;
    QComboBox *m_appearance;
    QPlainTextEdit *m_arguments;
    QToolButton *m_preview;
    QLabel *m_hint, *m_command, *m_validation;
    QTabWidget *m_tabs;
    IconButton *m_up, *m_down, *m_remove;
    bool m_loading = false, m_dirty = false;

    static QString tr(const char *text) {
        return QCoreApplication::translate("ui::ScriptService", text);
    }
    IconButton *tool(QHBoxLayout *row, icons::Glyph glyph, const QString &label,
                     std::function<void()> action) {
        auto *button = new IconButton(glyph, label, this);
        button->setButtonSize(32, 32);
        button->setGlyphSize(18);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setAccessibleName(label);
        row->addWidget(button);
        connect(button, &QAbstractButton::clicked, this, std::move(action));
        return button;
    }
    void append(const Json &entry) {
        if (m_list->count() >= 256) {
            m_validation->setText(tr("A toolbar supports up to 256 entries."));
            return;
        }
        const auto text =
            entry.value("separator", false)
                ? tr("Separator")
                : QString::fromStdString(entry.value("label", entry.value("command", "")));
        auto *item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, QString::fromStdString(entry.dump()));
        item->setData(Qt::UserRole + 1,
                      QString::fromStdString(entry.value("arguments", Json::object()).dump(2)));
        m_list->setCurrentItem(item);
        m_dirty = true;
    }
    void addButton() {
        QStringList labels, ids, captions;
        for (const auto &command : m_commands) {
            const auto id = QString::fromStdString(command["id"]);
            const auto caption = QString::fromStdString(command["label"]);
            auto label = QString::fromStdString(command["extension"]) + " — " + caption;
            if (labels.contains(label))
                label += " (" + id + ")";
            labels.append(label);
            ids.append(id);
            captions.append(caption);
        }
        if (labels.isEmpty()) {
            m_validation->setText(tr("Install an extension with commands first."));
            return;
        }
        bool ok = false;
        const auto choice =
            QInputDialog::getItem(this, tr("Add button"), tr("Command"), labels, 0, false, &ok);
        const auto index = labels.indexOf(choice);
        if (ok && index >= 0)
            append(
                {{"command", ids[index].toStdString()}, {"label", captions[index].toStdString()}});
    }
    void move(int delta) {
        const auto row = m_list->currentRow(), target = row + delta;
        if (row < 0 || target < 0 || target >= m_list->count())
            return;
        const QSignalBlocker blocker(m_list);
        auto *item = m_list->takeItem(row);
        m_list->insertItem(target, item);
        m_list->setCurrentItem(item);
        m_dirty = true;
        load();
    }
    void preview() {
        const auto image = buttonImage(m_image->text());
        m_preview->setIcon(QIcon(QPixmap::fromImage(image)));
        m_preview->setText(m_label->text().isEmpty() ? tr("Button") : m_label->text());
        m_preview->setToolButtonStyle(
            buttonStyle(m_appearance->currentData().toString(), !image.isNull()));
    }
    void load() {
        m_loading = true;
        auto *item = m_list->currentItem();
        const auto value =
            item ? Json::parse(item->data(Qt::UserRole).toString().toStdString()) : Json::object();
        const bool editable = item && !value.value("separator", false);
        m_tabs->setVisible(editable);
        m_command->setVisible(editable);
        m_hint->setVisible(!editable);
        m_hint->setText(item ? tr("This separator creates space between groups of buttons.")
                             : tr("Add a command, then choose its label and image. Changes are "
                                  "applied when you save."));
        const auto commandId = value.value("command", "");
        QString commandLabel = tr("Command is not installed");
        for (const auto &command : m_commands)
            if (command["id"] == commandId) {
                commandLabel = QString::fromStdString(command["extension"]) + " — " +
                               QString::fromStdString(command["label"]);
                break;
            }
        m_command->setText(commandLabel);
        m_command->setToolTip(QString::fromStdString(commandId));
        m_label->setText(QString::fromStdString(value.value("label", "")));
        m_image->setText(QString::fromStdString(value.value("icon", "")));
        m_appearance->setCurrentIndex(
            m_appearance->findData(QString::fromStdString(value.value("appearance", "both"))));
        m_arguments->setPlainText(item ? item->data(Qt::UserRole + 1).toString() : QString());
        m_up->setEnabled(item && m_list->currentRow() > 0);
        m_down->setEnabled(item && m_list->currentRow() + 1 < m_list->count());
        m_remove->setEnabled(item);
        preview();
        m_loading = false;
    }
    void store() {
        if (m_loading || !m_list->currentItem())
            return;
        auto *item = m_list->currentItem();
        auto value = Json::parse(item->data(Qt::UserRole).toString().toStdString());
        if (value.value("separator", false))
            return;
        value["label"] = m_label->text().toStdString();
        value["icon"] = m_image->text().toStdString();
        value["appearance"] = m_appearance->currentData().toString().toStdString();
        item->setData(Qt::UserRole, QString::fromStdString(value.dump()));
        item->setData(Qt::UserRole + 1, m_arguments->toPlainText());
        item->setText(m_label->text().isEmpty() ? tr("Button") : m_label->text());
        m_validation->clear();
        m_dirty = true;
        preview();
    }
    void save() {
        Json entries = Json::array();
        for (int i = 0; i < m_list->count(); ++i) {
            auto *item = m_list->item(i);
            auto value = Json::parse(item->data(Qt::UserRole).toString().toStdString());
            if (!value.value("separator", false)) {
                try {
                    const auto text = item->data(Qt::UserRole + 1).toString().toStdString();
                    if (text.size() > 65536)
                        throw std::runtime_error("Arguments exceed 64 KiB");
                    const auto args = Json::parse(text);
                    if (!args.is_object())
                        throw std::runtime_error("Expected a JSON object");
                    value["arguments"] = args;
                } catch (const std::exception &e) {
                    m_list->setCurrentItem(item);
                    m_tabs->setCurrentIndex(1);
                    m_validation->setText(tr("Arguments: %1").arg(QString::fromUtf8(e.what())));
                    m_arguments->setFocus();
                    return;
                }
                const auto image = QString::fromStdString(value.value("icon", ""));
                if (!image.isEmpty()) {
                    try {
                        value["icon"] = storeButtonImage(image).toStdString();
                    } catch (const std::exception &e) {
                        m_list->setCurrentItem(item);
                        m_tabs->setCurrentIndex(0);
                        m_validation->setText(tr("Image: %1").arg(QString::fromUtf8(e.what())));
                        m_image->setFocus();
                        return;
                    }
                }
                if (value.value("label", "").empty() && value.value("icon", "").empty()) {
                    m_list->setCurrentItem(item);
                    m_tabs->setCurrentIndex(0);
                    m_validation->setText(tr("Add a label or choose an image for the button."));
                    m_label->setFocus();
                    return;
                }
            }
            entries.push_back(std::move(value));
        }
        // Match the reader's preference limit; never save a draft that will be discarded on
        // restart.
        if (entries.dump().size() > 4 * 1024 * 1024) {
            m_validation->setText(tr("Toolbar settings exceed 4 MiB. Reduce fixed arguments."));
            return;
        }
        m_apply(entries);
        m_dirty = false;
        accept();
    }
    void reject() override {
        if (m_dirty) {
            const auto answer = QMessageBox::question(
                this, tr("Unsaved toolbar changes"),
                tr("Save your toolbar changes before closing?"),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
            if (answer == QMessageBox::Cancel)
                return;
            if (answer == QMessageBox::Save) {
                save();
                return;
            }
        }
        QDialog::reject();
    }

  public:
    ScriptToolbarEditor(const Json &entries, Json commands, std::function<void(const Json &)> apply,
                        QWidget *parent)
        : QDialog(parent, Qt::Tool), m_commands(std::move(commands)), m_apply(std::move(apply)) {
        setAttribute(Qt::WA_DeleteOnClose);
        setObjectName("ScriptToolbar.Configure");
        setWindowTitle(tr("Script toolbar"));
        resize(900, 580);
        setMinimumSize(740, 480);
        restoreGeometry(QSettings().value("scripts/toolbar/editorGeometry").toByteArray());
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(20, 20, 20, 20);
        layout->setSpacing(14);
        auto *title = new QLabel(tr("Make the toolbar yours"), this);
        title->setProperty("creatorRole", "heading");
        layout->addWidget(title);
        auto *intro =
            new QLabel(tr("Arrange commands on the left. Style each button on the right."), this);
        intro->setWordWrap(true);
        layout->addWidget(intro);
        auto *split = new QSplitter(this);
        split->setChildrenCollapsible(false);
        layout->addWidget(split, 1);
        auto *left = new QWidget(split);
        auto *leftLayout = new QVBoxLayout(left);
        leftLayout->setContentsMargins(0, 0, 12, 0);
        m_list = new QListWidget(left);
        m_list->setObjectName("ScriptToolbar.Buttons");
        m_list->setAccessibleName(tr("Toolbar buttons"));
        m_list->setDragDropMode(QAbstractItemView::InternalMove);
        leftLayout->addWidget(m_list, 1);
        auto *actions = new QHBoxLayout;
        leftLayout->addLayout(actions);
        auto *add = new QPushButton(tr("Add…"), left);
        actions->addWidget(add);
        connect(add, &QPushButton::clicked, this, [this] { addButton(); });
        tool(actions, icons::Glyph::Minus, tr("Separator"),
             [this] { append({{"separator", true}}); });
        actions->addStretch();
        m_up = tool(actions, icons::Glyph::ArrowUp, tr("Up"), [this] { move(-1); });
        m_down = tool(actions, icons::Glyph::ArrowDown, tr("Down"), [this] { move(1); });
        m_remove = tool(actions, icons::Glyph::Trash, tr("Remove"), [this] {
            delete m_list->takeItem(m_list->currentRow());
            m_dirty = true;
            load();
        });
        auto *right = new QWidget(split);
        auto *rightLayout = new QVBoxLayout(right);
        rightLayout->setContentsMargins(12, 0, 0, 0);
        m_hint = new QLabel(right);
        m_hint->setWordWrap(true);
        rightLayout->addWidget(m_hint);
        m_command = new QLabel(right);
        m_command->setWordWrap(true);
        m_command->setTextFormat(Qt::PlainText);
        m_command->setTextInteractionFlags(Qt::TextSelectableByMouse);
        rightLayout->addWidget(m_command);
        m_tabs = new QTabWidget(right);
        rightLayout->addWidget(m_tabs, 1);
        auto *style = new QWidget(m_tabs);
        auto *form = new QFormLayout(style);
        form->setContentsMargins(16, 16, 16, 16);
        form->setSpacing(14);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        m_tabs->addTab(style, tr("Appearance"));
        m_preview = new QToolButton(style);
        m_preview->setObjectName("ScriptToolbar.Preview");
        m_preview->setAccessibleName(tr("Button preview"));
        m_preview->setIconSize(QSize(24, 24));
        m_preview->setMinimumHeight(40);
        m_preview->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        form->addRow(tr("Preview"), m_preview);
        m_label = new QLineEdit(style);
        m_label->setObjectName("ScriptToolbar.Label");
        form->addRow(tr("Custom button label"), m_label);
        m_appearance = new QComboBox(style);
        m_appearance->setObjectName("ScriptToolbar.Appearance");
        m_appearance->addItem(tr("Text"), "text");
        m_appearance->addItem(tr("Image only"), "image");
        m_appearance->addItem(tr("Image and text"), "both");
        form->addRow(tr("Button appearance"), m_appearance);
        m_image = new QLineEdit(style);
        m_image->setObjectName("ScriptToolbar.Image");
        m_image->setAccessibleName(tr("Image or icon"));
        auto *imageRow = new QHBoxLayout;
        imageRow->addWidget(m_image, 1);
        tool(imageRow, icons::Glyph::Folder, tr("Choose image…"), [this] {
            const auto path = QFileDialog::getOpenFileName(
                this, tr("Choose image…"), {},
                "Images (*.svg *.png *.jpg *.jpeg *.webp *.bmp *.ico)");
            if (!path.isEmpty())
                m_image->setText(path);
        });
        tool(imageRow, icons::Glyph::Trash, tr("Remove image"), [this] { m_image->clear(); });
        form->addRow(tr("Image or icon"), imageRow);
        auto *advanced = new QWidget(m_tabs);
        auto *advancedLayout = new QVBoxLayout(advanced);
        auto *argumentHint = new QLabel(tr("Optional values passed to this button's command. Leave "
                                           "{} if the script does not require arguments."),
                                        advanced);
        argumentHint->setWordWrap(true);
        advancedLayout->addWidget(argumentHint);
        m_arguments = new QPlainTextEdit(advanced);
        m_arguments->setObjectName("ScriptToolbar.Arguments");
        m_arguments->setAccessibleName(tr("Fixed arguments as a JSON object"));
        advancedLayout->addWidget(m_arguments);
        m_tabs->addTab(advanced, tr("Arguments"));
        m_validation = new QLabel(this);
        m_validation->setWordWrap(true);
        m_validation->setTextFormat(Qt::PlainText);
        layout->addWidget(m_validation);
        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Save)->setProperty("creatorPrimary", true);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { save(); });
        connect(buttons, &QDialogButtonBox::rejected, this, [this] { reject(); });
        connect(m_list, &QListWidget::currentItemChanged, this, [this] { load(); });
        connect(m_list->model(), &QAbstractItemModel::rowsMoved, this, [this] {
            m_dirty = true;
            load();
        });
        connect(m_label, &QLineEdit::textChanged, this, [this] { store(); });
        connect(m_image, &QLineEdit::textChanged, this, [this] { store(); });
        connect(m_appearance, &QComboBox::currentIndexChanged, this, [this] { store(); });
        connect(m_arguments, &QPlainTextEdit::textChanged, this, [this] { store(); });
        for (const auto &entry : entries)
            append(entry);
        m_list->setCurrentRow(0);
        load();
        m_dirty = false;
        split->setSizes({330, 530});
        styleCreator(this);
        ScriptFocusFrame::install(this);
        connect(&ThemeManager::instance(), &ThemeManager::changed, this,
                [this] { styleCreator(this); });
    }
    ~ScriptToolbarEditor() override {
        QSettings().setValue("scripts/toolbar/editorGeometry", saveGeometry());
    }
};
} // namespace
QString ScriptService::permissionLabel(const std::string &permission) {
    if (permission == "project.read")
        return tr("Read project");
    if (permission == "project.write")
        return tr("Edit project and transport");
    if (permission == "files.read")
        return tr("Read project and chosen files");
    if (permission == "files.write")
        return tr("Write chosen files");
    if (permission == "network")
        return tr("Access the network");
    if (permission == "process")
        return tr("Launch granted programs");
    return QString::fromStdString(permission);
}
bool ScriptService::approvePermissions(const ScriptPackage &package, bool interactive) {
    auto prefs = settings(package.id());
    auto granted = prefs.value("permissions", Json::array());
    const auto requested = package.manifest.value("permissions", Json::array());
    // Read access is automatic; edits, output writes and external capabilities
    // still require approval. Actual paths stay scoped by permittedPath().
    if (prefs.value("automaticProjectRead", true)) {
        bool changed = false;
        for (const auto &permission : requested)
            if ((permission == "project.read" || permission == "files.read") &&
                std::find(granted.begin(), granted.end(), permission) == granted.end()) {
                granted.push_back(permission);
                changed = true;
            }
        if (changed) {
            prefs["permissions"] = granted;
            saveSettings(package.id(), prefs);
        }
    }
    QStringList added;
    for (const auto &permission : requested)
        if (std::find(granted.begin(), granted.end(), permission) == granted.end())
            added.append(QString::fromStdString(permission.get<std::string>()));
    if (added.isEmpty())
        return true;
    if (!interactive)
        return false;
    QStringList descriptions;
    for (const auto &permission : added)
        descriptions.append(permissionLabel(permission.toStdString()));
    QString explanation = tr("%1 requests:\n%2\n\nLinked project files are available automatically "
                             "for reading. Other files require a selection or a folder grant.")
                              .arg(package.name(), descriptions.join('\n'));
    if (added.contains("process"))
        explanation += tr("\nExternal programs run with your operating-system account's "
                          "permissions. Each executable must also be granted in the manager.");
    if (QMessageBox::question(m_window, tr("Extension permissions"), explanation,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return false;
    prefs["permissions"] = requested;
    saveSettings(package.id(), prefs);
    return true;
}
QString ScriptService::install(const QString &path, const QString &expectedId) {
    if (path.isEmpty())
        return {};
    ScriptPackage package;
    QString error;
    if (!ScriptLibrary::install(path, package, error, expectedId)) {
        QMessageBox::warning(m_window, tr("Install extension"), error);
        return {};
    }
    for (const auto &run : m_runs)
        if (run->package.id() == package.id() && !run->complete)
            finish(run.get(), tr("Extension updated"));
    auto prefs = settings(package.id());
    if (!prefs.contains("enabled"))
        prefs["enabled"] = true;
    if (!prefs.contains("autostart"))
        prefs["autostart"] = false;
    saveSettings(package.id(), prefs);
    reload();
    return package.id();
}
void ScriptService::reload() {
    for (const auto &id : m_actions.keys()) {
        m_shortcuts->unregisterCommand(id);
        m_window->removeAction(m_actions[id]);
        delete m_actions[id];
    }
    m_actions.clear();
    m_packages = ScriptLibrary::installed();
    for (const auto &run : m_runs)
        if (!run->complete && !run->preview && !run->package.directory.isEmpty()) {
            const auto installed =
                std::find_if(m_packages.begin(), m_packages.end(),
                             [&](const auto &p) { return p.id() == run->package.id(); });
            if (installed == m_packages.end() || installed->manifest != run->package.manifest ||
                installed->sources != run->package.sources ||
                installed->resources != run->package.resources) {
                run->autostart = false;
                finish(run.get(), tr("Extension updated"));
            }
        }
    for (const auto &package : m_packages) {
        const auto prefs = settings(package.id());
        for (const auto &command : package.manifest["commands"]) {
            const auto id = "script." + package.id() + "/" + QString::fromStdString(command["id"]);
            auto *action = new QAction(QString::fromStdString(command["label"]), this);
            action->setEnabled(prefs.value("enabled", true));
            m_actions[id] = action;
            m_window->addAction(action);
            ShortcutManager::Metadata meta;
            meta.description = QString::fromStdString(command.value("description", ""));
            meta.risk = ShortcutManager::Risk::Reversible;
            meta.remoteScope = ShortcutManager::RemoteScope::ForbiddenRemote;
            meta.modes = ShortcutManager::DoMode;
            m_shortcuts->registerCommand(id, action->text(), tr("Scripts"), {}, action, meta);
            connect(action, &QAction::triggered, this, [this, id] { run(id); });
        }
    }
    if (m_toolbar)
        rebuildToolbar();
    emit changed();
}
void ScriptService::setupMenu() {
    auto *menu = m_window->menuBar()->addMenu(tr("&Scripts"));
    menu->setObjectName("CommandMenu.Scripts");
    const auto add = [&](const QString &label, auto callback) {
        auto *action = menu->addAction(label);
        connect(action, &QAction::triggered, this, callback);
        return action;
    };
    add(tr("Run file…"), &ScriptService::runFile);
    add(tr("Choose command…"), &ScriptService::chooseCommand)
        ->setObjectName("ScriptCommandPicker.Open");
    add(tr("Script manager…"), &ScriptService::showManager);
    add(tr("Create in Creator…"), [this] { createScript(); });
    menu->addSeparator();
    m_toolbar = new ScriptToolBar(tr("Scripts"), m_window);
    m_toolbar->setObjectName("VltScriptToolbar");
    m_toolbar->setAllowedAreas(Qt::AllToolBarAreas);
    m_toolbar->setMovable(true);
    m_toolbar->setFloatable(true);
    m_toolbar->setIconSize(QSize(24, 24));
    m_toolbar->setStyleSheet("QToolBar#VltScriptToolbar { padding: 4px; spacing: 4px; }"
                             "QToolBar#VltScriptToolbar QToolButton { min-width: 28px; min-height: "
                             "28px; padding: 4px 6px; }");
    m_toolbar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_toolbar, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QMenu menu(m_toolbar);
        menu.addAction(tr("Configure toolbar…"), this, &ScriptService::configureToolbar);
        menu.exec(m_toolbar->mapToGlobal(position));
    });
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_window->addToolBar(Qt::TopToolBarArea, m_toolbar);
    m_toolbar->toggleViewAction()->setText(tr("Script toolbar"));
    menu->addAction(m_toolbar->toggleViewAction());
    add(tr("Configure toolbar…"), &ScriptService::configureToolbar);
    add(tr("Stop all"), &ScriptService::stopAll);
    menu->addSeparator();
    add(tr("User guide"), &ScriptService::showGuide);
    add(tr("SDK reference"), &ScriptService::showSdk);
    const auto place = [this](Qt::ToolBarArea area) {
        m_toolbar->setParent(m_window);
        m_toolbar->setWindowFlags(Qt::Widget);
        m_window->addToolBar(area, m_toolbar);
        m_toolbar->show();
    };
    auto *placement = menu->addMenu(tr("Toolbar position"));
    const QList<std::pair<QString, Qt::ToolBarArea>> edges = {{tr("Top"), Qt::TopToolBarArea},
                                                              {tr("Bottom"), Qt::BottomToolBarArea},
                                                              {tr("Left"), Qt::LeftToolBarArea},
                                                              {tr("Right"), Qt::RightToolBarArea}};
    for (const auto &[label, edge] : edges) {
        auto *action = placement->addAction(label);
        action->setObjectName("ScriptToolbar.Dock." + QString::number(int(edge)));
        connect(action, &QAction::triggered, this, [place, edge] { place(edge); });
    }
    auto *floating = placement->addAction(tr("Floating window"));
    floating->setObjectName("ScriptToolbar.Float");
    connect(floating, &QAction::triggered, this, [this] {
        m_window->removeToolBar(m_toolbar);
        m_toolbar->setParent(m_window, Qt::Tool);
        m_toolbar->show();
    });
    QSettings preferences;
    m_window->restoreState(preferences.value("scripts/toolbar/layout").toByteArray(), 1);
    if (preferences.value("scripts/toolbar/floating", false).toBool()) {
        m_window->removeToolBar(m_toolbar);
        m_toolbar->setParent(m_window, Qt::Tool);
        m_toolbar->restoreGeometry(preferences.value("scripts/toolbar/geometry").toByteArray());
    }
    m_toolbar->setVisible(preferences.value("scripts/toolbar/visible", false).toBool());
    auto *save = new QTimer(this);
    save->setInterval(500);
    connect(save, &QTimer::timeout, this, [this] {
        QSettings s;
        const auto layout = m_window->saveState(1);
        if (s.value("scripts/toolbar/layout").toByteArray() != layout)
            s.setValue("scripts/toolbar/layout", layout);
        s.setValue("scripts/toolbar/floating", m_toolbar->isFloating());
        s.setValue("scripts/toolbar/geometry", m_toolbar->saveGeometry());
        s.setValue("scripts/toolbar/visible", !m_toolbar->isHidden());
    });
    save->start();
    reload();
    QTimer::singleShot(0, this, [this] {
        for (const auto &package : m_packages) {
            const auto prefs = settings(package.id());
            if (prefs.value("enabled", true) && prefs.value("autostart", false) &&
                !prefs.value("autostartSuspended", false) && approvePermissions(package, false)) {
                const auto action = QString::fromStdString(package.manifest["commands"][0]["id"]);
                const auto id = runPackage(package, action, Json::object(), false, "do");
                for (const auto &run : m_runs)
                    if (run->id == id)
                        run->autostart = true;
            }
        }
    });
}
void ScriptService::showManager() {
    if (!m_manager)
        m_manager = new ScriptManagerWindow(this, m_window);
    m_manager->refresh();
    m_manager->show();
    m_manager->raise();
    m_manager->activateWindow();
}
void ScriptService::chooseCommand() {
    if (m_commandPicker) {
        m_commandPicker->show();
        m_commandPicker->raise();
        m_commandPicker->activateWindow();
        return;
    }
    auto *dialog = new QDialog(m_window, Qt::Tool);
    m_commandPicker = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("ScriptCommandPicker");
    dialog->setWindowTitle(tr("Choose script command"));
    dialog->resize(600, 460);
    dialog->setMinimumSize(440, 320);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);
    auto *heading = new QLabel(tr("Choose script command"), dialog);
    heading->setProperty("creatorRole", "heading");
    layout->addWidget(heading);
    auto *search = new QLineEdit(dialog);
    search->setObjectName("ScriptCommandPicker.Search");
    search->setPlaceholderText(tr("Search installed commands"));
    search->setAccessibleName(search->placeholderText());
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto *list = new QListWidget(dialog);
    list->setObjectName("ScriptCommandPicker.Commands");
    list->setAccessibleName(tr("Installed commands"));
    layout->addWidget(list, 1);
    auto *empty = new QLabel(tr("No commands found. Install or enable an extension in the script manager."), dialog);
    empty->setWordWrap(true);
    layout->addWidget(empty);
    auto *footer = new QHBoxLayout;
    auto *manager = new QPushButton(tr("Script manager…"), dialog);
    manager->setAutoDefault(false);
    footer->addWidget(manager);
    footer->addStretch();
    auto *runButton = new QPushButton(tr("Run"), dialog);
    runButton->setObjectName("ScriptCommandPicker.Run");
    runButton->setProperty("creatorPrimary", true);
    // Return in the search field and list already has an explicit handler.
    runButton->setAutoDefault(false);
    footer->addWidget(runButton);
    layout->addLayout(footer);
    const auto fill = [this, list, empty, runButton](const QString &query) {
        const auto selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole) : QVariant();
        list->clear();
        for (const auto &entry : describe(query))
            if (entry.value("enabled", true)) {
                auto *item =
                    new QListWidgetItem(QString::fromStdString(entry.value("label", "")) + " — " +
                                            QString::fromStdString(entry.value("extension", "")),
                                        list);
                item->setData(Qt::UserRole, QString::fromStdString(entry.value("id", "")));
                item->setToolTip(QString::fromStdString(entry.value("description", "")));
                if (item->data(Qt::UserRole) == selected)
                    list->setCurrentItem(item);
            }
        if (list->count() && !list->currentItem())
            list->setCurrentRow(0);
        empty->setVisible(list->count() == 0);
        runButton->setEnabled(list->currentItem() != nullptr);
    };
    connect(search, &QLineEdit::textChanged, dialog, fill);
    connect(this, &ScriptService::changed, dialog, [fill, search] { fill(search->text()); });
    fill({});
    const auto invoke = [this, dialog, list] {
        if (auto *item = list->currentItem()) {
            run(item->data(Qt::UserRole).toString());
            dialog->accept();
        }
    };
    connect(search, &QLineEdit::returnPressed, dialog, invoke);
    connect(list, &QListWidget::itemActivated, dialog, [invoke](auto *) { invoke(); });
    connect(runButton, &QPushButton::clicked, dialog, invoke);
    connect(manager, &QPushButton::clicked, dialog, [this, dialog] {
        dialog->accept();
        showManager();
    });
    const auto theme = [dialog] { styleCreator(dialog); };
    theme();
    connect(&ThemeManager::instance(), &ThemeManager::changed, dialog, theme);
    ScriptFocusFrame::install(dialog);
    dialog->show();
    search->setFocus();
}
void ScriptService::runFile() {
    const auto path = QFileDialog::getOpenFileName(m_window, tr("Run Lua file"), {},
                                                   "Lua (*.lua);;VLTONE scripts (*.vltscript)");
    if (path.isEmpty())
        return;
    const auto id = install(path);
    if (id.isEmpty())
        return;
    for (const auto &package : m_packages)
        if (package.id() == id)
            runPackage(package, QString::fromStdString(package.manifest["commands"][0]["id"]));
}
void ScriptService::createScript(const ScriptPackage *package, const QString &file, int line) {
    if (!m_creator)
        m_creator = new LuaCreatorWindow(this, m_window);
    if (package && !m_creator->openPackage(*package))
        return;
    m_creator->show();
    m_creator->raise();
    m_creator->activateWindow();
    if (!file.isEmpty())
        m_creator->reveal(file, line);
}
bool ScriptService::openLuaProject(const QString &path) {
    if (!m_creator)
        m_creator = new LuaCreatorWindow(this, m_window);
    QString error;
    if (!m_creator->openProject(path, error)) {
        if (!error.isEmpty())
            QMessageBox::warning(m_window, tr("Creator"), error);
        return false;
    }
    m_creator->show();
    m_creator->raise();
    m_creator->activateWindow();
    return true;
}
void ScriptService::showGuide() {
    const auto path = QCoreApplication::applicationDirPath() + "/ScriptSDK/" +
                      (QLocale().language() == QLocale::Russian ? "USER_RU.html" : "USER_EN.html");
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
void ScriptService::showSdk() {
    const auto path = QCoreApplication::applicationDirPath() + "/ScriptSDK/" +
                      (QLocale().language() == QLocale::Russian ? "SDK_RU.html" : "SDK_EN.html");
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
void ScriptService::appendContextMenu(QMenu *menu, daw::EngineController *controller,
                                      Json context) {
    if (instance())
        instance()->appendMenu(menu, controller, std::move(context));
}
void ScriptService::appendMenu(QMenu *menu, daw::EngineController *c, Json context) {
    if (!menu)
        return;
    if (!c)
        c = m_bindings.current().controller;
    if (!c)
        return;
    const auto project = m_bindings.current();
    if (project.controller != c)
        return;
    context["project"] = project.id.toStdString();
    context["generation"] = c->projectGeneration();
    if (context.contains("target") && context["target"].contains("device") &&
        context["target"].contains("track"))
        context["target"]["identity"] = daw::scripting::parameterIdentity(
            *c, context["target"]["track"], context["target"]["device"]);
    const auto type = context.value("type", "global");
    QMenu *submenu = nullptr;
    for (const auto &package : m_packages) {
        const auto prefs = settings(package.id());
        if (!prefs.value("enabled", true))
            continue;
        for (const auto &command : package.manifest["commands"]) {
            const auto contexts = command.value("contexts", Json::array());
            if (std::find(contexts.begin(), contexts.end(), type) == contexts.end())
                continue;
            const auto id = command["id"].get<std::string>();
            const auto placements = prefs.value("commands", Json::object());
            if (placements.contains(id) && placements[id].contains("contexts")) {
                const auto &chosen = placements[id]["contexts"];
                if (std::find(chosen.begin(), chosen.end(), type) == chosen.end())
                    continue;
            }
            const bool pinned = placements.contains(id) && placements[id].value("pinned", false);
            if (!pinned && !submenu)
                submenu = menu->addMenu(tr("Scripts"));
            auto *action =
                (pinned ? menu : submenu)->addAction(QString::fromStdString(command["label"]));
            const auto fullId = "script." + package.id() + "/" + QString::fromStdString(id);
            // Display the shortcut without adding a second active shortcut.
            if (!m_shortcuts->shortcut(fullId).isEmpty())
                action->setText(action->text() + "\t" + m_shortcuts->shortcut(fullId).toString());
            connect(action, &QAction::triggered, this,
                    [this, fullId, context] { run(fullId, context); });
        }
    }
}
void ScriptService::rebuildToolbar() {
    const auto previous = m_toolbar->actions();
    m_toolbar->clear();
    qDeleteAll(previous);
    for (const auto &entry : toolbarEntries()) {
        if (entry.value("separator", false)) {
            m_toolbar->addSeparator();
            continue;
        }
        const auto id = QString::fromStdString(entry.value("command", ""));
        const auto *command = m_shortcuts->command(id);
        if (!command)
            continue;
        auto caption = QString::fromStdString(entry.value("label", "")).trimmed();
        if (caption.isEmpty())
            caption = command->label;
        auto *action =
            m_toolbar->addAction(QIcon(QString::fromStdString(entry.value("icon", ""))), caption);
        action->setEnabled(command->action && command->action->isEnabled());
        action->setToolTip(command->label + " " + m_shortcuts->shortcut(id).toString());
        if (auto *button = qobject_cast<QToolButton *>(m_toolbar->widgetForAction(action))) {
            button->setToolButtonStyle(
                buttonStyle(QString::fromStdString(entry.value("appearance", "both")),
                            !action->icon().isNull()));
            button->setAccessibleName(action->text().isEmpty() ? command->label : action->text());
            button->setFocusPolicy(Qt::StrongFocus);
        }
        connect(action, &QAction::triggered, this, [this, id, entry] {
            run(id, Json::object(), entry.value("arguments", Json::object()));
        });
    }
}
void ScriptService::configureToolbar() {
    if (!m_toolbarEditor) {
        m_toolbarEditor = new ScriptToolbarEditor(
            toolbarEntries(), describe(),
            [this](const Json &entries) {
                QSettings().setValue("scripts/toolbar/buttons",
                                     QString::fromStdString(entries.dump()));
                rebuildToolbar();
            },
            m_window);
    }
    m_toolbarEditor->show();
    m_toolbarEditor->raise();
    m_toolbarEditor->activateWindow();
}

} // namespace ui
