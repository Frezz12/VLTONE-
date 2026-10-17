#include "FluxWidgets.hpp"
#include "ScrollMotion.hpp"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainterPath>
#include <QSettings>
#include <QStyleOptionComboBox>
#include <QStyleOptionSpinBox>
#include <QStylePainter>
#include <QStyledItemDelegate>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace fluxui {
constexpr auto sourceMime = "application/x-vlt-flux-source";
constexpr auto slotMime = "application/x-vlt-flux-slot";
QFont controlFont(int size) {
  static const QFont base = [] {
    QFont f;
    f.setFamilies({"Segoe UI", "Helvetica Neue", "Noto Sans"});
    f.setFeature(QFont::Tag("tnum"), 1);
    return f;
  }();
  QFont f = base;
  f.setPixelSize(size);
  return f;
}
QString label(const char *en, const char *ru) {
  return QString::fromUtf8(
      QSettings().value("ui/language", "ru").toString().startsWith("ru") ? ru
                                                                         : en);
}
QString localize(const QString &value) {
  if (!QSettings().value("ui/language", "ru").toString().startsWith("ru"))
    return value;
  static const std::pair<const char *, const char *> words[]{
      {"Gain", "Уровень"},
      {"Smoothing", "Сглаживание"},
      {"Threshold", "Порог"},
      {"Ratio", "Степень"},
      {"Attack", "Атака"},
      {"Release", "Восстановл."},
      {"Knee", "Колено"},
      {"Mode", "Режим"},
      {"Capture unit", "Ед. захвата"},
      {"Beats", "Доли"},
      {"Bars", "Такты"},
      {"Attack gain", "Акцент"},
      {"Sustain", "Сустейн"},
      {"External detector", "Sidechain"},
      {"Cutoff", "Срез"},
      {"Resonance", "Резонанс"},
      {"Slope", "Крутизна"},
      {"Model", "Модель"},
      {"Drive", "Драйв"},
      {"Upper cutoff", "Верхний срез"},
      {"Vowel", "Гласная"},
      {"Width", "Ширина"},
      {"Shift", "Сдвиг"},
      {"Bias", "Смещение"},
      {"Tone", "Тембр"},
      {"Auto gain", "Автоуровень"},
      {"Bits", "Биты"},
      {"Sample rate", "Частота"},
      {"Jitter", "Нестаб."},
      {"Dither", "Дизер"},
      {"Filter", "Фильтр"},
      {"Level", "Уровень"},
      {"Color", "Окраска"},
      {"Envelope follow", "Огибающая"},
      {"Gate", "Порог"},
      {"History offset", "Позиция"},
      {"Speed", "Скорость"},
      {"Capture", "Фрагмент"},
      {"De-click", "Стык"},
      {"History", "История"},
      {"Repeat length", "Повтор"},
      {"Decay", "Затухание"},
      {"Reverse", "Назад"},
      {"Shuffle", "Перемеш."},
      {"Wow", "Детонация"},
      {"Flutter", "Флаттер"},
      {"Transpose", "Высота"},
      {"Fine tune", "Точно"},
      {"Formant", "Форманты"},
      {"Preserve formants", "Сохранять"},
      {"Scale", "Лад"},
      {"Root", "Тоника"},
      {"Position", "Позиция"},
      {"Grain size", "Зерно"},
      {"Density", "Плотность"},
      {"Pitch", "Высота"},
      {"Scatter", "Разброс"},
      {"Spread", "Разнос"},
      {"Freeze", "Заморозка"},
      {"Blur", "Размытие"},
      {"Low cutoff", "Низ"},
      {"High cutoff", "Верх"},
      {"Prune", "Прорежив."},
      {"Root note", "Нота"},
      {"Damping", "Глушение"},
      {"Voices", "Голоса"},
      {"Excitation", "Возбуждение"},
      {"Frequency", "Частота"},
      {"Depth", "Глубина"},
      {"Carrier", "Носитель"},
      {"Stereo phase", "Стереофаза"},
      {"Shift direction", "Направл."},
      {"Rate", "Скорость"},
      {"Detune", "Детюн"},
      {"Feedback", "Обрат. связь"},
      {"Delay", "Задержка"},
      {"Stages", "Каскады"},
      {"Center", "Центр"},
      {"Pan", "Панорама"},
      {"Auto-pan rate", "Автопан Гц"},
      {"Auto-pan depth", "Автопан"},
      {"Time", "Время"},
      {"Low cut", "Низ"},
      {"High cut", "Верх"},
      {"Diffusion", "Диффузия"},
      {"Ducking", "Приглуш."},
      {"Free time", "Время мс"},
      {"Size", "Размер"},
      {"Pre-delay", "Предзад."},
      {"Carrier note", "Нота"},
      {"Bandwidth", "Полоса"},
      {"Consonants", "Согласные"},
      {"MIDI carrier", "По MIDI"},
      {"Intensity", "Сила"},
      {"Movement", "Движение"},
      {"Texture", "Текстура"},
      {"Space", "Пространство"},
      {"Serial", "Последоват."},
      {"Parallel", "Параллельно"},
      {"Multiband", "Три полосы"},
      {"Forward", "Вперёд"},
      {"Ping-Pong", "Туда-обратно"},
      {"Random", "Случайно"},
      {"Loop", "Цикл"},
      {"One Shot", "Однократно"},
      {"MIDI Hold", "Удержание"},
      {"Host Sync", "Темп проекта"},
      {"Audio", "Аудио"},
      {"Manual", "Вручную"},
      {"Sync", "Синхр."},
      {"Snap", "Привязка"},
      {"Soft clip", "Мягкий"},
      {"Hard clip", "Жёсткий"},
      {"Tube", "Лампа"},
      {"Tape", "Лента"},
      {"Wavefold", "Фолдинг"},
      {"Rectifier", "Выпрямитель"},
      {"Compressor", "Компрессор"},
      {"Expander", "Экспандер"},
      {"Transient", "Транзиенты"},
      {"Low pass", "ФНЧ"},
      {"High pass", "ФВЧ"},
      {"Band pass", "Полосовой"},
      {"Notch", "Режектор"},
      {"Peak", "Пик"},
      {"Low shelf", "Низкая полка"},
      {"High shelf", "Высокая полка"},
      {"Dual", "Двойной"},
      {"White", "Белый"},
      {"Pink", "Розовый"},
      {"Brown", "Коричневый"},
      {"Hiss", "Шипение"},
      {"Crackle", "Треск"},
      {"Curve", "Кривая"},
      {"Half time", "Вдвое"},
      {"Quarter time", "Вчетверо"},
      {"Scratch", "Скретч"},
      {"Varispeed", "Скорость"},
      {"Stretch", "Растяжение"},
      {"Chromatic", "Хроматич."},
      {"Major", "Мажор"},
      {"Minor", "Минор"},
      {"Pentatonic", "Пентатон."},
      {"Harmonics", "Гармоники"},
      {"Ring modulation", "Кольцевая"},
      {"Frequency shift", "Сдвиг частот"},
      {"Sine", "Синус"},
      {"Triangle", "Треугольник"},
      {"Square", "Прямоуг."},
      {"Room", "Комната"},
      {"Hall", "Зал"},
      {"Plate", "Пластина"},
      {"Gated", "Гейт"},
      {"Saw", "Пила"},
      {"Pulse", "Импульс"},
      {"Balance", "Баланс"},
      {"Mid pan", "Панорама M"}};
  for (auto &[en, ru] : words)
    if (value == QLatin1String(en))
      return QString::fromUtf8(ru);
  return value;
}
QString controlText(unsigned id, double value) {
  const auto &info = parameterTable()[id];
  if (id >= GlobalCount) {
    unsigned local = (id - GlobalCount) % bankSize;
    if (local >= CommonCount) {
      auto defs = fields(Kind(1 + (local - CommonCount) / fieldCount));
      unsigned field = (local - CommonCount) % fieldCount;
      if (field < defs.size() && defs[field].choices && *defs[field].choices)
        return localize(QString::fromUtf8(defs[field].choices)
                            .split('|')
                            .value(qRound(value)));
    }
  }
  QString number = QString::number(info.unit == "%" ? value * 100 : value, 'f',
                                   info.isStepped || info.unit == "%" ? 0
                                   : std::abs(value) < 10             ? 2
                                                                      : 1);
  if (info.unit == "Hz" && value >= 1000)
    number = QString::number(value / 1000, 'f', 1) + "k";
  return number + " " + QString::fromStdString(info.unit);
}
QColor color(Kind k) {
  switch (k) {
  case Kind::Filter:
  case Kind::Vowel:
  case Kind::Stereo:
  case Kind::Chorus:
    return teal;
  case Kind::Pitch:
  case Kind::Granular:
  case Kind::Spectral:
  case Kind::Reverb:
  case Kind::Vocoder:
    return purple;
  case Kind::Resonator:
  case Kind::Delay:
  case Kind::Phaser:
    return gold;
  case Kind::Drive:
  case Kind::Crush:
    return clay;
  default:
    return accent;
  }
}
void text(QPainter &p, QRectF r, const QString &s, int size, QColor color,
          int align) {
  QFont f = controlFont(size);
  f.setWeight(size >= 16 ? QFont::DemiBold : QFont::Medium);
  p.setFont(f);
  p.setPen(color);
  p.drawText(r, align, QFontMetrics(f).elidedText(s, Qt::ElideRight, int(r.width())));
}
void well(QPainter &p, QRectF r, QColor accent) {
  p.save();
  p.setPen(QPen(border, .8));
  p.setBrush(panelWhite);
  const double radius = std::min(18., r.height() * .23);
  p.drawRoundedRect(r, radius, radius);
  if (accent.isValid()) {
    accent.setAlpha(8);
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), radius - 1, radius - 1);
  }
  p.restore();
}
namespace {
bool isHighContrast(const QWidget *widget) {
  for (auto *parent = widget; parent; parent = parent->parentWidget())
    if (parent->property("fluxHighContrast").isValid())
      return parent->property("fluxHighContrast").toBool();
  return false;
}
void contrastBorder(QPainter &p, const QRect &rect) {
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(muted, 1.25));
  const auto r = QRectF(rect).adjusted(1, 1, -1, -1);
  const double radius = std::min(10., r.height() * .28);
  p.drawRoundedRect(r, radius, radius);
}
void controlSurface(QPainter &p, QRectF r, bool down, bool active, bool hover,
                    QColor tint = accent, bool dark = false, bool flat = false) {
  const QColor fill = dark ? (down ? QColor("#151b19") : hover ? QColor("#3c4541") : ink)
                          : down ? QColor("#dfe3dc") : hover ? QColor("#e6e9e1")
                          : active ? QColor("#e4eeeb") : field;
  p.setBrush(flat && !hover && !down && !active ? Qt::transparent : fill);
  p.setPen(flat ? QPen(Qt::NoPen) : QPen(dark ? fill : active ? tint : border, .8));
  const double radius = std::min(10., r.height() * .28);
  p.drawRoundedRect(r, radius, radius);
}
void focusRing(QPainter &p, const QRect &r) {
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(focus, 1.5));
  p.drawRoundedRect(QRectF(r).adjusted(2, 2, -2, -2), 7, 7);
}
void chevron(QPainter &p, QPointF c, double scale, bool open = false) {
  const double d = open ? -1 : 1;
  QPainterPath arrow;
  arrow.moveTo(c + QPointF(-3.5, -1.5 * d) * scale);
  arrow.lineTo(c + QPointF(0, 2 * d) * scale);
  arrow.lineTo(c + QPointF(3.5, -1.5 * d) * scale);
  p.setPen(QPen(ink, 1.25 * scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.drawPath(arrow);
}
class ChoiceDelegate final : public QStyledItemDelegate {
public:
  explicit ChoiceDelegate(QComboBox *owner) : QStyledItemDelegate(owner), combo(owner) {}
  QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
    auto size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(32, QFontMetrics(option.font).height() + 16));
    return size;
  }
  void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const bool hot = option.state & (QStyle::State_Selected | QStyle::State_MouseOver);
    if (hot) {
      p->setPen(Qt::NoPen);
      p->setBrush(QColor("#e4eeeb"));
      p->drawRoundedRect(QRectF(option.rect).adjusted(2, 1, -2, -1), 6, 6);
    }
    p->setFont(option.font);
    p->setPen(option.state & QStyle::State_Enabled ? ink : muted);
    const auto r = option.rect.adjusted(13, 0, -30, 0);
    p->drawText(r, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(option.font).elidedText(index.data().toString(), Qt::ElideRight, r.width()));
    if (index.row() == combo->currentIndex()) {
      const QPointF c(option.rect.right() - 16, option.rect.center().y());
      QPainterPath tick;
      tick.moveTo(c + QPointF(-4, 0));
      tick.lineTo(c + QPointF(-1, 3));
      tick.lineTo(c + QPointF(5, -4));
      p->setPen(QPen(focus, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p->drawPath(tick);
    }
    p->restore();
  }
private:
  QComboBox *combo;
};
} // namespace
Button::Button(QWidget *parent) : Button(QString(), parent) {}
Button::Button(const QString &caption, QWidget *parent) : QPushButton(caption, parent) {
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_Hover);
}
void Button::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  if (!isEnabled()) p.setOpacity(.4);
  const double u = property("fluxScale").isValid() ? property("fluxScale").toDouble() : 1.;
  const QColor tint = property("fluxColor").isValid() ? property("fluxColor").value<QColor>() : accent;
  const bool dark = property("fluxPrimary").toBool() || property("fluxSceneActive").toBool() ||
                    (property("fluxTab").toBool() && isChecked());
  const bool selected = isChecked() || property("fluxSceneSelected").toBool();
  if (property("fluxPower").toBool()) {
    const double h = std::min(height() - 4., 18 * u);
    const QRectF track(2, (height() - h) / 2, width() - 4, h);
    p.setPen(Qt::NoPen);
    p.setBrush(isChecked() ? tint : QColor("#c9cfc5"));
    p.drawRoundedRect(track, h / 2, h / 2);
    const QPointF c(isChecked() ? track.right() - h / 2 : track.left() + h / 2, track.center().y());
    p.setBrush(panelWhite);
    p.drawEllipse(c, h / 2 - 2 * u, h / 2 - 2 * u);
    p.setPen(QPen(isChecked() ? tint : muted, 1.1 * u, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(c - QPointF(0, 3 * u), c + QPointF(0, 3 * u));
  } else {
    controlSurface(p, QRectF(rect()).adjusted(1, 1, -1, -1), isDown(), selected,
                   underMouse(), tint, dark, property("fluxFlat").toBool());
    if (isHighContrast(this) && !property("fluxFlat").toBool()) contrastBorder(p, rect());
    if (property("fluxSceneQueued").toBool()) {
      p.setPen(QPen(tint, 1.25, Qt::DashLine));
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 6, 6);
    }
    p.setFont(font());
    p.setPen(dark ? panelWhite : ink);
    const double inset = (width() < 32 * u ? 2 : 7) * u;
    QRectF labelRect = QRectF(rect()).adjusted(inset, 0, -inset, 0);
    if (property("fluxDropdown").toBool()) {
      labelRect.adjust(3 * u, 0, -19 * u, 0);
      chevron(p, {width() - 13 * u, height() / 2.}, u);
    }
    const int align = property("fluxDropdown").toBool() || property("fluxAlignLeft").toBool()
                          ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignCenter;
    const double iconWidth = std::min(18. * u, height() - 8. * u);
    if (!icon().isNull()) {
      const auto titleWidth = text().isEmpty() ? 0 : fontMetrics().horizontalAdvance(text()) + 7 * u;
      const double left = (width() - iconWidth - titleWidth) / 2;
      icon().paint(&p, QRect(qRound(left), qRound((height() - iconWidth) / 2),
                            qRound(iconWidth), qRound(iconWidth)), Qt::AlignCenter,
                   isEnabled() ? QIcon::Normal : QIcon::Disabled);
      labelRect.setLeft(left + iconWidth + 7 * u);
    }
    p.drawText(labelRect, align, fontMetrics().elidedText(text(), Qt::ElideRight, int(labelRect.width())));
  }
  if (hasFocus()) focusRing(p, rect());
}
ComboBox::ComboBox(QWidget *parent) : QComboBox(parent) {
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_Hover);
  auto *list = new QListView(this);
  list->setObjectName("fluxChoiceList");
  list->setUniformItemSizes(true);
  list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list->setStyleSheet("QListView {background:#fdfdfa;color:#292d2e;border:1px solid #dcded7;"
                     "border-radius:9px;padding:5px;outline:0;}");
  setView(list);
  setItemDelegate(new ChoiceDelegate(this));
  setMaxVisibleItems(10);
}
void ComboBox::showPopup() {
  view()->setFont(font());
  int contentWidth = width();
  for (int row = 0; row < count(); ++row)
    contentWidth = std::max(contentWidth, fontMetrics().horizontalAdvance(itemText(row)) + 56);
  view()->setMinimumWidth(std::min(contentWidth, 420));
  QComboBox::showPopup();
  update();
}
void ComboBox::hidePopup() { QComboBox::hidePopup(); update(); }
bool ComboBox::event(QEvent *event) {
  if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) update();
  return QComboBox::event(event);
}
void ComboBox::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  if (!isEnabled()) p.setOpacity(.45);
  const bool open = view()->isVisible();
  controlSurface(p, QRectF(rect()).adjusted(1, 1, -1, -1), open, open, underMouse());
  if (isHighContrast(this)) contrastBorder(p, rect());
  const double u = height() / 30.;
  const QRectF labelRect(9 * u, 0, width() - 32 * u, height());
  p.setPen(ink);
  p.setFont(font());
  p.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
             fontMetrics().elidedText(currentText(), Qt::ElideRight, int(labelRect.width())));
  chevron(p, {width() - 12 * u, height() / 2.}, u, open);
  if (hasFocus()) focusRing(p, rect());
}
void Surface::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  if (draw)
    draw(p);
}
void NumberBox::paintEvent(QPaintEvent *event) {
  QDoubleSpinBox::paintEvent(event);
  if (buttonSymbols() == QAbstractSpinBox::NoButtons)
    return;
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  QStyleOptionSpinBox option;
  initStyleOption(&option);
  for (bool up : {true, false}) {
    const auto control = up ? QStyle::SC_SpinBoxUp : QStyle::SC_SpinBoxDown;
    const auto area =
        style()->subControlRect(QStyle::CC_SpinBox, &option, control, this);
    const QPointF centre = QRectF(area).center();
    const bool enabled =
        isEnabled() &&
        (option.stepEnabled & (up ? QAbstractSpinBox::StepUpEnabled
                                  : QAbstractSpinBox::StepDownEnabled));
    p.setPen(QPen(enabled ? ink : muted, 1., Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    const double direction = up ? -1 : 1;
    QPainterPath arrow;
    arrow.moveTo(centre + QPointF(-2.5, -direction));
    arrow.lineTo(centre + QPointF(0, direction));
    arrow.lineTo(centre + QPointF(2.5, -direction));
    p.drawPath(arrow);
  }
}
void Toggle::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const double scale =
      property("fluxScale").isValid() ? property("fluxScale").toDouble() : 1.;
  const QRectF track(1, (height() - 16 * scale) / 2, 30 * scale, 16 * scale);
  p.setPen(Qt::NoPen);
  if (!isEnabled()) p.setOpacity(.45);
  p.setBrush(isChecked() ? ink : QColor("#c9cfc5"));
  p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
  const double radius = 6 * scale;
  const QPointF centre(isChecked() ? track.right() - 8 * scale
                                   : track.left() + 8 * scale,
                       track.center().y());
  p.setPen(Qt::NoPen);
  p.setBrush(panelWhite);
  p.drawEllipse(centre, radius, radius);
  p.setPen(QPen(isChecked() ? ink : muted, scale, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(centre - QPointF(0, 3 * scale), centre + QPointF(0, 3 * scale));
  p.setPen(ink);
  p.setFont(font());
  p.drawText(QRectF(track.right() + 6 * scale, 0,
                    width() - track.right() - 6 * scale, height()),
             Qt::AlignLeft | Qt::AlignVCenter, text());
  if (hasFocus()) focusRing(p, rect());
}
Knob::Knob(unsigned id, QString label, QColor c, QWidget *parent)
    : ui::Knob(label, parent), index(id), caption(fluxui::localize(label)),
      accent(c) {
  const auto &info = parameterTable()[id];
  setRange(info.minValue, info.maxValue);
  setDefaultValue(info.defaultValue);
  setStepped(info.isStepped);
  logarithmic = info.minValue > 0 &&
                (info.unit == "Hz" || info.unit == "ms" || info.unit == "s");
  setLogarithmic(logarithmic);
  setAutomatable(true);
  setObjectName(QString::fromStdString(info.id));
  setAccessibleName(caption);
  setToolTip(
      QString::fromStdString(info.name + " · " + info.unit) + "\n" +
      fluxui::label(
          "Drag · Shift: fine · Right-click: value, modulation, MIDI learn",
          "Перетаскивание · Shift: точно · ПКМ: значение, модуляция, MIDI "
          "Learn"));
  setMinimumSize(0, 0);
  setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
  setAcceptDrops(true);
  setFormatter([id](double v) { return controlText(id, v); });
}
void Knob::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  if (!isEnabled()) p.setOpacity(.4);
  const double uiScale =
      property("fluxScale").isValid() ? property("fluxScale").toDouble() : 1.;
  const bool compact = property("fluxCompact").toBool();
  const bool captionTop = property("fluxCaptionTop").toBool();
  const double labelHeight = std::max(12., 16. * uiScale);
  const double labelRows = compact                          ? 1.
                           : property("fluxMacro").toBool() ? 3.
                                                            : 2.;
  const double scale =
      std::min(width() / 88.,
               std::max(20., height() - labelRows * labelHeight - 1) / 78.);
  p.save();
  p.translate((width() - 88 * scale) * .5,
              captionTop ? labelHeight + 2 * uiScale : 0);
  p.scale(scale, scale);
  QPointF centre(44, 38);
  const double rasterScale = devicePixelRatioF() * scale;
  if (face.isNull() || std::abs(face.devicePixelRatio() - rasterScale) > .001) {
    face = QPixmap(QSize(88, 78) * rasterScale);
    face.setDevicePixelRatio(rasterScale);
    face.fill(Qt::transparent);
    QPainter q(&face);
    q.setRenderHint(QPainter::Antialiasing);
    q.setPen(Qt::NoPen);
    q.setBrush(QColor(35, 42, 36, 20));
    q.drawEllipse(centre + QPointF(0, 2), 28, 28);
    QLinearGradient cap(20, 12, 65, 67);
    cap.setColorAt(0, QColor("#454e4b"));
    cap.setColorAt(1, QColor("#282f2c"));
    q.setBrush(cap);
    q.setPen(QPen(QColor("#303834"), .8));
    q.drawEllipse(centre, 27, 27);
    q.setPen(QPen(QColor(255, 255, 255, 28), .7));
    q.setBrush(Qt::NoBrush);
    q.drawEllipse(centre, 25.2, 25.2);
  }
  p.drawPixmap(0, 0, face);
  double v =
      logarithmic
          ? std::log(std::max(minimumValue(), value()) / minimumValue()) /
                std::log(maximumValue() / minimumValue())
          : (value() - minimumValue()) / (maximumValue() - minimumValue());
  v = std::clamp(v, 0., 1.);
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(border, 2.8, Qt::SolidLine, Qt::RoundCap));
  p.drawArc(QRectF(13, 7, 62, 62), 225 * 16, -270 * 16);
  p.setPen(QPen(accent, 2.8, Qt::SolidLine,
                Qt::RoundCap));
  p.drawArc(QRectF(13, 7, 62, 62), 225 * 16, int(-270 * v * 16));
  if (showEffective) {
    double e = logarithmic
                   ? std::log(std::max(minimumValue(), effective) / minimumValue()) /
                         std::log(maximumValue() / minimumValue())
                   : (effective - minimumValue()) / (maximumValue() - minimumValue());
    p.setPen(QPen(teal, 1));
    p.drawArc(QRectF(9, 3, 70, 70), 225 * 16,
              int(-270 * std::clamp(e, 0., 1.) * 16));
  }
  double angle = (-225 + 270 * v) * std::numbers::pi / 180;
  auto point = [&](double radius) {
    return centre + QPointF(std::cos(angle) * radius, std::sin(angle) * radius);
  };
  p.setPen(QPen(panelWhite, 2.4, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(point(18), point(23));
  p.setBrush(accent);
  p.setPen(QPen(panelWhite, 1.3));
  p.drawEllipse(point(31), 2.7, 2.7);
  p.restore();
  if (!compact) {
    QFont captionFont = controlFont(std::max(10, qRound(12 * uiScale)));
    const auto visibleCaption =
        QFontMetrics(captionFont)
            .elidedText(caption, Qt::ElideRight, width() - 4);
    fluxui::text(p,
                 {0, captionTop ? 0 : height() - labelHeight,
                  double(width()), labelHeight},
                 visibleCaption, captionFont.pixelSize(), muted);
  }
  auto valueFont = controlFont(std::max(compact ? 9 : 11, qRound((compact ? 10 : 14) * uiScale)));
  valueFont.setWeight(QFont::DemiBold);
  p.setFont(valueFont);
  p.setPen(ink);
  p.drawText(QRectF(0, height() - (compact || captionTop ? 1 : 2) * labelHeight,
                    width(), labelHeight), Qt::AlignCenter,
             QFontMetrics(valueFont).elidedText(controlText(index, value()), Qt::ElideRight, width() - 2));
  if (hasFocus()) focusRing(p, rect());
  if (locked)
    fluxui::text(p, {width() - 18., 0, 17, 16}, "L", 10, gold);
}
void Knob::dragEnterEvent(QDragEnterEvent *e) {
  if (e->mimeData()->hasFormat(sourceMime))
    e->acceptProposedAction();
}
void Knob::dropEvent(QDropEvent *e) {
  if (e->mimeData()->hasFormat(sourceMime) && assign) {
    assign(e->mimeData()->data(sourceMime).toUInt(), index);
    e->acceptProposedAction();
  }
}
void Knob::contextMenuEvent(QContextMenuEvent *e) {
  QMenu menu(this);
  menu.addAction(label("Enter value…", "Ввести значение…"), this, [this] {
    bool ok = false;
    double v = QInputDialog::getDouble(
        this, caption, QString::fromStdString(parameterTable()[index].unit),
        value(), minimumValue(), maximumValue(), 4, &ok);
    if (ok) {
      editValue(v);
      finishEditing();
    }
  });
  menu.addAction(label("Reset", "Сбросить"), this, [this] {
    editValue(parameterTable()[index].defaultValue);
    finishEditing();
  });
  menu.addAction(label("Create automation", "Создать автоматизацию"), this,
                 [this] { emit automateRequested(); });
  auto *mods = menu.addMenu(label("Assign modulation", "Назначить модуляцию"));
  for (unsigned s = 0; s < 13; ++s) {
    QString name = s < 4     ? QString("Macro %1").arg(s + 1)
                   : s < 6   ? QString("XY %1").arg(s == 4 ? "X" : "Y")
                   : s < 10  ? QString("Modulator %1").arg(s - 5)
                   : s == 10 ? "Velocity"
                   : s == 11 ? "Input envelope"
                             : "Sidechain envelope";
    mods->addAction(name, this, [this, s] {
      if (assign)
        assign(s, index);
    });
  }
  menu.addAction("MIDI Learn", this, [this] {
    if (learn)
      learn(index);
  });
  auto *lockedAction =
      menu.addAction(label("Lock for Random", "Не изменять при Random"));
  lockedAction->setCheckable(true);
  lockedAction->setChecked(locked);
  QObject::connect(lockedAction, &QAction::triggered, this, [this] {
    if (lock)
      lock(index);
  });
  menu.exec(e->globalPos());
}
void SourceButton::mouseMoveEvent(QMouseEvent *e) {
  if (e->buttons() & Qt::LeftButton) {
    auto *drag = new QDrag(this);
    auto *data = new QMimeData;
    data->setData(sourceMime, QByteArray::number(source));
    drag->setMimeData(data);
    drag->exec(Qt::CopyAction);
  }
}

CurveEditor::CurveEditor(QWidget *p) : QWidget(p) {
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName(label("Motion curve editor", "Редактор кривой движения"));
  setObjectName("flux.motion");
  setToolTip(
      label("Drag points. Ctrl: select. Alt: no snap. Wheel: zoom at cursor. "
            "Middle drag: pan.",
            "Перетаскивайте точки. Ctrl: выделение. Alt: без привязки. Колесо: "
            "масштаб под курсором. Средняя кнопка: перемещение."));
}
Curve &CurveEditor::data() {
  auto &c = state->scenes[state->editScene].modules[slot].curves[curve];
  selection.removeIf(
      [&](int i) { return i < 0 || unsigned(i) >= c.points.size(); });
  return c;
}
QRectF CurveEditor::area() const {
  const bool timeAxis =
      state && state->modules[slot].kind == Kind::Time &&
      state->scenes[state->editScene].modules[slot].curves[curve].target == 0;
  return QRectF(rect()).adjusted(timeAxis ? 55 : 16, 13, -15, -26);
}
QPointF CurveEditor::toView(const Point &pt) const {
  auto a = area();
  return {a.left() + (pt.x - offset) * zoom * a.width(),
          a.bottom() - pt.y * a.height()};
}
Point CurveEditor::fromView(QPointF at, Qt::KeyboardModifiers modifiers) const {
  auto a = area();
  double x = offset + (at.x() - a.left()) / (a.width() * zoom),
         y = 1 - (at.y() - a.top()) / a.height();
  if (snap && !(modifiers & Qt::AltModifier))
    x = std::round(x * grid * gridMultiplier) / (grid * gridMultiplier);
  return {float(std::clamp(x, 0., 1.)), float(std::clamp(y, 0., 1.)), 0,
          Shape::Smooth};
}
void CurveEditor::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  well(p, QRectF(rect()).adjusted(1, 1, -1, -1));
  if (!state)
    return;
  auto a = area();
  auto accent = color(state->modules[slot].kind);
  p.save();
  p.setClipRect(a);
  for (unsigned i = 0; i <= 32; ++i) {
    double t = i / 32.;
    double x = toView({float(t), 0}).x();
    p.setPen(QPen(i % 8 == 0   ? border
                  : i % 2 == 0 ? QColor("#e9ebe4")
                               : QColor("#f3f4ee"),
                  i % 8 == 0 ? 1 : .6));
    p.drawLine(QPointF(x, a.top()), QPointF(x, a.bottom()));
  }
  for (unsigned i = 0; i <= 8; ++i) {
    double y = a.top() + a.height() * i / 8;
    p.setPen(QPen(QColor("#e9ebe4"), .6));
    p.drawLine(QPointF(a.left(), y), QPointF(a.right(), y));
  }
  if (telemetry) {
    for (unsigned c = 0; c < 3; ++c)
      if (c == 0 ? waveInput : c == 1 ? waveOutput : waveSidechain) {
        QPainterPath path;
        for (unsigned i = 0; i < 256; ++i) {
          QPointF v(a.left() + a.width() * i / 255.,
                    a.center().y() -
                        telemetry->waveform[c][i] * a.height() * .4);
          if (i)
            path.lineTo(v);
          else
            path.moveTo(v);
        }
        auto col = c == 0 ? muted : c == 1 ? teal : purple;
        col.setAlpha(65);
        p.setPen(QPen(col, 1));
        p.drawPath(path);
      }
  }
  auto drawCurve = [&](unsigned which, bool main) {
    auto &c = state->scenes[state->editScene].modules[slot].curves[which];
    QPainterPath path;
    for (int i = 0; i <= int(a.width()); ++i) {
      double t = offset + i / (a.width() * zoom);
      QPointF point(a.left() + i, a.bottom() - evaluate(c, t) * a.height());
      if (i)
        path.lineTo(point);
      else
        path.moveTo(point);
    }
    QColor col = main ? accent : purple;
    col.setAlpha(main ? 255 : 90);
    if (main) {
      auto fill = path;
      fill.lineTo(a.bottomRight());
      fill.lineTo(a.bottomLeft());
      fill.closeSubpath();
      QLinearGradient gradient(a.topLeft(), a.bottomLeft());
      gradient.setColorAt(0, QColor(col.red(), col.green(), col.blue(), 35));
      gradient.setColorAt(1, Qt::transparent);
      p.fillPath(fill, gradient);
    }
    p.setPen(QPen(col, main ? 2. : 1.2));
    p.drawPath(path);
    if (main)
      for (unsigned i = 0; i < c.points.size(); ++i) {
        auto point = toView(c.points[i]);
        p.setPen(QPen(selection.contains(int(i)) ? ink : col, 1));
        p.setBrush(selection.contains(int(i)) ? ink : panelWhite);
        p.drawEllipse(point, selection.contains(int(i)) ? 4.5 : 3.4,
                      selection.contains(int(i)) ? 4.5 : 3.4);
      }
  };
  if (showOther)
    drawCurve(1 - curve, false);
  drawCurve(curve, true);
  if (telemetry) {
    double x = toView({telemetry->phases[slot][curve], 0}).x();
    p.setPen(QPen(ink, 1));
    p.drawLine(QPointF(x, a.top()), QPointF(x, a.bottom()));
  }
  if (state->modules[slot].kind == Kind::Time && telemetry &&
      telemetry->fill[slot] < 1) {
    QRectF unavailable(a.left(), a.top(), a.width(),
                       a.height() * (1 - telemetry->fill[slot]));
    p.fillRect(unavailable, QColor(108, 138, 156, 20));
    text(p, unavailable,
         label("Filling audio history…", "Заполнение истории аудио…"), 11,
         muted);
  }
  if (marquee) {
    p.setPen(QPen(teal, 1));
    p.setBrush(QColor(40, 151, 176, 25));
    p.drawRect(QRectF(down, last).normalized());
  }
  p.restore();
  if (state->modules[slot].kind == Kind::Time && data().target == 0) {
    const double bars = state->scenes[state->editScene].modules[slot].values[5];
    for (unsigned i = 0; i <= 4; ++i)
      text(p, {3, a.bottom() - a.height() * i / 4 - 7, 47, 14},
           i ? QString::number(-bars * i / 4, 'g', 3) + label(" bar", " т.")
             : label("Now", "Сейчас"),
           10, muted, Qt::AlignRight | Qt::AlignVCenter);
  }
  for (unsigned i = 0; i < 4; ++i)
    text(p, {a.left() + i * a.width() / 4, a.bottom() + 5, a.width() / 4, 17},
         QString("1.%1").arg(i + 1), 10, muted,
         Qt::AlignLeft | Qt::AlignVCenter);
  if (hasFocus()) {
    p.setPen(QPen(accent, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 6, 6);
  }
}
void CurveEditor::drawAt(QPointF at, Qt::KeyboardModifiers mods) {
  auto point = fromView(at, mods);
  auto &points = data().points;
  if (tool == 1 || tool == 3) {
    auto it = std::lower_bound(points.begin(), points.end(), point.x,
                               [](auto &p, float x) { return p.x < x; });
    point.shape = tool == 3 ? Shape::Hold : Shape::Linear;
    if (it != points.end() && std::abs(it->x - point.x) < .0001)
      *it = point;
    else if (points.size() < maxPoints)
      points.insert(it, point);
  } else if (tool == 2) {
    auto start = fromView(down, mods);
    start.shape = point.shape = Shape::Linear;
    points = original;
    const float lo = std::min(start.x, point.x),
                hi = std::max(start.x, point.x);
    std::erase_if(points, [&](auto &p) { return p.x > lo && p.x < hi; });
    for (auto p : {start, point}) {
      auto it = std::lower_bound(points.begin(), points.end(), p.x,
                                 [](auto &a, float x) { return a.x < x; });
      if (it != points.end() && std::abs(it->x - p.x) < .00001)
        *it = p;
      else if (points.size() < maxPoints)
        points.insert(it, p);
    }
  } else if (active >= 0 && unsigned(active) < points.size()) {
    if (tool == 4) {
      points[active].tension =
          std::clamp(float((down.y() - at.y()) / 100), -1.f, 1.f);
      points[active].shape = Shape::Exponential;
    } else {
      auto start = fromView(down, mods);
      float dx = point.x - start.x, dy = point.y - start.y, lo = -1, hi = 1;
      for (int i : selection)
        if (i > 0 && unsigned(i) + 1 < points.size()) {
          if (!selection.contains(i - 1))
            lo = std::max(lo, original[i - 1].x + .00001f - original[i].x);
          if (!selection.contains(i + 1))
            hi = std::min(hi, original[i + 1].x - .00001f - original[i].x);
        }
      dx = std::clamp(dx, lo, hi);
      for (int idx : selection) {
        auto v = original[idx];
        if (idx > 0 && unsigned(idx) + 1 < points.size())
          v.x += dx;
        v.y = std::clamp(v.y + dy, 0.f, 1.f);
        points[idx] = v;
      }
    }
  }
  if (changed)
    changed();
  update();
}
void CurveEditor::mousePressEvent(QMouseEvent *e) {
  if (!state)
    return;
  setFocus();
  ui::ScrollMotion::cancel(this);
  down = last = e->position();
  if (e->button() == Qt::MiddleButton) {
    panning = true;
    return;
  }
  if (e->button() != Qt::LeftButton)
    return;
  if (begin)
    begin();
  active = -1;
  auto &points = data().points;
  double best = 10;
  for (unsigned i = 0; i < points.size(); ++i) {
    double d = QLineF(toView(points[i]), e->position()).length();
    if (d < best) {
      best = d;
      active = int(i);
    }
  }
  if (e->modifiers() & Qt::ShiftModifier && active < 0) {
    marquee = true;
    selection.clear();
    update();
    return;
  }
  if (active < 0 && tool != 1 && tool != 3 && points.size() < maxPoints) {
    auto point = fromView(e->position(), e->modifiers());
    if (point.x > 0 && point.x < 1) {
      auto it = std::lower_bound(points.begin(), points.end(), point.x,
                                 [](auto &p, float x) { return p.x < x; });
      if (it == points.end() || std::abs(it->x - point.x) > .00001) {
        active = int(it - points.begin());
        points.insert(it, point);
      } else
        active = int(it - points.begin());
    }
  }
  if (!(e->modifiers() & Qt::ControlModifier) && !selection.contains(active))
    selection.clear();
  if (active >= 0)
    selection.insert(active);
  original = points;
  dragging = true;
  drawAt(e->position(), e->modifiers());
}
void CurveEditor::mouseMoveEvent(QMouseEvent *e) {
  if (panning) {
    if (e->type() == QEvent::MouseMove && !(e->buttons() & Qt::MiddleButton)) {
      ui::ScrollMotion::cancel(this); panning = false; return;
    }
    const double scale = area().width() * zoom;
    ui::ScrollMotion::pan(this, {last.x() - e->position().x(), 0},
        [this, scale] { return QPointF(offset * scale, 0); },
        [this, scale](QPointF at) { offset = at.x() / scale; update(); },
        [this, scale](QPointF at) { return QPointF(std::clamp(at.x(), 0., (1 - 1 / zoom) * scale), 0); });
    last = e->position();
    update();
  } else if (marquee) {
    last = e->position();
    update();
  } else if (dragging)
    drawAt(e->position(), e->modifiers());
}
void CurveEditor::mouseReleaseEvent(QMouseEvent *e) {
  if (panning) {
    mouseMoveEvent(e);
    ui::ScrollMotion::finishPan(this);
    panning = false;
    return;
  }
  if (marquee) {
    QRectF r = QRectF(down, e->position()).normalized();
    for (unsigned i = 0; i < data().points.size(); ++i)
      if (r.contains(toView(data().points[i])))
        selection.insert(int(i));
    marquee = false;
  }
  dragging = false;
  if (finish)
    finish();
  update();
}
void CurveEditor::mouseDoubleClickEvent(QMouseEvent *e) {
  mousePressEvent(e);
  mouseReleaseEvent(e);
}
void CurveEditor::wheelEvent(QWheelEvent *e) {
  ui::ScrollMotion::cancel(this);
  double anchor =
      offset + (e->position().x() - area().left()) / (area().width() * zoom);
  zoom = std::clamp(zoom * std::pow(1.0015, e->angleDelta().y()), 1., 32.);
  offset = std::clamp(anchor - (e->position().x() - area().left()) /
                                   (area().width() * zoom),
                      0., 1 - 1 / zoom);
  update();
  e->accept();
}
void CurveEditor::fit() {
  ui::ScrollMotion::cancel(this);
  zoom = 1;
  offset = 0;
  update();
}
void CurveEditor::command(unsigned op) {
  if (!state)
    return;
  auto &c = data();
  if (op == 5) {
    copied.clear();
    for (unsigned i = 0; i < c.points.size(); ++i)
      if (selection.isEmpty() || selection.contains(int(i)))
        copied.push_back(c.points[i]);
    return;
  }
  if (op == 6 && copied.empty())
    return;
  if (begin)
    begin();
  if (op < 5)
    transformCurve(c, op);
  else if (op == 6) {
    auto points = c.points;
    std::erase_if(points, [&](const auto &point) {
      return point.x >= copied.front().x && point.x <= copied.back().x;
    });
    points.insert(points.end(), copied.begin(), copied.end());
    std::sort(points.begin(), points.end(),
              [](auto &a, auto &b) { return a.x < b.x; });
    if (points.size() <= maxPoints)
      c.points = std::move(points);
    selection.clear();
  } else if (op == 8) {
    std::vector<Point> duplicates;
    for (unsigned i = 0; i < c.points.size(); ++i)
      if (selection.isEmpty() || selection.contains(int(i)))
        duplicates.push_back(c.points[i]);
    const float shift = float(1. / (grid * gridMultiplier));
    for (auto point : duplicates)
      if (point.x + shift > 0 && point.x + shift < 1 &&
          c.points.size() < maxPoints) {
        point.x += shift;
        auto it = std::lower_bound(c.points.begin(), c.points.end(), point.x,
                                   [](auto &p, float x) { return p.x < x; });
        if (it == c.points.end() || std::abs(it->x - point.x) > .00001)
          c.points.insert(it, point);
      }
    selection.clear();
  } else if (op == 7) {
    for (int i = int(c.points.size()) - 2; i > 0; --i)
      if (selection.contains(i))
        c.points.erase(c.points.begin() + i);
    selection.clear();
  }
  if (changed)
    changed();
  if (finish)
    finish();
  update();
}
void CurveEditor::keyPressEvent(QKeyEvent *e) {
  if (!state)
    return;
  data();
  if (e->matches(QKeySequence::Copy)) {
    command(5);
    return;
  }
  if (e->matches(QKeySequence::Paste)) {
    command(6);
    return;
  }
  if (e->key() == Qt::Key_D && e->modifiers() & Qt::ControlModifier) {
    command(8);
    return;
  }
  if (e->matches(QKeySequence::SelectAll)) {
    for (unsigned i = 0; i < data().points.size(); ++i)
      selection.insert(int(i));
    update();
    return;
  }
  if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
    command(7);
    return;
  }
  if (e->key() == Qt::Key_Home) {
    fit();
    return;
  }
  if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right ||
      e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
    if (begin)
      begin();
    if (selection.isEmpty())
      selection.insert(0);
    for (int i : selection) {
      auto &p = data().points[i];
      double step = e->modifiers() & Qt::ShiftModifier ? .001 : .01;
      if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down)
        p.y = std::clamp(p.y + float(e->key() == Qt::Key_Up ? step : -step),
                         0.f, 1.f);
      else if (i > 0 && unsigned(i) + 1 < data().points.size())
        p.x = std::clamp(p.x + float(e->key() == Qt::Key_Right ? step : -step),
                         data().points[i - 1].x + .00001f,
                         data().points[i + 1].x - .00001f);
    }
    if (changed)
      changed();
    if (finish)
      finish();
    update();
    return;
  }
  QWidget::keyPressEvent(e);
}
void CurveEditor::contextMenuEvent(QContextMenuEvent *e) {
  QMenu m(this);
  const char *names[]{"Invert",        "Reverse",       "Smooth segments",
                      "Smooth values", "Shift phase",   "Copy",
                      "Paste",         "Delete points", "Duplicate"};
  const char *ru[]{"Инверсия",          "Разворот",      "Плавные сегменты",
                   "Сгладить значения", "Сдвиг фазы",    "Копировать",
                   "Вставить",          "Удалить точки", "Дублировать"};
  for (unsigned i = 0; i < 9; ++i)
    m.addAction(label(names[i], ru[i]), this, [this, i] { command(i); });
  auto *shape = m.addMenu(label("Segment shape", "Форма сегмента"));
  const char *types[]{"Hold", "Linear", "Smooth", "Exponential", "Logarithmic"};
  for (unsigned i = 0; i < 5; ++i)
    shape->addAction(types[i], this, [this, i] {
      if (begin)
        begin();
      for (unsigned j = 0; j < data().points.size(); ++j)
        if (selection.isEmpty() || selection.contains(j))
          data().points[j].shape = Shape(i);
      if (changed)
        changed();
      if (finish)
        finish();
      update();
    });
  m.exec(e->globalPos());
}

