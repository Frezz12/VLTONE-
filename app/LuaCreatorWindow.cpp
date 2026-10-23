#include "LuaCreatorWindow.hpp"
#include <QUuid>
#include "AiChatShared.hpp"
#include "CreatorStyle.hpp"
#include "LlmClient.hpp"
#include "ScriptFocusFrame.hpp"
#include "ScriptService.hpp"
#include "Theme.hpp"
#include <QAbstractItemView>
#include <QAction>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSettings>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStringListModel>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoCommand>
#include <QUndoStack>
#include <chrono>

namespace ui {
namespace {
using Json = vlt::script::Json;
class LuaHighlight : public QSyntaxHighlighter {
  public:
    explicit LuaHighlight(QTextDocument *doc) : QSyntaxHighlighter(doc) {
        connect(&ThemeManager::instance(), &ThemeManager::changed, this,
                &QSyntaxHighlighter::rehighlight);
    }

  protected:
    void highlightBlock(const QString &text) override {
        const auto colors = creatorColors();
        static const QRegularExpression keywords(
            "\\b(and|break|do|else|elseif|end|false|for|function|goto|if|in|local|nil|not|or|"
            "repeat|return|then|true|until|while)\\b");
        static const QRegularExpression api("\\bvlt(?:\\.[\\w]+)+");
        static const QRegularExpression numbers("\\b\\d+(?:\\.\\d+)?\\b");
        const auto apply = [&](const QRegularExpression &pattern, const QColor &color) {
            auto matches = pattern.globalMatch(text);
            while (matches.hasNext()) {
                const auto m = matches.next();
                setFormat(m.capturedStart(), m.capturedLength(), color);
            }
        };
        apply(keywords, colors.gate);
        apply(api, colors.number);
        apply(numbers, colors.function);

        // Scan literals in order: "--" inside a string is not a comment. A block state
        // records the long-bracket delimiter's '=' count and whether it is a comment.
        int state = previousBlockState();
        setCurrentBlockState(-1);
        for (int i = 0; i < text.size();) {
            const int start = i;
            if (state < 0) {
                const auto quote = text[i];
                if (quote == '\'' || quote == '"') {
                    ++i;
                    while (i < text.size()) {
                        if (text[i] == '\\') {
                            i += 2;
                            continue;
                        }
                        if (text[i++] == quote)
                            break;
                    }
                    setFormat(start, std::min(i, int(text.size())) - start, colors.audio);
                    continue;
                }
                const bool comment = text.mid(i, 2) == "--";
                const int bracket = i + (comment ? 2 : 0);
                int end = bracket + 1;
                if (bracket < text.size() && text[bracket] == '[') {
                    while (end < text.size() && text[end] == '=')
                        ++end;
                    if (end < text.size() && text[end] == '[') {
                        state = 2 * (end - bracket - 1) + (comment ? 1 : 0);
                        i = end + 1;
                    }
                }
                if (state < 0) {
                    if (comment) {
                        setFormat(i, text.size() - i, colors.muted);
                        break;
                    }
                    ++i;
                    continue;
                }
            }
            const auto delimiter = "]" + QString(state / 2, '=') + "]";
            const int end = text.indexOf(delimiter, i);
            const auto color = state % 2 ? colors.muted : colors.audio;
            if (end < 0) {
                setFormat(start, text.size() - start, color);
                setCurrentBlockState(state);
                break;
            }
            i = end + delimiter.size();
            setFormat(start, i - start, color);
            state = -1;
        }
    }
};
class LuaEditor : public QPlainTextEdit {
    QCompleter m_completion;
    class LineNumbers final : public QWidget {
        LuaEditor *m_editor;

      public:
        explicit LineNumbers(LuaEditor *editor) : QWidget(editor), m_editor(editor) {
            setAttribute(Qt::WA_TransparentForMouseEvents);
        }
        void paintEvent(QPaintEvent *) override {
            m_editor->paintNumbers(this);
        }
    };
    LineNumbers *m_numbers;
    void updateGutter() {
        const int width =
            18 + fontMetrics().horizontalAdvance('9') * QString::number(blockCount()).size();
        setViewportMargins(width, 0, 0, 0);
        m_numbers->setGeometry(contentsRect().left(), contentsRect().top(), width,
                               contentsRect().height());
        setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    }
    void paintNumbers(QWidget *area) {
        QPainter painter(area);
        const auto colors = creatorColors();
        painter.fillRect(area->rect(), colors.panel);
        painter.setFont(font());
        auto block = firstVisibleBlock();
        int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        while (block.isValid() && top < area->height()) {
            const int height = qRound(blockBoundingRect(block).height());
            if (block.isVisible()) {
                painter.setPen(block.blockNumber() == textCursor().blockNumber() ? colors.text
                                                                                 : colors.muted);
                painter.drawText(0, top, area->width() - 8, height,
                                 Qt::AlignRight | Qt::AlignVCenter,
                                 QString::number(block.blockNumber() + 1));
            }
            top += height;
            block = block.next();
        }
    }

