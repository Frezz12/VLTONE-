#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QVariantMap>
#include <QWidget>
#include <functional>

class QAbstractButton;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QBoxLayout;
class QDialog;
namespace ui {
class Knob;
}

// Native faces for the built-in effects. Parameter IDs, gestures and presets
// remain owned by their panels; this view only presents and edits their state.
class NativePluginView final : public QWidget {
  Q_OBJECT
public:
  enum class Kind { Delay, Compressor, Pitch, Modulation };
  explicit NativePluginView(Kind, QWidget *parent = nullptr);
  void setSnapshot(const QVariantMap &);
  std::function<QString(const QString &, bool)> savePreset;
  std::function<QString(const QString &, const QString &)> renamePreset;
signals:
  void edit(int index, double value, bool finished);
  void finish(int index);
  void automate(int index);
  void toggleNote(int note);
  void toggleBypass();
  void factoryPreset(int index);
  void loadPreset(const QString &name);
  void deletePreset(const QString &name);
  void sendToAll();
  void reorder(int from, int to);
  void eqExpanded(bool expanded);

protected:
  void paintEvent(QPaintEvent *) override;

private:
  QWidget *knob(int index, int diameter, const QColor &face = {});
  QComboBox *choice(int index, const QStringList &labels);
  QAbstractButton *toggle(int index, const QString &label);
  QWidget *segments(int index, const QStringList &labels);
  QDoubleSpinBox *number(int index);
  void context(QWidget *, int index);
  void buildDelay();
  void buildCompressor();
  void buildPitch();
  void buildModulation();
  void buildEq(QBoxLayout *);
  void presetHeader(QBoxLayout *, const QString &title);
  void presetDialog();
  void syncEq();
  double value(int index) const;
  void change(int index, double value, bool finished = true);
  Kind m_kind;
  QVariantMap m_state;
  QVariantList m_parameters;
  QHash<int, ui::Knob *> m_knobs;
  QHash<int, QDoubleSpinBox *> m_numbers;
  QHash<int, QComboBox *> m_choices;
  QHash<int, QAbstractButton *> m_toggles;
  QHash<int, QList<QAbstractButton *>> m_segments;
  QList<QAbstractButton *> m_notes;
  QList<QWidget *> m_cards;
  QList<QWidget *> m_displays;
  QBoxLayout *m_cardLayout = nullptr;
  QLabel *m_status = nullptr;
  QLabel *m_timing = nullptr;
  QLabel *m_divisionReadout = nullptr;
  QComboBox *m_preset = nullptr;
  QAbstractButton *m_active = nullptr;
  QAbstractButton *m_tap = nullptr;
  QComboBox *m_eqBand = nullptr;
  QDoubleSpinBox *m_eqFrequency = nullptr, *m_eqGain = nullptr,
                 *m_eqQ = nullptr;
  QAbstractButton *m_eqOn = nullptr;
  QDialog *m_settings = nullptr;
  QElapsedTimer m_tapClock;
  QList<qint64> m_taps;
  bool m_built = false;
};
