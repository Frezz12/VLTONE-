#include "StripSilenceDialog.hpp"
#include <stdexcept>
#include "StripSilencePreferences.hpp"
#include "Controls.hpp"
#include "Theme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

class SilencePreview final : public QWidget {
public:
    explicit SilencePreview(QWidget* parent) : QWidget(parent) {
        setObjectName("StripSilencePreview");
        setMinimumHeight(172);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAccessibleName(StripSilenceDialog::tr("Silence boundaries preview"));
        connect(&ThemeManager::instance(), &ThemeManager::changed, this, qOverload<>(&QWidget::update));
    }
    void setMessage(QString text) { m_message = std::move(text); update(); }
    void setData(const daw::SilenceEnvelope* envelope, std::vector<daw::SilenceRegion> regions, double threshold) {
        if (m_envelope != envelope) {
            m_envelope = envelope;
            m_wave.assign(std::min<std::size_t>(2048, envelope->peaks.size()), 0);
            for (std::size_t i = 0; i < envelope->peaks.size(); ++i) {
                const auto bin = std::min(m_wave.size() - 1, i * m_wave.size() / envelope->peaks.size());
                m_wave[bin] = std::max(m_wave[bin], envelope->peaks[i]);
            }
        }
        m_regions = std::move(regions); m_threshold = threshold;
        m_message.clear(); update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        const auto& theme = ThemeManager::instance().theme();
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const QRectF panel = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
        p.setBrush(theme.well()); p.setPen(theme.separator());
        p.drawRoundedRect(panel, Theme::cornerRadius, Theme::cornerRadius);
        if (!m_message.isEmpty() || !m_envelope || m_wave.empty()) {
            p.setPen(theme.textSecondary);
            p.drawText(rect().adjusted(24, 12, -24, -12), Qt::AlignCenter | Qt::TextWordWrap, m_message);
            return;
        }
        const QRectF plot = panel.adjusted(42, 22, -14, -27);
        const double mid = plot.center().y(), half = plot.height() / 2;
        const double duration = m_envelope->durationSeconds;
        const auto x = [&](double seconds) { return plot.left() + seconds / duration * plot.width(); };
        const auto height = [](double db) { return std::clamp((db + 96) / 96, 0.0, 1.0); };
        QFont small = font(); small.setPixelSize(10); p.setFont(small);
        p.setPen(theme.textSecondary);
        p.drawText(QRectF(8, 3, 90, 16), Qt::AlignLeft, QStringLiteral("dBFS"));
        for (int db : {0, -24, -48, -72}) {
            const double y = mid - height(db) * half;
            p.setPen(theme.separator()); p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            p.setPen(theme.textSecondary); p.drawText(QRectF(2, y - 7, 33, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(db));
        }
        for (int i = 0; i <= 4; ++i) {
            const auto at = plot.left() + plot.width() * i / 4;
            p.setPen(theme.separator()); p.drawLine(QPointF(at, plot.top()), QPointF(at, plot.bottom()));
            p.setPen(theme.textSecondary);
            const double labelX = std::clamp(at - 30, panel.left() + 7, panel.right() - 67);
            p.drawText(QRectF(labelX, plot.bottom() + 6, 60, 16), Qt::AlignCenter,
                QString::number(duration * i / 4, 'f', duration < 10 ? 2 : 1) + QStringLiteral(" s"));
        }
        QColor tint = theme.accent; tint.setAlpha(theme.dark ? 26 : 20);
        int number = 0;
        for (const auto& region : m_regions) {
            const QRectF area(x(region.begin), plot.top(), x(region.end) - x(region.begin), plot.height());
            p.fillRect(area, tint);
            p.setPen(QPen(theme.accent, 1));
            p.drawLine(area.topLeft(), area.bottomLeft()); p.drawLine(area.topRight(), area.bottomRight());
            if (area.width() > 25) p.drawText(area.adjusted(5, 2, -3, -2), Qt::AlignLeft | Qt::AlignTop, QString::number(++number));
            else ++number;
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        std::size_t region = 0;
        for (int column = 0; column < int(plot.width()); ++column) {
            const double time = (column + .5) * duration / plot.width();
            while (region < m_regions.size() && m_regions[region].end < time) ++region;
            const bool kept = region < m_regions.size() && m_regions[region].begin <= time;
            const auto first = std::min(m_wave.size() - 1, std::size_t(column * m_wave.size() / plot.width()));
            const auto last = std::min(m_wave.size(), std::size_t((column + 1) * m_wave.size() / plot.width()) + 1);
            const float peak = *std::max_element(m_wave.begin() + first, m_wave.begin() + last);
            const double amplitude = peak > 0 ? height(20 * std::log10(peak)) * half : 0;
            QColor ink = kept ? theme.accent : theme.textSecondary;
            if (!kept) ink.setAlpha(85);
            p.setPen(ink);
            p.drawLine(QPointF(plot.left() + column, mid - amplitude), QPointF(plot.left() + column, mid + amplitude));
        }
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(theme.textPrimary, 1, Qt::DashLine));
        const double thresholdY = mid - height(m_threshold) * half;
        p.drawLine(QPointF(plot.left(), thresholdY), QPointF(plot.right(), thresholdY));
        p.drawLine(QPointF(plot.left(), mid + height(m_threshold) * half), QPointF(plot.right(), mid + height(m_threshold) * half));
        const QString label = StripSilenceDialog::tr("Threshold %1 dB").arg(m_threshold, 0, 'f', 1);
        const QRectF badge(plot.right() - 144, plot.top() - 19, 144, 16);
        p.fillRect(badge, theme.well()); p.drawText(badge, Qt::AlignRight | Qt::AlignVCenter, label);
    }
private:
    const daw::SilenceEnvelope* m_envelope = nullptr;
    std::vector<float> m_wave;
    std::vector<daw::SilenceRegion> m_regions;
    double m_threshold = -42;
    QString m_message;
};

StripSilenceDialog::StripSilenceDialog(daw::EngineController& controller,
    std::vector<daw::EngineController::ClipAddress> clips, QWidget* parent)
    : QDialog(parent), m_controller(controller), m_settings(controller.recordingPrefs().stripSilence),
      m_settingsOnly(clips.empty()) {
    setObjectName("StripSilenceDialog");
    setWindowTitle(m_settingsOnly ? tr("Auto Silence settings") : tr("Strip Silence"));
    setModal(true); resize(760, 660); setMinimumSize(640, 600);
    auto* root = new QVBoxLayout(this); root->setContentsMargins(20, 16, 20, 16); root->setSpacing(12);
    auto* hint = new QLabel(m_settingsOnly
        ? tr("Clean up recorded audio automatically with these settings.")
        : tr("Remove silence and keep every sound in its original position."), this);
    hint->setWordWrap(true); root->addWidget(hint);
    m_clip = new QComboBox(this); m_clip->setObjectName("StripSilenceClip");
    m_clip->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_clip->setMinimumContentsLength(18);
    m_clip->setAccessibleName(tr("Preview clip")); root->addWidget(m_clip);
    m_preview = new SilencePreview(this); root->addWidget(m_preview, 1);
    m_progress = new QProgressBar(this); m_progress->setRange(0, 0); m_progress->setTextVisible(false);
    m_progress->setFixedHeight(3); root->addWidget(m_progress);

    auto* fields = new QGridLayout; fields->setHorizontalSpacing(24); fields->setVerticalSpacing(10);
    std::vector<std::pair<QDoubleSpinBox*, double daw::StripSilenceSettings::*>> values;
    const auto control = [&](int row, int column, const char* name, const QString& label, const QString& tip,
                             double daw::StripSilenceSettings::* member, double low, double high,
                             double step, const QString& unit) {
        auto* host = new QWidget(this); auto* layout = new QVBoxLayout(host);
        layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(1);
        auto* line = new QHBoxLayout; line->setContentsMargins(0, 0, 0, 0);
        auto* caption = new QLabel(label, host); caption->setToolTip(tip);
        auto* value = new QDoubleSpinBox(host); value->setObjectName(QString::fromLatin1(name));
        value->setRange(low, high); value->setDecimals(step < 1 ? 1 : 0); value->setSingleStep(step);
        value->setButtonSymbols(QAbstractSpinBox::NoButtons);
        value->setAlignment(Qt::AlignRight);
        value->setSuffix(unit); value->setKeyboardTracking(false); value->setMinimumWidth(92);
        value->setMaximumWidth(116); value->setValue(m_settings.*member); value->setToolTip(tip);
        value->setAccessibleName(label); caption->setBuddy(value);
        line->addWidget(caption, 1); line->addWidget(value); layout->addLayout(line);
        auto* slider = new ui::GlassSlider(Qt::Horizontal, host);
        slider->setObjectName(QString::fromLatin1(name) + "Slider"); slider->setAccessibleName(label);
        slider->setRange(int(std::lround(low / step)), int(std::lround(high / step)));
        slider->setValue(int(std::lround(value->value() / step))); slider->setToolTip(tip);
        slider->setMinimumHeight(24); layout->addWidget(slider);
        connect(slider, &QSlider::valueChanged, value, [value, step](int v) { value->setValue(v * step); });
        connect(value, &QDoubleSpinBox::valueChanged, this, [this, slider, member, step](double v) {
            const QSignalBlocker block(slider); slider->setValue(int(std::lround(v / step)));
            m_settings.*member = v; updatePreview();
        });
        fields->addWidget(host, row, column); values.push_back({value, member});
    };
    control(0, 0, "SilenceThreshold", tr("Threshold"), tr("Sound must reach this level to open a region. Analysis is before Clip FX."),
        &daw::StripSilenceSettings::thresholdDb, -96, 0, .5, tr(" dB"));
    control(0, 1, "SilencePreRoll", tr("Before sound"), tr("Keep this much audio before each detected onset."),
        &daw::StripSilenceSettings::preRollMs, 0, 2000, 1, tr(" ms"));
    control(1, 0, "SilenceHysteresis", tr("Hysteresis"), tr("Once a region opens, retain its quieter tail this many dB below the threshold."),
        &daw::StripSilenceSettings::hysteresisDb, 0, 24, .5, tr(" dB"));
    control(1, 1, "SilencePostRoll", tr("After sound"), tr("Keep this much audio after each detected tail."),
        &daw::StripSilenceSettings::postRollMs, 0, 3000, 1, tr(" ms"));
    control(2, 0, "SilenceMinimumGap", tr("Minimum silence"), tr("Shorter pauses stay inside the same region."),
        &daw::StripSilenceSettings::minimumSilenceMs, 1, 5000, 1, tr(" ms"));
    control(2, 1, "SilenceFade", tr("Edge fades"), tr("Add short volume fades at new cuts to prevent clicks."),
        &daw::StripSilenceSettings::fadeMs, 0, 100, .5, tr(" ms"));
    control(3, 0, "SilenceMinimumSound", tr("Minimum sound"), tr("Discard shorter fragments, such as isolated clicks or noise."),
        &daw::StripSilenceSettings::minimumSoundMs, 0, 2000, 1, tr(" ms"));
    auto* gridHost = new QWidget(this); auto* gridLayout = new QVBoxLayout(gridHost);
    gridLayout->setContentsMargins(0, 0, 0, 0); gridLayout->setSpacing(3);
    auto* gridLabel = new QLabel(tr("Align edges to grid"), gridHost);
    m_grid = new QComboBox(gridHost); m_grid->setObjectName("SilenceGrid"); m_grid->setAccessibleName(gridLabel->text());
    m_grid->addItem(tr("Off"), 0.0);
    for (int denominator : {1, 2, 4, 8, 16, 32, 64}) m_grid->addItem(QStringLiteral("1/%1").arg(denominator), 4.0 / denominator);
    int gridIndex = m_grid->findData(m_settings.gridBeats);
    if (gridIndex < 0) { m_grid->addItem(tr("%1 beats").arg(m_settings.gridBeats), m_settings.gridBeats); gridIndex = m_grid->count() - 1; }
    m_grid->setCurrentIndex(gridIndex); gridLabel->setBuddy(m_grid);
    m_grid->setToolTip(tr("Expand edges outward to the project grid. Onsets and tails are never cut short."));
    gridLayout->addWidget(gridLabel); gridLayout->addWidget(m_grid); fields->addWidget(gridHost, 3, 1);
    connect(m_grid, &QComboBox::currentIndexChanged, this, [this] { m_settings.gridBeats = m_grid->currentData().toDouble(); updatePreview(); });
    fields->setColumnStretch(0, 1); fields->setColumnStretch(1, 1); root->addLayout(fields);

    auto* options = new QHBoxLayout;
    m_internal = new QCheckBox(tr("Split internal silence"), this); m_internal->setObjectName("SilenceSplitInternal");
    m_internal->setChecked(m_settings.splitInternal);
    m_internal->setToolTip(tr("Off: trim only the beginning and end of each clip."));
    connect(m_internal, &QCheckBox::toggled, this, [this](bool on) { m_settings.splitInternal = on; updatePreview(); });
    m_auto = new QCheckBox(tr("Auto Silence after recording"), this); m_auto->setObjectName("AutoSilenceEnabled");
    m_auto->setChecked(controller.recordingPrefs().autoSilence);
    m_auto->setEnabled(!controller.isRecording() && !controller.isCountingIn());
    m_auto->setToolTip(tr("Apply these settings to recorded audio when recording stops. Recording and cleanup share one Undo."));
    connect(m_auto, &QCheckBox::toggled, this, [this](bool on) { rememberSettings(); ui::silence::setAutomatic(m_controller, on); });
    options->addWidget(m_internal); options->addStretch(); options->addWidget(m_auto); root->addLayout(options);
    m_status = new QLabel(this); m_status->setObjectName("StripSilenceStatus"); m_status->setWordWrap(true);
    m_status->setMinimumHeight(32); m_status->setAccessibleName(tr("Silence analysis result")); root->addWidget(m_status);
    auto* buttons = new QDialogButtonBox(this);
    auto* reset = buttons->addButton(tr("Reset settings"), QDialogButtonBox::ResetRole);
    auto* cancel = buttons->addButton(tr("Close"), QDialogButtonBox::RejectRole);
    m_apply = buttons->addButton(m_settingsOnly ? tr("Done") : tr("Apply"), QDialogButtonBox::AcceptRole);
    m_apply->setObjectName("StripSilenceApply"); m_apply->setDefault(true);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_apply, &QPushButton::clicked, this, &StripSilenceDialog::apply);
    connect(reset, &QPushButton::clicked, this, [this, values] {
        const daw::StripSilenceSettings defaults;
        for (const auto& [value, member] : values) value->setValue(defaults.*member);
        m_grid->setCurrentIndex(m_grid->findData(defaults.gridBeats)); m_internal->setChecked(defaults.splitInternal);
    });
    root->addWidget(buttons);
    connect(m_clip, &QComboBox::currentIndexChanged, this, [this] { updatePreview(); });
    if (m_settingsOnly) {
        m_ready = true; m_clip->hide(); m_progress->hide();
        m_preview->setMessage(tr("Select an audio clip to preview silence boundaries.\nThese settings are shared with Auto Silence."));
        updatePreview(); return;
    }
    auto prepared = controller.prepareStripSilence(clips, m_sources);
    if (!prepared) {
        m_clip->hide(); m_progress->hide(); m_apply->setEnabled(false);
        m_preview->setMessage(tr("Audio could not be analyzed."));
        m_status->setText(QString::fromStdString(prepared.message())); return;
    }
    for (const auto& source : m_sources) m_clip->addItem(QString::fromStdString(source.original.name));
    m_clip->setVisible(m_sources.size() > 1);
    if (m_sources.size() > 1) {
        hint->setText(tr("The same settings will be applied to %1 clips. Choose a clip to preview its boundaries.").arg(m_sources.size()));
        m_apply->setText(tr("Apply to %1 clips").arg(m_sources.size()));
    }
    m_preview->setMessage(tr("Analyzing audio…")); m_apply->setEnabled(false);
    m_status->setText(tr("Preparing the waveform. You can adjust the settings now."));
    m_analysis = std::async(std::launch::async, [sources = m_sources, cancel = m_cancel] {
        std::vector<daw::SilenceEnvelope> result;
        for (const auto& source : sources) {
            if (cancel->load()) return result;
            result.push_back(daw::buildSilenceEnvelope(source.placements, source.durationSeconds, source.sampleRate,
                [cancel] { return !cancel->load(); }));
        }
        return result;
    });
    auto* timer = new QTimer(this); timer->setInterval(25);
    connect(timer, &QTimer::timeout, this, [this, timer] { pollAnalysis(); if (!m_analysis.valid()) timer->stop(); }); timer->start();
}

