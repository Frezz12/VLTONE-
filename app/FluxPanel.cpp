#include "FluxPanel.hpp"
#include "EngineController.hpp"
#include "FluxPresetText.hpp"
#include "FluxWidgets.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

using namespace daw::plugins::flux;
namespace {
using fluxui::label;
QString str(const std::string &s) { return QString::fromStdString(s); }
bool same(const State &a, const State &b) { return a == b; }
QString userDirectory() {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
         "/Flux/Presets";
}
QIcon icon(const QString &key, QColor color = fluxui::ink) {
  static QHash<QString, QIcon> cache;
  const auto cacheKey = key + color.name(QColor::HexArgb);
  if (auto found = cache.constFind(cacheKey); found != cache.cend())
    return *found;
  QPixmap image(48, 48);
  image.fill(Qt::transparent);
  QPainter p(&image);
  p.setRenderHint(QPainter::Antialiasing);
  p.scale(2, 2);
  p.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.setBrush(Qt::NoBrush);
  if (key == "undo" || key == "redo") {
    if (key == "redo") {
      p.translate(24, 0);
      p.scale(-1, 1);
    }
    QPainterPath a;
    a.moveTo(7, 7);
    a.lineTo(3, 11);
    a.lineTo(7, 15);
    a.moveTo(4, 11);
    a.lineTo(13, 11);
    a.cubicTo(21, 11, 21, 21, 13, 21);
    p.drawPath(a);
  } else if (key == "settings") {
    for (unsigned i = 0; i < 8; ++i) {
      p.save();
      p.translate(12, 12);
      p.rotate(i * 45);
      p.drawLine(QPointF(0, -7), QPointF(0, -10));
      p.restore();
    }
    p.drawEllipse(QPointF(12, 12), 7, 7);
    p.drawEllipse(QPointF(12, 12), 2.7, 2.7);
  } else if (key == "random") {
    p.drawRoundedRect(QRectF(3, 3, 18, 18), 4, 4);
    p.setBrush(color);
    for (auto pt : {QPointF(7, 7), QPointF(17, 7), QPointF(12, 12),
                    QPointF(7, 17), QPointF(17, 17)})
      p.drawEllipse(pt, 1, 1);
  } else if (key == "folder") {
    QPainterPath folder;
    folder.moveTo(3, 7);
    folder.lineTo(3, 5);
    folder.lineTo(10, 5);
    folder.lineTo(13, 8);
    folder.lineTo(21, 8);
    folder.lineTo(21, 20);
    folder.lineTo(3, 20);
    folder.closeSubpath();
    p.setPen(QPen(fluxui::muted, 1.3));
    p.setBrush(Qt::NoBrush);
    p.drawPath(folder);
  } else if (key == "heart") {
    QPainterPath heart;
    heart.moveTo(12, 21);
    heart.cubicTo(9, 17, 2, 12, 2, 7);
    heart.cubicTo(2, 1, 9, 1, 12, 6);
    heart.cubicTo(15, 1, 22, 1, 22, 7);
    heart.cubicTo(22, 12, 15, 17, 12, 21);
    p.drawPath(heart);
  } else if (key == "search") {
    p.drawEllipse(QPointF(10, 10), 6.5, 6.5);
    p.drawLine(QPointF(15, 15), QPointF(21, 21));
  } else if (key == "power") {
    p.drawArc(QRectF(5, 5, 14, 14), 130 * 16, 280 * 16);
    p.drawLine(QPointF(12, 3), QPointF(12, 11));
  } else {
    p.setBrush(color);
    for (unsigned i = 0; i < 3; ++i)
      p.drawEllipse(QPointF(6 + i * 6, 12), 1, 1);
  }
  auto result = QIcon(image);
  cache.insert(cacheKey, result);
  return result;
}
class ModuleTile : public fluxui::Surface {
public:
  using Surface::Surface;
  unsigned slot = 0;
  QPoint start;
  std::function<void(unsigned)> dropped;

protected:
  void mousePressEvent(QMouseEvent *e) override { start = e->pos(); }
  void mouseMoveEvent(QMouseEvent *e) override {
    if (!(e->buttons() & Qt::LeftButton) ||
        (e->pos() - start).manhattanLength() < 8)
      return;
    auto *d = new QDrag(this);
    auto *data = new QMimeData;
    data->setData("application/x-vlt-flux-slot", QByteArray::number(slot));
    d->setMimeData(data);
    d->exec(Qt::MoveAction);
  }
  void dragEnterEvent(QDragEnterEvent *e) override {
    if (e->mimeData()->hasFormat("application/x-vlt-flux-slot"))
      e->acceptProposedAction();
  }
  void dropEvent(QDropEvent *e) override {
    if (dropped)
      dropped(e->mimeData()->data("application/x-vlt-flux-slot").toUInt());
    e->acceptProposedAction();
  }
};
QString skin() {
  return QStringLiteral(R"(
QWidget#fluxSurface {color:#292d2e;background:transparent;}
QDialog {background:#f2f2ed;color:#292d2e;font-family:"Segoe UI";font-size:13px;}
QPushButton,QComboBox,QLineEdit,QDoubleSpinBox,QSpinBox {color:#292d2e;border:1px solid #dcded7;border-radius:8px;padding:5px 9px;background:#eeefe9;}
QPushButton:hover,QComboBox:hover {border-color:#aeb6ac;background:#e6e9e1;}
QPushButton:pressed,QPushButton:checked {color:#205e67;background:#e4eeeb;}
QPushButton:disabled,QComboBox:disabled {color:#92988f;border-color:#e1e3dc;background:#eeefe9;}
QComboBox::drop-down {border:0;width:22px;}
QComboBox::down-arrow {image:none;width:0;height:0;}
QDoubleSpinBox::up-button,QSpinBox::up-button {subcontrol-origin:border;subcontrol-position:top right;width:17px;border:0;border-left:1px solid #dcded7;border-bottom:1px solid #dcded7;border-top-right-radius:7px;background:#e6e9e1;}
QDoubleSpinBox::down-button,QSpinBox::down-button {subcontrol-origin:border;subcontrol-position:bottom right;width:17px;border:0;border-left:1px solid #dcded7;border-bottom-right-radius:7px;background:#e6e9e1;}
QDoubleSpinBox::up-arrow,QSpinBox::up-arrow,QDoubleSpinBox::down-arrow,QSpinBox::down-arrow {width:7px;height:4px;}
QComboBox QAbstractItemView {background:#fdfdfa;color:#292d2e;border:1px solid #dcded7;selection-background-color:#e4eeeb;selection-color:#205e67;}
QLineEdit {selection-background-color:#d8e8e2;selection-color:#292d2e;}
QTreeWidget,QListWidget,QTableWidget {color:#292d2e;background:transparent;border:0;outline:0;selection-background-color:#e4eeeb;selection-color:#205e67;alternate-background-color:#eeefe9;gridline-color:#dcded7;}
QTreeWidget::item {padding:8px 4px;border:0;border-radius:6px;}
QTreeWidget::item:selected {background:#e4eeeb;color:#205e67;}
QTreeWidget::item:hover {background:#eeefe9;}
QTreeWidget::branch {background:transparent;}
QHeaderView::section {color:#626a69;background:#eeefe9;border:1px solid #dcded7;padding:7px;}
QCheckBox,QRadioButton {color:#292d2e;background:transparent;spacing:7px;}
QCheckBox::indicator {width:14px;height:14px;border:1px solid #aeb6ac;border-radius:4px;background:#fdfdfa;}
QCheckBox::indicator:checked {border-color:#292d2e;background:#292d2e;}
QRadioButton::indicator {width:14px;height:14px;border:1px solid #aeb6ac;border-radius:8px;background:#fdfdfa;}
QRadioButton::indicator:checked {border:4px solid #318b98;background:#fdfdfa;}
QLabel {background:transparent;color:#292d2e;}
QScrollArea {background:transparent;border:0;}
QScrollBar:horizontal {height:8px;background:transparent;} QScrollBar:vertical {width:8px;background:transparent;}
QScrollBar::handle {background:#b9c1b6;border-radius:4px;min-width:24px;min-height:24px;}QScrollBar::add-line,QScrollBar::sub-line {width:0;height:0;}
QScrollBar::add-page,QScrollBar::sub-page {background:transparent;}
QMenu {color:#292d2e;background:#fdfdfa;border:1px solid #dcded7;border-radius:10px;padding:7px;}
QMenu::item {padding:8px 28px 8px 16px;margin:2px;border-radius:6px;}QMenu::item:selected {color:#205e67;background:#e4eeeb;}
QMenu::item:disabled {color:#92988f;}QMenu::separator {height:1px;background:#dcded7;margin:6px 10px;}
QToolTip {color:#292d2e;background:#fdfdfa;border:1px solid #dcded7;padding:6px;}
QTabWidget::pane {border:1px solid #dcded7;border-radius:10px;background:#fdfdfa;}
QTabBar::tab {color:#626a69;background:#eeefe9;padding:10px 15px;margin:3px;border-radius:7px;}
QTabBar::tab:selected {color:#fdfdfa;background:#292d2e;}
QSlider::groove:horizontal {height:5px;background:#dcded7;border-radius:3px;}
QSlider::sub-page:horizontal {background:#318b98;border-radius:3px;}
QSlider::handle:horizontal {width:13px;margin:-4px 0;background:#292d2e;border:1px solid #fdfdfa;border-radius:7px;}
)");
}
} // namespace
struct FluxPanel::Impl {
  FluxPanel *q;
  daw::EngineController *controller;
  std::string channel, insert;
  State state = initialState(), comparison[2];
  FluxTelemetry telemetry;
  std::optional<State> gesture;
  std::vector<State> previewUndo, previewRedo;
  QTimer timer;
  QScrollArea *viewport = nullptr;
  fluxui::Surface *surface = nullptr;
  struct Positioned {
    QWidget *widget;
    QRectF rect;
    unsigned page = 0;
  };
  std::vector<Positioned> widgets;
  std::vector<fluxui::Knob *> knobs, parameterKnobs;
  fluxui::CurveEditor *graph = nullptr;
  fluxui::SequenceEditor *sequence = nullptr;
  fluxui::SequenceEditor *compact = nullptr;
  fluxui::Pad *pad = nullptr;
  fluxui::RoutingEditor *routing = nullptr;
  fluxui::BandEditor *bands = nullptr;
  QTreeWidget *presets = nullptr;
  QLineEdit *search = nullptr;
  QLabel *description = nullptr, *status = nullptr,
         *moduleTitle = nullptr;
  QPushButton *favorite = nullptr, *ab = nullptr, *randomButton = nullptr,
              *recordButton = nullptr, *presetActions = nullptr;
  QComboBox *kindChoice = nullptr, *targetChoice = nullptr,
            *curveChoice = nullptr, *routingChoice = nullptr,
            *quality = nullptr, *rate = nullptr, *trigger = nullptr,
            *playMode = nullptr, *division = nullptr, *length = nullptr,
            *direction = nullptr;
  QCheckBox *syncBox = nullptr, *seqOn = nullptr;
  std::array<QPushButton *, 16> sceneButtons{};
  std::array<QPushButton *, 3> pageButtons{};
  std::array<QPushButton *, 2> libraryButtons{};
  QScrollArea *chainScroll = nullptr;
  QWidget *chainBody = nullptr;
  QPushButton *chainAdd = nullptr;
  std::vector<QWidget *> cards;
  std::array<Slot, slotCount> displayedSlots{};
  std::array<unsigned, slotCount> displayedOrder{};
  unsigned displayedSlot = slotCount;
  Kind displayedKind = Kind::Count, targetKind = Kind::Count;
  QStringList factoryDescriptions;
  unsigned selected = 0, motion = 0, page = 0, comparisonSlot = 0,
           lastRevision = 0, randomCounter = 1, lastParameterRevision = 0;
  unsigned midiLearnRevision = 0;
  unsigned heldScene = 0;
  int midiLearn = -1;
  bool updating = false, userMode = false, onlyFavorites = false,
       onlyRecent = false, mixLock = false, outputLock = false,
       recording = false, reduced = false, highContrast = false;
  double zoom = 1, zoomSetting = 0, recordStart = 0, logicalWidth = 1344;
  bool collapsed = false;
  QStringList favorites, recent;
  QString userFile;
  QPixmap background;
  Impl(FluxPanel *panel, daw::EngineController *c, QString ch, QString in)
      : q(panel), controller(c), channel(ch.toStdString()),
        insert(in.toStdString()) {
    if (controller)
      if (auto s = controller->fluxSnapshot(channel, insert)) {
        state = std::move(s->state);
        telemetry = s->telemetry;
      }
    comparison[0] = comparison[1] = state;
    QSettings settings;
    favorites = settings.value("Flux/favorites").toStringList();
    recent = settings.value("Flux/recent").toStringList();
    zoomSetting = settings.value("Flux/zoom", 0).toDouble();
    reduced = settings.value("Flux/reduceMotion", false).toBool();
    highContrast = settings.value("Flux/highContrast", false).toBool();
    mixLock = settings.value("Flux/lockMix", false).toBool();
    outputLock = settings.value("Flux/lockOutput", false).toBool();
    build();
    sync();
    QObject::connect(&timer, &QTimer::timeout, q, [this] { refresh(); });
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(reduced ? 50 : 16);
  }
  ~Impl() {
    QSettings settings;
    settings.setValue("Flux/favorites", favorites);
    settings.setValue("Flux/recent", recent);
    settings.setValue("Flux/zoom", zoomSetting);
    settings.setValue("Flux/lockMix", mixLock);
    settings.setValue("Flux/lockOutput", outputLock);
  }
  void add(QWidget *widget, QRectF r, unsigned page = 0) {
    widgets.push_back({widget, r, page});
  }
  QPushButton *button(QString caption, QRectF r, std::function<void()> action,
                      bool check = false) {
    auto *b = new fluxui::Button(caption, surface);
    b->setCheckable(check);
    b->setAccessibleName(caption);
    if (caption == "↶" || caption == "↷" || caption == "⚙" || caption == "⋯" ||
        caption.contains("⚄")) {
      b->setIcon(icon(caption == "↶"          ? "undo"
                      : caption == "↷"        ? "redo"
                      : caption == "⚙"        ? "settings"
                      : caption.contains("⚄") ? "random"
                                              : "menu"));
      b->setIconSize({18, 18});
      b->setText(caption.contains("Random") ? "Random" : "");
      if (caption == "↶")
        b->setObjectName("flux.undo");
      if (caption == "↷")
        b->setObjectName("flux.redo");
    }
    if (r.width() < 30)
      b->setStyleSheet("padding:0;");
    else if (r.height() < 28)
      b->setStyleSheet("padding:0 2px;");
    if (caption == "A⇄B")
      b->setText("A > B");
    if (caption == "↶") b->setAccessibleName(label("Undo", "Отменить"));
    if (caption == "↷") b->setAccessibleName(label("Redo", "Повторить"));
    if (caption == "⚙") b->setAccessibleName(label("Settings", "Настройки"));
    if (caption == "‹") b->setAccessibleName(label("Previous preset", "Предыдущий пресет"));
    if (caption == "›") b->setAccessibleName(label("Next preset", "Следующий пресет"));
    b->setToolTip(b->accessibleName());
    if (caption.contains("▾")) {
      b->setText(caption.remove("▾").trimmed());
      b->setProperty("fluxDropdown", true);
    }
    add(b, r);
    QObject::connect(b, &QPushButton::clicked, q, [this, action] {
      if (!updating)
        action();
    });
    return b;
  }
  QComboBox *combo(QStringList names, QRectF r,
                   std::function<void(int)> action) {
    auto *box = new fluxui::ComboBox(surface);
    for (auto &name : names)
      name = fluxui::localize(name);
    box->addItems(names);
    box->setAccessibleName(names.value(0));
    add(box, r);
    QObject::connect(box, qOverload<int>(&QComboBox::activated), q,
                     [this, action](int i) {
                       if (!updating)
                         action(i);
                     });
    return box;
  }
  QCheckBox *check(QString caption, QRectF r,
                   std::function<void(bool)> action) {
    auto *b = new fluxui::Toggle(caption, surface);
    b->setAccessibleName(caption);
    add(b, r);
    QObject::connect(b, &QCheckBox::clicked, q, [this, action](bool on) {
      if (!updating)
        action(on);
    });
    return b;
  }
  double value(unsigned id) const;
  void setValue(unsigned id, double v);
  fluxui::Knob *knob(unsigned id, QString caption, QRectF r,
                     QColor accent = fluxui::accent);
  void begin() {
    if (!gesture)
      gesture = state;
  }
  void live() {
    if (controller)
      controller->applyFluxState(channel, insert, state, "Edit Flux", false);
    graph->update();
    sequence->update();
    compact->update();
    pad->update();
    routing->update();
    emit q->projectEdited();
  }
  bool apply(State next, const QString &caption, bool history = true,
             const State *previous = nullptr) {
    if (!validState(next))
      return false;
    State before = previous ? *previous : state;
    if (controller) {
      if (!controller->applyFluxState(channel, insert, next,
                                      caption.toStdString(), history, previous))
        return false;
    } else if (history && !same(before, next)) {
      previewUndo.push_back(before);
      previewRedo.clear();
    }
    state = std::move(next);
    sync();
    emit q->projectEdited();
    return true;
  }
  void finish(QString title = {}) {
    if (!gesture)
      return;
    State before = std::move(*gesture);
    gesture.reset();
    if (!same(before, state))
      apply(state,
            title.isEmpty() ? label("Edit Flux", "Изменить Flux") : title, true,
            &before);
  }
  template <class F> void edit(QString name, F f) {
    State next = state;
    f(next);
    apply(std::move(next), name);
  }
  void build();
  void layout();
  void sync();
  void refresh();
  void buildParameters();
  void buildCards();
  void updatePresetHeader();
  void populatePresets();
  void drawBackground(QPainter &);
  QRectF place(QRectF) const;
  void browsePresets();
  void presetMenu();
  void settingsMenu();
  void moduleMenu(unsigned);
  void sequenceMenu();
  void modulationDialog();
  void triggerDialog();
  void saveUser(bool duplicate = false, bool move = false);
  void load(unsigned);
  void changeScene(unsigned, bool trigger = true);
  void assign(unsigned, unsigned);
  void setPage(unsigned);
};

double FluxPanel::Impl::value(unsigned id) const {
  if (id < GlobalCount)
    return state.globals[id];
  unsigned slot = (id - GlobalCount) / bankSize,
           local = (id - GlobalCount) % bankSize;
  const auto &s = state.scenes[state.editScene].modules[slot];
  if (local < CommonCount) {
    switch (local) {
    case Enabled:
      return s.enabled;
    case SlotMix:
      return s.mix;
    case SlotOutput:
      return s.output;
    case Solo:
      return s.solo;
    case DepthA:
    case DepthB:
      return s.curves[local - DepthA].depth;
    case RateA:
    case RateB: {
      auto &c = s.curves[local - RateA];
      return c.sync ? c.bars : c.hz;
    }
    }
  }
  auto kind = Kind(1 + (local - CommonCount) / fieldCount);
  return kind == state.modules[slot].kind
             ? s.values[(local - CommonCount) % fieldCount]
             : parameterTable()[id].defaultValue;
}
void FluxPanel::Impl::setValue(unsigned id, double v) {
  if (id < GlobalCount) {
    state.globals[id] = v;
    if (id >= Macro1 && id <= Macro4)
      state.scenes[state.editScene].macros[id - Macro1] = v;
    return;
  }
  unsigned slot = (id - GlobalCount) / bankSize,
           local = (id - GlobalCount) % bankSize;
  auto &s = state.scenes[state.editScene].modules[slot];
  if (local < CommonCount) {
    switch (local) {
    case Enabled:
      s.enabled = v > .5;
      break;
    case SlotMix:
      s.mix = v;
      break;
    case SlotOutput:
      s.output = v;
      break;
    case Solo:
      s.solo = v > .5;
      break;
    case DepthA:
    case DepthB:
      s.curves[local - DepthA].depth = v;
      break;
    case RateA:
    case RateB: {
      auto &c = s.curves[local - RateA];
      if (c.sync)
        c.bars = std::min(32., v);
      else
        c.hz = std::max(.01, v);
      break;
    }
    }
  } else
    s.values[(local - CommonCount) % fieldCount] = v;
}
fluxui::Knob *FluxPanel::Impl::knob(unsigned id, QString caption, QRectF r,
                                    QColor accent) {
  auto *k = new fluxui::Knob(id, caption, accent, surface);
  if (id == Input || id == Output || id == Mix ||
      (id >= LowGain && id <= HighGain))
    k->setProperty("fluxCaptionTop", true);
  add(k, r);
  knobs.push_back(k);
  k->setValue(value(id));
  QObject::connect(k, &ui::Knob::valueChanged, q, [this, k](double v) {
    if (updating)
      return;
    begin();
    setValue(k->index, v);
    if (controller && state.editScene == telemetry.playingScene)
      controller->setInsertParameter(channel, insert,
                                     parameterTable()[k->index].id, v);
    else
      live();
    emit q->projectEdited();
  });
  QObject::connect(k, &ui::Knob::editFinished, q, [this] {
    finish(label("Adjust Flux parameter", "Изменить параметр Flux"));
  });
  QObject::connect(k, &ui::Knob::automateRequested, q, [this, k] {
    emit q->automationRequested(str(parameterTable()[k->index].id));
  });
  k->assign = [this](unsigned source, unsigned target) {
    assign(source, target);
  };
  k->learn = [this](unsigned id) {
    midiLearn = int(id);
    midiLearnRevision = telemetry.midiRevision;
    if (controller)
      if (auto current = controller->fluxTelemetry(channel, insert))
        midiLearnRevision = current->midiRevision;
    status->setText(
        label("Move a MIDI controller…", "Переместите MIDI-контроллер…"));
  };
  k->lock = [this](unsigned id) {
    edit(label("Lock parameter", "Зафиксировать параметр"), [id](State &s) {
      auto i = std::find(s.parameterLocks.begin(), s.parameterLocks.end(), id);
      if (i == s.parameterLocks.end())
        s.parameterLocks.push_back(id);
      else
        s.parameterLocks.erase(i);
    });
  };
  return k;
}
void FluxPanel::Impl::build() {
  q->setObjectName("FluxPanel");
  viewport = new QScrollArea(q);
  viewport->setFrameShape(QFrame::NoFrame);
  viewport->setWidgetResizable(false);
  surface = new fluxui::Surface;
  surface->setObjectName("fluxSurface");
  surface->setProperty("fluxHighContrast", highContrast);
  QPalette palette = surface->palette();
  for (auto role : {QPalette::Window, QPalette::Button, QPalette::Base,
                    QPalette::ToolTipBase})
    palette.setColor(role, fluxui::paper);
  for (auto role : {QPalette::WindowText, QPalette::ButtonText, QPalette::Text,
                    QPalette::ToolTipText})
    palette.setColor(role, fluxui::ink);
  palette.setColor(QPalette::Highlight, QColor("#e4eeeb"));
  palette.setColor(QPalette::HighlightedText, fluxui::ink);
  palette.setColor(QPalette::PlaceholderText, fluxui::muted);
  surface->setPalette(palette);
  surface->setFont(fluxui::controlFont());
  viewport->setPalette(palette);
  q->setPalette(palette);
  q->setFont(fluxui::controlFont());
  q->setStyleSheet(skin());
  surface->setStyleSheet(
      skin() + (highContrast
                    ? "QPushButton,QComboBox,QLineEdit "
                      "{border-color:#626a69;} QTreeWidget {color:#292d2e;}"
                    : ""));
  surface->draw = [this](QPainter &p) { drawBackground(p); };
  viewport->setWidget(surface);
  button("‹", {253, 22, 36, 38}, [this] {
    const unsigned number = str(state.presetId).mid(8).toUInt();
    const unsigned n = number >= 1 && number <= 100 ? number - 1 : 0;
    load((n + 99) % 100);
  });
  button("›", {294, 22, 36, 38}, [this] {
    const unsigned number = str(state.presetId).mid(8).toUInt();
    const unsigned n = number >= 1 && number <= 100 ? number - 1 : 0;
    load((n + 1) % 100);
  });
  presetActions = button(label("Preset actions", "Действия с пресетом"),
                         {340, 22, 296, 38}, [this] { presetMenu(); });
  presetActions->setProperty("fluxDropdown", true);
  presetActions->setToolTip(
      label("Browse, save or import a preset",
            "Выбрать, сохранить или импортировать пресет"));
  favorite = button(
      "☆", {644, 22, 36, 38},
      [this] {
        QString id = str(state.presetId);
        if (favorites.contains(id))
          favorites.removeAll(id);
        else
          favorites.push_back(id);
        sync();
      },
      true);
  favorite->setText({});
  favorite->setIcon(icon("heart"));
  favorite->setIconSize({20, 20});
  favorite->setAccessibleName(label("Favorite preset", "Избранный пресет"));
  favorite->setToolTip(favorite->accessibleName());
  ab = button("A", {694, 22, 40, 38}, [this] {
    comparison[comparisonSlot] = state;
    comparisonSlot = 1 - comparisonSlot;
    apply(comparison[comparisonSlot], label("Compare A/B", "Сравнение A/B"));
  });
  ab->setCheckable(true);
  ab->setProperty("fluxTab", true);
  button("A⇄B", {740, 22, 53, 38}, [this] {
    comparison[1 - comparisonSlot] = state;
    status->setText(
        label("Comparison copied", "Состояние сравнения скопировано"));
  });
  button("↶", {804, 22, 35, 38}, [this] {
    if (controller) {
      controller->undo();
      lastRevision = 0;
      refresh();
    } else if (!previewUndo.empty()) {
      previewRedo.push_back(state);
      state = previewUndo.back();
      previewUndo.pop_back();
      sync();
    }
  });
  button("↷", {844, 22, 35, 38}, [this] {
    if (controller) {
      controller->redo();
      lastRevision = 0;
      refresh();
    } else if (!previewRedo.empty()) {
      previewUndo.push_back(state);
      state = previewRedo.back();
      previewRedo.pop_back();
      sync();
    }
  });
  randomButton =
      button("⚄ Random", {892, 22, 106, 38}, [this] { q->randomizeParameters(); });
  randomButton->setProperty("fluxPrimary", true);
  randomButton->setIcon(icon("random", fluxui::panelWhite));
  randomButton->setObjectName("flux.random");
  randomButton->setAccessibleName(
      label("Randomize everything", "Рандомизировать всё"));
  randomButton->setToolTip(
      label("Randomize the complete sound: modules, routing, scenes, curves, "
            "sequences and modulation. Locked controls stay "
            "unchanged. Undo restores the previous sound.",
            "Новый звук целиком: эффекты, маршрутизация, сцены, кривые, "
            "секвенсор и модуляция. Зафиксированные "
            "настройки сохраняются. Undo возвращает предыдущий звук."));
  search = new QLineEdit(surface);
  search->setPlaceholderText(label("Search presets…", "Поиск пресетов…"));
  search->setAccessibleName(label("Search presets", "Поиск пресетов"));
  search->addAction(icon("search"), QLineEdit::LeadingPosition);
  add(search, {1010, 22, 254, 38});
  QObject::connect(search, &QLineEdit::textChanged, q,
                   [this] { populatePresets(); });
  button("⚙", {1278, 22, 48, 38}, [this] { settingsMenu(); });
  libraryButtons[0] = button("Factory (100)", {16, 83, 137, 28}, [this] {
    userMode = false;
    populatePresets();
  });
  libraryButtons[1] = button("User", {157, 83, 57, 28}, [this] {
    userMode = true;
    populatePresets();
  });
  for (auto *tab : libraryButtons) {
    tab->setCheckable(true);
    tab->setProperty("fluxTab", true);
  }
  presets = new QTreeWidget(surface);
  presets->setColumnCount(2);
  presets->setHeaderHidden(true);
  presets->header()->setStretchLastSection(false);
  presets->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  presets->header()->setSectionResizeMode(1, QHeaderView::Fixed);
  presets->setColumnWidth(1, 25);
  presets->setIndentation(11);
  presets->setRootIsDecorated(false);
  presets->setObjectName("flux.presets");
  presets->setAccessibleName(label("Preset folders", "Папки пресетов"));
  presets->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  add(presets, {14, 117, 202, 399});
  QObject::connect(presets, &QTreeWidget::itemClicked, q,
                   [this](QTreeWidgetItem *item, int) {
                     if (item->data(0, Qt::UserRole).isValid()) {
                       if (userMode)
                         q->importPreset(
                             item->data(0, Qt::UserRole).toString());
                       else
                         load(item->data(0, Qt::UserRole).toUInt());
                     } else
                       item->setExpanded(!item->isExpanded());
                   });
  description = new QLabel(surface);
  description->setWordWrap(true);
  description->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  description->setStyleSheet("color:#626a69;");
  add(description, {22, 529, 188, 85});
  pageButtons[0] = button(label("Motion", "Движение"), {236, 83, 131, 31},
                          [this] { setPage(0); });
  pageButtons[1] = button(label("Sequence", "Секвенсор"), {371, 83, 132, 31},
                          [this] { setPage(1); });
  pageButtons[2] = button(label("Routing", "Маршрутизация"), {507, 83, 140, 31},
                          [this] { setPage(2); });
  for (unsigned i = 0; i < pageButtons.size(); ++i) {
    auto *tab = pageButtons[i];
    tab->setObjectName(QString("flux.page.%1").arg(i));
    tab->setCheckable(true);
    tab->setProperty("fluxTab", true);
  }
  pageButtons[page]->setChecked(true);
  kindChoice = combo({}, {655, 83, 187, 31}, [this](int k) {
    edit(label("Replace module", "Заменить модуль"),
         [&](State &s) { setKind(s, selected, Kind(k + 1)); });
    buildParameters();
    buildCards();
    sync();
  });
  for (unsigned i = 1; i < unsigned(Kind::Count); ++i)
    kindChoice->addItem(kindName(Kind(i)));
  button("+ FX", {850, 83, 111, 31}, [this] { moduleMenu(slotCount); });
  curveChoice =
      combo({"Motion A", "Motion B"}, {237, 121, 98, 28}, [this](int i) {
        motion = unsigned(i);
        graph->curve = motion;
        sync();
      });
  targetChoice = combo({}, {341, 121, 131, 28}, [this](int i) {
    edit(label("Motion target", "Цель огибающей"), [&](State &s) {
      s.scenes[s.editScene].modules[selected].curves[motion].target =
          unsigned(i);
    });
  });
  targetChoice->setObjectName("flux.motion.target");
  rate = combo({"1/128", "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar",
                "2 bars", "4 bars", "8 bars", "16 bars", "32 bars"},
               {478, 121, 96, 28}, [this](int i) {
                 edit(label("Motion length", "Длина огибающей"), [&](State &s) {
                   auto &c =
                       s.scenes[s.editScene].modules[selected].curves[motion];
                   c.bars = std::exp2(i - 7);
                   c.sync = true;
                 });
               });
  combo({label("Points", "Точки"), label("Draw", "Рисовать"),
         label("Line", "Линия"), label("Steps", "Ступени"),
         label("Bend", "Изгиб")},
        {580, 121, 93, 28}, [this](int i) { graph->tool = unsigned(i); });
  combo({"1/4", "1/8", "1/16", "1/32", "1/64", "1/128", "1/8 T", "1/16 T",
         "1/8 D", "1/16 D"},
        {680, 121, 85, 28}, [this](int i) {
          graph->grid = i < 6 ? 4u << i : i % 2 ? 16 : 8;
          graph->gridMultiplier = i < 6 ? 1 : i < 8 ? 1.5 : 2. / 3;
          graph->update();
        });
  check("Snap", {776, 122, 65, 25}, [this](bool v) {
    graph->snap = v;
  })->setChecked(true);
  combo({label("Shapes…", "Формы…"), "Pump", "Ramp", "Triangle", "Sine",
         "Pulse", "Gate", "Staircase", "Random"},
        {842, 121, 116, 28}, [this](int i) {
          if (!i)
            return;
          edit(label("Draw motion shape", "Форма огибающей"), [&](State &s) {
            auto &c = s.scenes[s.editScene].modules[selected].curves[motion];
            makeShape(c, unsigned(i - 1), ++randomCounter);
            c.depth = 1;
          });
        });
  graph = new fluxui::CurveEditor(surface);
  graph->state = &state;
  graph->telemetry = &telemetry;
  add(graph, {235, 157, 726, 227}, 1);
  sequence = new fluxui::SequenceEditor(surface);
  sequence->state = &state;
  sequence->telemetry = &telemetry;
  add(sequence, {235, 157, 726, 227}, 2);
  routing = new fluxui::RoutingEditor(surface);
  routing->state = &state;
  routing->select = [this](unsigned s) {
    selected = s;
    buildParameters();
    sync();
  };
  add(routing, {235, 157, 726, 227}, 3);
  for (auto *w : {graph, static_cast<fluxui::CurveEditor *>(nullptr)})
    if (w) {
      w->begin = [this] { begin(); };
      w->changed = [this] { live(); };
      w->finish = [this] {
        finish(label("Edit motion curve", "Изменить кривую"));
      };
    }
  sequence->begin = [this] { begin(); };
  sequence->changed = [this] { live(); };
  sequence->finish = [this] {
    finish(label("Edit sequence", "Изменить секвенсор"));
  };
  routing->begin = [this] { begin(); };
  routing->changed = [this] { live(); };
  routing->finish = [this] {
    finish(label("Route effects", "Маршрутизация эффектов"));
    buildCards();
  };
  moduleTitle = new QLabel(surface);
  add(moduleTitle, {246, 388, 260, 22});
  button(label("Motion settings…", "Настройки кривой…"), {517, 389, 177, 23},
         [this] { triggerDialog(); });
  button(label("Module…", "Модуль…"), {698, 389, 106, 23},
         [this] { moduleMenu(selected); });
  button(label("Waves…", "Волны…"), {810, 389, 145, 23}, [this] {
    QMenu m(surface);
    for (unsigned c = 0; c < 3; ++c) {
      auto *a = m.addAction(c == 0   ? label("Input", "Вход")
                            : c == 1 ? label("Output", "Выход")
                                     : "Sidechain");
      a->setCheckable(true);
      a->setChecked(c == 0   ? graph->waveInput
                    : c == 1 ? graph->waveOutput
                             : graph->waveSidechain);
      QObject::connect(a, &QAction::triggered, q, [this, c](bool on) {
        (c == 0   ? graph->waveInput
         : c == 1 ? graph->waveOutput
                  : graph->waveSidechain) = on;
        graph->update();
      });
    }
    m.exec(QCursor::pos());
  });
  for (unsigned i = 0; i < 4; ++i) {
    knob(Macro1 + i, str(state.macroNames[i]), {987. + i * 85, 103, 80, 109})
        ->setProperty("fluxMacro", true);
    auto *source = new fluxui::SourceButton(QString::number(i + 1), surface);
    source->setText(QString("Macro %1").arg(i + 1));
    source->setProperty("fluxFlat", true);
    source->source = i;
    source->setAccessibleName(label("Drag macro source", "Перетащить макрос") +
                              QString::number(i + 1));
    source->setToolTip(source->accessibleName());
    add(source, {987. + i * 85, 162, 80, 16});
  }
  pad = new fluxui::Pad(surface);
  pad->state = &state;
  pad->telemetry = &telemetry;
  pad->begin = [this] { begin(); };
  pad->changed = [this] { live(); };
  pad->finish = [this] { finish(label("Move XY pad", "Движение XY")); };
  add(pad, {986, 253, 166, 160});
  recordButton = button(
      "● REC", {988, 419, 77, 24},
      [this] {
        recording = !recording;
        if (recording) {
          begin();
          state.path.clear();
          recordStart = telemetry.elapsedBeats;
        } else
          finish(label("Record XY motion", "Запись движения XY"));
        sync();
      },
      true);
  button("↻ XY", {1071, 419, 78, 24}, [this] {
    edit(label("Play XY path", "Воспроизвести XY"),
         [](State &s) { s.pathEnabled = !s.pathEnabled; });
  });
  trigger =
      combo({"Host Sync", "MIDI", "Audio", "Sidechain", "Manual"},
            {1164, 243, 159, 29}, [this](int v) {
              edit(label("Motion trigger", "Запуск огибающей"), [&](State &s) {
                s.scenes[s.editScene].modules[selected].curves[motion].trigger =
                    TriggerMode(v);
              });
            });
  playMode = combo(
      {"Loop", "One Shot", "MIDI Hold"}, {1164, 279, 159, 29}, [this](int v) {
        edit(label("Playback mode", "Режим огибающей"), [&](State &s) {
          s.scenes[s.editScene].modules[selected].curves[motion].play =
              PlayMode(v);
        });
      });
  button("▶ Trigger", {1164, 315, 159, 30}, [this] {
    if (controller) {
      controller->setInsertParameter(channel, insert, "trigger", 0);
      controller->setInsertParameter(channel, insert, "trigger", 1);
    }
    status->setText(label("Triggered", "Запущено"));
  });
  button(label("Detector…", "Детектор…"), {1164, 352, 159, 28},
         [this] { triggerDialog(); });
  button(label("Modulation…", "Модуляция…"), {1164, 386, 159, 28}, [this] {
    modulationDialog();
  })->setObjectName("flux.modulation");
  for (unsigned i = 0; i < 16; ++i) {
    sceneButtons[i] =
        button(QString::number(i + 1),
               {986. + (i % 8) * 42.5, 467. + (i / 8) * 29, 37, 25}, [this, i] {
                 if (state.sceneMode != 1)
                   changeScene(i, !(QApplication::keyboardModifiers() &
                                    Qt::ShiftModifier));
               });
    QObject::connect(sceneButtons[i], &QPushButton::pressed, q, [this, i] {
      if (state.sceneMode == 1) {
        heldScene = telemetry.playingScene;
        changeScene(i);
      }
    });
    QObject::connect(sceneButtons[i], &QPushButton::released, q, [this] {
      if (state.sceneMode == 1)
        changeScene(heldScene);
    });
    sceneButtons[i]->setObjectName(QString("flux.scene.%1").arg(i + 1));
  }
  button(label("Scenes…", "Сцены…"), {1198, 522, 127, 25}, [this] {
    QMenu m(surface);
    m.addAction(label("Rename scene…", "Переименовать сцену…"), q, [this] {
      bool ok = false;
      auto name = QInputDialog::getText(
          q, "Flux", label("Scene name", "Название сцены"), QLineEdit::Normal,
          str(state.scenes[state.editScene].name), &ok);
      if (ok && !name.isEmpty())
        edit(label("Rename scene", "Имя сцены"), [&](State &s) {
          s.scenes[s.editScene].name = name.left(128).toStdString();
        });
    });
    m.addAction(label("Copy to next scene", "Копировать в следующую сцену"), q,
                [this] {
                  edit(label("Copy scene", "Копирование сцены"), [](State &s) {
                    auto copy = s.scenes[s.editScene];
                    unsigned next = (s.editScene + 1) % 16;
                    ensureScene(s, next) = copy;
                  });
                });
    auto *quant =
        m.addMenu(label("Switch quantize", "Квантование переключения"));
    for (unsigned v = 0; v < 3; ++v)
      quant->addAction(v == 0   ? label("Immediate", "Сразу")
                       : v == 1 ? label("Next beat", "На доле")
                                : label("Next bar", "На такте"),
                       q, [this, v] {
                         edit("Scene quantize",
                              [v](State &s) { s.globals[Quantize] = v; });
                       });
    auto *mode = m.addMenu(label("Scene behaviour", "Поведение сцен"));
    for (unsigned v = 0; v < 3; ++v)
      mode->addAction(v == 0   ? "Latch"
                      : v == 1 ? "Momentary"
                               : "Fill",
                      q, [this, v] {
                        edit("Scene mode", [v](State &s) { s.sceneMode = v; });
                      });
    m.addAction(
        label("MIDI notes 36–51 / channel 16", "MIDI-ноты 36–51 / канал 16"), q,
        [this] {
          edit("MIDI scenes", [](State &s) { s.midiScenes = !s.midiScenes; });
        });
    m.exec(QCursor::pos());
  });
  seqOn = check(
      label("Sequencer", "Секвенсор"), {245, 538, 135, 27}, [this](bool on) {
        edit(label("Enable sequence", "Включить секвенсор"), [&](State &s) {
          s.scenes[s.editScene].modules[selected].sequence.enabled = on;
        });
      });
  division =
      combo({"1/4", "1/8", "1/16", "1/32", "1/64", "1/128", "1/8 T", "1/16 T",
             "1/8 D", "1/16 D"},
            {391, 538, 95, 28}, [this](int i) {
              double v = i < 6    ? std::exp2(-i)
                         : i == 6 ? 1. / 3
                         : i == 7 ? 1. / 6
                         : i == 8 ? .75
                                  : .375;
              edit("Sequence rate", [&](State &s) {
                s.scenes[s.editScene].modules[selected].sequence.division = v;
              });
            });
  length = combo(
      {"8 Steps", "16 Steps", "24 Steps", "32 Steps", "48 Steps", "64 Steps"},
      {493, 538, 105, 28}, [this](int i) {
        unsigned n[] = {8, 16, 24, 32, 48, 64};
        edit("Sequence length", [&](State &s) {
          s.scenes[s.editScene].modules[selected].sequence.length = n[i];
        });
      });
  direction =
      combo({"Forward", "Reverse", "Ping-Pong", "Random"}, {605, 538, 116, 28},
            [this](int i) {
              edit("Sequence direction", [&](State &s) {
                s.scenes[s.editScene].modules[selected].sequence.direction =
                    Direction(i);
              });
            });
  button(label("Pattern…", "Рисунок…"), {729, 538, 113, 28},
         [this] { sequenceMenu(); });
  button("⚄", {850, 538, 44, 28}, [this] { q->randomizeParameters(0, 4); });
  button(label("Clear", "Сброс"), {901, 538, 60, 28}, [this] {
    edit("Reset sequence", [&](State &s) {
      s.scenes[s.editScene].modules[selected].sequence = Sequence{};
    });
  });
  compact = new fluxui::SequenceEditor(surface);
  compact->compact = true;
  compact->state = &state;
  compact->telemetry = &telemetry;
  compact->begin = [this] { begin(); };
  compact->changed = [this] { live(); };
  compact->finish = [this] {
    finish(label("Edit sequence", "Изменить секвенсор"));
  };
  add(compact, {235, 574, 1092, 55});
  routingChoice = combo({"Serial", "Parallel", "Multiband"},
                        {968, 640, 140, 25}, [this](int i) {
                          edit("Flux routing", [&](State &s) {
                            s.routing = Routing(i);
                            if (s.routing == Routing::Parallel)
                              for (auto &slot : s.modules)
                                if (slot.lane == Lane::C)
                                  slot.lane = Lane::B;
                          });
                        });
  button(label("+ Add effect", "+ Добавить"), {1117, 640, 131, 25},
         [this] { moduleMenu(slotCount); });
  button("⋯", {1257, 640, 65, 25}, [this] { moduleMenu(selected); });
  chainScroll = new QScrollArea(surface);
  chainScroll->setWidgetResizable(false);
  chainScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  chainScroll->viewport()->setAutoFillBackground(false);
  chainBody = new QWidget;
  chainBody->setStyleSheet("background:transparent;");
  chainScroll->setWidget(chainBody);
  add(chainScroll, {17, 672, 1309, 79});
  knob(Input, label("Input", "Вход"), {57, 712, 89, 110});
  knob(Output, label("Output", "Выход"), {1233, 712, 89, 110});
  knob(Mix, "Dry / wet", {952, 712, 100, 110});
  knob(Balance, "A / B", {315, 712, 100, 110});
  bands = new fluxui::BandEditor(surface);
  bands->state = &state;
  bands->begin = [this] { begin(); };
  bands->changed = [this] { live(); };
  bands->finish = [this] {
    finish(label("Crossover frequencies", "Частоты разделения полос"));
  };
  bands->automate = [this](unsigned id) {
    emit q->automationRequested(str(parameterTable()[id].id));
  };
  add(bands, {165, 733, 409, 81});
  for (unsigned i = 0; i < 3; ++i) {
    knob(LowGain + i,
         i == 0   ? "Low"
         : i == 1 ? "Mid"
                  : "High",
         {604. + i * 104, 712, 84, 110},
         i == 0   ? fluxui::teal
         : i == 1 ? fluxui::gold
                  : fluxui::purple);
    button(
        "M", {682. + i * 104, 738, 20, 21},
        [this, i] {
          edit("Mute band", [i](State &s) {
            s.globals[LowMute + i] = 1 - s.globals[LowMute + i];
          });
        },
        true)
        ->setObjectName(QString("flux.band.mute.%1").arg(i));
    button(
        "S", {682. + i * 104, 766, 20, 21},
        [this, i] {
          edit("Solo band", [i](State &s) {
            s.globals[LowSolo + i] = 1 - s.globals[LowSolo + i];
          });
        },
        true)
        ->setObjectName(QString("flux.band.solo.%1").arg(i));
  }
  quality = combo({"Live", "Studio"}, {1068, 714, 118, 27}, [this](int i) {
    edit("Flux quality", [&](State &s) { s.studio = i == 1; });
  });
  button("Panic", {1068, 746, 118, 26}, [this] {
    if (controller) {
      controller->setInsertParameter(channel, insert, "panic", 0);
      controller->setInsertParameter(channel, insert, "panic", 1);
    }
    status->setText(label("Buffers cleared", "Буферы очищены"));
  });
  check(label("Protect", "Защита"), {1068, 778, 118, 20}, [this](bool on) {
    edit("Output protection", [&](State &s) { s.globals[Protect] = on; });
  })->setChecked(state.globals[Protect] > .5);
  check("Bypass", {1068, 800, 118, 20}, [this](bool on) {
    edit("Flux bypass", [&](State &s) { s.globals[Bypass] = on; });
  });
  status = new QLabel(surface);
  status->setStyleSheet("font-size:10px;color:#626a69;");
  add(status, {260, 559, 697, 18});
  // Reserve a full parameter row and keep the sequence and chain in their own
  // panels. Coordinates below are shared by every editor page.
  for (auto &w : widgets) {
    if (w.widget == presets)
      w.rect = {18, 135, 196, 370};
    else if (w.widget == description)
      w.rect = {24, 522, 184, 78};
    else if (w.widget == graph || w.widget == sequence || w.widget == routing)
      w.rect = {241, 174, 714, 176};
    else if (w.rect.y() >= 388 && w.rect.y() <= 412 && w.rect.x() < 966)
      w.rect.moveTop(365);
    else if (w.rect.y() == 538)
      w.rect.moveTop(530);
    else if (w.widget == compact)
      w.rect = {235, 562, 726, 55};
    else if (w.widget == chainScroll)
      w.rect = {20, 662, 1304, 87};
    else if (w.rect.y() == 640)
      w.rect.moveTop(632);
    else if (w.widget == status)
      w.rect = {246, 618, 697, 12};
    else if (w.rect.y() >= 712)
      w.rect.translate(0, 54);
    else if (w.rect.x() >= 975 && w.rect.y() >= 467)
      w.rect.translate(0, w.rect.y() == 522 ? 50 : 44);
    else if (w.rect.x() == 1164 && w.rect.y() >= 243 && w.rect.y() <= 386) {
      w.rect.translate(9, 32);
      w.rect.setWidth(150);
    }
    else if (w.rect.y() >= 73 && w.rect.y() < 467)
      w.rect.translate(0, 16);
  }
  buildParameters();
  buildCards();
  populatePresets();
  layout();
}
void FluxPanel::Impl::drawBackground(QPainter &p) {
  if (background.isNull() ||
      background.size() != surface->size() * surface->devicePixelRatioF()) {
    background = QPixmap(surface->size() * surface->devicePixelRatioF());
    background.setDevicePixelRatio(surface->devicePixelRatioF());
    background.fill(fluxui::paper);
    QPainter b(&background);
    b.setRenderHint(QPainter::Antialiasing);
    b.scale(zoom, zoom);
    auto well = [&](QRectF r) {
      r = place(r);
      if (!r.isEmpty()) {
        fluxui::well(b, r);
        if (highContrast) {
          b.setPen(QPen(fluxui::muted, 1));
          b.setBrush(Qt::NoBrush);
          b.drawRoundedRect(r, 18, 18);
        }
      }
    };
    auto paintText = [&](QRectF r, const QString &t, int size, QColor color,
                         int align = Qt::AlignCenter) {
      r = place(r);
      if (!r.isEmpty())
        fluxui::text(b, r, t, size, color, align);
    };
    b.fillRect(QRectF(0, 0, logicalWidth, 900), fluxui::paper);
    well({8, 84, 216, 532});
    well({229, 84, 738, 432});
    well({975, 84, 361, 154});
    well({975, 246, 181, 222});
    well({1164, 246, 172, 222});
    well({975, 476, 361, 140});
    well({229, 524, 738, 94});
    well({8, 624, 1328, 128});
    well({8, 758, 147, 130});
    well({163, 758, 774, 130});
    well({945, 758, 243, 130});
    well({1196, 758, 140, 130});
    QFont font = fluxui::controlFont(38);
    font.setWeight(QFont::DemiBold);
    b.setFont(font);
    b.setPen(fluxui::ink);
    b.drawText(QRectF(78, 13, 160, 45), "flux");
    b.save();
    b.setPen(Qt::NoPen);
    b.setBrush(fluxui::ink);
    b.drawRoundedRect(QRectF(24, 22, 42, 42), 12, 12);
    b.setPen(QPen(fluxui::panelWhite, 1.6, Qt::SolidLine, Qt::RoundCap));
    for (int i = 0; i < 3; ++i) {
      QPainterPath wave;
      wave.moveTo(33, 40 + i * 4);
      wave.cubicTo(40, 28 + i * 4, 48, 55 + i * 2, 58, 36 + i * 4);
      b.drawPath(wave);
    }
    b.restore();
    paintText({80, 55, 165, 18}, label("Rhythmic effects", "Ритмические эффекты"), 11, fluxui::muted,
              Qt::AlignLeft | Qt::AlignVCenter);
    paintText({992, 94, 134, 22}, label("Macros", "Макросы"), 15, fluxui::ink,
              Qt::AlignLeft | Qt::AlignVCenter);
    paintText({989, 249, 151, 18}, "Motion pad", 13, fluxui::ink,
              Qt::AlignLeft | Qt::AlignVCenter);
    paintText({1177, 249, 150, 18}, label("Trigger", "Запуск"), 13, fluxui::ink,
              Qt::AlignLeft | Qt::AlignVCenter);
    paintText({992, 485, 179, 18}, label("Scenes", "Сцены"), 13, fluxui::ink,
              Qt::AlignLeft | Qt::AlignVCenter);
    paintText({24, 631, 650, 26},
              label("Effect chain", "Цепочка эффектов") +
                  QString("   /   %1").arg(cards.size()),
              15, fluxui::ink, Qt::AlignLeft | Qt::AlignVCenter);
    paintText({187, 767, 374, 19},
              state.routing == Routing::Parallel
                  ? label("Parallel routing", "Параллельная обработка")
              : state.routing == Routing::Serial
                  ? label("Frequency split · inactive", "Частотные полосы · выключены")
                  : label("Multiband processing", "Многополосная обработка"),
              12, fluxui::muted, Qt::AlignLeft | Qt::AlignVCenter);
    for (double x : {589., 1059.}) {
      b.setPen(fluxui::border);
      auto line = place({x, 775, 1, 94});
      b.drawLine(line.topLeft(), line.bottomLeft());
    }
    paintText({24, 889, 200, 11}, "F L U X  /  V L T O N E", 8, fluxui::muted,
              Qt::AlignLeft | Qt::AlignVCenter);
    for (unsigned i = 0; i < 4; ++i) {
      b.setPen(Qt::NoPen);
      b.setBrush(i == 0 ? fluxui::teal : i == 1 ? fluxui::purple : i == 2 ? fluxui::clay : fluxui::gold);
      b.drawEllipse(QPointF(logicalWidth - 59 + i * 13, 895), 2, 2);
    }
  }
  p.drawPixmap(0, 0, background);
  p.save();
  p.scale(zoom, zoom);
  for (unsigned meter = 0; meter < 4; ++meter) {
    double value =
        meter < 2 ? telemetry.input[meter] : telemetry.output[meter - 2];
    double level =
        std::clamp((20 * std::log10(std::max(1e-6, value)) + 60) / 60, 0., 1.);
    double x = meter < 2 ? 22 + meter * 15 : 1203 + (meter - 2) * 14;
    x *= logicalWidth / 1344;
    p.fillRect(QRectF(x, 779, 9, 86), fluxui::field);
    for (unsigned step = 0; step < 43; ++step) {
      auto color = step > 37 ? fluxui::gold : fluxui::teal;
      if (value >= 1 && step > 40)
        color = QColor("#ff7755");
      if (step / 43. >= level)
        color = QColor("#dcded7");
      p.fillRect(QRectF(x + 1, 863 - step * 1.9, 7, 1.3), color);
    }
  }
  fluxui::text(p, place({986, 576, 202, 19}),
               QString("%1  →  %2     |     %3 %4")
                   .arg(telemetry.playingScene + 1)
                   .arg(telemetry.queuedScene < 16
                            ? QString::number(telemetry.queuedScene + 1)
                            : "—")
                   .arg(telemetry.latency)
                   .arg(label("samples", "отсч.")),
               10, fluxui::muted);
  fluxui::text(p, place({992, 599, 157, 13}),
               label("Correlation ", "Корреляция ") +
                   QString::number(telemetry.correlation, 'f', 2),
               9, fluxui::muted, Qt::AlignLeft | Qt::AlignVCenter);
  auto correlation = place({1157, 604, 168, 3});
  p.fillRect(correlation, fluxui::border);
  double centre = correlation.center().x(),
         end = centre + std::clamp(double(telemetry.correlation), -1., 1.) *
                            correlation.width() * .5;
  p.fillRect(QRectF(QPointF(std::min(centre, end), correlation.top()),
                    QPointF(std::max(centre, end), correlation.bottom())),
             telemetry.correlation < 0 ? fluxui::gold : fluxui::teal);
  p.restore();
}
QRectF FluxPanel::Impl::place(QRectF r) const {
  if (!collapsed)
    return r;
  if (r.y() < 84) {
    if (r.x() >= 1270)
      r.moveLeft(logicalWidth - 66);
    return r;
  }
  if (r.y() < 622) {
    if (r.x() < 225)
      return {};
    if (r.x() < 975) {
      double factor = (logicalWidth - 385) / 738;
      r.setRect(8 + (r.x() - 229) * factor, r.y(), r.width() * factor,
                r.height());
    } else
      r.translate(logicalWidth - 1344, 0);
    return r;
  }
  if (r.y() < 764) {
    if (r.width() > 1300)
      r.setWidth(logicalWidth - r.x() - 18);
    else if (r.x() > 950)
      r.translate(logicalWidth - 1344, 0);
    return r;
  }
  double factor = logicalWidth / 1344;
  r.setRect(r.x() * factor, r.y(), r.width() * factor, r.height());
  return r;
}
void FluxPanel::Impl::layout() {
  viewport->setGeometry(q->rect());
  double fit = std::min(q->width() / 1344., q->height() / 900.);
  zoom = zoomSetting > 0 ? zoomSetting : std::max(.75, fit);
  double available =
      q->width() - (qRound(900 * zoom) > q->height()
                        ? q->style()->pixelMetric(QStyle::PM_ScrollBarExtent)
                        : 0);
  collapsed = available / zoom < 1300;
  logicalWidth = collapsed ? std::max(1080., available / zoom) : 1344;
  surface->resize(qRound(logicalWidth * zoom), qRound(900 * zoom));
  for (auto &item : widgets) {
    auto r = place(item.rect);
    item.widget->setGeometry(qRound(r.x() * zoom), qRound(r.y() * zoom),
                             qRound(r.width() * zoom),
                             qRound(r.height() * zoom));
    QFont font = fluxui::controlFont(std::max(10, qRound(
        (item.widget == presetActions ? 16 : item.widget == moduleTitle ? 14 : 12) * zoom)));
    if (item.widget == moduleTitle) font.setWeight(QFont::DemiBold);
    item.widget->setFont(font);
    item.widget->setProperty("fluxScale", zoom);
    if (dynamic_cast<fluxui::ComboBox *>(item.widget))
      item.widget->setStyleSheet(zoom < 1 ? "padding:2px 3px 2px 5px;" : "");
    bool mode = true;
    if (item.widget == bands)
      mode = state.routing != Routing::Parallel;
    if (auto *k = dynamic_cast<fluxui::Knob *>(item.widget);
        k && k->index == Balance)
      mode = state.routing == Routing::Parallel;
    item.widget->setVisible(mode && !r.isEmpty() &&
                            !(collapsed && item.widget == search) &&
                            (!item.page || item.page == page + 1));
  }
  for (unsigned i = 0; i < cards.size(); ++i) {
    cards[i]->setGeometry(qRound(i * 157 * zoom), 0, qRound(151 * zoom),
                          qRound(82 * zoom));
    for (auto *child : cards[i]->findChildren<QWidget *>(
             QString(), Qt::FindDirectChildrenOnly)) {
      if (!child->property("fluxRect").isValid())
        child->setProperty("fluxRect", child->geometry());
      auto r = child->property("fluxRect").toRect();
      child->setGeometry(qRound(r.x() * zoom), qRound(r.y() * zoom),
                         qRound(r.width() * zoom), qRound(r.height() * zoom));
      child->setProperty("fluxScale", zoom);
      if (dynamic_cast<fluxui::Button *>(child))
        child->setFont(fluxui::controlFont(std::max(9, qRound(11 * zoom))));
    }
  }
  if (chainAdd) {
    chainAdd->setGeometry(qRound(cards.size() * 157 * zoom), 0,
                          qRound(95 * zoom), qRound(82 * zoom));
    chainAdd->setFont(fluxui::controlFont(qRound(30 * zoom)));
    chainAdd->setProperty("fluxScale", zoom);
  }
  chainBody->resize(
      qRound(std::max(chainScroll->width() / zoom, cards.size() * 157. + 95) *
             zoom),
      qRound(82 * zoom));
  updatePresetHeader();
  background = {};
  surface->update();
}
void FluxPanel::Impl::setPage(unsigned p) {
  page = p;
  for (unsigned i = 0; i < pageButtons.size(); ++i)
    pageButtons[i]->setChecked(i == p);
  layout();
}
void FluxPanel::Impl::buildParameters() {
  displayedSlot = selected;
  displayedKind = state.modules[selected].kind;
  for (auto *k : parameterKnobs) {
    std::erase_if(widgets, [k](auto &item) { return item.widget == k; });
    std::erase(knobs, k);
    delete k;
  }
  parameterKnobs.clear();
  auto fieldsList = fields(state.modules[selected].kind);
  if (state.modules[selected].kind == Kind::Empty)
    return;
  double width = 710. / std::max<std::size_t>(fieldsList.size(), 6);
  for (unsigned p = 0; p < fieldsList.size(); ++p) {
    auto *k = knob(effectParam(selected, state.modules[selected].kind, p),
                   QString::fromUtf8(fieldsList[p].name),
                   {242 + p * width, 397, width - 3, 108},
                   fluxui::color(state.modules[selected].kind));
    parameterKnobs.push_back(k);
  }
  if (viewport)
    layout();
}
void FluxPanel::Impl::buildCards() {
  displayedSlots = state.modules;
  displayedOrder = state.order;
  for (auto *card : cards)
    delete card;
  cards.clear();
  delete chainAdd;
  chainAdd = nullptr;
  for (unsigned slot : state.order)
    if (state.modules[slot].kind != Kind::Empty) {
      auto *card = new ModuleTile(chainBody);
      card->slot = slot;
      card->setAcceptDrops(true);
      card->setAccessibleName(
          QString::fromUtf8(kindName(state.modules[slot].kind)));
      card->setToolTip(label("Drag to reorder. Routing tab changes branches.",
                             "Перетаскивайте для перестановки. Вкладка "
                             "маршрутизации меняет ветвь."));
      card->draw = [this, slot, card](QPainter &p) {
        p.scale(card->width() / 151., card->height() / 82.);
        auto accent = fluxui::color(state.modules[slot].kind);
        fluxui::well(p, {1, 1, 149, 80}, accent);
        if (slot == selected) {
          p.setPen(QPen(accent, 1.1));
          p.setBrush(Qt::NoBrush);
          p.drawRoundedRect(QRectF(1, 1, 149, 80), 17, 17);
        }
        p.setPen(Qt::NoPen);
        auto tint = accent;
        tint.setAlpha(23);
        p.setBrush(tint);
        p.drawRoundedRect(QRectF(8, 8, 20, 21), 5, 5);
        fluxui::text(p, {8, 8, 20, 21}, QString::number(slot + 1), 11, accent.darker(125));
        QPainterPath path;
        auto &c = state.scenes[state.editScene].modules[slot].curves[0];
        for (unsigned i = 0; i <= 40; ++i) {
          QPointF pt(11 + i * 1.3, 63 - 22 * evaluate(c, i / 40.));
          if (i)
            path.lineTo(pt);
          else
            path.moveTo(pt);
        }
        p.setPen(QPen(accent, 1.2));
        p.drawPath(path);
        QString lane =
            state.modules[slot].lane == Lane::Pre ? "PRE"
            : state.modules[slot].lane == Lane::Post
                ? "POST"
                : QString(QChar('A' + int(state.modules[slot].lane) - 1));
        fluxui::text(p, {9, 65, 55, 12}, lane, 8, fluxui::muted);
      };
      auto *name = new fluxui::Button(card);
      const QString title =
          QString::fromUtf8(kindName(state.modules[slot].kind));
      QFont titleFont = fluxui::controlFont(11);
      name->setText(
          QFontMetrics(titleFont).elidedText(title, Qt::ElideRight, 75));
      name->setToolTip(title);
      name->setAccessibleName(title);
      name->setProperty("fluxFlat", true);
      name->setProperty("fluxAlignLeft", true);
      name->setFont(titleFont);
      name->setGeometry(28, 8, 88, 21);
      QObject::connect(name, &QPushButton::clicked, q, [this, slot] {
        selected = slot;
        buildParameters();
        sync();
      });
      auto *menu = new fluxui::Button(card);
      menu->setIcon(icon("menu"));
      menu->setAccessibleName(label("Module menu", "Меню модуля"));
      menu->setGeometry(68, 52, 20, 20);
      menu->setProperty("fluxFlat", true);
      menu->setIconSize({14, 14});
      QObject::connect(menu, &QPushButton::clicked, q,
                       [this, slot] { moduleMenu(slot); });
      auto *mix =
          new fluxui::Knob(commonParam(slot, SlotMix), "Mix",
                           fluxui::color(state.modules[slot].kind), card);
      mix->setGeometry(91, 25, 52, 56);
      mix->setProperty("fluxCompact", true);
      mix->setValue(value(mix->index));
      QObject::connect(mix, &ui::Knob::valueChanged, q, [this, mix](double v) {
        if (updating)
          return;
        begin();
        setValue(mix->index, v);
        if (controller && state.editScene == telemetry.playingScene)
          controller->setInsertParameter(channel, insert,
                                         parameterTable()[mix->index].id, v);
        else
          live();
      });
      QObject::connect(mix, &ui::Knob::editFinished, q,
                       [this] { finish("Module Mix"); });
      QObject::connect(mix, &ui::Knob::automateRequested, q, [this, mix] {
        emit q->automationRequested(str(parameterTable()[mix->index].id));
      });
      mix->assign = [this](unsigned s, unsigned t) { assign(s, t); };
      auto *on = new fluxui::Button(card);
      on->setCheckable(true);
      on->setGeometry(115, 8, 31, 21);
      on->setIconSize({16, 16});
      on->setProperty("fluxPower", true);
      on->setProperty("fluxColor", fluxui::color(state.modules[slot].kind));
      on->setProperty("fluxEnabledSlot", slot);
      on->setAccessibleName(label("Enable module", "Включить модуль"));
      on->setChecked(state.scenes[state.editScene].modules[slot].enabled);
      on->setIcon(
          icon("power", on->isChecked() ? fluxui::accent : fluxui::muted));
      QObject::connect(
          on, &QPushButton::clicked, q, [this, slot](bool enabled) {
            edit("Module bypass", [&](State &s) {
              s.scenes[s.editScene].modules[slot].enabled = enabled;
            });
          });
      card->dropped = [this, slot](unsigned source) {
        if (source >= slotCount || source == slot)
          return;
        edit(label("Reorder modules", "Порядок модулей"), [&](State &s) {
          auto from = std::find(s.order.begin(), s.order.end(), source),
               to = std::find(s.order.begin(), s.order.end(), slot);
          unsigned index = unsigned(to - s.order.begin());
          std::vector<unsigned> order;
          for (unsigned i : s.order)
            if (i != source)
              order.push_back(i);
          order.insert(order.begin() + std::min<unsigned>(index, order.size()),
                       source);
          std::copy(order.begin(), order.end(), s.order.begin());
          s.modules[source].lane = s.modules[slot].lane;
        });
        buildCards();
      };
      cards.push_back(card);
      card->show();
    }
  chainAdd = new fluxui::Button("+", chainBody);
  chainAdd->setAccessibleName(label("Add effect", "Добавить эффект"));
  chainAdd->setToolTip(chainAdd->accessibleName());
  chainAdd->setFont(fluxui::controlFont(30));
  chainAdd->setEnabled(cards.size() < slotCount);
  QObject::connect(chainAdd, &QPushButton::clicked, q,
                   [this] { moduleMenu(slotCount); });
  chainAdd->show();
  layout();
}
void FluxPanel::Impl::updatePresetHeader() {
  presetActions->setText(str(state.name));
  presetActions->setToolTip(str(state.name) + "\n" + fluxui::presetDescription(state));
}
void FluxPanel::Impl::sync() {
  updating = true;
  selected = std::min(selected, slotCount - 1);
  state.editScene =
      std::min<unsigned>(state.editScene, state.scenes.size() - 1);
  if (displayedSlot != selected ||
      displayedKind != state.modules[selected].kind)
    buildParameters();
  if (displayedSlots != state.modules || displayedOrder != state.order)
    buildCards();
  graph->slot = selected;
  graph->curve = motion;
  sequence->selectedSlot = compact->selectedSlot = selected;
  routing->selected = selected;
  updatePresetHeader();
  description->setText(fluxui::presetDescription(state) + "\n\n" +
                       str(state.tags));
  favorite->setChecked(favorites.contains(str(state.presetId)));
  favorite->setIcon(
      icon("heart", favorite->isChecked() ? fluxui::accent : fluxui::ink));
  ab->setText(comparisonSlot ? "B" : "A");
  ab->setChecked(true);
  for (unsigned i = 0; i < pageButtons.size(); ++i)
    pageButtons[i]->setChecked(i == page);
  recordButton->setChecked(recording);
  auto kind = state.modules[selected].kind;
  auto &ss = state.scenes[state.editScene].modules[selected];
  auto &cv = ss.curves[motion];
  moduleTitle->setText(
      QString("%1  ·  %2").arg(selected + 1).arg(kindName(kind)));
  kindChoice->setCurrentIndex(kind == Kind::Empty ? -1 : int(kind) - 1);
  curveChoice->setCurrentIndex(int(motion));
  if (targetKind != kind) {
    targetChoice->clear();
    for (auto &field : fields(kind))
      targetChoice->addItem(fluxui::localize(field.name));
    targetKind = kind;
  }
  targetChoice->setCurrentIndex(int(cv.target));
  rate->setCurrentIndex(
      std::clamp(int(std::round(std::log2(cv.bars))) + 7, 0, 12));
  trigger->setCurrentIndex(int(cv.trigger));
  playMode->setCurrentIndex(int(cv.play));
  if (routingChoice->currentIndex() != int(state.routing)) {
    background = {};
    layout();
  }
  routingChoice->setCurrentIndex(int(state.routing));
  quality->setCurrentIndex(state.studio ? 1 : 0);
  seqOn->setChecked(ss.sequence.enabled);
  direction->setCurrentIndex(int(ss.sequence.direction));
  constexpr unsigned lengths[]{8, 16, 24, 32, 48, 64};
  auto li =
      std::find(std::begin(lengths), std::end(lengths), ss.sequence.length);
  length->setCurrentIndex(
      li == std::end(lengths) ? -1 : int(li - std::begin(lengths)));
  double divs[]{1, .5, .25, .125, .0625, .03125, 1. / 3, 1. / 6, .75, .375};
  int di = -1;
  for (unsigned i = 0; i < 10; ++i)
    if (std::abs(divs[i] - ss.sequence.division) < 1e-6)
      di = int(i);
  division->setCurrentIndex(di);
  for (auto *k : knobs) {
    if (!k->isEditing())
      k->setValue(value(k->index));
    k->locked =
        std::find(state.parameterLocks.begin(), state.parameterLocks.end(),
                  k->index) != state.parameterLocks.end();
    if (k->index >= Macro1 && k->index <= Macro4)
      k->caption = fluxui::localize(str(state.macroNames[k->index - Macro1]));
    k->update();
  }
  for (unsigned i = 0; i < 16; ++i) {
    sceneButtons[i]->setToolTip(i < state.scenes.size()
                                    ? str(state.scenes[i].name)
                                    : label("Create scene", "Создать сцену"));
    sceneButtons[i]->setProperty("fluxSceneActive", i == telemetry.playingScene);
    sceneButtons[i]->setProperty("fluxSceneQueued", i == telemetry.queuedScene);
    sceneButtons[i]->setProperty("fluxSceneSelected", i == state.editScene);
    sceneButtons[i]->update();
  }
  for (auto *k : knobs)
    if (k->index >= LowGain && k->index <= HighGain) {
      unsigned band = k->index - LowGain;
      k->setEnabled(state.routing == Routing::Multiband ||
                    (state.routing == Routing::Parallel && band < 2));
      k->caption = state.routing == Routing::Parallel
                       ? QString(QChar('A' + band))
                   : band == 0 ? "Low"
                   : band == 1 ? "Mid"
                               : "High";
    }
  for (unsigned i = 0; i < 3; ++i) {
    bool enabled = state.routing == Routing::Multiband ||
                   (state.routing == Routing::Parallel && i < 2);
    for (bool solo : {false, true})
      if (auto *button = q->findChild<QPushButton *>(
              QString(solo ? "flux.band.solo.%1" : "flux.band.mute.%1")
                  .arg(i))) {
        button->setChecked(state.globals[(solo ? LowSolo : LowMute) + i] > .5);
        button->setEnabled(enabled);
      }
  }
  if (bands)
    bands->setEnabled(state.routing == Routing::Multiband);
  graph->update();
  sequence->update();
  compact->update();
  routing->update();
  pad->update();
  if (bands)
    bands->update();
  for (auto *card : cards) {
    for (auto *button : card->findChildren<QPushButton *>())
      if (button->property("fluxEnabledSlot").isValid()) {
        const auto slot = button->property("fluxEnabledSlot").toUInt();
        const bool enabled =
            state.scenes[state.editScene].modules[slot].enabled;
        button->setChecked(enabled);
        button->setIcon(
            icon("power", enabled ? fluxui::accent : fluxui::muted));
      }
    for (auto *base : card->findChildren<ui::Knob *>())
      if (auto *mix = dynamic_cast<fluxui::Knob *>(base);
          mix && !mix->isEditing())
        mix->setValue(value(mix->index));
    card->update();
  }
  surface->update();
  updating = false;
}
void FluxPanel::Impl::refresh() {
  const auto previousTelemetry = telemetry;
  if (controller) {
    auto current = controller->fluxTelemetry(channel, insert);
    if (!current) {
      timer.stop();
      surface->setEnabled(false);
      return;
    }
    telemetry = *current;
    if (!gesture &&
        (telemetry.revision != lastRevision ||
         telemetry.parameterRevision != lastParameterRevision ||
         telemetry.playingScene != previousTelemetry.playingScene)) {
      if (auto snapshot = controller->fluxSnapshot(channel, insert)) {
        auto oldKind = state.modules[selected].kind;
        auto oldSlots = state.modules;
        unsigned edited = state.editScene;
        state = std::move(snapshot->state);
        state.editScene = std::min<unsigned>(edited, state.scenes.size() - 1);
        lastRevision = telemetry.revision;
        lastParameterRevision = telemetry.parameterRevision;
        if (oldKind != state.modules[selected].kind)
          buildParameters();
        bool changed = false;
        for (unsigned i = 0; i < 12; ++i)
          changed |= oldSlots[i].kind != state.modules[i].kind ||
                     oldSlots[i].lane != state.modules[i].lane;
        if (changed)
          buildCards();
        sync();
      }
    }
    if (telemetry.queuedScene != previousTelemetry.queuedScene)
      sync();
  }
  if (recording) {
    double t =
        std::clamp((telemetry.elapsedBeats - recordStart) /
                       (std::max(1.f, telemetry.barBeats) * state.pathBars),
                   0., 1.);
    if (state.path.empty() || t - state.path.back().time >= 1. / 255) {
      if (state.path.size() < maxPoints)
        state.path.push_back(
            {float(t), float(state.globals[X]), float(state.globals[Y])});
    }
    if (t >= 1) {
      recording = false;
      state.pathEnabled = true;
      finish(label("Record XY motion", "Запись движения XY"));
      sync();
    }
  }
  if (midiLearn >= 0 && telemetry.lastCC >= 0 &&
      telemetry.midiRevision != midiLearnRevision) {
    unsigned id = unsigned(midiLearn);
    midiLearn = -1;
    edit("MIDI Learn", [&](State &s) {
      std::erase_if(s.midi, [&](auto &m) { return m.parameter == id; });
      if (s.midi.size() < 128)
        s.midi.push_back({unsigned(telemetry.lastCC), id,
                          unsigned(telemetry.lastChannel + 1)});
    });
    status->setText(QString("MIDI CC %1 → %2")
                        .arg(telemetry.lastCC)
                        .arg(str(parameterTable()[id].name)));
  }
  for (auto *k : parameterKnobs) {
    unsigned f = (k->index - GlobalCount - CommonCount) % fieldCount;
    const double effective = telemetry.effective[selected][f];
    const bool show = std::abs(effective - k->value()) > .001;
    if (k->effective != effective || k->showEffective != show) {
      k->effective = effective;
      k->showEffective = show;
      k->update();
    }
  }
  if (telemetry.phases[selected] != previousTelemetry.phases[selected] ||
      telemetry.fill[selected] != previousTelemetry.fill[selected] ||
      telemetry.waveform != previousTelemetry.waveform)
    graph->update();
  if (telemetry.step != previousTelemetry.step) {
    sequence->update();
    compact->update();
  }
  if (telemetry.x != previousTelemetry.x || telemetry.y != previousTelemetry.y)
    pad->update();
  auto repaint = [&](QRectF area) {
    area = place(area);
    surface->update(QRectF(area.x() * zoom, area.y() * zoom,
                           area.width() * zoom, area.height() * zoom)
                        .toAlignedRect());
  };
  if (telemetry.input != previousTelemetry.input)
    repaint({18, 722, 31, 93});
  if (telemetry.output != previousTelemetry.output)
    repaint({1199, 722, 33, 93});
  if (telemetry.correlation != previousTelemetry.correlation ||
      telemetry.playingScene != previousTelemetry.playingScene ||
      telemetry.queuedScene != previousTelemetry.queuedScene ||
      telemetry.latency != previousTelemetry.latency)
    repaint({974, 548, 353, 29});
}
void FluxPanel::Impl::populatePresets() {
  libraryButtons[0]->setChecked(!userMode);
  libraryButtons[1]->setChecked(userMode);
  QString selectedId = str(state.presetId), query = search->text().trimmed();
  presets->clear();
  QMap<QString, QTreeWidgetItem *> folders;
  auto group = [&](QString name) {
    if (!folders.contains(name)) {
      auto *root = new QTreeWidgetItem(presets, {name});
      root->setIcon(0, icon("folder"));
      root->setForeground(0, QBrush(fluxui::ink));
      folders[name] = root;
    }
    return folders[name];
  };
  if (!userMode) {
    auto library = factoryPresets();
    if (factoryDescriptions.isEmpty()) {
      State metadata;
      for (unsigned i = 0; i < library.size(); ++i) {
        metadata.presetId =
            QString("factory.%1").arg(i + 1, 3, 10, QChar('0')).toStdString();
        metadata.description = library[i].description;
        factoryDescriptions.push_back(fluxui::presetDescription(metadata));
      }
    }
    for (unsigned i = 0; i < library.size(); ++i) {
      const auto &p = library[i];
      QString id = QString("factory.%1").arg(i + 1, 3, 10, QChar('0')),
              name = QString::fromUtf8(p.name),
              description = factoryDescriptions[i],
              tags = QString::fromUtf8(p.tags);
      if (onlyFavorites && !favorites.contains(id))
        continue;
      if (onlyRecent && !recent.contains(id))
        continue;
      if (!query.isEmpty() && !(name + " " + description + " " + tags)
                                   .contains(query, Qt::CaseInsensitive))
        continue;
      auto *root = group(QString::fromUtf8(p.category).mid(5));
      auto *item = new QTreeWidgetItem(root, {name});
      item->setFirstColumnSpanned(true);
      item->setData(0, Qt::UserRole, i);
      item->setToolTip(0, description + "\n" + tags);
      if (selectedId == id) {
        presets->setCurrentItem(item);
        root->setExpanded(true);
      }
      if (!query.isEmpty())
        root->setExpanded(true);
    }
  } else {
    QDirIterator it(userDirectory(), {"*.fluxpreset"}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
      QString file = it.next();
      QFile f(file);
      if (!f.open(QIODevice::ReadOnly) || f.size() > 16 * 1024 * 1024)
        continue;
      auto bytes = f.readAll();
      auto decoded = decodeState(
          {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
           std::size_t(bytes.size())});
      if (!decoded)
        continue;
      QString name = str(decoded->name), id = str(decoded->presetId);
      if (onlyFavorites && !favorites.contains(id))
        continue;
      if (onlyRecent && !recent.contains(id))
        continue;
      if (!query.isEmpty() &&
          !(name + " " + str(decoded->tags) + " " + str(decoded->description))
               .contains(query, Qt::CaseInsensitive))
        continue;
      auto *root =
          group(decoded->category.empty() ? "User" : str(decoded->category));
      auto *item = new QTreeWidgetItem(root, {name});
      item->setFirstColumnSpanned(true);
      item->setData(0, Qt::UserRole, file);
      root->setExpanded(true);
    }
  }
  for (auto *root : folders) {
    root->setText(1, QString::number(root->childCount()));
    root->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    root->setForeground(1, fluxui::muted);
    root->setToolTip(0, root->text(0));
  }
  if (presets->topLevelItemCount() && !presets->currentItem())
    presets->topLevelItem(0)->setExpanded(true);
}
void FluxPanel::Impl::load(unsigned i) { q->applyFactoryPreset(i); }
void FluxPanel::Impl::changeScene(unsigned scene, bool play) {
  State next = state;
  ensureScene(next, scene);
  next.editScene = scene;
  if (next.scenes.size() != state.scenes.size())
    apply(next, label("Create scene", "Создать сцену"));
  else
    state.editScene = scene;
  if (play) {
    if (controller)
      controller->setInsertParameter(channel, insert, "scene", scene);
    else {
      state.globals[SceneSelect] = scene;
      telemetry.playingScene = scene;
    }
  }
  buildParameters();
  sync();
}
void FluxPanel::Impl::assign(unsigned source, unsigned target) {
  if (!canModulate(target) || source > 36)
    return;
  edit(label("Assign modulation", "Назначить модуляцию"), [&](State &s) {
    auto &routes = s.scenes[s.editScene].routes;
    if (routes.size() < maxRoutes)
      routes.push_back({source, target, .35, 0, 1, true, true});
  });
  status->setText(label("Modulation assigned. Edit depth in Modulation…",
                        "Модуляция назначена. Глубина — в окне «Модуляция…»"));
}
void FluxPanel::Impl::browsePresets() {
  QDialog dialog(q);
  dialog.setWindowTitle(
      label("Flux · preset browser", "Flux · браузер пресетов"));
  dialog.resize(560, 680);
  dialog.setStyleSheet(skin());
  QVBoxLayout layout(&dialog);
  QHBoxLayout banks;
  fluxui::Button factory("Factory (100)"), user("User");
  banks.addWidget(&factory);
  banks.addWidget(&user);
  layout.addLayout(&banks);
  QLineEdit filter;
  filter.setPlaceholderText(search->placeholderText());
  filter.setText(search->text());
  layout.addWidget(&filter);
  QTreeWidget tree;
  tree.setHeaderHidden(true);
  layout.addWidget(&tree);
  QLabel details;
  details.setWordWrap(true);
  layout.addWidget(&details);
  auto populate = [&] {
    search->setText(filter.text());
    populatePresets();
    tree.clear();
    for (int i = 0; i < presets->topLevelItemCount(); ++i) {
      auto *copy = presets->topLevelItem(i)->clone();
      tree.addTopLevelItem(copy);
      copy->setExpanded(!filter.text().isEmpty() || userMode);
    }
  };
  QObject::connect(&factory, &QPushButton::clicked, &dialog, [&] {
    userMode = false;
    populate();
  });
  QObject::connect(&user, &QPushButton::clicked, &dialog, [&] {
    userMode = true;
    populate();
  });
  QObject::connect(&filter, &QLineEdit::textChanged, &dialog,
                   [&] { populate(); });
  QObject::connect(&tree, &QTreeWidget::itemClicked, &dialog,
                   [&](QTreeWidgetItem *item, int) {
                     if (!item->data(0, Qt::UserRole).isValid()) {
                       item->setExpanded(!item->isExpanded());
                       return;
                     }
                     if (userMode)
                       q->importPreset(item->data(0, Qt::UserRole).toString());
                     else
                       load(item->data(0, Qt::UserRole).toUInt());
                     details.setText(fluxui::presetDescription(state));
                   });
  QDialogButtonBox close(QDialogButtonBox::Close);
  layout.addWidget(&close);
  QObject::connect(&close, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  populate();
  dialog.exec();
}
void FluxPanel::Impl::presetMenu() {
  QMenu menu(surface);
  menu.addAction(label("Browse presets…", "Браузер пресетов…"), q,
                 [this] { browsePresets(); });
  menu.addAction("Init", q, [this] {
    apply(initialState(), "Init Flux");
    buildParameters();
    buildCards();
  });
  menu.addSeparator();
  menu.addAction(
      label("Save user preset…", "Сохранить пользовательский пресет…"), q,
      [this] { saveUser(); });
  menu.addAction(label("Duplicate preset…", "Дублировать пресет…"), q,
                 [this] { saveUser(true); });
  if (!userFile.isEmpty())
    menu.addAction(label("Rename / move user preset…",
                         "Переименовать / переместить пресет…"),
                   q, [this] { saveUser(false, true); });
  menu.addAction(label("Import .fluxpreset…", "Импорт .fluxpreset…"), q,
                 [this] {
                   auto file = QFileDialog::getOpenFileName(
                       q, "Flux", {}, "Flux presets (*.fluxpreset)");
                   if (!file.isEmpty() && !q->importPreset(file))
                     QMessageBox::warning(
                         q, "Flux",
                         label("Invalid or unsupported preset.",
                               "Повреждённый или неподдерживаемый пресет."));
                 });
  menu.addAction(label("Export .fluxpreset…", "Экспорт .fluxpreset…"), q,
                 [this] {
                   auto file = QFileDialog::getSaveFileName(
                       q, "Flux", str(state.name) + ".fluxpreset",
                       "Flux presets (*.fluxpreset)");
                   if (!file.isEmpty() && !q->exportPreset(file))
                     QMessageBox::warning(q, "Flux",
                                          label("Could not write the preset.",
                                                "Не удалось записать пресет."));
                 });
  menu.addSeparator();
  auto *fav = menu.addAction(label("Favorites only", "Только избранное"));
  fav->setCheckable(true);
  fav->setChecked(onlyFavorites);
  QObject::connect(fav, &QAction::triggered, q, [this](bool on) {
    onlyFavorites = on;
    populatePresets();
  });
  auto *rec = menu.addAction(label("Recently used", "Недавние"));
  rec->setCheckable(true);
  rec->setChecked(onlyRecent);
  QObject::connect(rec, &QAction::triggered, q, [this](bool on) {
    onlyRecent = on;
    populatePresets();
  });
  auto *ml = menu.addAction(
      label("Lock Mix when browsing", "Не менять Mix при переборе"));
  ml->setCheckable(true);
  ml->setChecked(mixLock);
  QObject::connect(ml, &QAction::triggered, q,
                   [this](bool on) { mixLock = on; });
  auto *ol = menu.addAction(
      label("Lock output when browsing", "Не менять выход при переборе"));
  ol->setCheckable(true);
  ol->setChecked(outputLock);
  QObject::connect(ol, &QAction::triggered, q,
                   [this](bool on) { outputLock = on; });
  menu.exec(QCursor::pos());
}
void FluxPanel::Impl::saveUser(bool duplicate, bool move) {
  QDialog dialog(q);
  dialog.setWindowTitle(label("Save Flux preset", "Сохранить пресет Flux"));
  dialog.setStyleSheet(skin());
  QFormLayout form(&dialog);
  QLineEdit name(duplicate ? str(state.name) + " Copy" : str(state.name)),
      folder(state.category.empty() ? "User" : str(state.category));
  QLineEdit tags(str(state.tags)), desc(str(state.description));
  form.addRow(label("Name", "Название"), &name);
  form.addRow(label("Folder", "Папка"), &folder);
  form.addRow(label("Tags", "Теги"), &tags);
  form.addRow(label("Description", "Описание"), &desc);
  QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  form.addRow(&buttons);
  QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog,
                   &QDialog::accept);
  QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted || name.text().trimmed().isEmpty())
    return;
  State next = state;
  next.name = name.text().trimmed().left(256).toStdString();
  next.category = folder.text().trimmed().left(256).toStdString();
  next.description = desc.text().left(2048).toStdString();
  next.tags = tags.text().left(1024).toStdString();
  if (!move)
    next.presetId =
        "user." + std::to_string(QRandomGenerator::global()->generate());
  auto safe = [](QString name) {
    name.replace(QRegularExpression("[<>:\"/\\\\|?*]"), "_");
    name = name.trimmed();
    while (name.endsWith('.') || name.endsWith(' '))
      name.chop(1);
    return name.isEmpty() ? QString("User") : name;
  };
  QString path = userDirectory() + "/" + safe(folder.text());
  QDir().mkpath(path);
  path += "/" + safe(name.text()) + ".fluxpreset";
  if (QFile::exists(path) &&
      QMessageBox::question(
          q, "Flux",
          label("Replace the existing user preset?",
                "Заменить существующий пользовательский пресет?")) !=
          QMessageBox::Yes)
    return;
  std::vector<std::uint8_t> bytes;
  if (!encodeState(next, bytes))
    return;
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(reinterpret_cast<const char *>(bytes.data()),
                 qint64(bytes.size())) != qint64(bytes.size()) ||
      !file.commit()) {
    QMessageBox::warning(
        q, "Flux",
        label("Could not save the preset.", "Не удалось сохранить пресет."));
    return;
  }
  if (move && !userFile.isEmpty() &&
      QFileInfo(userFile).absoluteFilePath() !=
          QFileInfo(path).absoluteFilePath() &&
      !QFile::remove(userFile))
    QMessageBox::warning(
        q, "Flux",
        label("Saved a copy; the original file could not be removed.",
              "Копия сохранена; исходный файл не удалось удалить."));
  userFile = path;
  apply(std::move(next), label("Save Flux preset", "Сохранить пресет Flux"));
  userMode = true;
  populatePresets();
}
void FluxPanel::Impl::settingsMenu() {
  QMenu menu(surface);
  auto *size = menu.addMenu(label("Interface scale", "Масштаб интерфейса"));
  for (double scale : {0., .75, 1., 1.25, 1.5, 2.}) {
    auto *a = size->addAction(scale == 0 ? "Fit"
                                         : QString::number(scale * 100) + "%");
    a->setCheckable(true);
    a->setChecked(zoomSetting == scale);
    QObject::connect(a, &QAction::triggered, q, [this, scale] {
      zoomSetting = scale;
      layout();
    });
  }
  auto *motionAction =
      menu.addAction(label("Reduce motion", "Уменьшить движение"));
  motionAction->setCheckable(true);
  motionAction->setChecked(reduced);
  QObject::connect(motionAction, &QAction::triggered, q, [this](bool on) {
    reduced = on;
    QSettings().setValue("Flux/reduceMotion", on);
    timer.setInterval(on ? 50 : 16);
  });
  auto *contrast =
      menu.addAction(label("High contrast", "Повышенная контрастность"));
  contrast->setCheckable(true);
  contrast->setChecked(highContrast);
  QObject::connect(contrast, &QAction::triggered, q, [this](bool on) {
    highContrast = on;
    surface->setProperty("fluxHighContrast", on);
    QSettings().setValue("Flux/highContrast", on);
    background = {};
    surface->setStyleSheet(
        skin() + (on ? "QPushButton,QComboBox,QLineEdit "
                       "{border-color:#626a69;} QTreeWidget {color:#292d2e;}"
                     : ""));
    surface->update();
    for (auto *widget : surface->findChildren<QWidget *>()) widget->update();
  });
  auto *random = menu.addMenu("Random");
  random->addAction(label("Everything", "Всё"), q,
                    [this] { q->randomizeParameters(); });
  random->addAction(label("Parameters", "Параметры"), q,
                    [this] { q->randomizeParameters(0, 1); });
  random->addAction(label("Curves", "Кривые"), q,
                    [this] { q->randomizeParameters(0, 2); });
  random->addAction(label("Sequence", "Секвенсор"), q,
                    [this] { q->randomizeParameters(0, 4); });
  random->addAction(
      label("Parameters + curves + sequence", "Параметры + кривые + секвенсор"),
      q, [this] { q->randomizeParameters(0, 7); });
  menu.addAction(
      label("Modulation / MIDI assignments…", "Модуляция / MIDI-назначения…"),
      q, [this] { modulationDialog(); });
  menu.addAction(label("Reset view", "Сбросить вид"), q, [this] {
    zoomSetting = 0;
    graph->fit();
    layout();
  });
  menu.exec(QCursor::pos());
}
void FluxPanel::Impl::moduleMenu(unsigned slot) {
  QMenu menu(surface);
  unsigned target = slot;
  if (target == slotCount) {
    for (unsigned i = 0; i < slotCount; ++i)
      if (state.modules[i].kind == Kind::Empty) {
        target = i;
        break;
      }
    if (target == slotCount) {
      status->setText(label("All twelve modules are in use.",
                            "Все двенадцать слотов заняты."));
      return;
    }
  }
  auto *types = slot == slotCount
                    ? &menu
                    : menu.addMenu(label("Replace effect", "Заменить эффект"));
  for (unsigned k = 1; k < unsigned(Kind::Count); ++k)
    types->addAction(kindName(Kind(k)), q, [this, target, k] {
      edit(label("Set effect", "Выбрать эффект"),
           [&](State &s) { setKind(s, target, Kind(k)); });
      selected = target;
      buildParameters();
      buildCards();
      sync();
    });
  if (slot < slotCount) {
    menu.addAction(label("Reset module", "Сбросить модуль"), q, [this, slot] {
      edit("Reset module",
           [&](State &s) { setKind(s, slot, s.modules[slot].kind); });
      buildParameters();
      sync();
    });
    menu.addAction(label("Duplicate module", "Дублировать модуль"), q,
                   [this, slot] {
                     unsigned empty = slotCount;
                     for (unsigned i = 0; i < slotCount; ++i)
                       if (state.modules[i].kind == Kind::Empty) {
                         empty = i;
                         break;
                       }
                     if (empty == slotCount)
                       return;
                     edit("Duplicate module", [&](State &s) {
                       s.modules[empty] = s.modules[slot];
                       for (auto &scene : s.scenes)
                         scene.modules[empty] = scene.modules[slot];
                     });
                     buildCards();
                   });
    auto *lane = menu.addMenu(label("Move to branch", "Переместить в ветвь"));
    QStringList lanes{"Pre", "A / Low", "B / Mid", "C / High", "Post"};
    for (unsigned l = 0; l < 5; ++l)
      if (l != 3 || state.routing != Routing::Parallel)
        lane->addAction(lanes[l], q, [this, slot, l] {
          edit("Module branch",
               [&](State &s) { s.modules[slot].lane = Lane(l); });
          buildCards();
        });
    auto *solo = menu.addAction("Solo");
    solo->setCheckable(true);
    solo->setChecked(state.scenes[state.editScene].modules[slot].solo);
    QObject::connect(solo, &QAction::triggered, q, [this, slot](bool on) {
      edit("Solo module",
           [&](State &s) { s.scenes[s.editScene].modules[slot].solo = on; });
    });
    auto *lock =
        menu.addAction(label("Lock for Random", "Не изменять при Random"));
    lock->setCheckable(true);
    lock->setChecked(state.modules[slot].locked);
    QObject::connect(lock, &QAction::triggered, q, [this, slot](bool on) {
      edit("Lock module", [&](State &s) { s.modules[slot].locked = on; });
    });
    menu.addAction(label("Remove module", "Удалить модуль"), q, [this, slot] {
      edit("Remove module", [&](State &s) { setKind(s, slot, Kind::Empty); });
      buildParameters();
      buildCards();
      sync();
    });
  }
  menu.exec(QCursor::pos());
}
void FluxPanel::Impl::sequenceMenu() {
  QMenu menu(surface);
  menu.addAction(label("Euclidean rhythm…", "Евклидов ритм…"), q, [this] {
    bool ok = false;
    unsigned len =
        state.scenes[state.editScene].modules[selected].sequence.length;
    int pulses = QInputDialog::getInt(q, "Euclidean", label("Hits", "Импульсы"),
                                      5, 0, int(len), 1, &ok);
    if (ok)
      edit("Euclidean rhythm", [&](State &s) {
        auto &q = s.scenes[s.editScene].modules[selected].sequence;
        q.enabled = true;
        euclidean(q, unsigned(pulses), 0);
      });
  });
  menu.addAction(label("Swing…", "Свинг…"), q, [this] {
    bool ok = false;
    double v = QInputDialog::getDouble(
        q, "Swing", "%",
        state.scenes[state.editScene].modules[selected].sequence.swing * 100, 0,
        75, 1, &ok);
    if (ok)
      edit("Swing", [&](State &s) {
        s.scenes[s.editScene].modules[selected].sequence.swing = v / 100;
      });
  });
  menu.addAction(label("Shift one step", "Сдвинуть на шаг"), q, [this] {
    edit("Shift pattern", [&](State &s) {
      auto &seq = s.scenes[s.editScene].modules[selected].sequence;
      std::rotate(seq.steps.begin(), seq.steps.begin() + seq.length - 1,
                  seq.steps.begin() + seq.length);
    });
  });
  menu.addAction(
      label("Copy row to every module", "Копировать строку во все модули"), q,
      [this] {
        edit("Copy sequence", [&](State &s) {
          auto pattern = s.scenes[s.editScene].modules[selected].sequence;
          for (auto &slot : s.scenes[s.editScene].modules)
            slot.sequence = pattern;
        });
      });
  menu.addAction(
      label("Probability for row…", "Вероятность для строки…"), q, [this] {
        bool ok = false;
        double probability =
            QInputDialog::getDouble(q, "Probability", "%", 80, 0, 100, 1, &ok);
        if (ok)
          edit("Sequence probability", [&](State &s) {
            for (auto &step :
                 s.scenes[s.editScene].modules[selected].sequence.steps)
              step.probability = float(probability / 100);
          });
      });
  menu.exec(QCursor::pos());
}
void FluxPanel::Impl::triggerDialog() {
  State next = state;
  QDialog dialog(q);
  dialog.setWindowTitle(
      label("Flux · motion and trigger", "Flux · огибающая и запуск"));
  dialog.setStyleSheet(skin());
  QFormLayout form(&dialog);
  auto &curve = next.scenes[next.editScene].modules[selected].curves[motion];
  auto number = [&](QString name, double &target, double lo, double hi,
                    int decimals = 3) {
    auto *box = new fluxui::NumberBox(&dialog);
    box->setRange(lo, hi);
    box->setDecimals(decimals);
    box->setValue(target);
    box->setKeyboardTracking(false);
    form.addRow(name, box);
    QObject::connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged),
                     &dialog, [&target](double v) { target = v; });
  };
  auto flag = [&](QString name, bool &target) {
    auto *box = new fluxui::Toggle(&dialog);
    box->setChecked(target);
    form.addRow(name, box);
    QObject::connect(box, &QCheckBox::toggled, &dialog,
                     [&target](bool v) { target = v; });
  };
  number(label("Motion depth", "Глубина огибающей"), curve.depth, -1, 1);
  flag(label("Tempo sync", "Синхронизация с темпом"), curve.sync);
  number(label("Length: note fraction / bars", "Длина: доля ноты / такты"),
         curve.bars, .0078125, 32, 6);
  number(label("Free frequency, Hz", "Свободная частота, Гц"), curve.hz, .01,
         40);
  number(label("Phase", "Фаза"), curve.phase, 0, 1);
  flag(label("Bipolar", "Биполярная"), curve.bipolar);
  number(label("Threshold, dB", "Порог, дБ"), next.detector.threshold, -80, 0);
  number(label("Sensitivity", "Чувствительность"), next.detector.sensitivity,
         .01, 10);
  number(label("Attack, s", "Атака, с"), next.detector.attack, .0001, 1, 4);
  number(label("Release, s", "Отпускание, с"), next.detector.release, .001, 5);
  number(label("Holdoff, s", "Пауза детектора, с"), next.detector.holdoff, .001,
         4);
  number(label("Hysteresis, dB", "Гистерезис, дБ"), next.detector.hysteresis, 0,
         24);
  number(label("Detector high-pass, Hz", "Нижняя граница детектора, Гц"),
         next.detector.highpass, 20, 10000, 1);
  number(label("Detector low-pass, Hz", "Верхняя граница детектора, Гц"),
         next.detector.lowpass, 100, 20000, 1);
  QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  form.addRow(&buttons);
  QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog,
                   &QDialog::accept);
  QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  if (dialog.exec() == QDialog::Accepted &&
      !apply(std::move(next), label("Motion settings", "Настройки движения")))
    QMessageBox::warning(q, "Flux",
                         label("Check the detector frequency range.",
                               "Проверьте границы частот детектора."));
}
void FluxPanel::Impl::modulationDialog() {
  State next = state;
  auto &scene = next.scenes[next.editScene];
  QDialog dialog(q);
  dialog.setWindowTitle(
      label("Flux · modulation and MIDI", "Flux · модуляция и MIDI"));
  dialog.resize(1050, 580);
  dialog.setStyleSheet(skin());
  QVBoxLayout outer(&dialog);
  QTabWidget tabs;
  outer.addWidget(&tabs);
  QWidget matrix;
  QVBoxLayout matrixLayout(&matrix);
  QTableWidget table;
  table.setColumnCount(7);
  table.setHorizontalHeaderLabels(
      {label("Source", "Источник"), label("Target", "Цель"),
       label("Depth", "Глубина"), label("Min", "Мин."), label("Max", "Макс."),
       "±", label("On", "Вкл.")});
  table.horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table.horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  matrixLayout.addWidget(&table);
  tabs.addTab(&matrix, label("Matrix · 64 routes", "Матрица · 64 связи"));
  std::vector<unsigned> ids;
  for (unsigned i = 0; i < GlobalCount; ++i)
    if (canModulate(i))
      ids.push_back(i);
  for (unsigned s = 0; s < slotCount; ++s)
    if (next.modules[s].kind != Kind::Empty) {
      for (unsigned c = 0; c < CommonCount; ++c)
        ids.push_back(commonParam(s, c));
      for (unsigned f = 0; f < fields(next.modules[s].kind).size(); ++f)
        ids.push_back(effectParam(s, next.modules[s].kind, f));
    }
  auto append = [&](ModRoute route) {
    int row = table.rowCount();
    table.insertRow(row);
    auto *source = new fluxui::ComboBox;
    for (unsigned i = 0; i < 37; ++i)
      source->addItem(i < 4     ? QString("Macro %1").arg(i + 1)
                      : i < 6   ? QString("XY %1").arg(i == 4 ? "X" : "Y")
                      : i < 10  ? QString("Modulator %1").arg(i - 5)
                      : i == 10 ? "Velocity"
                      : i == 11 ? "Input envelope"
                      : i == 12 ? "Sidechain envelope"
                                : QString("Slot %1 Motion %2")
                                      .arg((i - 13) / 2 + 1)
                                      .arg((i - 13) % 2 ? "B" : "A"));
    source->setCurrentIndex(int(route.source));
    table.setCellWidget(row, 0, source);
    auto *target = new fluxui::ComboBox;
    for (auto id : ids)
      target->addItem(str(parameterTable()[id].name), id);
    int current = target->findData(route.target);
    if (current < 0) {
      target->addItem(str(parameterTable()[route.target].name), route.target);
      current = target->count() - 1;
    }
    target->setCurrentIndex(current);
    table.setCellWidget(row, 1, target);
    for (unsigned c = 2; c <= 4; ++c) {
      auto *value = new fluxui::NumberBox;
      value->setRange(c == 2 ? -1 : 0, 1);
      value->setDecimals(3);
      value->setSingleStep(.01);
      value->setValue(c == 2 ? route.amount : c == 3 ? route.low : route.high);
      table.setCellWidget(row, int(c), value);
    }
    for (unsigned c = 5; c < 7; ++c) {
      auto *flag = new fluxui::Toggle;
      flag->setAccessibleName(c == 5 ? label("Bipolar route", "Биполярная связь")
                                     : label("Enable route", "Включить связь"));
      flag->setChecked(c == 5 ? route.bipolar : route.enabled);
      table.setCellWidget(row, int(c), flag);
    }
  };
  for (auto &r : scene.routes)
    append(r);
  QHBoxLayout commands;
  fluxui::Button add(label("+ Route", "+ Связь")),
      remove(label("Remove selected", "Удалить выбранную"));
  commands.addWidget(&add);
  commands.addWidget(&remove);
  matrixLayout.addLayout(&commands);
  QObject::connect(&add, &QPushButton::clicked, &dialog, [&] {
    if (table.rowCount() < 64)
      append({0, Mix, .25, 0, 1, true, true});
  });
  QObject::connect(&remove, &QPushButton::clicked, &dialog, [&] {
    if (table.currentRow() >= 0)
      table.removeRow(table.currentRow());
  });
  QWidget modulators;
  QGridLayout modLayout(&modulators);
  tabs.addTab(&modulators, label("Modulators", "Модуляторы"));
  for (unsigned i = 0; i < 4; ++i) {
    auto &mod = scene.modulators[i];
    auto *panel = new QWidget;
    auto *form = new QFormLayout(panel);
    modLayout.addWidget(panel, i / 2, i % 2);
    auto *title = new QLabel(QString("Modulator %1").arg(i + 1));
    form->addRow(title);
    auto *type = new fluxui::ComboBox;
    type->addItems({"LFO", "Step", "Random", "Envelope Follower"});
    type->setCurrentIndex(int(mod.type));
    form->addRow(label("Type", "Тип"), type);
    QObject::connect(type, qOverload<int>(&QComboBox::currentIndexChanged),
                     &dialog, [&mod](int v) { mod.type = unsigned(v); });
    auto *shape = new fluxui::ComboBox;
    shape->addItems({"Sine", "Ramp", "Triangle", "Pulse"});
    shape->setCurrentIndex(int(mod.shape));
    form->addRow(label("Shape", "Форма"), shape);
    QObject::connect(shape, qOverload<int>(&QComboBox::currentIndexChanged),
                     &dialog, [&mod](int v) { mod.shape = unsigned(v); });
    auto number = [&](QString label, double &field, double min, double max) {
      auto *spin = new fluxui::NumberBox;
      spin->setDecimals(4);
      spin->setRange(min, max);
      spin->setValue(field);
      form->addRow(label, spin);
      QObject::connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                       &dialog, [&field](double v) { field = v; });
    };
    number(label("Rate", "Скорость"), mod.rate, .01, 40);
    number(label("Phase", "Фаза"), mod.phase, 0, 1);
    number(label("Attack, s", "Атака, с"), mod.attack, .0001, 2);
    number(label("Release, s", "Отпускание, с"), mod.release, .001, 5);
    number(label("Smoothing, s", "Сглаживание, с"), mod.smooth, 0, 1);
    auto *synced = new fluxui::Toggle("Sync");
    synced->setChecked(mod.sync);
    form->addRow(synced);
    QObject::connect(synced, &QCheckBox::toggled, &dialog,
                     [&mod](bool v) { mod.sync = v; });
    auto *external = new fluxui::Toggle("Sidechain");
    external->setChecked(mod.sidechain);
    form->addRow(external);
    QObject::connect(external, &QCheckBox::toggled, &dialog,
                     [&mod](bool v) { mod.sidechain = v; });
    auto *steps = new QLineEdit;
    QStringList values;
    for (float v : mod.steps)
      values << QString::number(v, 'f', 2);
    steps->setText(values.join(' '));
    form->addRow("16 steps (0…1)", steps);
    QObject::connect(
        steps, &QLineEdit::editingFinished, &dialog, [&mod, steps] {
          auto list = steps->text().split(QRegularExpression("[ ,;]+"),
                                          Qt::SkipEmptyParts);
          for (unsigned j = 0; j < 16 && j < unsigned(list.size()); ++j) {
            bool ok = false;
            double v = list[j].toDouble(&ok);
            if (ok)
              mod.steps[j] = float(std::clamp(v, 0., 1.));
          }
        });
  }
  QWidget xy;
  QFormLayout xyForm(&xy);
  tabs.addTab(&xy, "Macros / XY / MIDI");
  for (unsigned i = 0; i < 4; ++i) {
    auto *name = new QLineEdit(str(next.macroNames[i]));
    name->setMaxLength(64);
    xyForm.addRow(QString("Macro %1").arg(i + 1), name);
    QObject::connect(name, &QLineEdit::textChanged, &dialog,
                     [&next, i](const QString &name) {
                       next.macroNames[i] = name.toStdString();
                     });
  }
  auto *bars = new fluxui::NumberBox;
  bars->setRange(.125, 8);
  bars->setValue(next.pathBars);
  xyForm.addRow(label("XY bars", "Такты XY"), bars);
  QObject::connect(bars, qOverload<double>(&QDoubleSpinBox::valueChanged),
                   &dialog, [&](double v) { next.pathBars = v; });
  auto *smoothing = new fluxui::NumberBox;
  smoothing->setRange(0, 1);
  smoothing->setDecimals(3);
  smoothing->setValue(next.pathSmooth);
  xyForm.addRow(label("XY smoothing, s", "Сглаживание XY, с"), smoothing);
  QObject::connect(smoothing, qOverload<double>(&QDoubleSpinBox::valueChanged),
                   &dialog, [&](double v) { next.pathSmooth = v; });
  auto *reverse = new fluxui::Toggle(label("Reverse path", "Обратная траектория"));
  reverse->setChecked(next.pathReverse);
  xyForm.addRow(reverse);
  QObject::connect(reverse, &QCheckBox::toggled, &dialog,
                   [&](bool v) { next.pathReverse = v; });
  auto *clearMidi = new fluxui::Button(
      label("Clear MIDI Learn assignments", "Очистить MIDI Learn"));
  xyForm.addRow(QString("MIDI Learn: %1").arg(next.midi.size()), clearMidi);
  QObject::connect(clearMidi, &QPushButton::clicked, &dialog, [&] {
    next.midi.clear();
    clearMidi->setEnabled(false);
  });
  QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  outer.addWidget(&buttons);
  QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog,
                   &QDialog::accept);
  QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted)
    return;
  scene.routes.clear();
  for (int row = 0; row < table.rowCount(); ++row) {
    ModRoute r;
    r.source = unsigned(
        qobject_cast<QComboBox *>(table.cellWidget(row, 0))->currentIndex());
    r.target = qobject_cast<QComboBox *>(table.cellWidget(row, 1))
                   ->currentData()
                   .toUInt();
    r.amount =
        qobject_cast<QDoubleSpinBox *>(table.cellWidget(row, 2))->value();
    r.low = qobject_cast<QDoubleSpinBox *>(table.cellWidget(row, 3))->value();
    r.high = qobject_cast<QDoubleSpinBox *>(table.cellWidget(row, 4))->value();
    r.bipolar =
        qobject_cast<QCheckBox *>(table.cellWidget(row, 5))->isChecked();
    r.enabled =
        qobject_cast<QCheckBox *>(table.cellWidget(row, 6))->isChecked();
    scene.routes.push_back(r);
  }
  if (!apply(std::move(next), label("Edit modulation", "Изменить модуляцию")))
    QMessageBox::warning(q, "Flux",
                         label("Check the modulation ranges.",
                               "Проверьте диапазоны модуляции."));
}

FluxPanel::FluxPanel(daw::EngineController *controller, QString channel,
                     QString insert, QWidget *parent)
    : QWidget(parent),
      d(std::make_unique<Impl>(this, controller, std::move(channel),
                               std::move(insert))) {
  setMinimumSize(minimumSizeHint());
}
FluxPanel::~FluxPanel() = default;
void FluxPanel::keyPressEvent(QKeyEvent *event) {
  if (event->matches(QKeySequence::Undo) ||
      event->matches(QKeySequence::Redo)) {
    if (auto *b = findChild<QPushButton *>(
            event->matches(QKeySequence::Undo) ? "flux.undo" : "flux.redo"))
      b->click();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}
void FluxPanel::resizeEvent(QResizeEvent *) { d->layout(); }
void FluxPanel::showEvent(QShowEvent *) {
  d->timer.start();
  d->refresh();
}
void FluxPanel::hideEvent(QHideEvent *) { d->timer.stop(); }
bool FluxPanel::event(QEvent *event) {
  if (event->type() == QEvent::LanguageChange && d) {
    QTimer::singleShot(0, this, [this] {
      if (!d)
        return;
      auto old = std::move(d);
      old->timer.stop();
      delete old->viewport;
      old->viewport = nullptr;
      d = std::make_unique<Impl>(this, old->controller, str(old->channel),
                                 str(old->insert));
      d->state = std::move(old->state);
      d->previewUndo = std::move(old->previewUndo);
      d->previewRedo = std::move(old->previewRedo);
      d->comparison[0] = std::move(old->comparison[0]);
      d->comparison[1] = std::move(old->comparison[1]);
      d->comparisonSlot = old->comparisonSlot;
      d->selected = old->selected;
      d->motion = old->motion;
      d->page = old->page;
      d->userFile = old->userFile;
      d->buildParameters();
      d->buildCards();
      d->populatePresets();
      d->sync();
      if (isVisible()) {
        d->viewport->show();
        d->timer.start();
      }
    });
  }
  return QWidget::event(event);
}
bool FluxPanel::applyFactoryPreset(unsigned i) {
  if (i >= 100)
    return false;
  State next = factoryState(i);
  if (d->mixLock)
    next.globals[Mix] = d->state.globals[Mix];
  if (d->outputLock)
    next.globals[Output] = d->state.globals[Output];
  if (!d->apply(next, label("Load Flux preset", "Загрузить пресет Flux")))
    return false;
  d->recent.removeAll(str(next.presetId));
  d->recent.prepend(str(next.presetId));
  while (d->recent.size() > 24)
    d->recent.removeLast();
  d->status->clear();
  d->userFile.clear();
  d->selected = 0;
  d->buildParameters();
  d->buildCards();
  d->populatePresets();
  d->sync();
  return true;
}
bool FluxPanel::randomizeParameters(unsigned seed, unsigned scope) {
  State next = d->state;
  const auto locks = next.parameterLocks;
  if (d->mixLock && std::find(locks.begin(), locks.end(), Mix) == locks.end())
    next.parameterLocks.push_back(Mix);
  if (d->outputLock &&
      std::find(locks.begin(), locks.end(), Output) == locks.end())
    next.parameterLocks.push_back(Output);
  randomize(next, seed ? seed : QRandomGenerator::global()->generate(), scope);
  next.parameterLocks = locks;
  if (!d->apply(std::move(next),
                label("Random Flux variation", "Случайный вариант Flux")))
    return false;
  d->userFile.clear();
  d->status->setText(label("Random variation · unsaved · Undo available",
                           "Случайный вариант · не сохранён · доступен Undo"));
  return true;
}
bool FluxPanel::importPreset(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
    return false;
  auto bytes = file.readAll();
  auto next =
      decodeState({reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                   std::size_t(bytes.size())});
  if (!next)
    return false;
  if (d->mixLock)
    next->globals[Mix] = d->state.globals[Mix];
  if (d->outputLock)
    next->globals[Output] = d->state.globals[Output];
  if (!d->apply(*next, label("Import Flux preset", "Импорт пресета Flux")))
    return false;
  d->userFile =
      QFileInfo(path).canonicalFilePath().startsWith(
          QDir(userDirectory()).canonicalPath() + "/", Qt::CaseInsensitive)
          ? QFileInfo(path).canonicalFilePath()
          : QString();
  d->selected = 0;
  d->buildParameters();
  d->buildCards();
  d->sync();
  return true;
}
bool FluxPanel::exportPreset(const QString &path) const {
  std::vector<std::uint8_t> bytes;
  if (!encodeState(d->state, bytes))
    return false;
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(reinterpret_cast<const char *>(bytes.data()),
                    qint64(bytes.size())) == qint64(bytes.size()) &&
         file.commit();
}
bool FluxPanel::visualUpdatesActive() const { return d->timer.isActive(); }
State FluxPanel::currentState() const { return d->state; }