  public:
    explicit LuaEditor(QWidget *parent)
        : QPlainTextEdit(parent), m_completion(this), m_numbers(new LineNumbers(this)) {
        setLineWrapMode(NoWrap);
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { updateGutter(); });
        connect(this, &QPlainTextEdit::cursorPositionChanged, m_numbers,
                qOverload<>(&QWidget::update));
        connect(this, &QPlainTextEdit::updateRequest, this, [this](QRect rect, int dy) {
            if (dy)
                m_numbers->scroll(0, dy);
            else
                m_numbers->update(0, rect.y(), m_numbers->width(), rect.height());
        });
        connect(&ThemeManager::instance(), &ThemeManager::changed, m_numbers,
                qOverload<>(&QWidget::update));
        QStringList names;
        for (const auto &method : vlt::script::apiCatalog()["methods"])
            names.append("vlt." + QString::fromStdString(method["name"]));
        for (const auto &[type, description] : vlt::script::apiCatalog()["widgets"].items())
            names.append(QString::fromStdString(type));
        for (const auto &[property, description] : vlt::script::apiCatalog()["uiStyle"]["properties"].items())
            names.append(QString::fromStdString(property));
        names.append({"children", "onClick", "onChange", "onPointer", "onWheel", "onKey", "onResize",
            "drawing", "columns", "rows", "readOnly", "editable", "orientation", "tooltip", "minWidth", "minHeight",
            "style", "styles", "theme", "use", "hover", "pressed", "checked", "disabled", "focus", "wrap", "align"});
        m_completion.setModel(new QStringListModel(names, &m_completion));
        m_completion.setWidget(this);
        m_completion.setCaseSensitivity(Qt::CaseInsensitive);
        connect(&m_completion, qOverload<const QString &>(&QCompleter::activated), this,
                [this](const QString &word) {
                    auto cursor = textCursor();
                    int length = m_completion.completionPrefix().size();
                    for (int i = 0; i < length; ++i)
                        cursor.deletePreviousChar();
                    cursor.insertText(word);
                    setTextCursor(cursor);
                });
        updateGutter();
    }

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QPlainTextEdit::resizeEvent(event);
        updateGutter();
    }
    void keyPressEvent(QKeyEvent *e) override {
        if (m_completion.popup()->isVisible() &&
            (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter ||
             e->key() == Qt::Key_Escape || e->key() == Qt::Key_Tab)) {
            e->ignore();
            return;
        }
        if (e->key() == Qt::Key_Space && e->modifiers() == Qt::ControlModifier) {
            auto cursor = textCursor();
            const auto line = cursor.block().text().left(cursor.positionInBlock());
            const auto match = QRegularExpression("[\\w.]+$").match(line);
            m_completion.setCompletionPrefix(match.captured());
            m_completion.complete(cursorRect());
            return;
        }
        QPlainTextEdit::keyPressEvent(e);
    }
};
class DiagnosticLog : public QPlainTextEdit {
  public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<void(const QString &, int)> reveal;

  protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        QPlainTextEdit::mouseDoubleClickEvent(event);
        const auto line = cursorForPosition(event->position().toPoint()).block().text();
        const auto match = QRegularExpression("([^\\r\\n:]+\\.lua):(\\d+):").match(line);
        if (match.hasMatch() && reveal)
            reveal(match.captured(1).trimmed(), match.captured(2).toInt());
    }
};
class DocumentEdit : public QUndoCommand {
  public:
    Json before, after;
    std::function<void(const Json &)> apply;
    DocumentEdit(Json b, Json a, QString label, std::function<void(const Json &)> fn)
        : QUndoCommand(label), before(std::move(b)), after(std::move(a)), apply(std::move(fn)) {}
    void undo() override {
        apply(before);
    }
    void redo() override {
        apply(after);
    }
};
// Typing stores only one source. Embedded audio/resources never enter each keystroke's history.
class SourceEdit final : public QUndoCommand {
  public:
    using Clock = std::chrono::steady_clock;
    QString file;
    std::string before, after;
    std::uint64_t group;
    Clock::time_point time = Clock::now();
    std::function<void(const QString &, const std::string &)> apply;
    SourceEdit(QString path, std::string oldText, std::string newText, std::uint64_t editGroup,
               const QString &label, decltype(apply) callback)
        : QUndoCommand(label), file(std::move(path)), before(std::move(oldText)),
          after(std::move(newText)), group(editGroup), apply(std::move(callback)) {}
    void undo() override {
        apply(file, before);
    }
    void redo() override {
        apply(file, after);
    }
    int id() const override {
        return 7302;
    }
    bool mergeWith(const QUndoCommand *command) override {
        const auto *edit = dynamic_cast<const SourceEdit *>(command);
        if (!edit || file != edit->file || group != edit->group ||
            edit->time - time > std::chrono::milliseconds(750))
            return false;
        after = edit->after;
        time = edit->time;
        return true;
    }
};
} // namespace
LuaCreatorWindow::LuaCreatorWindow(ScriptService *service, QWidget *parent)
    : QMainWindow(parent), m_service(service) {
    setWindowTitle(tr("Creator — Lua script") + "[*]");
    setMinimumSize(900, 600);
    resize(1180, 760);
    restoreGeometry(QSettings().value("scripts/creator/geometry").toByteArray());
    m_undo = new QUndoStack(this);
    m_undo->setUndoLimit(100);
    connect(m_undo, &QUndoStack::cleanChanged, this,
            [this](bool clean) { setWindowModified(!clean); });
    ScriptFocusFrame::install(this);
    auto *toolbar = addToolBar(tr("Lua project"));
    toolbar->setObjectName("CreatorToolbar");
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(18, 18));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto *brand = new QLabel("Creator · Lua", toolbar);
    brand->setProperty("creatorRole", "brand");
    toolbar->addWidget(brand);
    const auto button = [&](icons::Glyph glyph, const QString &label, auto callback,
                            bool prominent = false) {
        auto *action = toolbar->addAction(label);
        creatorIcon(action, glyph, prominent);
        connect(action, &QAction::triggered, this, callback);
        auto *control = qobject_cast<QToolButton *>(toolbar->widgetForAction(action));
        control->setFocusPolicy(Qt::StrongFocus);
        control->setAccessibleName(label);
        control->setAutoRaise(true);
        control->setMinimumSize(30, 30);
        if (prominent) {
            control->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            control->setProperty("creatorPrimary", true);
        }
        return action;
    };
    button(icons::Glyph::Plus, tr("New"), &LuaCreatorWindow::newProject)
        ->setShortcut(QKeySequence::New);
    button(icons::Glyph::Folder, tr("Open…"), &LuaCreatorWindow::open)
        ->setShortcut(QKeySequence::Open);
    button(icons::Glyph::Save, tr("Save"), [this] { save(); })->setShortcut(QKeySequence::Save);
    auto *saveAs = new QAction(tr("Save as…"), this);
    saveAs->setShortcut(QKeySequence::SaveAs);
    addAction(saveAs);
    connect(saveAs, &QAction::triggered, this, [this] { save(true); });
    toolbar->addSeparator();
    auto *undo = button(icons::Glyph::Undo, tr("Undo"), [this] { m_undo->undo(); });
    undo->setShortcut(QKeySequence::Undo);
    undo->setEnabled(false);
    auto *redo = button(icons::Glyph::Redo, tr("Redo"), [this] { m_undo->redo(); });
    redo->setShortcut(QKeySequence::Redo);
    redo->setEnabled(false);
    connect(m_undo, &QUndoStack::canUndoChanged, undo, &QAction::setEnabled);
    connect(m_undo, &QUndoStack::canRedoChanged, redo, &QAction::setEnabled);
    toolbar->addSeparator();
    button(icons::Glyph::Check, tr("Check"), [this] {
        QString error;
        m_log->appendPlainText(check(error) ? tr("Validation passed") : error);
    });
    auto *preview = button(icons::Glyph::Play, tr("Preview copy"), [this] { execute(true); }, true);
    preview->setObjectName("LuaCreator.Preview");
    auto *live = button(icons::Glyph::Play, tr("Run in real project"), [this] { execute(false); });
    live->setObjectName("LuaCreator.Run");
    qobject_cast<QToolButton *>(toolbar->widgetForAction(live))
        ->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_stop = button(icons::Glyph::Stop, tr("Stop"), [this] {
        m_service->stop(m_run);
        stopAi();
    });
    m_stop->setEnabled(false);
    toolbar->addSeparator();
    button(icons::Glyph::Download, tr("Install"), [this] {
        QString error;
        m_log->appendPlainText(install(error) ? tr("Installed") : error);
    });
    button(icons::Glyph::Export, tr("Export…"), [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("Export script"), {},
                                                       "VLTONE script (*.vltscript)");
        if (path.isEmpty())
            return;
        try {
            QString error;
            if (!ScriptLibrary::exportPackage(package(), path, error))
                m_log->appendPlainText(error);
        } catch (const std::exception &e) {
            m_log->appendPlainText(e.what());
        }
    });
    auto *space = new QWidget(toolbar);
    space->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(space);
    button(icons::Glyph::Notebook, tr("SDK reference"), [this] { m_service->showSdk(); });
    auto *assistant = button(icons::Glyph::Assistant, tr("Creator assistant"), [] {});
    assistant->setCheckable(true);
    assistant->setChecked(true);
    auto *split = new QSplitter(this);
    setCentralWidget(split);
    auto *left = new QWidget(split);
    auto *leftLayout = new QVBoxLayout(left);
    auto *fileActions = new QHBoxLayout;
    auto *fileHeading = new QLabel(tr("Files and resources"), left);
    fileHeading->setProperty("creatorRole", "heading");
    fileActions->addWidget(fileHeading, 1);
    leftLayout->addLayout(fileActions);
    m_files = new QTreeWidget(left);
    m_files->setHeaderHidden(true);
    m_files->setAccessibleName(tr("Files and resources"));
    leftLayout->addWidget(m_files);
    m_files->header()->setStyleSheet(
        "QHeaderView::section { background-color: palette(button); color: palette(button-text); "
        "padding: 4px 6px; border: none; }");
    auto *add = new IconButton(icons::Glyph::Plus, tr("Add Lua file…"), left);
    add->setObjectName("LuaCreator.AddSource");
    fileActions->addWidget(add);
    connect(add, &QAbstractButton::clicked, this, [this] {
        bool ok = false;
        auto name = QInputDialog::getText(this, tr("New source"), tr("Relative Lua path"),
                                          QLineEdit::Normal, "module.lua", &ok);
        if (!ok)
            return;
        if (!vlt::script::safeRelativePath(name.toStdString()) || !name.endsWith(".lua")) {
            m_log->appendPlainText(tr("Enter a relative path ending in .lua."));
            return;
        }
        for (const auto &[path, source] : m_document["sources"].items()) {
            if (QString::fromStdString(path).compare(name, Qt::CaseInsensitive) == 0) {
                reveal(QString::fromStdString(path), 1);
                m_log->appendPlainText(tr("This source already exists. Its contents were kept."));
                return;
            }
        }
        auto doc = m_document;
        doc["sources"][name.toStdString()] = "";
        editDocument(doc, tr("Add source"));
        reveal(name, 1);
    });
    auto *resource = new IconButton(icons::Glyph::Folder, tr("Add resource…"), left);
    resource->setObjectName("LuaCreator.AddResource");
    fileActions->addWidget(resource);
    connect(resource, &QAbstractButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Add resource"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) {
            m_log->appendPlainText(tr("Cannot read resource, or it exceeds 16 MiB."));
            return;
        }
        const auto resourcePath = "resources/" + QFileInfo(path).fileName().toStdString();
        if (m_document["resources"].contains(resourcePath) &&
            QMessageBox::question(
                this, tr("Replace resource"), tr("Replace the existing resource with this file?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        auto doc = m_document;
        doc["resources"]["resources/" + QFileInfo(path).fileName().toStdString()] =
            file.readAll().toBase64().toStdString();
        editDocument(doc, tr("Add resource"));
    });
    for (auto *control : {add, resource}) {
        control->setButtonSize(30, 30);
        control->setGlyphSize(18);
        control->setFocusPolicy(Qt::StrongFocus);
        control->setAccessibleName(control->toolTip());
    }
    auto *center = new QWidget(split);
    auto *centerLayout = new QVBoxLayout(center);
    auto *commandRow = new QHBoxLayout;
    commandRow->addWidget(new QLabel(tr("Command"), center));
    m_commands = new QComboBox(center);
    m_commands->setObjectName("LuaCreator.Command");
    m_commands->setAccessibleName(tr("Command to run"));
    commandRow->addWidget(m_commands, 1);
    centerLayout->addLayout(commandRow);
    auto *find = new QLineEdit(center);
    find->setAccessibleName(tr("Find in source"));
    find->setClearButtonEnabled(true);
    find->setPlaceholderText(tr("Find in source — Enter for next result"));
    centerLayout->addWidget(find);
    const auto codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_editor = new LuaEditor(center);
    m_editor->setObjectName("LuaCreator.Source");
    m_editor->setUndoRedoEnabled(false);
    m_editor->setFont(codeFont);
    m_editor->setTabStopDistance(m_editor->fontMetrics().horizontalAdvance(' ') * 4);
    new LuaHighlight(m_editor->document());
    centerLayout->addWidget(m_editor, 1);
    m_editor->setAccessibleName(tr("Lua source"));
    m_editor->setStyleSheet("QPlainTextEdit { font-family: 'Cascadia Code', 'Consolas', 'Menlo', "
                            "monospace; font-size: 13px; }");
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, [find] {
        find->setFocus();
        find->selectAll();
    });
    connect(find, &QLineEdit::returnPressed, this, [this, find] {
        if (!m_editor->find(find->text())) {
            auto cursor = m_editor->textCursor();
            cursor.movePosition(QTextCursor::Start);
            m_editor->setTextCursor(cursor);
            m_editor->find(find->text());
        }
    });
    auto *diagnostics = new DiagnosticLog(center);
    diagnostics->reveal = [this](const QString &file, int line) { reveal(file, line); };
    m_log = diagnostics;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(512);
    m_log->setMaximumHeight(160);
    centerLayout->addWidget(m_log);
    m_log->setAccessibleName(tr("Script log"));
    m_log->setPlaceholderText(tr("Diagnostics and script output"));
    auto *ai = new QWidget(split);
    ai->setObjectName("CreatorAiPanel");
    connect(assistant, &QAction::toggled, ai, &QWidget::setVisible);
    auto *aiLayout = new QVBoxLayout(ai);
    m_models = new QComboBox(ai);
    m_models->setAccessibleName(tr("AI model"));
    aiLayout->addWidget(m_models);
    for (const auto &model : aiprefs::availableModels())
        m_models->addItem(model.displayName, model.id);
    m_models->setCurrentIndex(std::max(0, m_models->findData(aiprefs::activeModelId())));
    m_transcript = new QPlainTextEdit(ai);
    m_transcript->setReadOnly(true);
    m_transcript->setMaximumBlockCount(1000);
    m_transcript->setAccessibleName(tr("Conversation with AI"));
    aiLayout->addWidget(m_transcript, 1);
    m_prompt = new QPlainTextEdit(ai);
    m_prompt->setAccessibleName(tr("Message to AI"));
    m_prompt->setPlaceholderText(tr("Ask AI to create or improve this Lua extension"));
    m_prompt->setMaximumHeight(150);
    aiLayout->addWidget(m_prompt);
    auto *send = new QPushButton(tr("Send to AI"), ai);
    aiLayout->addWidget(send);
    connect(send, &QPushButton::clicked, this, &LuaCreatorWindow::sendAi);
    split->setSizes({210, 650, 320});
    connect(m_files, &QTreeWidget::itemSelectionChanged, this, &LuaCreatorWindow::selectFile);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &LuaCreatorWindow::changedText);
    connect(m_editor, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        statusBar()->showMessage(tr("%1 — line %2, column %3 — Ctrl+Space: completion")
                                     .arg(m_file)
                                     .arg(m_editor->textCursor().blockNumber() + 1)
                                     .arg(m_editor->textCursor().positionInBlock() + 1));
    });
    connect(service, &ScriptService::runChanged, this, [this](const QString &id) {
        if (id == m_run)
            refreshRunLog();
    });
    m_workspace = std::make_unique<LuaAiWorkspace>(*this);
    m_session = std::make_unique<daw::ai::AiSession>(*m_workspace);
    ScriptPackage initial{
        {},
        ScriptLibrary::manifestForLua(tr("My Lua script")),
        {{"main.lua",
          "local context = vlt.context.get()\nprint(\"Hello from VLTONE\", context.project)\n"}}};
    openPackage(initial);
    styleCreator(this);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            [this] { styleCreator(this); });
}

