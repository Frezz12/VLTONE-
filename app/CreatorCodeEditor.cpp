#include "CreatorCodeEditor.hpp"
#include "CreatorStyle.hpp"
#include "Creator/CodeUtilities.hpp"
#include "Theme.hpp"
#include <QAbstractItemView>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSyntaxHighlighter>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolBar>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <set>

namespace ui {
using namespace daw::plugins::mini;
namespace {
class Highlight final : public QSyntaxHighlighter {
public:
  explicit Highlight(QTextDocument *document) : QSyntaxHighlighter(document) {}
  void highlightBlock(const QString &text) override {
    const auto color = [&](const QString &pattern, const QColor &c) {
      QTextCharFormat format;
      format.setForeground(c);
      auto matches = QRegularExpression(pattern).globalMatch(text);
      while (matches.hasNext()) {
        const auto m = matches.next();
        setFormat(m.capturedStart(), m.capturedLength(), format);
      }
    };
    const bool dark = th().dark;
    color(QStringLiteral(
              "\\b(auto|bool|break|case|class|const|constexpr|continue|"
              "decltype|double|else|enum|false|float|for|if|inline|int|long|"
              "namespace|noexcept|nullptr|private|public|return|short|signed|"
              "sizeof|static|struct|switch|template|true|typename|unsigned|"
              "using|void|volatile|while)\\b"),
          dark ? QColor(202, 158, 241) : QColor(115, 57, 152));
    color(QStringLiteral("\\b(AudioFrame|Context|PrepareContext|Function|"
                         "Buffer|Random|VLT_PORT|VLT_NODE)\\b"),
          dark ? QColor(94, 195, 190) : QColor(0, 109, 111));
    color(QStringLiteral(
              "\\b[0-9]+(?:\\.[0-9]*)?(?:[eE][+-]?[0-9]+)?[fFuUlL]*\\b"),
          dark ? QColor(223, 180, 109) : QColor(138, 85, 9));
    color(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\"|'(?:[^'\\\\]|\\\\.)*'"),
          dark ? QColor(164, 202, 127) : QColor(66, 116, 34));
    color(QStringLiteral("//[^\\n]*"), creatorColors().muted);
    setCurrentBlockState(0);
    int start = previousBlockState() == 1 ? 0 : text.indexOf("/*");
    while (start >= 0) {
      const int end = text.indexOf("*/", start + 2);
      if (end < 0) {
        setFormat(start, text.size() - start, creatorColors().muted);
        setCurrentBlockState(1);
        break;
      }
      setFormat(start, end - start + 2, creatorColors().muted);
      start = text.indexOf("/*", end + 2);
    }
  }
};
class Numbers final : public QWidget {
public:
  explicit Numbers(CreatorCodeText *editor) : QWidget(editor), editor(editor) {}
  void paintEvent(QPaintEvent *e) override { editor->paintNumbers(e); }
  CreatorCodeText *editor;
};
} // namespace
CreatorCodeText::CreatorCodeText(QWidget *parent)
    : QPlainTextEdit(parent), m_numbers(new Numbers(this)) {
  setObjectName("CreatorCppSource");
  setAccessibleName(tr("C++ source"));
  setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  setLineWrapMode(NoWrap);
  setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
  setUndoRedoEnabled(
      false); // Creator's project history includes source and port edits.
  auto *highlight = new Highlight(document());
  connect(&ThemeManager::instance(), &ThemeManager::changed, this,
          [this, highlight] {
            highlight->rehighlight();
            m_numbers->update();
          });
  connect(this, &QPlainTextEdit::blockCountChanged, this,
          [this] { updateGutter(); });
  connect(this, &QPlainTextEdit::updateRequest, this,
          [this](QRect rect, int dy) {
            if (dy)
              m_numbers->scroll(0, dy);
            else
              m_numbers->update(0, rect.y(), m_numbers->width(), rect.height());
          });
  const QStringList sdk = {"vlt::AudioFrame",
                           "vlt::Context",
                           "vlt::PrepareContext",
                           "vlt::Function<float(float)>",
                           "vlt::Buffer<float>",
                           "vlt::Random",
                           "vlt::tanh",
                           "vlt::sin",
                           "vlt::cos",
                           "vlt::exp",
                           "vlt::clamp",
                           "vlt::mix",
                           "VLT_PORT",
                           "VLT_NODE",
                           "sampleRate",
                           "sampleTime",
                           "maxBlockSize",
                           "prepare",
                           "reset"};
  m_completer = new QCompleter(sdk, this);
  m_completer->setWidget(this);
  m_completer->setCaseSensitivity(Qt::CaseInsensitive);
  m_completer->setCompletionMode(QCompleter::PopupCompletion);
  connect(m_completer, qOverload<const QString &>(&QCompleter::activated), this,
          [this](const QString &text) {
            auto c = textCursor();
            c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor,
                           m_completer->completionPrefix().size());
            c.insertText(text);
            setTextCursor(c);
          });
  updateGutter();
}
void CreatorCodeText::updateGutter() {
  int digits = 1;
  for (int n = blockCount(); n >= 10; n /= 10)
    ++digits;
  const int width = 18 + fontMetrics().horizontalAdvance('9') * digits;
  setViewportMargins(width, 0, 0, 0);
  m_numbers->setGeometry(0, contentsRect().top(), width,
                         contentsRect().height());
}
void CreatorCodeText::resizeEvent(QResizeEvent *e) {
  QPlainTextEdit::resizeEvent(e);
  updateGutter();
}
void CreatorCodeText::paintNumbers(QPaintEvent *) {
  QPainter p(m_numbers);
  p.fillRect(m_numbers->rect(), creatorColors().panel);
  p.setFont(font());
  auto block = firstVisibleBlock();
  int top =
      qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
  while (block.isValid() && top < m_numbers->height()) {
    const int height = qRound(blockBoundingRect(block).height());
    if (block.isVisible()) {
      p.setPen(m_errors.contains(unsigned(block.blockNumber() + 1))
                   ? QColor(217, 87, 91)
                   : creatorColors().muted);
      p.drawText(0, top, m_numbers->width() - 8, height,
                 Qt::AlignRight | Qt::AlignVCenter,
                 QString::number(block.blockNumber() + 1));
    }
    top += height;
    block = block.next();
  }
}
void CreatorCodeText::markErrors(const QList<unsigned> &lines) {
  m_errors = lines;
  m_numbers->update();
}
void CreatorCodeText::reveal(unsigned line, unsigned column) {
  const auto block = document()->findBlockByNumber(std::max(1u, line) - 1);
  if (!block.isValid())
    return;
  QTextCursor cursor(block);
  cursor.movePosition(
      QTextCursor::Right, QTextCursor::MoveAnchor,
      std::min(int(std::max(1u, column) - 1), block.length() - 1));
  setTextCursor(cursor);
  centerCursor();
  setFocus();
  markErrors({line});
}
void CreatorCodeText::keyPressEvent(QKeyEvent *e) {
  if (e->matches(QKeySequence::Undo)) {
    emit undoRequested();
    e->accept();
    return;
  }
  if (e->matches(QKeySequence::Redo)) {
    emit redoRequested();
    e->accept();
    return;
  }
  if (m_completer->popup()->isVisible() &&
      (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter ||
       e->key() == Qt::Key_Escape || e->key() == Qt::Key_Tab)) {
    e->ignore();
    return;
  }
  if (e->key() == Qt::Key_Space &&
      e->modifiers().testFlag(Qt::ControlModifier)) {
    const auto before =
        textCursor().block().text().left(textCursor().positionInBlock());
    const auto prefix =
        QRegularExpression(QStringLiteral("[A-Za-z_][A-Za-z0-9_:]*$"))
            .match(before)
            .captured();
    m_completer->setCompletionPrefix(prefix);
    auto rect = cursorRect();
    rect.setWidth(300);
    m_completer->complete(rect);
    e->accept();
    return;
  }
  if (e->key() == Qt::Key_Tab && e->modifiers() == Qt::NoModifier) {
    insertPlainText("    ");
    return;
  }
  QPlainTextEdit::keyPressEvent(e);
}
CreatorCodeEditor::CreatorCodeEditor(QWidget *parent) : QWidget(parent) {
  setObjectName("CreatorCodeEditor");
  setMinimumHeight(180);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(4);
  m_toolbar = new QToolBar(this);
  m_toolbar->setObjectName("CreatorCodeToolbar");
  m_toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
  m_toolbar->setIconSize({16, 16});
  layout->addWidget(m_toolbar);
  auto *update = m_toolbar->addAction(tr("Update node"));
  creatorIcon(update, icons::Glyph::Reload);
  qobject_cast<QToolButton *>(m_toolbar->widgetForAction(update))->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  update->setToolTip(tr("Check C++ and refresh ports"));
  connect(update, &QAction::triggered, this,
          &CreatorCodeEditor::updateRequested);
  m_toolbar->addSeparator();
  auto *create = m_toolbar->addAction(tr("Create function…"));
  creatorIcon(create, icons::Glyph::Plus);
  connect(create, &QAction::triggered,
          this, &CreatorCodeEditor::createRequested);
  auto *extract = m_toolbar->addAction(tr("Extract function…"));
  creatorIcon(extract, icons::Glyph::Send);
  connect(extract, &QAction::triggered,
          this, &CreatorCodeEditor::extractRequested);
  auto *files = m_toolbar->addAction(tr("Import .cpp…"));
  creatorIcon(files, icons::Glyph::Import);
  auto *exportFile = m_toolbar->addAction(tr("Export .cpp…"));
  creatorIcon(exportFile, icons::Glyph::Export);
  auto *space = new QWidget(m_toolbar);
  space->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  m_toolbar->addWidget(space);
  auto *close = m_toolbar->addAction(tr("Close editor"));
  creatorIcon(close, icons::Glyph::Close);
  connect(close, &QAction::triggered, this, &CreatorCodeEditor::closeRequested);
  auto *row = new QHBoxLayout;
  row->setContentsMargins(8, 0, 8, 0);
  layout->addLayout(row);
  row->addWidget(new QLabel(tr("Entry function"), this));
  m_entry = new QLineEdit(this);
  m_entry->setMaximumWidth(150);
  m_entry->setMinimumWidth(60);
  m_entry->setAccessibleName(tr("Entry function"));
  row->addWidget(m_entry);
  m_state = new QLabel(this);
  m_state->setProperty("creatorRole", "muted");
  m_state->setWordWrap(true);
  m_state->setMinimumWidth(0);
  m_state->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  row->addWidget(m_state, 1);
  m_find = new QLineEdit(this);
  m_find->setPlaceholderText(tr("Find in code · Enter: next · Escape: close"));
  m_find->setAccessibleName(tr("Find in code"));
  m_find->hide();
  layout->addWidget(m_find);
  m_text = new CreatorCodeText(this);
  layout->addWidget(m_text, 1);
  connect(m_text, &CreatorCodeText::undoRequested, this,
          &CreatorCodeEditor::undoRequested);
  connect(m_text, &CreatorCodeText::redoRequested, this,
          &CreatorCodeEditor::redoRequested);
  auto *find = new QShortcut(QKeySequence::Find, this);
  find->setContext(Qt::WidgetWithChildrenShortcut);
  connect(find, &QShortcut::activated, this, [this] {
    m_find->show();
    m_find->setFocus();
    m_find->selectAll();
  });
  auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_find);
  escape->setContext(Qt::WidgetShortcut);
  connect(escape, &QShortcut::activated, this, [this] {
    m_find->hide();
    m_text->setFocus();
  });
  connect(m_find, &QLineEdit::returnPressed, this, [this] {
    if (!m_text->find(m_find->text())) {
      m_text->moveCursor(QTextCursor::Start);
      m_text->find(m_find->text());
    }
  });
  connect(m_text, &QPlainTextEdit::textChanged, this, [this] {
    m_text->markErrors({});
    m_state->setText(tr("Draft · update ports before compiling"));
    emit edited();
  });
  connect(m_entry, &QLineEdit::textEdited, this, &CreatorCodeEditor::edited);
  connect(m_text, &QPlainTextEdit::cursorPositionChanged, this,
          [this] { emit cursorMoved(cursorPosition()); });
  connect(files, &QAction::triggered, this, [this] {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("Import C++ source"), {}, tr("C++ source (*.cpp)"));
    if (path.isEmpty())
      return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) ||
        file.size() > qint64(kMaxFunctionSourceBytes)) {
      QMessageBox::warning(this, tr("Creator"),
                           tr("Cannot read source (maximum 256 KiB)."));
      return;
    }
    m_text->setPlainText(QString::fromUtf8(file.readAll()));
  });
  connect(exportFile, &QAction::triggered, this, [this] {
    auto path = QFileDialog::getSaveFileName(this, tr("Export C++ source"),
                                             entry() + ".cpp",
                                             tr("C++ source (*.cpp)"));
    if (path.isEmpty())
      return;
    if (!path.endsWith(".cpp", Qt::CaseInsensitive))
      path += ".cpp";
    QSaveFile file(path);
    const auto bytes = source().toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit())
      QMessageBox::warning(this, tr("Creator"), file.errorString());
  });
}
void CreatorCodeEditor::setFunction(const FunctionDefinition &function,
                                    int position) {
  const QSignalBlocker a(m_text), b(m_entry);
  if (m_entry->text() != QString::fromStdString(function.entry))
    m_entry->setText(QString::fromStdString(function.entry));
  if (m_text->toPlainText() != QString::fromStdString(function.source))
    m_text->setPlainText(QString::fromStdString(function.source));
  auto cursor = m_text->textCursor();
  cursor.setPosition(
      std::clamp(position, 0, m_text->document()->characterCount() - 1));
  m_text->setTextCursor(cursor);
  m_state->setText(function.analyzedHash == functionSourceHash(function)
                       ? tr("Ports checked")
                       : tr("Draft · update ports before compiling"));
}
QString CreatorCodeEditor::source() const { return m_text->toPlainText(); }
QString CreatorCodeEditor::entry() const { return m_entry->text().trimmed(); }
int CreatorCodeEditor::cursorPosition() const {
  return m_text->textCursor().position();
}
void CreatorCodeEditor::reveal(unsigned line, unsigned column) {
  m_text->reveal(line, column);
}
void CreatorCodeEditor::setBusy(bool busy) { m_toolbar->setEnabled(!busy); }