StripSilenceDialog::~StripSilenceDialog() {
    m_cancel->store(true);
    rememberSettings();
    if (m_analysis.valid()) m_analysis.wait();
}

void StripSilenceDialog::rememberSettings() {
    auto prefs = m_controller.recordingPrefs(); prefs.stripSilence = m_settings;
    m_controller.setRecordingPrefs(prefs); ui::silence::persist(m_settings);
}

void StripSilenceDialog::pollAnalysis() {
    if (!m_analysis.valid() || m_analysis.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    m_progress->hide();
    try {
        m_envelopes = m_analysis.get();
        m_ready = m_envelopes.size() == m_sources.size();
        for (const auto& envelope : m_envelopes) m_ready &= !envelope.peaks.empty();
        if (!m_ready) throw std::runtime_error("Audio analysis did not complete.");
        m_apply->setEnabled(true); updatePreview();
    } catch (const std::exception& error) {
        m_preview->setMessage(tr("Audio could not be analyzed."));
        m_status->setText(QString::fromUtf8(error.what())); m_apply->setEnabled(false);
    }
}

void StripSilenceDialog::updatePreview() {
    if (!m_status || !m_ready) return;
    if (m_settingsOnly) {
        m_status->setText(tr("Last settings are remembered. Auto Silence processes audio clips only.")); return;
    }
    const auto index = std::size_t(std::max(0, m_clip->currentIndex()));
    if (index >= m_envelopes.size()) return;
    const auto& source = m_sources[index];
    auto regions = daw::detectSilenceRegions(m_envelopes[index], m_settings, source.original.startSeconds, source.tempo);
    double kept = 0; for (const auto& region : regions) kept += region.end - region.begin;
    const double removed = std::max(0.0, source.durationSeconds - kept);
    m_status->setText(regions.empty() ? tr("No sound above the threshold. This clip will be removed; Undo restores it.") :
        removed < .001 ? tr("No silence to remove with these settings.") :
        tr("%1 fragments · %2 s of silence removed\nColored regions stay in place. The original audio file is preserved.")
            .arg(regions.size()).arg(removed, 0, 'f', 2));
    m_preview->setData(&m_envelopes[index], std::move(regions), m_settings.thresholdDb);
}

void StripSilenceDialog::apply() {
    if (!m_ready) return;
    rememberSettings();
    if (m_settingsOnly) { accept(); return; }
    std::vector<std::vector<daw::SilenceRegion>> regions;
    bool changed = false;
    for (std::size_t i = 0; i < m_sources.size(); ++i) {
        const auto& source = m_sources[i];
        auto kept = daw::detectSilenceRegions(m_envelopes[i], m_settings, source.original.startSeconds, source.tempo);
        changed |= kept.size() != 1 || kept.front().begin > 1e-9 || kept.front().end < source.durationSeconds - 1e-9;
        regions.push_back(std::move(kept));
    }
    m_apply->setEnabled(false);
    const auto result = m_controller.applyStripSilence(m_sources, regions, m_settings, m_created);
    if (!result) { m_status->setText(QString::fromStdString(result.message())); m_apply->setEnabled(true); return; }
    m_applied = changed; accept();
}