LuaCreatorWindow::~LuaCreatorWindow() {
    stopAi();
    m_service->stop(m_run);
}

bool LuaCreatorWindow::askSave() {
    if (m_undo->isClean())
        return true;
    const auto answer = QMessageBox::question(
        this, tr("Save Lua project?"), tr("Save your changes before continuing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    return answer == QMessageBox::Discard || (answer == QMessageBox::Save && save());
}

void LuaCreatorWindow::closeEvent(QCloseEvent *event) {
    if (!askSave()) {
        event->ignore();
        return;
    }
    stopAi();
    m_service->stop(m_run);
    QSettings().setValue("scripts/creator/geometry", saveGeometry());
    event->accept();
}

bool LuaCreatorWindow::openPackage(const ScriptPackage &package) {
    if (!askSave())
        return false;
    stopAi();
    m_service->stop(m_run);
    m_origin = package;
    setProperty("vlt.documentId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (property("vlt.projectId").toString().isEmpty()) setProperty("vlt.projectId", m_service->m_bindings.current().id);
    m_path.clear();
    setWindowFilePath({});
    apply({{"format", "vltcreator"},
           {"version", 4},
           {"kind", "lua"},
           {"name", package.name().toStdString()},
           {"manifest", package.manifest},
           {"manifestSource", package.manifest.dump(2)},
           {"sources", package.sources},
           {"resources", package.resources}});
    m_undo->clear();
    m_undo->setClean();
    reveal("main.lua", 1);
    return true;
}

void LuaCreatorWindow::apply(const Json &document) {
    m_loading = true;
    m_document = document;
    ++m_revision;
    const auto selected = m_file;
    m_files->clear();
    auto *manifest = new QTreeWidgetItem(m_files, {"manifest.json"});
    manifest->setData(0, Qt::UserRole, "manifest.json");
    for (const auto &[path, source] : document.at("sources").items()) {
        auto *item = new QTreeWidgetItem(m_files, {QString::fromStdString(path)});
        item->setData(0, Qt::UserRole, QString::fromStdString(path));
    }
    for (const auto &[path, data] : document.value("resources", Json::object()).items()) {
        auto *item = new QTreeWidgetItem(m_files, {QString::fromStdString(path)});
        item->setDisabled(true);
    }
    for (int i = 0; i < m_files->topLevelItemCount(); ++i)
        if (m_files->topLevelItem(i)->data(0, Qt::UserRole).toString() == selected)
            m_files->setCurrentItem(m_files->topLevelItem(i));
    if (!m_files->currentItem())
        m_files->setCurrentItem(m_files->topLevelItem(m_document["sources"].empty() ? 0 : 1));
    m_loading = false;
    selectFile();
    refreshCommands();
}

void LuaCreatorWindow::selectFile() {
    if (m_loading || !m_files->currentItem())
        return;
    const auto file = m_files->currentItem()->data(0, Qt::UserRole).toString();
    if (file != m_file)
        ++m_editGroup;
    m_file = file;
    if (m_file.isEmpty())
        return;
    const auto text =
        m_file == "manifest.json"
            ? QString::fromStdString(m_document.at("manifestSource").get<std::string>())
            : QString::fromStdString(
                  m_document["sources"].at(m_file.toStdString()).get<std::string>());
    if (m_editor->toPlainText() == text)
        return;
    m_loading = true;
    const int pos = m_editor->textCursor().position();
    m_editor->setPlainText(text);
    auto cursor = m_editor->textCursor();
    cursor.setPosition(std::min(pos, int(text.size())));
    m_editor->setTextCursor(cursor);
    m_loading = false;
}

void LuaCreatorWindow::changedText() {
    if (m_loading || m_file.isEmpty())
        return;
    const auto &previous = m_file == "manifest.json"
                               ? m_document.at("manifestSource")
                               : m_document.at("sources").at(m_file.toStdString());
    const auto text = m_editor->toPlainText().toStdString();
    if (previous == text)
        return;
    m_undo->push(new SourceEdit(
        m_file, previous.get<std::string>(), text, m_editGroup, tr("Edit Lua source"),
        [this](const QString &file, const std::string &value) { applySource(file, value); }));
}

void LuaCreatorWindow::applySource(const QString &file, const std::string &text) {
    if (file == "manifest.json")
        m_document["manifestSource"] = text;
    else
        m_document["sources"][file.toStdString()] = text;
    ++m_revision;
    if (file == m_file)
        selectFile();
    else
        reveal(file, 1);
    if (file == "manifest.json")
        refreshCommands();
}

void LuaCreatorWindow::refreshCommands() {
    const QSignalBlocker blocker(m_commands);
    const auto selected = m_commands->currentData();
    m_commands->clear();
    try {
        const auto manifest = Json::parse(m_document.at("manifestSource").get<std::string>());
        for (const auto &command : manifest.at("commands"))
            m_commands->addItem(QString::fromStdString(command.at("label")),
                                QString::fromStdString(command.at("id")));
        const int index = m_commands->findData(selected);
        if (index >= 0)
            m_commands->setCurrentIndex(index);
    } catch (const std::exception &) {
        m_commands->clear();
    }
    m_commands->setEnabled(m_commands->count() > 1);
}

void LuaCreatorWindow::refreshRunLog() {
    const auto status = m_service->status(m_run);
    if (status.is_object()) {
        QStringList lines;
        for (const auto &line : status.value("log", Json::array()))
            lines.append(QString::fromStdString(line.get<std::string>()));
        const auto text = lines.join('\n');
        if (m_log->toPlainText() != text) {
            const auto position = m_log->verticalScrollBar()->value();
            const bool atEnd = position == m_log->verticalScrollBar()->maximum();
            m_log->setPlainText(text);
            m_log->verticalScrollBar()->setValue(atEnd ? m_log->verticalScrollBar()->maximum()
                                                       : position);
        }
    }
    updateRunState();
}

void LuaCreatorWindow::updateRunState() {
    const auto status = m_service->status(m_run);
    const bool scriptRunning = status.is_object() && !status.value("complete", true);
    m_stop->setEnabled(scriptRunning || (m_session && m_session->running()));
}

void LuaCreatorWindow::editDocument(const Json &document, const QString &label) {
    m_undo->push(
        new DocumentEdit(m_document, document, label, [this](const Json &value) { apply(value); }));
}

void LuaCreatorWindow::reveal(const QString &file, int line) {
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        auto *item = m_files->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() != file)
            continue;
        m_files->setCurrentItem(item);
        const int block = std::clamp(line - 1, 0, m_editor->document()->blockCount() - 1);
        m_editor->setTextCursor(QTextCursor(m_editor->document()->findBlockByNumber(block)));
        m_editor->centerCursor();
        m_editor->setFocus();
        return;
    }
}

ScriptPackage LuaCreatorWindow::package() const {
    return {m_origin.directory, Json::parse(m_document.at("manifestSource").get<std::string>()),
            m_document.at("sources"), m_document.value("resources", Json::object())};
}

bool LuaCreatorWindow::check(QString &error) const {
    try {
        return ScriptLibrary::validate(package(), error);
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}

QString LuaCreatorWindow::execute(bool preview, const QString &action) {
    QString error;
    if (!check(error)) {
        m_log->appendPlainText(error);
        return {};
    }
    m_service->stop(m_run);
    const auto p = package();
    const auto command = action.isEmpty() ? m_commands->currentData().toString() : action;
    m_run = m_service->runPackage(p, command, Json::object(), preview);
    if (m_run.isEmpty())
        m_log->appendPlainText(tr(
            "The command could not start. Check the selected command, project and permissions."));
    refreshRunLog();
    return m_run;
}

bool LuaCreatorWindow::install(QString &error) {
    try {
        if (!ScriptLibrary::install(package(), error))
            return false;
        m_service->reload();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

bool LuaCreatorWindow::save(bool saveAs) {
    auto path = m_path;
    if (path.isEmpty() || saveAs)
        path = QFileDialog::getSaveFileName(this, tr("Save Lua Creator project"),
                                            m_path.isEmpty() ? "My script.vltcreator" : m_path,
                                            "Creator (*.vltcreator)");
    if (path.isEmpty())
        return false;
    auto document = m_document;
    try {
        document["manifest"] = Json::parse(document["manifestSource"].get<std::string>());
    } catch (...) {
    }
    QSaveFile file(path);
    const auto bytes = QByteArray::fromStdString(document.dump(2));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        m_log->appendPlainText(file.errorString());
        return false;
    }
    m_path = path;
    setWindowFilePath(path);
    m_undo->setClean();
    return true;
}

bool LuaCreatorWindow::openProject(const QString &path, QString &error) {
    Json doc;
    if (!ScriptLibrary::readCreatorDocument(path, doc, error) || !askSave())
        return false;
    stopAi();
    m_service->stop(m_run);
    m_origin = {};
    setProperty("vlt.documentId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_path = path;
    setWindowFilePath(path);
    apply(doc);
    m_undo->clear();
    m_undo->setClean();
    reveal("main.lua", 1);
    return true;
}

void LuaCreatorWindow::open() {
    const auto path = QFileDialog::getOpenFileName(this, tr("Open Lua Creator project"), {},
                                                   "Creator (*.vltcreator);;Lua (*.lua)");
    if (path.isEmpty())
        return;
    QString error;
    if (path.endsWith(".vltcreator")) {
        if (!openProject(path, error) && !error.isEmpty())
            m_log->appendPlainText(error);
        return;
    }
    ScriptPackage p;
    if (!ScriptLibrary::read(path, p, error)) {
        m_log->appendPlainText(error);
        return;
    }
    openPackage(p);
}

void LuaCreatorWindow::newProject() {
    bool accepted = false;
    const auto kind =
        QInputDialog::getItem(this, tr("New project"), tr("Project type"),
                              {tr("Lua script"), tr("Mini-module")}, 0, false, &accepted);
    if (!accepted)
        return;
    if (kind == tr("Mini-module")) {
        if (!askSave())
            return;
        m_service->m_bindings.command("view.creator", m_service->m_bindings.current().id);
        return;
    }
    ScriptPackage p{{},
                    ScriptLibrary::manifestForLua(tr("New Lua script")),
                    {{"main.lua", "print(\"New extension\")\n"}}};
    openPackage(p);
}

void LuaCreatorWindow::stopAi() {
    if (m_client)
        m_client->cancel();
    if (m_session)
        m_session->cancel();
    updateRunState();
}

void LuaCreatorWindow::sendAi() {
    if (m_session->running() || property("vlt.mcp.busy").toBool() || m_prompt->toPlainText().trimmed().isEmpty())
        return;
    aiprefs::ModelConnection connection;
    if (!aiprefs::modelById(m_models->currentData().toString(), &connection)) {
        m_log->appendPlainText(tr("Configure an AI model in Settings."));
        return;
    }
    m_client = std::make_unique<LlmClient>(connection.provider == aiprefs::Provider::OpenAi
                                               ? LlmClient::Provider::OpenAi
                                               : LlmClient::Provider::Anthropic);
    m_client->setConfig(aiModelConfig(connection));
    m_session->setMaxIterations(aiprefs::maxIterations());
    m_session->setHistoryLimit(aiprefs::historyLimit());
    daw::ai::ToolContext context;
    context.mode = daw::ai::InteractionMode::Do;
    m_session->setContext(context);
    m_transcript->appendPlainText("> " + m_prompt->toPlainText());
    m_session->begin(m_prompt->toPlainText().toStdString());
    m_prompt->clear();
    updateRunState();
    stepAi();
}

void LuaCreatorWindow::stepAi() {
    if (!m_session->running() || !m_client)
        return;
    m_client->setAvailableTools(m_session->availableTools());
    m_client->send(QString::fromStdString(m_session->systemPrompt()), m_session->wireMessages(),
                   [this](daw::ai::ModelReply reply) {
                       if (!reply.text.empty())
                           m_transcript->appendPlainText(QString::fromStdString(reply.text));
                       const auto next = m_session->applyReply(reply);
                       updateRunState();
                       if (next == daw::ai::AiSession::Step::NeedsRequest)
                           QTimer::singleShot(0, this, &LuaCreatorWindow::stepAi);
                       else if (next == daw::ai::AiSession::Step::Failed)
                           m_transcript->appendPlainText(
                               QString::fromStdString(m_session->lastError()));
                   });
}

std::uint64_t LuaAiWorkspace::revision() const {
    return m_window.revision();
}

std::string LuaAiWorkspace::systemPrompt(const daw::ai::ToolContext &) const {
    return "You create VLTONE Lua 5.5.1 extensions using SDK 1. Read lua_context and lua_sdk "
           "first. Use lua_read for source pages and lua_example for shipped working examples. "
           "Use the public vlt API only; no reaper API, native modules, io/os or arbitrary process "
           "access. "
           "Edit source/manifest with lua_edit, check every change, run preview and inspect "
           "status/errors. "
           "Preview uses copied data. Never claim preview edited the real project. Only install "
           "when the user asks. "
           "Changes belong to Creator undo history. Project edits use project.apply with revision "
           "from project.get, musical time is quarter-note beats. "
           "For UIs read widgets, widgetCommon, uiStyle and canvasEvents from lua_sdk. Use named styles, "
           "window-local theme tokens and native layouts; never raw CSS. See styled-workbench for custom design. Use ui.get/set for "
           "state, ui.replace for validated tree replacement, dock for native panels, and "
           "workspace.get/open/select for VLTONE navigation. See interactive-workbench via "
           "lua_example. Native navigation is blocked in preview and requires the active pinned "
           "project. Use PCM blocks and jobs for offline "
           "audio. "
           "Use vlt.array({}) for empty arrays. Refer to SDK examples and preserve explicit "
           "invocation targets.";
}

std::vector<daw::ai::ToolSpec> LuaAiWorkspace::tools(daw::ai::InteractionMode mode) const {
    return toolSpecs(mode);
}
std::vector<daw::ai::ToolSpec> LuaAiWorkspace::toolSpecs(daw::ai::InteractionMode mode) {
    using Effect = daw::ai::ToolSpec::Effect;
    std::vector<daw::ai::ToolSpec> result;
    const auto add = [&](const char *name, const char *description, Json properties, Json required,
                         Effect effect) {
        daw::ai::ToolSpec spec{
            name,
            description,
            {{"type", "object"}, {"properties", properties}, {"required", required}},
            effect};
        if (daw::ai::toolAllowedInMode(spec, mode))
            result.push_back(spec);
    };
    add("lua_context",
        "Read Creator metadata, file sizes and small sources. Use lua_read for large files; binary "
        "resources are indexed, not embedded.",
        Json::object(), Json::array(), Effect::ReadOnly);
    add("lua_read", "Read up to 400 lines of a Creator source or manifest.json.",
        {{"path", {{"type", "string"}}},
         {"line", {{"type", "integer"}}},
         {"count", {{"type", "integer"}}}},
        {"path"}, Effect::ReadOnly);
    add("lua_example", "List bundled examples, or read one by name.",
        {{"name", {{"type", "string"}}}}, Json::array(), Effect::ReadOnly);
    add("lua_sdk", "Read the public SDK contract.", Json::object(), Json::array(),
        Effect::ReadOnly);
    add("lua_edit",
        "Replace one source or manifest.json; expected revision prevents overwriting user changes.",
        {{"path", {{"type", "string"}}},
         {"source", {{"type", "string"}}},
         {"revision", {{"type", "integer"}}}},
        {"path", "source", "revision"}, Effect::ReversibleEdit);
    add("lua_check", "Validate package and compile every source without execution.", Json::object(),
        Json::array(), Effect::ReadOnly);
    add("lua_preview", "Run a command in the isolated preview project (default: selected command).",
        {{"action", {{"type", "string"}}}}, Json::array(), Effect::ReversibleEdit);
    add("lua_status", "Read preview results, diagnostics and log.", Json::object(), Json::array(),
        Effect::ReadOnly);
    add("lua_stop", "Stop the preview instance.", Json::object(), Json::array(),
        Effect::LiveControl);
    add("lua_install", "Validate and install the current extension without running it.",
        Json::object(), Json::array(), Effect::ExternalSideEffect);
    return result;
}

std::optional<daw::ai::ToolResult> LuaAiWorkspace::execute(const daw::ai::ToolCall &call,
                                                           const daw::ai::ToolContext &context,
                                                           std::uint64_t) {
    try {
        const auto allowed = tools(context.mode);
        if (std::none_of(allowed.begin(), allowed.end(),
                         [&](const auto &t) { return t.name == call.name; }))
            throw std::runtime_error("Tool is unavailable in this mode");
        Json result;
        if (call.name == "lua_context") {
            auto document = m_window.document();
            Json files = Json::array();
            size_t budget = 0;
            for (const auto &[path, source] : document["sources"].items())
                files.push_back(
                    {{"path", path}, {"bytes", source.get_ref<const std::string &>().size()}});
            for (auto &[path, source] : document["sources"].items()) {
                budget += source.get_ref<const std::string &>().size();
                if (budget > 65536)
                    source = "Use lua_read to read this source";
            }
            for (auto &[path, data] : document["resources"].items())
                data = {{"encodedBytes", data.get_ref<const std::string &>().size()}};
            result = {{"revision", revision()}, {"document", document}, {"files", files}};
        } else if (call.name == "lua_read") {
            const auto path = call.args.at("path").get<std::string>();
            const auto document = m_window.document();
            const auto text = path == "manifest.json"
                                  ? document.at("manifestSource").get<std::string>()
                                  : document.at("sources").at(path).get<std::string>();
            const auto lines = QString::fromStdString(text).split('\n');
            const int start = std::clamp(call.args.value("line", 1), 1,
                                         std::max(1, int(lines.size()))) -
                              1,
                      count = std::clamp(call.args.value("count", 120), 1, 400);
            const auto page = lines.mid(start, count).join('\n');
            if (page.toUtf8().size() > 65536)
                throw std::runtime_error(
                    "Source lines exceed 64 KiB; format the source before reading");
            result = {{"revision", revision()},
                      {"path", path},
                      {"line", start + 1},
                      {"totalLines", lines.size()},
                      {"text", page.toStdString()}};
        } else if (call.name == "lua_example") {
            const QDir examples(QCoreApplication::applicationDirPath() + "/ScriptSDK/examples");
            QStringList names;
            for (const auto &entry : examples.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name))
                if (QFileInfo(examples.filePath(entry + "/manifest.json")).isFile())
                    names.append(entry);
            const auto name = QString::fromStdString(call.args.value("name", ""));
            if (name.isEmpty()) {
                result = Json::array();
                for (const auto &n : names)
                    result.push_back(n.toStdString());
            } else {
                if (!names.contains(name))
                    throw std::runtime_error("Unknown SDK example");
                ScriptPackage example;
                QString error;
                if (!ScriptLibrary::readDirectory(QCoreApplication::applicationDirPath() +
                                                      "/ScriptSDK/examples/" + name,
                                                  example, error))
                    throw std::runtime_error(error.toStdString());
                result = {{"manifest", example.manifest}, {"sources", example.sources}};
            }
        } else if (call.name == "lua_sdk")
            result = vlt::script::apiCatalog();
        else if (call.name == "lua_edit") {
            if (call.args.at("revision").get<std::uint64_t>() != revision())
                throw std::runtime_error("Creator changed; reread context");
            const auto path = call.args.at("path").get<std::string>();
            if (!vlt::script::safeRelativePath(path) ||
                (path != "manifest.json" && !path.ends_with(".lua")))
                throw std::runtime_error("Invalid source path");
            const auto source = call.args.at("source").get<std::string>();
            if (source.size() > 1024 * 1024)
                throw std::runtime_error("Source exceeds limit");
            auto document = m_window.document();
            if (path == "manifest.json")
                document["manifestSource"] = source;
            else
                document["sources"][path] = source;
            m_window.editDocument(document, "AI: edit Lua extension");
            result = {{"revision", revision()}};
        } else if (call.name == "lua_check") {
            QString error;
            const bool ok = m_window.check(error);
            result = {{"valid", ok}, {"error", error.toStdString()}};
        } else if (call.name == "lua_preview") {
            const auto id =
                m_window.execute(true, QString::fromStdString(call.args.value("action", "")));
            if (id.isEmpty())
                throw std::runtime_error("Validation failed; call lua_check");
            result = {{"run", id.toStdString()}};
        } else if (call.name == "lua_status")
            result = m_window.service()->status(m_window.m_run);
        else if (call.name == "lua_stop") {
            cancel();
            result = true;
        } else if (call.name == "lua_install") {
            QString error;
            if (!m_window.install(error))
                throw std::runtime_error(error.toStdString());
            result = true;
        }
        return daw::ai::ToolResult{true, result, {}};
    } catch (const std::exception &e) {
        return daw::ai::ToolResult{false, {}, e.what()};
    }
}

void LuaAiWorkspace::cancel() {
    m_window.service()->stop(m_window.m_run);
}
} // namespace ui
