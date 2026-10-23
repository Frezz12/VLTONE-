#include "Controls.hpp"
#include "CreatorStyle.hpp"
#include "RackParameterBinding.hpp"
#include "ScriptFocusFrame.hpp"
#include "ScriptRun.hpp"
#include "ScriptService.hpp"
#include "ScriptStyle.hpp"
#include "Theme.hpp"
#include "scripting/ScriptProject.hpp"
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QImageReader>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStyleOption>
#include <QTabWidget>
#include <QTableWidget>
#include <QWheelEvent>
#include <cmath>

namespace ui {
namespace {
using Json = vlt::script::Json;
QString string(const Json &j, const char *key, const char *fallback = "") {
    return QString::fromStdString(j.value(key, fallback));
}
double numeric(const Json &value) {
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        throw std::runtime_error("Expected a finite numeric control value");
    return value.get<double>();
}
int selectionIndex(const Json &value, int count) {
    const double index = numeric(value);
    if (index < 0 || index > count || std::floor(index) != index)
        throw std::runtime_error("Selection index must be 0 (none) or an existing item");
    return int(index) - 1;
}
class ScriptSlider final : public GlassSlider {
    double m_low, m_high;

  public:
    ScriptSlider(double low, double high, QWidget *parent)
        : GlassSlider(Qt::Horizontal, parent), m_low(low), m_high(high) {
        setRange(0, 1000000);
        setFocusPolicy(Qt::StrongFocus);
    }
    double scriptValue() const { return m_low + (m_high - m_low) * value() / 1000000.0; }
    void setScriptValue(double value) {
        setValue(
            int(std::round(std::clamp((value - m_low) / (m_high - m_low), 0.0, 1.0) * 1000000)));
    }
};
void validateDrawing(const Json &drawing, bool waveform) {
    if (!drawing.is_array() || drawing.size() > 10000)
        throw std::runtime_error("Drawing must contain at most 10000 items");
    for (const auto &shape : drawing) {
        if (waveform) {
            if (!shape.is_number() || !std::isfinite(shape.get<double>()))
                throw std::runtime_error("Waveform peaks must be finite numbers");
            continue;
        }
        if (!shape.is_object())
            throw std::runtime_error("Drawing item must be an object");
        const auto type = shape.at("type").get<std::string>();
        if (type != "line" && type != "text" && type != "rect" && type != "ellipse")
            throw std::runtime_error("Unknown drawing item");
        for (const char *key : {"x", "y", "w", "h", "x2", "y2", "width"})
            if (shape.contains(key) &&
                (!shape[key].is_number() || !std::isfinite(shape[key].get<double>()) ||
                 std::abs(shape[key].get<double>()) > 1e6))
                throw std::runtime_error("Invalid drawing coordinate");
        for (const char *key : {"color", "text"})
            if (shape.contains(key) && !shape[key].is_string())
                throw std::runtime_error("Invalid drawing text");
        if (shape.contains("fill") && !shape["fill"].is_boolean())
            throw std::runtime_error("Invalid drawing fill");
    }
}
void validateCallbacks(const Json &node) {
    for (const char *key : {"onClick", "onChange", "onPointer", "onWheel", "onKey", "onResize"})
        if (node.contains(key)) {
            const auto &value = node[key];
            if (!value.is_object() || !value.contains("$callback") ||
                !value["$callback"].is_number_integer() || value["$callback"].get<int>() < 1)
                throw std::runtime_error("UI callbacks must be Lua functions");
        }
}
QStringList textItems(const Json &items) {
    if (!items.is_array() || items.size() > 4096)
        throw std::runtime_error("Expected at most 4096 items");
    QStringList result;
    for (const auto &item : items) {
        if (!item.is_string() || item.get_ref<const std::string &>().size() > 16384)
            throw std::runtime_error("Items must be strings of at most 16 KiB");
        result.append(QString::fromStdString(item.get<std::string>()));
    }
    return result;
}
QList<QStringList> tableData(const Json &columns, const Json &rows) {
    const auto headings = textItems(columns);
    if (headings.isEmpty() || headings.size() > 32 || !rows.is_array() || rows.size() > 1024 ||
        rows.size() * headings.size() > 16384)
        throw std::runtime_error("Table limit: 32 columns, 1024 rows, 16384 cells");
    QList<QStringList> data;
    for (const auto &row : rows) {
        auto cells = textItems(row);
        if (cells.size() > headings.size())
            throw std::runtime_error("Table row exceeds its column count");
        data.append(std::move(cells));
    }
    return data;
}
void setTable(QTableWidget *table, const Json &columns, const Json &rows) {
    const auto headings = textItems(columns);
    const auto data = tableData(columns, rows);
    const QSignalBlocker blocker(table);
    table->clear();
    table->setColumnCount(headings.size());
    table->setHorizontalHeaderLabels(headings);
    table->setRowCount(data.size());
    for (int row = 0; row < data.size(); ++row)
        for (int col = 0; col < data[row].size(); ++col)
            table->setItem(row, col, new QTableWidgetItem(data[row][col]));
}
class ScriptCanvas final : public QWidget {
  public:
    Json drawing = Json::array();
    bool waveform = false;
    bool acceptsPointer = false, acceptsWheel = false;
    unsigned pointerSequence = 0;
    std::function<void(const char *, const Json &, bool)> notify;
    void pointer(QMouseEvent *event, const char *phase) {
        if (!notify || !acceptsPointer) {
            event->ignore();
            return;
        }
        if (std::string_view(phase) != "move")
            ++pointerSequence;
        const auto point = event->position();
        notify("onPointer",
               {{"phase", phase},
                {"x", point.x()},
                {"y", point.y()},
                {"nx", point.x() / std::max(1, width())},
                {"ny", point.y() / std::max(1, height())},
                {"width", width()},
                {"height", height()},
                {"button", int(event->button())},
                {"buttons", int(event->buttons())},
                {"modifiers", int(event->modifiers())}},
               std::string_view(phase) == "move");
        event->accept();
    }
    explicit ScriptCanvas(QWidget *parent) : QWidget(parent) {
        setMinimumSize(200, 100);
        setFocusPolicy(Qt::StrongFocus);
    }

