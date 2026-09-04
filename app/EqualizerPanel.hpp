#pragma once

#include "Internal/EqualizerInstance.hpp"

#include <QColor>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <array>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class QShowEvent;
class QHideEvent;

namespace ui { class Knob; }
namespace daw { class EngineController; }

class EqualizerGraph final : public QWidget {
    Q_OBJECT
public:
    /// Samples per band curve. Each enabled band draws its own filled shape, so
    /// this is walked once per band per repaint — dense enough that a 96 dB/oct
    /// cut has no visible corners, cheap enough at twenty-four of them.
    static constexpr int kCurvePoints = 240;
    using Curve = std::array<float, kCurvePoints>;
    using CurveSet = std::array<Curve, daw::plugins::equalizer::kBandCount>;

    explicit EqualizerGraph(QWidget* parent = nullptr);

    void setData(const std::array<daw::plugins::equalizer::BandState,
                                  daw::plugins::equalizer::kBandCount>& bands,
                 const std::array<double, 256>& response,
                 const daw::plugins::equalizer::Telemetry& telemetry,
                 bool linearPhase, double displayRange,
                 const CurveSet& bandCurves);

    /// The band inspector floats over the curve instead of sitting under it, so
    /// the graph owns its placement.
    void setOverlay(QWidget* overlay);

    /// The colour a band's curve, fill and handle are drawn in. The inspector
    /// tints itself to match whatever is selected.
    static QColor colorForBand(int band, bool dark);
    void setSelection(std::vector<int> bands);
    const std::vector<int>& selection() const noexcept { return m_selection; }

    std::function<void(const std::vector<int>&)> selectionChanged;
    std::function<void()> gestureStarted;
    std::function<void(const std::vector<int>&, double frequencyRatio,
                       double gainDelta, double dynamicDelta)> bandsMoved;
    std::function<void(const std::vector<int>&, double factor)> qChanged;
    std::function<void()> gestureFinished;
    std::function<void(double frequency, double gain)> bandCreated;
    std::function<void(const std::vector<int>&)> bandsDeleted;
    std::function<void(int)> bandDuplicated;
    std::function<void(const std::vector<int>&)> gainsInverted;
    std::function<void(const std::vector<int>&)> bandsReset;
    std::function<void(int)> auditionChanged;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void placeOverlay();
    QRectF plotRect() const;
    QPointF bandPoint(int band) const;
    int bandAt(const QPointF& point) const;
    double frequencyAt(double x) const;
    double gainAt(double y) const;
    double xForFrequency(double frequency) const;
    double yForGain(double gain) const;
    void choose(int band, Qt::KeyboardModifiers modifiers);
    void nudge(double xOctaves, double gainDb);

    std::array<daw::plugins::equalizer::BandState,
               daw::plugins::equalizer::kBandCount> m_bands{};
    std::array<double, 256> m_response{};
    CurveSet m_bandCurves{};
    QPointer<QWidget> m_overlay;
    daw::plugins::equalizer::Telemetry m_telemetry;
    std::vector<int> m_selection;
    QPointF m_pressPosition;
    bool m_dragging = false;
    bool m_dynamicDrag = false;
    bool m_linearPhase = false;
    double m_displayRange = 12.0;
};

class EqualizerPanel final : public QWidget {
    Q_OBJECT
public:
    EqualizerPanel(daw::EngineController* controller, QString channelId,
                   QString insertId, QWidget* parent = nullptr);

    bool checkForTest();

signals:
    void projectEdited();
    void automationRequested(const QString& parameterId);

protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    using Values = std::array<double, daw::plugins::equalizer::kParameterCount>;
    struct UserPreset { QString name; Values values{}; };

    daw::plugins::equalizer::EqualizerInstance* equalizerInstance() const;
    double readParameter(const QString& id) const;
    void writeParameter(const QString& id, double value);
    ui::Knob* makeKnob(const QString& id, const QString& caption, int size = 52);
    void beginGesture(const std::vector<QString>& ids);
    void finishGesture(const char* label);
    void selectBands(const std::vector<int>& bands);
    int selectedBand() const;
    QString bandId(int band, daw::plugins::equalizer::BandParam field) const;
    void createBand(double frequency, double gain);
    void deleteBands(const std::vector<int>& bands);
    void duplicateBand(int band);
    void resetBands(const std::vector<int>& bands);
    void invertGains(const std::vector<int>& bands);
    void moveBands(const std::vector<int>& bands, double frequencyRatio,
                   double gainDelta, double dynamicDelta);
    void changeQ(const std::vector<int>& bands, double factor);
    void refresh();
    void refreshBandControls();
    Values currentValues() const;
    void applyValues(const Values& values, const QString& kind,
                     const QString& name, const char* label);
    void applyFactoryPreset(int index);
    void applyUserPreset(const QString& name);
    void showPresetMenu();
    void reloadUserPresets();
    void storeUserPresets() const;
    void saveUserPreset();
    void renameUserPreset();
    void deleteUserPreset();
    void switchComparison(char slot);
    void copyComparison();
    void updateAnalyzerConfig();

    daw::EngineController* m_controller = nullptr;
    QString m_channelId;
    QString m_insertId;
    std::string m_channelKey;
    std::string m_insertKey;
    EqualizerGraph* m_graph = nullptr;
    QPushButton* m_preset = nullptr;
    QPushButton* m_a = nullptr;
    QPushButton* m_b = nullptr;
    QComboBox* m_mode = nullptr;
    QComboBox* m_resolution = nullptr;
    QComboBox* m_type = nullptr;
    QComboBox* m_slope = nullptr;
    QComboBox* m_placement = nullptr;
    QComboBox* m_detectorMode = nullptr;
    QComboBox* m_displayRange = nullptr;
    QCheckBox* m_dynamic = nullptr;
    QCheckBox* m_dynamicAuto = nullptr;
    QCheckBox* m_external = nullptr;
    QCheckBox* m_autoGain = nullptr;
    QCheckBox* m_polarity = nullptr;
    QCheckBox* m_pre = nullptr;
    QCheckBox* m_post = nullptr;
    QCheckBox* m_side = nullptr;
    QCheckBox* m_freeze = nullptr;
    QLabel* m_sidechainStatus = nullptr;
    QWidget* m_bandPanel = nullptr;
    QWidget* m_dynamicPanel = nullptr;
    QHash<QString, ui::Knob*> m_knobs;
    QHash<QString, double> m_gestureStart;
    std::array<daw::plugins::equalizer::BandState,
               daw::plugins::equalizer::kBandCount> m_graphGestureStart{};
    QTimer* m_timer = nullptr;
    std::vector<UserPreset> m_userPresets;
    QString m_selectedKind{QStringLiteral("factory")};
    QString m_selectedName{QStringLiteral("Flat")};
    bool m_refreshing = false;
};