std::optional<FunctionDefinition> createFunctionDialog(QWidget *parent) {
  const auto connect = [](auto &&...args) {
    return QObject::connect(std::forward<decltype(args)>(args)...);
  };
  QDialog dialog(parent);
  styleCreator(&dialog);
  dialog.setWindowTitle(CreatorCodeEditor::tr("C++ Function"));
  dialog.resize(590, 560);
  auto *layout = new QVBoxLayout(&dialog);
  auto *form = new QFormLayout;
  layout->addLayout(form);
  auto *name = new QLineEdit("process", &dialog);
  name->setAccessibleName(CreatorCodeEditor::tr("Function name"));
  form->addRow(CreatorCodeEditor::tr("Function name"), name);
  auto makeTable = [&](bool input) {
    layout->addWidget(new QLabel(input ? CreatorCodeEditor::tr("Inputs")
                                       : CreatorCodeEditor::tr("Outputs"),
                                 &dialog));
    auto *table = new QTableWidget(0, input ? 3 : 2, &dialog);
    table->setHorizontalHeaderLabels(
        input ? QStringList{CreatorCodeEditor::tr("Name"),
                            CreatorCodeEditor::tr("Type"),
                            CreatorCodeEditor::tr("Default")}
              : QStringList{CreatorCodeEditor::tr("Name"),
                            CreatorCodeEditor::tr("Type")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(36);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table);
    auto *row = new QHBoxLayout;
    auto *add = new QPushButton(CreatorCodeEditor::tr("Add port"), &dialog);
    auto *remove =
        new QPushButton(CreatorCodeEditor::tr("Remove port"), &dialog);
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch();
    layout->addLayout(row);
    const auto append = [table, input] {
      if (table->rowCount() >= int(kMaxFunctionPorts))
        return;
      const int r = table->rowCount();
      table->insertRow(r);
      table->setItem(r, 0,
                     new QTableWidgetItem(r       ? QString("value%1").arg(r)
                                          : input ? "input"
                                                  : "output"));
      auto *type = new QComboBox(table);
      type->addItem("AudioFrame", "audio");
      type->addItem("float", "number");
      type->addItem("bool", "gate");
      if (r)
        type->setCurrentIndex(1);
      table->setCellWidget(r, 1, type);
      if (input) {
        auto *value = new CreatorNumberField(table);
        value->setRange(-1e6, 1e6);
        value->setDecimals(6);
        value->setValue(r ? 1 : 0);
        value->setDefaultValue(r ? 1 : 0);
        table->setCellWidget(r, 2, value);
        QObject::connect(type, qOverload<int>(&QComboBox::currentIndexChanged),
                         value, [value, type] {
                           value->setEnabled(type->currentData() != "audio");
                           value->setRange(
                               type->currentData() == "gate" ? 0 : -1e6,
                               type->currentData() == "gate" ? 1 : 1e6);
                         });
        value->setEnabled(r > 0);
      }
    };
    QObject::connect(add, &QPushButton::clicked, &dialog, append);
    QObject::connect(remove, &QPushButton::clicked, &dialog, [table] {
      if (table->currentRow() >= 0)
        table->removeRow(table->currentRow());
    });
    append();
    return table;
  };
  auto *inputs = makeTable(true);
  auto *outputs = makeTable(false);
  auto *error = new QLabel(&dialog);
  error->setWordWrap(true);
  layout->addWidget(error);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Cancel)
      ->setText(CreatorCodeEditor::tr("Cancel"));
  layout->addWidget(buttons);
  std::optional<FunctionDefinition> result;
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    FunctionDefinition f;
    f.entry = name->text().trimmed().toStdString();
    if (!isCppIdentifier(f.entry) || outputs->rowCount() == 0) {
      error->setText(CreatorCodeEditor::tr(
          "Use a C++ identifier and at least one output."));
      return;
    }
    const auto read = [&](QTableWidget *table, bool input, auto &ports) {
      std::set<std::string> names;
      for (int r = 0; r < table->rowCount(); ++r) {
        FunctionPort p;
        p.name = table->item(r, 0)->text().trimmed().toStdString();
        if (!isCppIdentifier(p.name) || !names.insert(p.name).second)
          return false;
        p.id = "p_" + QUuid::createUuid().toString(QUuid::Id128).toStdString();
        auto *type = qobject_cast<QComboBox *>(table->cellWidget(r, 1));
        p.type = type->currentData().toString().toStdString();
        p.cppType = p.type == "audio"  ? "vlt::AudioFrame"
                    : p.type == "gate" ? "bool"
                                       : "float";
        if (input)
          p.initial =
              qobject_cast<QDoubleSpinBox *>(table->cellWidget(r, 2))->value();
        if (p.type == "gate") {
          p.minimum = 0;
          p.maximum = 1;
          p.initial = p.initial != 0;
        }
        ports.push_back(p);
      }
      return true;
    };
    if (!read(inputs, true, f.inputs) || !read(outputs, false, f.outputs)) {
      error->setText(
          CreatorCodeEditor::tr("Port names must be unique C++ identifiers."));
      return;
    }
    if (f.outputs.size() == 1)
      f.outputs[0].id = "out";
    f.source = functionScaffold(f.entry, f.inputs, f.outputs);
    result = std::move(f);
    dialog.accept();
  });
  dialog.exec();
  return result;
}
} // namespace ui