  protected:
    void mousePressEvent(QMouseEvent *e) override {
        setFocus();
        pointer(e, "down");
    }
    void mouseMoveEvent(QMouseEvent *e) override { pointer(e, "move"); }
    void mouseReleaseEvent(QMouseEvent *e) override { pointer(e, "up"); }
    void mouseDoubleClickEvent(QMouseEvent *e) override { pointer(e, "doubleClick"); }
    void wheelEvent(QWheelEvent *e) override {
        if (!notify || !acceptsWheel) {
            e->ignore();
            return;
        }
        notify("onWheel",
               {{"x", e->position().x()},
                {"y", e->position().y()},
                {"dx", e->angleDelta().x()},
                {"dy", e->angleDelta().y()},
                {"modifiers", int(e->modifiers())}},
               false);
        e->accept();
    }
    bool event(QEvent *e) override {
        if (e->type() == QEvent::ShortcutOverride && property("vlt.scriptInput").toBool()) {
            e->accept();
            return true;
        }
        return QWidget::event(e);
    }
    void keyPressEvent(QKeyEvent *e) override {
        if (!property("vlt.scriptInput").toBool()) {
            QWidget::keyPressEvent(e);
            return;
        }
        notify("onKey",
               {{"key", QKeySequence(e->key()).toString(QKeySequence::PortableText).toStdString()},
                {"text", e->text().toStdString()},
                {"code", e->key()},
                {"repeat", e->isAutoRepeat()},
                {"modifiers", int(e->modifiers())}},
               false);
        e->accept();
    }
    void resizeEvent(QResizeEvent *e) override {
        QWidget::resizeEvent(e);
        if (notify)
            notify("onResize", {{"width", width()}, {"height", height()}}, true);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto authoredPalette = property("vlt.scriptPaletteData");
        const auto colors = authoredPalette.isValid() ? authoredPalette.value<QPalette>() : palette();
        const auto localStyle = scriptstyle::read(this);
        const int radius = localStyle.value("radius", 0);
        const int border = localStyle.value("borderWidth", 0);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()).adjusted(border, border, -border, -border), radius,
                            radius);
        QStyleOption option;
        option.initFrom(this);
        style()->drawPrimitive(QStyle::PE_Widget, &option, &p, this);
        p.save();
        p.setClipPath(clip);
        p.fillRect(rect(), colors.base());
        if (waveform) {
            p.setPen(colors.highlight().color());
            const int count = int(drawing.size());
            for (int i = 0; i < count; ++i)
                if (drawing[i].is_number()) {
                    const double peak = std::clamp(drawing[i].get<double>(), 0.0, 1.0);
                    const double x = double(i) * width() / std::max(1, count),
                                 half = height() * .48 * peak;
                    p.drawLine(QPointF(x, height() * .5 - half), QPointF(x, height() * .5 + half));
                }
        } else
            for (const auto &shape : drawing) {
                if (!shape.is_object())
                    continue;
                QColor color(string(shape, "color"));
                if (!color.isValid())
                    color = colors.text().color();
                p.setPen(QPen(color, std::clamp(shape.value("width", 1.0), .1, 20.0)));
                const auto type = shape.value("type", "");
                const double x = shape.value("x", 0.0), y = shape.value("y", 0.0),
                             w = shape.value("w", 0.0), h = shape.value("h", 0.0);
                if (type == "line")
                    p.drawLine(QPointF(x, y),
                               QPointF(shape.value("x2", 0.0), shape.value("y2", 0.0)));
                else if (type == "text")
                    p.drawText(QPointF(x, y), string(shape, "text"));
                else if (type == "rect") {
                    if (shape.value("fill", false))
                        p.fillRect(QRectF(x, y, w, h), color);
                    else
                        p.drawRect(QRectF(x, y, w, h));
                } else if (type == "ellipse") {
                    p.setBrush(shape.value("fill", false) ? QBrush(color) : QBrush(Qt::NoBrush));
                    p.drawEllipse(QRectF(x, y, w, h));
                    p.setBrush(Qt::NoBrush);
                }
            }
        p.restore();
        if (hasFocus()) {
            p.setPen(colors.highlight().color());
            p.drawRect(rect().adjusted(1, 1, -2, -2));
        }
    }
};
} // namespace
ScriptService::Json ScriptService::uiCall(ScriptRun *run, const std::string &method,
                                          const Json &a) {
    if (method == "ui.chooseFile") {
        const auto mode = string(a, "mode", "open");
        QString path;
        if (run->preview) {
            if (mode == "folder")
                return run->temporary.filePath(a.value("write", false) ? "output" : "input")
                    .toStdString();
            return run->temporary
                .filePath(mode == "save" ? "output/preview-output.wav" : "input/preview.wav")
                .toStdString();
        }
        if (mode == "folder")
            path = QFileDialog::getExistingDirectory(m_window, tr("Choose folder"));
        else if (mode == "save")
            path = QFileDialog::getSaveFileName(m_window, tr("Choose output file"), {},
                                                string(a, "filter", "All files (*)"));
        else
            path = QFileDialog::getOpenFileName(m_window, tr("Choose input file"), {},
                                                string(a, "filter", "All files (*)"));
        if (path.isEmpty())
            return nullptr;
        grantPath(run, path, mode == "save" || a.value("write", false), mode == "folder");
        return path.toStdString();
    }
    // Closed windows leave QPointers null; background tools may create new
    // windows indefinitely.
    run->widgets.removeIf([](auto it) { return it.value().isNull(); });
    run->bindings.removeIf([](const auto &binding) { return binding.isNull(); });
    const auto id = string(a, "id", "main");
    if (method == "ui.close") {
        if (auto widget = run->windows.value(id))
            widget->close();
        return true;
    }
    if (method == "ui.get" || method == "ui.focus") {
        auto *widget = run->widgets.value(id, run->windows.value(id)).data();
        if (!widget)
            throw std::runtime_error("Widget no longer exists");
        if (method == "ui.focus") {
            // A docked control's QWidget::window() is the DAW, not its owning panel.
            for (auto panel : run->windows)
                if (panel && (panel == widget || panel->isAncestorOf(widget))) {
                    panel->show();
                    panel->raise();
                    break;
                }
            widget->window()->show();
            widget->window()->raise();
            widget->window()->activateWindow();
            widget->setFocus(Qt::OtherFocusReason);
            return true;
        }
        Json state{{"id", id.toStdString()},
                   {"type", widget->property("vlt.type").toString().toStdString()},
                   {"enabled", widget->isEnabled()},
                   {"visible", widget->isVisible()},
                   {"width", widget->width()},
                   {"height", widget->height()}};
        state["style"] = scriptstyle::read(
            run->windowContents.contains(id) ? run->windowContents.value(id).data() : widget);
        if (auto *v = qobject_cast<QLineEdit *>(widget))
            state["value"] = v->text().toStdString();
        else if (auto *v = qobject_cast<QPlainTextEdit *>(widget))
            state["value"] = v->toPlainText().toStdString();
        else if (auto *v = qobject_cast<QDoubleSpinBox *>(widget))
            state["value"] = v->value();
        else if (auto *v = qobject_cast<Knob *>(widget))
            state["value"] = v->value();
        else if (auto *v = dynamic_cast<ScriptSlider *>(widget))
            state["value"] = v->scriptValue();
        else if (auto *v = qobject_cast<QSlider *>(widget))
            state["value"] = v->value();
        else if (auto *v = qobject_cast<QCheckBox *>(widget))
            state["value"] = v->isChecked();
        else if (auto *v = qobject_cast<QProgressBar *>(widget))
            state["value"] = v->value() / 1000.0;
        else if (auto *v = qobject_cast<QComboBox *>(widget))
            state["value"] = v->currentIndex() + 1;
        else if (auto *v = qobject_cast<QListWidget *>(widget))
            state["value"] = v->currentRow() + 1;
        else if (auto *v = qobject_cast<QTabWidget *>(widget))
            state["value"] = v->currentIndex() + 1;
        else if (auto *v = qobject_cast<QTableWidget *>(widget)) {
            state["value"] = v->currentRow() + 1;
            state["rows"] = Json::array();
            for (int row = 0; row < v->rowCount(); ++row) {
                Json cells = Json::array();
                for (int col = 0; col < v->columnCount(); ++col)
                    cells.push_back(v->item(row, col) ? v->item(row, col)->text().toStdString()
                                                      : "");
                state["rows"].push_back(std::move(cells));
            }
        }
        if (auto *v = qobject_cast<QLabel *>(widget))
            state["text"] = v->text().toStdString();
        if (auto *v = qobject_cast<QAbstractButton *>(widget))
            state["text"] = v->text().toStdString();
        if (widget->property("vlt.bindingAddress").isValid()) {
            authorize(run, *vlt::script::apiMethod("parameters.get"));
            state["value"] = parameterCall(
                run, "parameters.get",
                Json::parse(widget->property("vlt.bindingAddress").toByteArray().toStdString()));
            if (widget->property("vlt.type").toString() == "toggle")
                state["value"] = state["value"].get<double>() >= .5;
        }
        return state;
    }
    if (method == "ui.set") {
        auto *widget = run->widgets.value(id, run->windows.value(id)).data();
        if (!widget)
            throw std::runtime_error("Widget no longer exists");
        const auto validation = vlt::script::validateArguments(*vlt::script::apiMethod(method), a);
        if (!validation.empty())
            throw std::runtime_error(validation);
        for (const char *key : {"width", "height"})
            if (a.contains(key) && (a[key].get<double>() < 0 || a[key].get<double>() > 1600))
                throw std::runtime_error(std::string("Invalid widget ") + key);
        QWidget *window = nullptr, *content = nullptr;
        for (auto it = run->windows.cbegin(); it != run->windows.cend(); ++it)
            if (it.value() && (it.value() == widget || it.value()->isAncestorOf(widget))) {
                window = it.value();
                content = run->windowContents.value(it.key());
                break;
            }
        if (!content)
            throw std::runtime_error("Window no longer exists");
        auto *styled = widget == window ? content : widget;
        const auto styles =
            Json::parse(content->property("vlt.styles").toByteArray().toStdString());
        const auto style =
            a.contains("style") ? scriptstyle::resolve(a["style"], styles) : Json::object();
        if (a.contains("theme")) {
            if (widget != window)
                throw std::runtime_error("theme applies to a window");
            scriptstyle::validateTheme(a["theme"]);
        }
        if (a.contains("title") && widget != window)
            throw std::runtime_error("title applies to a window");
        if (a.contains("text") && !qobject_cast<QLabel *>(widget) &&
            !qobject_cast<QLineEdit *>(widget) && !qobject_cast<QAbstractButton *>(widget) &&
            !qobject_cast<QPlainTextEdit *>(widget))
            throw std::runtime_error("text requires a label, button or text field");
        if (a.contains("items")) {
            textItems(a["items"]);
            if (!qobject_cast<QComboBox *>(widget) && !qobject_cast<QListWidget *>(widget))
                throw std::runtime_error("items applies to choice and list");
        }
        Json columns = Json::array(), rows = Json::array();
        if (a.contains("rows") || a.contains("columns")) {
            auto *table = qobject_cast<QTableWidget *>(widget);
            if (!table)
                throw std::runtime_error("rows/columns applies to table");
            for (int col = 0; col < table->columnCount(); ++col)
                columns.push_back(table->horizontalHeaderItem(col)->text().toStdString());
            for (int row = 0; row < table->rowCount(); ++row) {
                Json cells = Json::array();
                for (int col = 0; col < table->columnCount(); ++col)
                    cells.push_back(
                        table->item(row, col) ? table->item(row, col)->text().toStdString() : "");
                rows.push_back(std::move(cells));
            }
            columns = a.value("columns", columns);
            rows = a.value("rows", rows);
            tableData(columns, rows);
        }
        if (a.contains("drawing")) {
            auto *canvas = dynamic_cast<ScriptCanvas *>(widget);
            if (!canvas)
                throw std::runtime_error("drawing applies to canvas and waveform");
            validateDrawing(a["drawing"], canvas->waveform);
        }
        const bool bound = widget->property("vlt.bindingAddress").isValid();
        if (a.contains("value")) {
            const auto &value = a["value"];
            if (widget->property("vlt.type").toString() == "toggle") {
                if (!value.is_boolean())
                    throw std::runtime_error("Toggle value must be boolean");
            } else if (bound) {
                numeric(value);
            } else if (qobject_cast<QLineEdit *>(widget) ||
                       qobject_cast<QPlainTextEdit *>(widget)) {
                if (!value.is_string())
                    throw std::runtime_error("Field value must be text");
            } else if (qobject_cast<QSlider *>(widget) || qobject_cast<QDoubleSpinBox *>(widget) ||
                       qobject_cast<Knob *>(widget) || qobject_cast<QProgressBar *>(widget))
                numeric(value);
            else if (auto *v = qobject_cast<QTabWidget *>(widget))
                selectionIndex(value, v->count());
            else if (auto *v = qobject_cast<QTableWidget *>(widget))
                selectionIndex(value, a.contains("rows") ? int(rows.size()) : v->rowCount());
            else if (auto *v = qobject_cast<QComboBox *>(widget))
                selectionIndex(value, a.contains("items") ? int(a["items"].size()) : v->count());
            else if (auto *v = qobject_cast<QListWidget *>(widget))
                selectionIndex(value, a.contains("items") ? int(a["items"].size()) : v->count());
            else
                throw std::runtime_error("Widget has no value");
        }
        // Validate the entire update before touching live widgets. A denied project
        // edit must not leave cosmetic changes behind either.
        if (a.contains("value") && bound) {
            authorize(run, *vlt::script::apiMethod("parameters.set"));
            auto address =
                Json::parse(widget->property("vlt.bindingAddress").toByteArray().toStdString());
            address["value"] =
                a["value"].is_boolean() ? Json(a["value"].get<bool>() ? 1.0 : 0.0) : a["value"];
            parameterCall(run, "parameters.set", address);
            for (auto binding : run->bindings)
                if (binding)
                    binding->refresh();
            if (!run->preview)
                m_bindings.edited(run->projectId);
        }
        const QSignalBlocker blocker(widget);
        if (a.contains("items")) {
            const auto items = textItems(a["items"]);
            if (auto *v = qobject_cast<QComboBox *>(widget)) {
                v->clear();
                v->addItems(items);
            } else if (auto *v = qobject_cast<QListWidget *>(widget)) {
                v->clear();
                v->addItems(items);
            } else
                throw std::runtime_error("items applies to choice and list");
        }
        if (a.contains("rows") || a.contains("columns")) {
            auto *table = qobject_cast<QTableWidget *>(widget);
            setTable(table, columns, rows);
        }
        if (a.contains("tooltip"))
            widget->setToolTip(string(a, "tooltip"));
        if (a.contains("title") && run->windows.value(id) == widget)
            widget->setWindowTitle(string(a, "title") + " — " + run->projectName +
                                   (run->preview ? tr(" (Preview)") : ""));
        if (widget == window) {
            if (a.contains("width") || a.contains("height"))
                widget->resize(std::clamp(a.value("width", widget->width()), 24, 1600),
                               std::clamp(a.value("height", widget->height()), 24, 1200));
        } else {
            if (a.contains("width")) {
                widget->setProperty("vlt.fixedWidth", a["width"].get<int>());
                widget->setFixedWidth(a["width"].get<int>());
            }
            if (a.contains("height")) {
                widget->setProperty("vlt.fixedHeight", a["height"].get<int>());
                widget->setFixedHeight(a["height"].get<int>());
            }
        }
        auto *presentation =
            widget->property("vlt.labelled").toBool() ? widget->parentWidget() : widget;
        if (a.contains("enabled"))
            presentation->setEnabled(a["enabled"]);
        if (a.contains("visible"))
            presentation->setVisible(a["visible"]);
        if (a.contains("text")) {
            const auto text = string(a, "text");
            if (auto *v = qobject_cast<QLabel *>(widget))
                v->setText(text);
            if (auto *v = qobject_cast<QLineEdit *>(widget))
                v->setText(text);
            if (auto *v = qobject_cast<QAbstractButton *>(widget))
                v->setText(text);
            if (auto *v = qobject_cast<QPlainTextEdit *>(widget))
                v->setPlainText(text);
        }
        if (a.contains("value") && !bound) {
            if (auto *v = dynamic_cast<ScriptSlider *>(widget))
                v->setScriptValue(numeric(a["value"]));
            else if (auto *v = qobject_cast<QDoubleSpinBox *>(widget))
                v->setValue(numeric(a["value"]));
            else if (auto *v = qobject_cast<Knob *>(widget))
                v->setValue(numeric(a["value"]));
            else if (auto *v = qobject_cast<QProgressBar *>(widget))
                v->setValue(int(std::clamp(numeric(a["value"]), 0.0, 1.0) * 1000));
            else if (auto *v = qobject_cast<QCheckBox *>(widget))
                v->setChecked(a["value"].get<bool>());
            else if (auto *v = qobject_cast<QLineEdit *>(widget))
                v->setText(QString::fromStdString(a["value"].get<std::string>()));
            else if (auto *v = qobject_cast<QPlainTextEdit *>(widget))
                v->setPlainText(QString::fromStdString(a["value"].get<std::string>()));
            else if (auto *v = qobject_cast<QTabWidget *>(widget))
                v->setCurrentIndex(selectionIndex(a["value"], v->count()));
            else if (auto *v = qobject_cast<QTableWidget *>(widget))
                v->setCurrentCell(selectionIndex(a["value"], v->rowCount()), 0);
            else if (auto *v = qobject_cast<QComboBox *>(widget))
                v->setCurrentIndex(selectionIndex(a["value"], v->count()));
            else if (auto *v = qobject_cast<QListWidget *>(widget))
                v->setCurrentRow(selectionIndex(a["value"], v->count()));
        }
        if (auto *canvas = dynamic_cast<ScriptCanvas *>(widget); canvas && a.contains("drawing")) {
            canvas->drawing = a["drawing"];
            canvas->update();
        }
        if (a.contains("style"))
            scriptstyle::remember(styled, style);
        if (a.contains("theme"))
            content->setProperty("vlt.theme", QByteArray::fromStdString(a["theme"].dump()));
        if (a.contains("style") || a.contains("theme"))
            scriptstyle::applyWindow(window, content);
        return true;
    }
    if (method != "ui.window" && method != "ui.replace")
        throw std::runtime_error("Unknown UI method");
    auto *existing = run->windows.value(id).data();
    if (method == "ui.window" && existing) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return id.toStdString();
    }
    if (method == "ui.replace" && !existing)
        throw std::runtime_error("Window no longer exists");
    if (!existing && run->windows.size() >= 16)
        throw std::runtime_error("Window limit reached");
    const auto dock = string(a, "dock", "float");
    if (!QStringList{"float", "left", "right", "top", "bottom"}.contains(dock))
        throw std::runtime_error("Unknown dock edge");
    QHash<QString, QPointer<QWidget>> widgets;
    QList<int> callbacks;
    if (run->widgets.value(id))
        throw std::runtime_error("Window id conflicts with a widget id");
    auto *oldContent = run->windowContents.value(id).data();
    const auto previous = [&](const char *property) {
        return oldContent ? Json::parse(oldContent->property(property).toByteArray().toStdString())
                          : Json::object();
    };
    const auto styles = a.value("styles", previous("vlt.styles"));
    const auto theme = a.value("theme", previous("vlt.theme"));
    scriptstyle::validateDefinitions(styles);
    scriptstyle::validateTheme(theme);
    const auto style = scriptstyle::resolve(
        a.value("style", oldContent ? scriptstyle::read(oldContent) : Json::object()), styles);
    std::unique_ptr<QWidget> content(
        buildScriptContent(run, a.at("children"), styles, widgets, callbacks));
    content->setProperty("vlt.theme", QByteArray::fromStdString(theme.dump()));
    content->setProperty("vlt.styles", QByteArray::fromStdString(styles.dump()));
    scriptstyle::remember(content.get(), style);
    for (auto it = widgets.cbegin(); it != widgets.cend(); ++it) {
        if (it.key() == id || run->windows.contains(it.key()))
            throw std::runtime_error("Widget id conflicts with a window id");
        auto *old = run->widgets.value(it.key()).data();
        if (old && (!oldContent || !oldContent->isAncestorOf(old)))
            throw std::runtime_error("Duplicate widget id in another window");
    }
    std::unique_ptr<QWidget> window;
    if (!existing) {
        if (dock != "float" && !run->preview) {
            auto *panel = new QDockWidget(m_window);
            panel->setAllowedAreas(Qt::AllDockWidgetAreas);
            window.reset(panel);
        } else {
            window = std::make_unique<QWidget>(m_window, Qt::Tool);
            new QVBoxLayout(window.get());
            window->layout()->setContentsMargins(0, 0, 0, 0);
        }
        existing = window.get();
        existing->setAttribute(Qt::WA_DeleteOnClose);
        existing->setObjectName("ScriptWindow." + run->id + "." + id);
        existing->setProperty("vlt.projectId", run->projectId);
        existing->setProperty("vlt.type", "window");
        existing->setWindowTitle(string(a, "title") + " — " + run->projectName +
                                 (run->preview ? tr(" (Preview)") : ""));
        existing->resize(std::clamp(a.value("width", 560), 240, 1600),
                         std::clamp(a.value("height", 400), 120, 1200));
    }
    // Build and validate the entire replacement before destroying live controls.
    if (oldContent) {
        run->widgets.removeIf(
            [&](auto it) { return it.value() && oldContent->isAncestorOf(it.value()); });
        const auto oldCallbacks = run->windowCallbacks.value(id);
        run->pending.removeIf([&](const auto &e) { return oldCallbacks.contains(e.callback); });
        if (!qobject_cast<QDockWidget *>(existing))
            existing->layout()->removeWidget(oldContent);
    }
    auto *installed = content.release();
    if (auto *panel = qobject_cast<QDockWidget *>(existing))
        panel->setWidget(installed);
    else
        existing->layout()->addWidget(installed);
    delete oldContent;
    for (auto it = widgets.cbegin(); it != widgets.cend(); ++it)
        run->widgets[it.key()] = it.value();
    run->windowContents[id] = installed;
    run->windowCallbacks[id] = callbacks;
    installed->setProperty("vlt.installed", true);
    scriptstyle::applyWindow(existing, installed);
    if (!window)
        return id.toStdString();
    auto *owned = window.release();
    run->windows[id] = owned;
    run->hadWindow = true;
    ScriptFocusFrame::install(owned);
    connect(&ThemeManager::instance(), &ThemeManager::changed, owned, [owned, run, id] {
        if (auto content = run->windowContents.value(id))
            scriptstyle::applyWindow(owned, content);
    });
    connect(owned, &QObject::destroyed, this, [this, run, id] {
        run->windows.remove(id);
        run->windowContents.remove(id);
        const auto callbacks = run->windowCallbacks.take(id);
        run->pending.removeIf([&](const auto &e) { return callbacks.contains(e.callback); });
        if (!run->complete && run->windows.isEmpty() && !run->background)
            QTimer::singleShot(0, this, [this, alive = run->shared_from_this()] {
                if (!alive->complete && alive->windows.isEmpty() && !alive->background)
                    finish(alive.get(), {}, true);
            });
    });
    if (auto *panel = qobject_cast<QDockWidget *>(owned)) {
        const auto area = dock == "left"    ? Qt::LeftDockWidgetArea
                          : dock == "right" ? Qt::RightDockWidgetArea
                          : dock == "top"   ? Qt::TopDockWidgetArea
                                            : Qt::BottomDockWidgetArea;
        m_window->addDockWidget(area, panel);
    }
    owned->show();
    return id.toStdString();
}
QWidget *ScriptService::buildScriptContent(ScriptRun *run, const Json &children, const Json &styles,
                                           QHash<QString, QPointer<QWidget>> &widgets,
                                           QList<int> &callbacks) {
    auto content = std::make_unique<QWidget>();
    int count = 0;
    if (!children.is_array() || children.size() > 512)
        throw std::runtime_error("Invalid window children");
    const auto connectCallback = [this, run,
                                  root = content.get()](QWidget *widget, const Json &node,
                                                        const char *key, const Json &value) {
        if (!root->property("vlt.installed").toBool() || !node.contains(key))
            return;
        const int callback = node.at(key).at("$callback");
        // Changes may be coalesced; distinct button clicks must all be
        // delivered.
        const auto coalesceKey =
            std::string_view(key) == "onChange" && !qobject_cast<QTableWidget *>(widget)
                ? QString::number(quintptr(widget), 16) + "/" + key
                : QString();
        event(run, callback, {{"id", widget->objectName().toStdString()}, {"value", value}},
              coalesceKey);
    };
    std::function<QWidget *(const Json &, QWidget *, int)> build;
    build = [&](const Json &node, QWidget *parent, int depth) -> QWidget * {
        if (++count > 512 || depth > 16)
            throw std::runtime_error("Widget tree exceeds limit");
        if (!node.is_object())
            throw std::runtime_error("Widget must be an object");
        validateCallbacks(node);
        for (const char *key : {"id", "type", "text", "title", "label", "tooltip", "unit", "path"})
            if (node.contains(key) && !node[key].is_string())
                throw std::runtime_error(std::string("Widget ") + key + " must be text");
        for (const char *key : {"width", "height", "minWidth", "minHeight", "stretch", "spacing",
                                "margin", "columns", "decimals"})
            if (node.contains(key) && key != std::string_view("columns") &&
                (!node[key].is_number_integer() || node[key].get<double>() < 0 ||
                 node[key].get<double>() > 1600))
                throw std::runtime_error(std::string("Invalid widget ") + key);
        for (const char *key : {"enabled", "visible", "readOnly", "editable", "wrap"})
            if (node.contains(key) && !node[key].is_boolean())
                throw std::runtime_error(std::string("Widget ") + key + " must be boolean");
        if (node.contains("children") &&
            (!node["children"].is_array() || node["children"].size() > 512))
            throw std::runtime_error("Invalid widget children");
        for (const char *key : {"onClick", "onChange", "onPointer", "onWheel", "onKey", "onResize"})
            if (node.contains(key))
                callbacks.append(node[key]["$callback"].get<int>());
        const auto type = node.value("type", "column");
        const auto style = scriptstyle::resolve(node.value("style", Json::object()), styles);
        const auto align = string(node, "align", "left");
        if (!QStringList{"left", "center", "right"}.contains(align))
            throw std::runtime_error("align must be left, center or right");
        const auto caption = string(node, "text");
        if (type == "knob" || type == "number" || type == "slider") {
            const double low = numeric(node.value("min", Json(0.0))),
                         high = numeric(node.value("max", Json(1.0)));
            if (low >= high || std::abs(low) > 1e12 || std::abs(high) > 1e12)
                throw std::runtime_error("Invalid control range");
            if (node.contains("value"))
                numeric(node["value"]);
        }
        if (node.contains("items") && (!node["items"].is_array() || node["items"].size() > 4096))
            throw std::runtime_error("Too many control items");
        QWidget *widget = nullptr;
        if (node.contains("bind")) {
            if (type != "knob" && type != "number" && type != "slider" && type != "toggle")
                throw std::runtime_error(
                    "Parameter binding requires knob, number, slider or toggle");
            authorize(run, *vlt::script::apiMethod("parameters.get"));
            const auto &bind = node.at("bind");
            auto *c = controller(run);
            if (!c)
                throw std::runtime_error("Project closed");
            parameterCall(run, "parameters.get", bind);
            auto *binding =
                new RackParameterBinding(c, string(bind, "track"), string(bind, "device"), parent);
            if (run->preview) {
                std::vector<daw::plugins::ParameterInfo> metadata;
                for (const auto &item : parameterCall(run, "parameters.list", bind)) {
                    daw::plugins::ParameterInfo p;
                    p.id = item["id"];
                    p.name = item["name"];
                    p.unit = item["unit"];
                    p.minValue = item["min"];
                    p.maxValue = item["max"];
                    p.defaultValue = item["default"];
                    p.isStepped = item["stepped"];
                    p.isReadOnly = item["readOnly"];
                    metadata.push_back(p);
                }
                binding->setDocumentPreviewParameters(std::move(metadata));
            }
            binding->setEditGuard([this, run, bind] {
                try {
                    authorize(run, *vlt::script::apiMethod("parameters.set"));
                    parameterCall(run, "parameters.get", bind);
                    return true;
                } catch (...) {
                    return false;
                }
            });
            const auto parameter = string(bind, "parameter");
            if (type == "slider")
                widget = binding->slider(parameter, parent);
            else if (type == "number")
                widget = binding->number(parameter, parent);
            else if (type == "toggle")
                widget = binding->toggle(parameter, caption, parent);
            else
                widget = binding->knob(parameter, parent);
            widget->setProperty("vlt.bindingAddress", QByteArray::fromStdString(bind.dump()));
            connect(binding, &RackParameterBinding::edited, this, [=, this] {
                if (!run->preview && !run->complete)
                    m_bindings.edited(run->projectId);
                const double value = binding->value(parameter);
                connectCallback(widget, node, "onChange",
                                type == "toggle" ? Json(value >= .5) : Json(value));
            });
            run->bindings.append(binding);
        } else if (type == "column" || type == "row" || type == "group") {
            widget = type == "group" ? static_cast<QWidget *>(new QGroupBox(caption, parent))
                                     : new QWidget(parent);
            auto *layout = type == "row" ? static_cast<QBoxLayout *>(new QHBoxLayout(widget))
                                         : static_cast<QBoxLayout *>(new QVBoxLayout(widget));
            const int margin = node.value("margin", type == "group" ? 12 : 0);
            layout->setContentsMargins(margin, margin, margin, margin);
            layout->setSpacing(node.value("spacing", 10));
            for (const auto &child : node.value("children", Json::array()))
                layout->addWidget(build(child, widget, depth + 1), child.value("stretch", 0));
        } else if (type == "grid") {
            const auto columns = node.value("columns", Json(2));
            if (!columns.is_number_integer() || columns.get<int>() < 1 || columns.get<int>() > 32)
                throw std::runtime_error("Grid columns must be 1..32");
            widget = new QWidget(parent);
            auto *grid = new QGridLayout(widget);
            const int margin = node.value("margin", 0);
            grid->setContentsMargins(margin, margin, margin, margin);
            grid->setSpacing(node.value("spacing", 10));
            int index = 0;
            for (const auto &child : node.value("children", Json::array())) {
                grid->addWidget(build(child, widget, depth + 1), index / columns.get<int>(),
                                index % columns.get<int>());
                ++index;
            }
        } else if (type == "scroll") {
            auto *scroll = new QScrollArea(parent);
            widget = scroll;
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setWidgetResizable(true);
            scroll->setWidget(build({{"type", "column"},
                                     {"children", node.value("children", Json::array())},
                                     {"margin", node.value("margin", 0)},
                                     {"spacing", node.value("spacing", 10)}},
                                    scroll, depth + 1));
        } else if (type == "splitter") {
            const auto direction = node.value("orientation", "horizontal");
            if (direction != "horizontal" && direction != "vertical")
                throw std::runtime_error("Invalid splitter orientation");
            auto *split =
                new QSplitter(direction == "vertical" ? Qt::Vertical : Qt::Horizontal, parent);
            widget = split;
            split->setChildrenCollapsible(false);
            for (const auto &child : node.value("children", Json::array()))
                split->addWidget(build(child, split, depth + 1));
        } else if (type == "separator") {
            auto *line = new QFrame(parent);
            widget = line;
            line->setFrameShape(node.value("orientation", "horizontal") == "vertical"
                                    ? QFrame::VLine
                                    : QFrame::HLine);
        } else if (type == "spacer") {
            widget = new QWidget(parent);
            widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        } else if (type == "tabs") {
            auto *tabs = new QTabWidget(parent);
            widget = tabs;
            for (const auto &child : node.value("children", Json::array()))
                tabs->addTab(build(child, tabs, depth + 1), string(child, "title"));
            tabs->setCurrentIndex(
                selectionIndex(node.value("value", Json(tabs->count() ? 1 : 0)), tabs->count()));
            connect(tabs, &QTabWidget::currentChanged, this,
                    [=](int index) { connectCallback(tabs, node, "onChange", index + 1); });
        } else if (type == "button") {
            auto *button = new QPushButton(caption, parent);
            widget = button;
            connect(button, &QPushButton::clicked, this,
                    [=] { connectCallback(button, node, "onClick", true); });
        } else if (type == "menu") {
            auto *button = new QPushButton(caption, parent);
            widget = button;
            auto *menu = new QMenu(button);
            button->setMenu(menu);
            for (const auto &item : node.value("items", Json::array())) {
                validateCallbacks(item);
                if (item.contains("onClick"))
                    callbacks.append(item["onClick"]["$callback"].get<int>());
                auto *action = menu->addAction(string(item, "text"));
                connect(action, &QAction::triggered, this,
                        [=] { connectCallback(button, item, "onClick", true); });
            }
        } else if (type == "textEdit") {
            auto *edit = new QPlainTextEdit(parent);
            widget = edit;
            edit->setPlainText(string(node, "value"));
            edit->setPlaceholderText(caption);
            edit->setReadOnly(node.value("readOnly", false));
            edit->setMaximumBlockCount(4096);
            connect(edit, &QPlainTextEdit::textChanged, this, [=] {
                connectCallback(edit, node, "onChange", edit->toPlainText().toStdString());
            });
        } else if (type == "table") {
            auto *table = new QTableWidget(parent);
            widget = table;
            setTable(table, node.at("columns"), node.value("rows", Json::array()));
            table->setSelectionBehavior(QAbstractItemView::SelectRows);
            table->setSelectionMode(QAbstractItemView::SingleSelection);
            table->setEditTriggers(node.value("editable", false)
                                       ? QAbstractItemView::DoubleClicked |
                                             QAbstractItemView::EditKeyPressed
                                       : QAbstractItemView::NoEditTriggers);
            table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
            table->verticalHeader()->hide();
            connect(table, &QTableWidget::cellChanged, this, [=](int row, int col) {
                connectCallback(table, node, "onChange",
                                {{"row", row + 1},
                                 {"column", col + 1},
                                 {"text", table->item(row, col)->text().toStdString()}});
            });
            connect(table, &QTableWidget::cellClicked, this, [=](int row, int col) {
                connectCallback(table, node, "onClick", {{"row", row + 1}, {"column", col + 1}});
            });
        } else if (type == "image") {
            authorize(run, *vlt::script::apiMethod("files.read"));
            auto path = string(node, "path");
            if (QDir::isRelativePath(path))
                path = QDir(run->package.directory).filePath(path);
            if (!permittedPath(run, path, false))
                throw std::runtime_error("Image requires a granted resource");
            QImageReader reader(path);
            reader.setAutoTransform(true);
            const auto size = reader.size();
            if (QFileInfo(path).size() > 8 * 1024 * 1024 || !size.isValid() ||
                qint64(size.width()) * size.height() > 64 * 1024 * 1024)
                throw std::runtime_error("Image limit: 8 MiB / 64 megapixels");
            reader.setScaledSize(size.scaled(node.value("width", 320), node.value("height", 240),
                                             Qt::KeepAspectRatio));
            const auto image = reader.read();
            if (image.isNull())
                throw std::runtime_error("Cannot decode image");
            auto *label = new QLabel(parent);
            widget = label;
            label->setPixmap(QPixmap::fromImage(image));
            label->setAlignment(Qt::AlignCenter);
        } else if (type == "field") {
            auto *field = new QLineEdit(string(node, "value"), parent);
            widget = field;
            field->setPlaceholderText(caption);
            field->setReadOnly(node.value("readOnly", false));
            connect(field, &QLineEdit::editingFinished, this,
                    [=] { connectCallback(field, node, "onChange", field->text().toStdString()); });
        } else if (type == "toggle") {
            auto *toggle = new QCheckBox(caption, parent);
            widget = toggle;
            toggle->setChecked(node.value("value", false));
            connect(toggle, &QCheckBox::toggled, this,
                    [=](bool value) { connectCallback(toggle, node, "onChange", value); });
        } else if (type == "list") {
            auto *list = new QListWidget(parent);
            widget = list;
            list->addItems(textItems(node.value("items", Json::array())));
            list->setCurrentRow(
                selectionIndex(node.value("value", Json(list->count() ? 1 : 0)), list->count()));
            connect(list, &QListWidget::currentRowChanged, this,
                    [=](int index) { connectCallback(list, node, "onChange", index + 1); });
        } else if (type == "choice") {
            auto *list = new QComboBox(parent);
            widget = list;
            list->addItems(textItems(node.value("items", Json::array())));
            list->setCurrentIndex(
                selectionIndex(node.value("value", Json(list->count() ? 1 : 0)), list->count()));
            connect(list, qOverload<int>(&QComboBox::activated), this,
                    [=](int index) { connectCallback(list, node, "onChange", index + 1); });
        } else if (type == "knob") {
            auto *knob = new Knob(caption, parent);
            widget = knob;
            knob->setFocusPolicy(Qt::StrongFocus);
            knob->setRange(node.value("min", 0.0), node.value("max", 1.0));
            knob->setValue(node.value("value", 0.0));
            connect(knob, &Knob::editFinished, this,
                    [=] { connectCallback(knob, node, "onChange", knob->value()); });
        } else if (type == "slider") {
            auto *slider = new ScriptSlider(node.value("min", 0.0), node.value("max", 1.0), parent);
            widget = slider;
            slider->setScriptValue(node.value("value", 0.0));
            connect(slider, &QSlider::sliderReleased, this,
                    [=] { connectCallback(slider, node, "onChange", slider->scriptValue()); });
            connect(slider, &QSlider::valueChanged, this, [=] {
                if (!slider->isSliderDown())
                    connectCallback(slider, node, "onChange", slider->scriptValue());
            });
        } else if (type == "number") {
            auto *spin = new QDoubleSpinBox(parent);
            widget = spin;
            spin->setRange(node.value("min", 0.0), node.value("max", 1.0));
            spin->setDecimals(std::clamp(node.value("decimals", 2), 0, 8));
            if (node.contains("step")) {
                const auto step = numeric(node["step"]);
                if (step <= 0)
                    throw std::runtime_error("Control step must be positive");
                spin->setSingleStep(step);
            }
            spin->setValue(node.value("value", 0.0));
            const auto unit = string(node, "unit");
            spin->setSuffix(unit.isEmpty() ? QString() : " " + unit);
            connect(spin, &QDoubleSpinBox::editingFinished, this,
                    [=] { connectCallback(spin, node, "onChange", spin->value()); });
        } else if (type == "progress") {
            auto *progress = new QProgressBar(parent);
            widget = progress;
            progress->setRange(0, 1000);
            progress->setValue(
                int(std::clamp(numeric(node.value("value", Json(0.0))), 0.0, 1.0) * 1000));
        } else if (type == "canvas" || type == "waveform") {
            validateDrawing(node.value("drawing", Json::array()), type == "waveform");
            auto *canvas = new ScriptCanvas(parent);
            widget = canvas;
            canvas->waveform = type == "waveform";
            canvas->drawing = node.value("drawing", Json::array());
            canvas->acceptsPointer = node.contains("onPointer");
            canvas->acceptsWheel = node.contains("onWheel");
            canvas->setMouseTracking(canvas->acceptsPointer);
            canvas->setProperty("vlt.scriptInput", node.contains("onKey"));
            canvas->notify = [this, run, canvas, node, root = content.get()](
                                 const char *key, const Json &value, bool coalesce) {
                if (!root->property("vlt.installed").toBool() || !node.contains(key))
                    return;
                event(run, node[key]["$callback"].get<int>(),
                      {{"id", canvas->objectName().toStdString()}, {"value", value}},
                      coalesce ? QString::number(quintptr(canvas), 16) + "/" + key +
                                     (std::string_view(key) == "onPointer"
                                          ? QString::number(canvas->pointerSequence)
                                          : QString())
                               : QString());
            };
        } else if (type == "label") {
            auto *label = new QLabel(caption, parent);
            widget = label;
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(node.value("wrap", true));
            label->setAlignment(Qt::AlignVCenter | (align == "center"  ? Qt::AlignHCenter
                                                    : align == "right" ? Qt::AlignRight
                                                                       : Qt::AlignLeft));
        } else
            throw std::runtime_error("Unknown widget type: " + type);
        const auto widgetId = string(node, "id");
        if (!widgetId.isEmpty()) {
            if (widgets.contains(widgetId))
                throw std::runtime_error("Duplicate widget id");
            widget->setObjectName(widgetId);
            widgets[widgetId] = widget;
        }
        widget->setProperty("vlt.type", QString::fromStdString(type));
        scriptstyle::remember(widget, style);
        widget->setEnabled(node.value("enabled", true));
        widget->setVisible(node.value("visible", true));
        if (node.contains("minWidth")) {
            widget->setProperty("vlt.minWidth", node["minWidth"].get<int>());
            widget->setMinimumWidth(node["minWidth"]);
        }
        if (node.contains("minHeight")) {
            widget->setProperty("vlt.minHeight", node["minHeight"].get<int>());
            widget->setMinimumHeight(node["minHeight"]);
        }
        if (node.contains("width")) {
            widget->setProperty("vlt.fixedWidth", node["width"].get<int>());
            widget->setFixedWidth(node["width"]);
        }
        if (node.contains("height")) {
            widget->setProperty("vlt.fixedHeight", node["height"].get<int>());
            widget->setFixedHeight(node["height"]);
        }
        widget->setAccessibleName(string(node, "label").isEmpty() ? caption
                                                                  : string(node, "label"));
        if (node.contains("tooltip"))
            widget->setToolTip(string(node, "tooltip"));
        // Keep ids/bindings on the actual control so ui.set still reaches it.
        const auto labelText = string(node, "label");
        if (!labelText.isEmpty() && (type == "number" || type == "field" || type == "slider" ||
                                     type == "choice" || type == "list")) {
            auto *row = new QWidget(parent);
            widget->setProperty("vlt.labelled", true);
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(12);
            auto *label = new QLabel(labelText, row);
            label->setTextFormat(Qt::PlainText);
            label->setBuddy(widget);
            layout->addWidget(label);
            layout->addWidget(widget, 1);
            row->setVisible(node.value("visible", true));
            row->setEnabled(node.value("enabled", true));
            return row;
        }
        return widget;
    };
    auto *layout = new QVBoxLayout(content.get());
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    for (const auto &node : children)
        layout->addWidget(build(node, content.get(), 0), node.value("stretch", 0));
    return content.release();
}
} // namespace ui