namespace {
std::vector<std::vector<Step>> stepClipboard;
std::vector<Sequence> patternClipboard;
} // namespace
SequenceEditor::SequenceEditor(QWidget *p) : QWidget(p) {
  setFocusPolicy(Qt::StrongFocus);
  setObjectName("flux.sequence");
  setAccessibleName(
      label("Effect step sequencer", "Пошаговый секвенсор эффектов"));
  setToolTip(label(
      "Drag to paint. Ctrl-drag selects a range. Shift-drag sets strength. "
      "Ctrl+C/V copies a range or row; Ctrl+Shift+C/V copies the whole "
      "pattern.",
      "Рисование мышью. Ctrl: выделить диапазон. Shift: сила шага. Ctrl+C/V: "
      "копировать строку или диапазон. Ctrl+Shift+C/V: весь рисунок."));
}
std::vector<unsigned> SequenceEditor::rows() const {
  if (compact)
    return {selectedSlot};
  std::vector<unsigned> r;
  if (state)
    for (unsigned i : state->order)
      if (state->modules[i].kind != Kind::Empty)
        r.push_back(i);
  return r;
}
void SequenceEditor::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  well(p, QRectF(rect()).adjusted(1, 1, -1, -1));
  auto row = rows();
  if (row.empty() || !state)
    return;
  double left = compact ? 10 : 110, w = width() - left - 10,
         h = (height() - 22.) / row.size();
  unsigned max = 16;
  for (auto slot : row)
    max = std::max(
        max, state->scenes[state->editScene].modules[slot].sequence.length);
  for (unsigned r = 0; r < row.size(); ++r) {
    unsigned slot = row[r];
    auto &q = state->scenes[state->editScene].modules[slot].sequence;
    if (!compact)
      text(p, {8, 5 + r * h, 97, h},
           QString::fromUtf8(kindName(state->modules[slot].kind)), 11,
           color(state->modules[slot].kind), Qt::AlignLeft | Qt::AlignVCenter);
    for (unsigned i = 0; i < q.length; ++i) {
      auto &step = q.steps[i];
      QRectF cell(left + i * w / max + 1, 6 + r * h, w / max - 3, h - 5);
      p.setPen(Qt::NoPen);
      p.setBrush(field);
      p.drawRoundedRect(cell, 4, 4);
      if (step.enabled) {
        auto bar = cell.adjusted(1, 1, -1, -1);
        bar.setTop(bar.bottom() - bar.height() * step.amount);
        auto tint = color(state->modules[slot].kind);
        tint.setAlpha(q.enabled ? 165 : 70);
        p.setBrush(tint);
        p.drawRoundedRect(bar, 3, 3);
      }
      if (step.probability < 1) {
        p.setPen(QPen(teal, 1));
        p.drawLine(cell.bottomLeft() + QPointF(1, -2),
                   cell.bottomLeft() +
                       QPointF(cell.width() * step.probability, -2));
      }
      if (step.ratchet > 1)
        text(p, cell, QString::number(step.ratchet), 10, ink);
      if (telemetry && unsigned(telemetry->step[slot]) == i) {
        p.setPen(QPen(ink, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(cell, 2, 2);
      }
      if (hasSelection && int(r) >= std::min(anchorRow, focusedRow) &&
          int(r) <= std::max(anchorRow, focusedRow) &&
          int(i) >= std::min(anchorStep, focusedStep) &&
          int(i) <= std::max(anchorStep, focusedStep)) {
        p.fillRect(cell, QColor(40, 151, 176, 40));
        p.setPen(QPen(teal, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(cell);
      }
      if (hasFocus() && int(r) == focusedRow && int(i) == focusedStep) {
        p.setPen(QPen(teal, 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(cell.adjusted(2, 2, -2, -2));
      }
    }
  }
  for (unsigned i = 0; i < max; ++i)
    if (max <= 32 || i % 4 == 0)
      text(p, {left + i * w / max, height() - 18., w / max, 16},
           QString::number(i + 1), 9, muted);
}
void SequenceEditor::edit(QPointF at, bool first) {
  auto rs = rows();
  if (rs.empty())
    return;
  double left = compact ? 10 : 110;
  unsigned max = 16;
  for (unsigned s : rs)
    max = std::max(max,
                   state->scenes[state->editScene].modules[s].sequence.length);
  int row = std::clamp(int((at.y() - 5) / ((height() - 22.) / rs.size())), 0,
                       int(rs.size()) - 1),
      step = int((at.x() - left) / ((width() - left - 10) / max));
  if (step < 0 ||
      step >=
          int(state->scenes[state->editScene].modules[rs[row]].sequence.length))
    return;
  focusedRow = row;
  focusedStep = step;
  auto &q = state->scenes[state->editScene].modules[rs[row]].sequence;
  if (first)
    paintOn = !q.steps[step].enabled;
  q.enabled = true;
  q.steps[step].enabled = paintOn;
  if (QApplication::keyboardModifiers() & Qt::ShiftModifier)
    q.steps[step].amount = float(std::clamp(
        1 - std::fmod(std::max(0., at.y() - 5), (height() - 22.) / rs.size()) /
                ((height() - 22.) / rs.size()),
        0., 1.));
  if (changed)
    changed();
  update();
}
QPoint SequenceEditor::cellAt(QPointF at) const {
  auto rs = rows();
  if (rs.empty())
    return {};
  unsigned max = 16;
  for (auto s : rs)
    max = std::max(max,
                   state->scenes[state->editScene].modules[s].sequence.length);
  double left = compact ? 10 : 110;
  return {std::clamp(int((at.x() - left) / ((width() - left - 10) / max)), 0,
                     int(max) - 1),
          std::clamp(int((at.y() - 5) / ((height() - 22.) / rs.size())), 0,
                     int(rs.size()) - 1)};
}
void SequenceEditor::mousePressEvent(QMouseEvent *e) {
  if (!state || e->button() != Qt::LeftButton)
    return;
  setFocus();
  auto at = cellAt(e->position());
  selecting = e->modifiers() & Qt::ControlModifier;
  hasSelection = selecting;
  anchorStep = focusedStep = at.x();
  anchorRow = focusedRow = at.y();
  if (selecting) {
    update();
    return;
  }
  if (begin)
    begin();
  dragging = true;
  edit(e->position(), true);
}
void SequenceEditor::mouseMoveEvent(QMouseEvent *e) {
  if (selecting) {
    auto at = cellAt(e->position());
    focusedStep = at.x();
    focusedRow = at.y();
    update();
  } else if (dragging)
    edit(e->position(), false);
}
void SequenceEditor::mouseReleaseEvent(QMouseEvent *) {
  if (dragging && finish)
    finish();
  dragging = selecting = false;
}
void SequenceEditor::copy(bool pattern) {
  auto rs = rows();
  if (rs.empty())
    return;
  focusedRow = std::clamp(focusedRow, 0, int(rs.size()) - 1);
  if (pattern) {
    patternClipboard.clear();
    for (auto s : rs)
      patternClipboard.push_back(
          state->scenes[state->editScene].modules[s].sequence);
    return;
  }
  stepClipboard.clear();
  int first = hasSelection ? std::min(anchorRow, focusedRow) : focusedRow,
      last = hasSelection ? std::max(anchorRow, focusedRow) : focusedRow;
  for (int r = first; r <= last && r < int(rs.size()); ++r) {
    auto &q = state->scenes[state->editScene].modules[rs[r]].sequence;
    int lo = hasSelection ? std::min(anchorStep, focusedStep) : 0,
        hi = hasSelection ? std::max(anchorStep, focusedStep)
                          : int(q.length) - 1;
    lo = std::clamp(lo, 0, 63);
    hi = std::clamp(hi, lo, 63);
    stepClipboard.emplace_back(q.steps.begin() + lo, q.steps.begin() + hi + 1);
  }
}
void SequenceEditor::paste(bool pattern) {
  auto rs = rows();
  if (rs.empty() ||
      (pattern ? patternClipboard.empty() : stepClipboard.empty()))
    return;
  if (begin)
    begin();
  if (pattern) {
    for (unsigned i = 0; i < rs.size() && i < patternClipboard.size(); ++i)
      state->scenes[state->editScene].modules[rs[i]].sequence =
          patternClipboard[i];
  } else {
    for (unsigned r = 0; r < stepClipboard.size() && focusedRow + r < rs.size();
         ++r) {
      auto &q =
          state->scenes[state->editScene].modules[rs[focusedRow + r]].sequence;
      for (unsigned i = 0; i < stepClipboard[r].size() && focusedStep + i < 64;
           ++i) {
        q.steps[focusedStep + i] = stepClipboard[r][i];
        q.length = std::max(q.length, unsigned(focusedStep + i + 1));
      }
      q.enabled = true;
    }
  }
  if (changed)
    changed();
  if (finish)
    finish();
  update();
}
void SequenceEditor::keyPressEvent(QKeyEvent *e) {
  auto rs = rows();
  if (rs.empty())
    return;
  focusedRow = std::clamp(focusedRow, 0, int(rs.size()) - 1);
  auto &q = state->scenes[state->editScene].modules[rs[focusedRow]].sequence;
  focusedStep = std::min(focusedStep, int(q.length) - 1);
  if (e->modifiers() & Qt::ControlModifier) {
    if (e->key() == Qt::Key_C) {
      copy(e->modifiers() & Qt::ShiftModifier);
      return;
    }
    if (e->key() == Qt::Key_V) {
      paste(e->modifiers() & Qt::ShiftModifier);
      return;
    }
  }
  if (e->matches(QKeySequence::SelectAll)) {
    anchorRow = anchorStep = 0;
    focusedRow = int(rs.size()) - 1;
    focusedStep = 63;
    hasSelection = true;
    update();
    return;
  }
  if (e->modifiers() & Qt::ShiftModifier) {
    if (!hasSelection) {
      anchorRow = focusedRow;
      anchorStep = focusedStep;
      hasSelection = true;
    }
  } else
    hasSelection = false;
  if (e->key() == Qt::Key_Left)
    focusedStep = std::max(0, focusedStep - 1);
  else if (e->key() == Qt::Key_Right)
    focusedStep = std::min(int(q.length) - 1, focusedStep + 1);
  else if (e->key() == Qt::Key_Up)
    focusedRow = std::max(0, focusedRow - 1);
  else if (e->key() == Qt::Key_Down)
    focusedRow = std::min(int(rs.size()) - 1, focusedRow + 1);
  else if (e->key() == Qt::Key_Space || e->key() == Qt::Key_Delete) {
    if (begin)
      begin();
    q.enabled = true;
    q.steps[focusedStep].enabled =
        e->key() == Qt::Key_Space ? !q.steps[focusedStep].enabled : false;
    if (changed)
      changed();
    if (finish)
      finish();
  } else
    QWidget::keyPressEvent(e);
  update();
}
void SequenceEditor::contextMenuEvent(QContextMenuEvent *e) {
  auto rs = rows();
  if (rs.empty() || !state)
    return;
  double h = (height() - 22.) / rs.size();
  focusedRow = std::clamp(int((e->pos().y() - 5) / h), 0, int(rs.size()) - 1);
  auto &q = state->scenes[state->editScene].modules[rs[focusedRow]].sequence;
  double left = compact ? 10 : 110;
  unsigned max = 16;
  for (unsigned s : rs)
    max = std::max(max,
                   state->scenes[state->editScene].modules[s].sequence.length);
  focusedStep =
      std::clamp(int((e->pos().x() - left) / ((width() - left - 10) / max)), 0,
                 int(q.length) - 1);
  auto &step = q.steps[focusedStep];
  QMenu m(this);
  auto numeric = [&](QString name, double current, double min, double max,
                     std::function<void(double)> set) {
    m.addAction(name, this, [&, name, current, min, max, set] {
      bool ok = false;
      double v =
          QInputDialog::getDouble(this, name, name, current, min, max, 2, &ok);
      if (ok) {
        if (begin)
          begin();
        set(v);
        if (changed)
          changed();
        if (finish)
          finish();
        update();
      }
    });
  };
  numeric(label("Strength", "Сила"), step.amount, 0, 1,
          [&](double v) { step.amount = float(v); });
  numeric(label("Probability", "Вероятность"), step.probability, 0, 1,
          [&](double v) { step.probability = float(v); });
  numeric("Ratchet", step.ratchet, 1, 8,
          [&](double v) { step.ratchet = unsigned(std::round(v)); });
  m.addAction(label("Toggle retrigger", "Перезапуск огибающей"), this, [&] {
    if (begin)
      begin();
    step.retrigger = !step.retrigger;
    if (changed)
      changed();
    if (finish)
      finish();
  });
  numeric(label("Row length", "Длина строки"), q.length, 1, 64,
          [&](double v) { q.length = unsigned(std::round(v)); });
  numeric("Swing", q.swing, 0, .75, [&](double v) { q.swing = v; });
  m.addSeparator();
  m.addAction(label("Copy range / row", "Копировать диапазон / строку"), this,
              [this] { copy(); });
  m.addAction(label("Paste", "Вставить"), this, [this] { paste(); });
  m.addAction(label("Copy whole pattern", "Копировать весь рисунок"), this,
              [this] { copy(true); });
  m.addAction(label("Paste whole pattern", "Вставить весь рисунок"), this,
              [this] { paste(true); });
  m.exec(e->globalPos());
}

Pad::Pad(QWidget *p) : QWidget(p) {
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName(label("XY motion pad", "Панель движения XY"));
  setObjectName("flux.xy");
}
void Pad::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  well(p, QRectF(rect()).adjusted(1, 1, -1, -1));
  auto a = QRectF(rect()).adjusted(11, 10, -11, -20);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor("#d5d9ce"));
  for (unsigned x = 0; x <= 8; ++x)
    for (unsigned y = 0; y <= 8; ++y)
      p.drawEllipse(QPointF(a.left() + x * a.width() / 8,
                            a.top() + y * a.height() / 8), .8, .8);
  if (state) {
    QPainterPath path;
    for (unsigned i = 0; i < state->path.size(); ++i) {
      auto &pt = state->path[i];
      QPointF v(a.left() + pt.x * a.width(), a.bottom() - pt.y * a.height());
      if (i)
        path.lineTo(v);
      else
        path.moveTo(v);
    }
    p.setPen(QPen(QColor(40, 151, 176, 95), 1.2));
    p.drawPath(path);
    double x = telemetry && state->pathEnabled ? telemetry->x
                                               : state->globals[X],
           y = telemetry && state->pathEnabled ? telemetry->y
                                               : state->globals[Y];
    QPointF c(a.left() + x * a.width(), a.bottom() - y * a.height());
    p.setPen(QPen(border, .8));
    p.drawLine(QPointF(a.left(), c.y()), QPointF(a.right(), c.y()));
    p.drawLine(QPointF(c.x(), a.top()), QPointF(c.x(), a.bottom()));
    p.setPen(QPen(accent, 1));
    p.setBrush(panelWhite);
    p.drawEllipse(c, 11, 11);
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawEllipse(c, 6, 6);
  }
  text(p, {0, height() - 18., double(width()), 16},
       label("X: Move · Y: Texture", "X: Движ. · Y: Текст."), 9, muted);
  if (hasFocus()) {
    p.setPen(QPen(accent, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(3, 3, -3, -3));
  }
}
void Pad::edit(QPointF at) {
  if (!state)
    return;
  auto a = QRectF(rect()).adjusted(11, 10, -11, -20);
  state->globals[X] = std::clamp((at.x() - a.left()) / a.width(), 0., 1.);
  state->globals[Y] = std::clamp(1 - (at.y() - a.top()) / a.height(), 0., 1.);
  if (changed)
    changed();
  update();
}
void Pad::mousePressEvent(QMouseEvent *e) {
  if (e->button() != Qt::LeftButton)
    return;
  setFocus();
  if (begin)
    begin();
  dragging = true;
  edit(e->position());
}
void Pad::mouseMoveEvent(QMouseEvent *e) {
  if (dragging)
    edit(e->position());
}
void Pad::mouseReleaseEvent(QMouseEvent *) {
  if (dragging && finish)
    finish();
  dragging = false;
}
void Pad::keyPressEvent(QKeyEvent *e) {
  if (!state)
    return;
  double dx = e->key() == Qt::Key_Left    ? -.01
              : e->key() == Qt::Key_Right ? .01
                                          : 0,
         dy = e->key() == Qt::Key_Down ? -.01
              : e->key() == Qt::Key_Up ? .01
                                       : 0;
  if (dx || dy) {
    if (begin)
      begin();
    state->globals[X] = std::clamp(state->globals[X] + dx, 0., 1.);
    state->globals[Y] = std::clamp(state->globals[Y] + dy, 0., 1.);
    if (changed)
      changed();
    if (finish)
      finish();
    update();
  } else
    QWidget::keyPressEvent(e);
}

RoutingEditor::RoutingEditor(QWidget *p) : QWidget(p) {
  setAcceptDrops(true);
  setFocusPolicy(Qt::StrongFocus);
  setObjectName("flux.routing");
  setAccessibleName(label("Effect routing", "Маршрутизация эффектов"));
}
QRectF RoutingEditor::card(unsigned slot) const {
  if (!state)
    return {};
  unsigned rank = 0;
  auto lane = state->modules[slot].lane;
  for (unsigned i : state->order) {
    if (i == slot)
      break;
    if (state->modules[i].kind != Kind::Empty && state->modules[i].lane == lane)
      ++rank;
  }
  return {108. + rank * 102 - horizontal,
          8. + unsigned(lane) * (height() - 16.) / 5, 96,
          (height() - 16.) / 5 - 9};
}
void RoutingEditor::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  well(p, QRectF(rect()).adjusted(1, 1, -1, -1));
  if (!state)
    return;
  QStringList labels{"PRE", state->routing == Routing::Multiband ? "LOW" : "A",
                     state->routing == Routing::Multiband ? "MID" : "B",
                     state->routing == Routing::Multiband ? "HIGH" : "C",
                     "POST"};
  for (unsigned i = 0; i < 5; ++i) {
    double y = 8 + i * (height() - 16.) / 5;
    QColor c = i == 1 ? teal : i == 2 ? gold : i == 3 ? purple : muted;
    text(p, {8, y, 90, (height() - 16.) / 5 - 8}, labels[i], 12, c);
    c.setAlpha(65);
    p.setPen(QPen(c, 1));
    p.drawLine(QPointF(95, y + 21), QPointF(width() - 15, y + 21));
  }
  p.save();
  p.setClipRect(QRectF(103, 3, width() - 106, height() - 6));
  for (unsigned s : state->order)
    if (state->modules[s].kind != Kind::Empty) {
      auto r = card(s);
      well(p, r, color(state->modules[s].kind));
      text(p, r.adjusted(4, 2, -4, -2),
           QString::fromUtf8(kindName(state->modules[s].kind)), 11,
           color(state->modules[s].kind));
      if (s == selected) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(accent, 1.3));
        p.drawRoundedRect(r, 6, 6);
      }
    }
  p.restore();
}
void RoutingEditor::mousePressEvent(QMouseEvent *e) {
  press = e->pos();
  dragged = -1;
  if (!state)
    return;
  for (unsigned i : state->order)
    if (state->modules[i].kind != Kind::Empty &&
        card(i).contains(e->position())) {
      dragged = int(i);
      selected = i;
      if (select)
        select(i);
      break;
    }
  update();
}
void RoutingEditor::mouseMoveEvent(QMouseEvent *e) {
  if (dragged < 0 || !(e->buttons() & Qt::LeftButton) ||
      (e->pos() - press).manhattanLength() < 8)
    return;
  auto *d = new QDrag(this);
  auto *mime = new QMimeData;
  mime->setData(slotMime, QByteArray::number(dragged));
  d->setMimeData(mime);
  d->exec(Qt::MoveAction);
  dragged = -1;
}
void RoutingEditor::dragEnterEvent(QDragEnterEvent *e) {
  if (e->mimeData()->hasFormat(slotMime))
    e->acceptProposedAction();
}
void RoutingEditor::dropEvent(QDropEvent *e) {
  if (!state || !e->mimeData()->hasFormat(slotMime))
    return;
  unsigned slot = e->mimeData()->data(slotMime).toUInt();
  if (slot >= slotCount)
    return;
  unsigned lane = unsigned(
      std::clamp(int((e->position().y() - 8) / ((height() - 16.) / 5)), 0, 4));
  if (state->routing == Routing::Parallel && lane == 3)
    lane = 2;
  if (state->routing == Routing::Serial && (lane == 2 || lane == 3))
    lane = 1;
  if (begin)
    begin();
  state->modules[slot].lane = Lane(lane);
  unsigned pos = unsigned(
      std::clamp(int((e->position().x() - 108 + horizontal) / 102), 0, 11));
  std::vector<unsigned> order;
  for (unsigned s : state->order)
    if (s != slot)
      order.push_back(s);
  unsigned found = 0, where = unsigned(order.size());
  for (unsigned i = 0; i < order.size(); ++i)
    if (state->modules[order[i]].lane == Lane(lane) && found++ >= pos) {
      where = i;
      break;
    }
  order.insert(order.begin() + where, slot);
  std::copy(order.begin(), order.end(), state->order.begin());
  if (changed)
    changed();
  if (finish)
    finish();
  update();
  e->acceptProposedAction();
}
void RoutingEditor::wheelEvent(QWheelEvent *e) {
  if (!state)
    return;
  std::array<unsigned, 5> counts{};
  for (auto &m : state->modules)
    if (m.kind != Kind::Empty)
      ++counts[unsigned(m.lane)];
  double maximum =
      std::max(0., *std::max_element(counts.begin(), counts.end()) * 102. -
                       (width() - 120));
  horizontal =
      std::clamp(horizontal - (e->angleDelta().x() ? e->angleDelta().x()
                                                   : e->angleDelta().y()) *
                                  .5,
                 0., maximum);
  update();
  e->accept();
}
void RoutingEditor::keyPressEvent(QKeyEvent *e) {
  if (!state)
    return;
  std::vector<unsigned> order;
  for (auto i : state->order)
    if (state->modules[i].kind != Kind::Empty)
      order.push_back(i);
  if (order.empty())
    return;
  auto found = std::find(order.begin(), order.end(), selected);
  int index = found == order.end() ? 0 : int(found - order.begin());
  if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) {
    index = std::clamp(index + (e->key() == Qt::Key_Left ? -1 : 1), 0,
                       int(order.size()) - 1);
    selected = order[index];
    auto r = card(selected);
    if (r.left() < 108)
      horizontal = std::max(0., horizontal + r.left() - 108);
    else if (r.right() > width() - 10)
      horizontal += r.right() - width() + 10;
    if (select)
      select(selected);
    update();
    return;
  }
  if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
    std::vector<Lane> lanes{Lane::Pre, Lane::A};
    if (state->routing != Routing::Serial)
      lanes.push_back(Lane::B);
    if (state->routing == Routing::Multiband)
      lanes.push_back(Lane::C);
    lanes.push_back(Lane::Post);
    auto at =
        std::find(lanes.begin(), lanes.end(), state->modules[selected].lane);
    int n = at == lanes.end() ? 1 : int(at - lanes.begin());
    n = std::clamp(n + (e->key() == Qt::Key_Up ? -1 : 1), 0,
                   int(lanes.size()) - 1);
    if (begin)
      begin();
    state->modules[selected].lane = lanes[n];
    if (changed)
      changed();
    if (finish)
      finish();
    update();
    return;
  }
  QWidget::keyPressEvent(e);
}
BandEditor::BandEditor(QWidget *p) : QWidget(p) {
  setFocusPolicy(Qt::StrongFocus);
  setObjectName("flux.bands");
  setAccessibleName(label("Crossover frequencies", "Частоты разделения полос"));
  setToolTip(label("Drag the crossover handles. Left/right: adjust. Up/down: "
                   "select band. Right-click: value or automation.",
                   "Перетаскивайте точки разделения. Влево/вправо: частота. "
                   "Вверх/вниз: выбор. ПКМ: значение и автоматизация."));
}
QRectF BandEditor::area() const {
  return QRectF(rect()).adjusted(25, 8, -9, -21);
}
double BandEditor::position(double f) const {
  auto a = area();
  return a.left() + std::log10(std::clamp(f, 20., 20000.) / 20) / 3 * a.width();
}
void BandEditor::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  if (!state)
    return;
  if (!isEnabled())
    p.setOpacity(.4);
  auto a = area();
  p.fillRect(a, panelWhite);
  for (double f :
       {20., 50., 100., 200., 500., 1000., 2000., 5000., 10000., 20000.}) {
    p.setPen(QPen(border, .7));
    p.drawLine(QPointF(position(f), a.top()), QPointF(position(f), a.bottom()));
  }
  for (unsigned i = 0; i < 4; ++i) {
    double y = a.top() + a.height() * i / 3;
    p.setPen(QPen(border, .7));
    p.drawLine(QPointF(a.left(), y), QPointF(a.right(), y));
    text(p, {0, y - 7, 22, 14}, QString::number(-int(i) * 12), 9, muted,
         Qt::AlignRight | Qt::AlignVCenter);
  }
  for (unsigned band = 0; band < 3; ++band) {
    QPainterPath path;
    QColor c = band == 0 ? teal : band == 1 ? gold : purple;
    for (int x = 0; x <= int(a.width()); ++x) {
      double f = 20 * std::pow(1000., x / a.width());
      double lo = 1 / (1 + std::pow(f / state->globals[LowSplit], 4)),
             hi = 1 / (1 + std::pow(f / state->globals[HighSplit], 4));
      double gain = band == 0 ? lo : band == 1 ? (1 - lo) * hi : 1 - hi;
      double db = 20 * std::log10(std::max(1e-6, gain)) +
                  state->globals[LowGain + band];
      QPointF pt(a.left() + x,
                 a.top() + std::clamp(-db / 36, 0., 1.) * a.height());
      if (x)
        path.lineTo(pt);
      else
        path.moveTo(pt);
    }
    auto fill = path;
    fill.lineTo(a.bottomRight());
    fill.lineTo(a.bottomLeft());
    fill.closeSubpath();
    QLinearGradient g(a.topLeft(), a.bottomLeft());
    auto top = c;
    top.setAlpha(state->globals[LowMute + band] > .5 ? 12 : 50);
    g.setColorAt(0, top);
    g.setColorAt(1, Qt::transparent);
    p.fillPath(fill, g);
    p.setPen(QPen(c, 1.5));
    p.drawPath(path);
  }
  for (unsigned i = 0; i < 2; ++i) {
    double x = position(state->globals[LowSplit + i]);
    p.setPen(QPen(i == 0 ? teal : purple, 1, Qt::DashLine));
    p.drawLine(QPointF(x, a.top()), QPointF(x, a.bottom()));
    p.setBrush(i == 0 ? teal : purple);
    p.setPen(QPen(i == selected && hasFocus() ? ink : QColor("#FFFFFF"), 1.5));
    p.drawEllipse(QPointF(x, a.top() + a.height() / 6), 5, 5);
  }
  for (double f : {20., 100., 1000., 10000., 20000.})
    text(p, {position(f) - 19, a.bottom() + 4, 38, 15},
         f >= 1000 ? QString::number(f / 1000) + "k" : QString::number(f), 10,
         muted);
  if (hasFocus()) {
    p.setPen(QPen(accent, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(a);
  }
}
void BandEditor::move(double f) {
  auto &v = state->globals;
  v[LowSplit + selected] =
      std::clamp(f, selected ? std::max(200., v[LowSplit] + 10) : 30.,
                 selected ? 16000. : std::min(4000., v[HighSplit] - 10));
  if (changed)
    changed();
  update();
}
void BandEditor::mousePressEvent(QMouseEvent *e) {
  if (!state || e->button() != Qt::LeftButton)
    return;
  setFocus();
  selected =
      std::abs(e->position().x() - position(state->globals[LowSplit])) <
              std::abs(e->position().x() - position(state->globals[HighSplit]))
          ? 0
          : 1;
  dragging = true;
  if (begin)
    begin();
  mouseMoveEvent(e);
}
void BandEditor::mouseMoveEvent(QMouseEvent *e) {
  if (dragging)
    move(20 *
         std::pow(1000., (e->position().x() - area().left()) / area().width()));
}
void BandEditor::mouseReleaseEvent(QMouseEvent *) {
  if (dragging && finish)
    finish();
  dragging = false;
}
void BandEditor::keyPressEvent(QKeyEvent *e) {
  if (!state)
    return;
  if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
    selected = 1 - selected;
    update();
    return;
  }
  if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) {
    if (begin)
      begin();
    move(state->globals[LowSplit + selected] *
         std::pow(e->modifiers() & Qt::ShiftModifier ? 1.002 : 1.02,
                  e->key() == Qt::Key_Left ? -1 : 1));
    if (finish)
      finish();
    return;
  }
  QWidget::keyPressEvent(e);
}
void BandEditor::contextMenuEvent(QContextMenuEvent *e) {
  QMenu menu(this);
  menu.addAction(
      label("Enter crossover frequency…", "Частота разделения…"), this, [this] {
        bool ok = false;
        auto v = QInputDialog::getDouble(this, "Flux", "Hz",
                                         state->globals[LowSplit + selected],
                                         40, 16000, 1, &ok);
        if (ok) {
          if (begin)
            begin();
          move(v);
          if (finish)
            finish();
        }
      });
  menu.addAction(label("Create automation", "Создать автоматизацию"), this,
                 [this] {
                   if (automate)
                     automate(LowSplit + selected);
                 });
  menu.exec(e->globalPos());
}
} // namespace fluxui
