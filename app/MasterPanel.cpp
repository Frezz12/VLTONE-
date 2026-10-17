#include "MasterPanel.hpp"
#include "Controls.hpp"
#include "ChannelStrip.hpp"
#include "ConsoleLevelWell.hpp"
#include "EngineController.hpp"
#include "PluginPickerMenu.hpp"
#include "Theme.hpp"
#include "UiFrameClock.hpp"
#include "graphics/ScenePaintSource.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QGridLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
constexpr auto master = daw::EngineController::kMasterChannelId;
QString number(double value, int decimals = 1) {
    if (std::isnan(value)) return QStringLiteral("—");
    if (!std::isfinite(value)) return QStringLiteral("−∞");
    return QString::number(value, 'f', decimals);
}
double db(double gain) { return gain > 1e-9 ? 20 * std::log10(gain) : -INFINITY; }
QLabel* label(const QString& text, const char* role, QWidget* parent) {
    auto* result = new QLabel(text, parent);
    result->setProperty("masterRole", role);
    return result;
}
QToolButton* button(const QString& text, QWidget* parent) {
    auto* result = new QToolButton(parent);
    result->setText(text); result->setMinimumHeight(28);
    result->setFocusPolicy(Qt::StrongFocus);
    result->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return result;
}
void clearRows(QVBoxLayout* layout) {
    while (auto* item = layout->takeAt(0)) {
        if (auto* widget = item->widget()) { widget->hide(); widget->deleteLater(); }
        delete item;
    }
}
}

class MasterMeter final : public QWidget, public ui::graphics::ScenePaintSource {
public:
    explicit MasterMeter(QWidget* parent) : QWidget(parent) {
        setObjectName("MasterVuMeter"); setFixedHeight(148);
        setAccessibleName(MasterPanel::tr("Stereo VU meter"));
    }
    void setLevels(daw::engine::LoudnessLevels levels, daw::AudioMeterSnapshot peaks, int reference) {
        if (m_levels.vuLeft==levels.vuLeft && m_levels.vuRight==levels.vuRight &&
            m_peaks.left==peaks.left && m_peaks.right==peaks.right &&
            m_peaks.hold==peaks.hold && m_reference==reference) return;
        m_levels = levels; m_peaks = peaks; m_reference = reference;
        ui::FrameClock::instance().request(this, rect());
    }
    void paintScene(QPainter& p, const QRegion&) override {
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF body = QRectF(rect()).adjusted(0.5, .5, -.5, -.5);
        QLinearGradient casing(0, 0, 0, height());
        casing.setColorAt(0, QColor("#3c3e3e")); casing.setColorAt(.15, QColor("#222625"));
        casing.setColorAt(1, QColor("#121715"));
        p.setPen(QColor("#080b0a")); p.setBrush(casing); p.drawRoundedRect(body, 10, 10);
        const QRectF face = body.adjusted(6, 6, -6, -30);
        QLinearGradient glow(face.topLeft(), face.bottomLeft());
        glow.setColorAt(0, QColor("#bfa777")); glow.setColorAt(.2, QColor("#f2dfaa"));
        glow.setColorAt(.65, QColor("#e6c98d")); glow.setColorAt(1, QColor("#c09e62"));
        p.setPen(QPen(QColor("#655233"), 1)); p.setBrush(glow); p.drawRoundedRect(face, 6, 6);
        p.save(); QPainterPath clip; clip.addRoundedRect(face, 6, 6); p.setClipPath(clip);
        QRadialGradient light(QPointF(face.center().x(), face.top()+18), face.width()*.7);
        light.setColorAt(0, QColor(255,248,206,105)); light.setColorAt(1, QColor(255,230,150,0));
        p.fillRect(face, light);
        const QPointF pivot(face.center().x(), face.bottom()+10);
        const double radius = std::min({(face.width()-34.)/1.732, face.height()-12., 126.});
        const auto position = [&](double value, double length) {
            // VU's scale follows voltage, giving the familiar compressed
            // negative range and open 0…+3 region on the right.
            const double fraction = std::clamp(std::pow(10., value / 20.) / std::pow(10., 3. / 20.), 0., 1.);
            const double radians = (150. - 120. * fraction) * 3.141592653589793 / 180.;
            return pivot + QPointF(std::cos(radians)*length, -std::sin(radians)*length);
        };
        QPainterPath scale;
        for (int i=0; i<=100; ++i) {
            const auto point = position(-20. + 23.*i/100., radius);
            if (!i) scale.moveTo(point); else scale.lineTo(point);
        }
        p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor("#594f36"), 1)); p.drawPath(scale);
        QFont font = p.font(); font.setPixelSize(9); font.setWeight(QFont::DemiBold); p.setFont(font);
        for (const int tick : {-20,-10,-7,-5,-3,-2,-1,0,1,2,3}) {
            p.setPen(QPen(tick > 0 ? QColor("#aa3226") : QColor("#453d2b"), tick==0 ? 2 : 1));
            p.drawLine(position(tick, radius-10), position(tick, radius+1));
            if (tick==-7 || tick==-2 || tick==-1 || tick==2) continue;
            if (face.width()<220 && (tick==-5 || tick==1)) continue;
            const auto at = position(tick, radius+13);
            p.drawText(QRectF(at.x()-15, at.y()-7, 30, 14), Qt::AlignCenter,
                       tick>0 ? QStringLiteral("+%1").arg(tick) : QString::number(tick));
        }
        font.setPixelSize(20); font.setLetterSpacing(QFont::AbsoluteSpacing, 2); p.setFont(font);
        p.setPen(QColor("#6a5939")); p.drawText(QRectF(face.left(),face.bottom()-43,face.width(),26),Qt::AlignCenter,"VU");
        font.setPixelSize(8); font.setLetterSpacing(QFont::AbsoluteSpacing, .8); p.setFont(font);
        p.drawText(QRectF(face.left(),face.bottom()-20,face.width(),14),Qt::AlignCenter,"STEREO");
        const auto needle = [&](float amplitude, QColor color, double offset) {
            const auto tip = position(db(amplitude) - m_reference, radius-8);
            p.setPen(QPen(QColor(20,16,10,40), 3, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(pivot+QPointF(offset+2,2), tip+QPointF(2,2));
            p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap)); p.drawLine(pivot+QPointF(offset,0),tip);
        };
        needle(m_levels.vuRight, QColor("#bf5c22"), 2);
        needle(m_levels.vuLeft, QColor("#343c32"), -2);
        p.setBrush(QColor("#3e4136")); p.setPen(QColor("#897c5a")); p.drawEllipse(pivot,11,11);
        QLinearGradient glass(face.topLeft(),face.bottomLeft());
        glass.setColorAt(0,QColor(255,255,255,50)); glass.setColorAt(.3,QColor(255,255,255,0));
        p.fillRect(face,glass); p.restore();
        // A separate peak bar keeps fast overloads visible beside the slow VU.
        font.setPixelSize(9); font.setLetterSpacing(QFont::AbsoluteSpacing,0); p.setFont(font);
        for (int ch=0;ch<2;++ch) {
            const double y=height()-20+ch*9;
            p.setPen(QColor("#b5bdb6")); p.drawText(QRectF(11,y-4,12,12),Qt::AlignCenter,ch==0?"L":"R");
            const double w=width()-80;
            p.fillRect(QRectF(30,y,w,4),QColor("#080d0a"));
            const double gain=ch==0?m_peaks.left:m_peaks.right;
            const double fraction=std::clamp((db(gain)+60)/60.,0.,1.);
            QLinearGradient led(30,0,30+w,0); led.setColorAt(0,QColor("#5a9875"));
            led.setColorAt(.8,QColor("#d5b967")); led.setColorAt(1,QColor("#ef6b4d"));
            p.fillRect(QRectF(30,y,w*fraction,4),led);
        }
        p.setPen(m_peaks.hold>=1 ? QColor("#ff795b") : QColor("#a2aaa2"));
        p.drawText(QRectF(width()-46,height()-31,38,25),Qt::AlignCenter,m_peaks.hold>=1?"CLIP":"PEAK");
    }
protected:
    void paintEvent(QPaintEvent*) override { QPainter p(this); paintScene(p, QRegion(rect())); }
private:
    daw::engine::LoudnessLevels m_levels;
    daw::AudioMeterSnapshot m_peaks;
    int m_reference=-18;
};

MasterPanel::MasterPanel(daw::EngineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setObjectName("MasterPanel"); setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* root = new QVBoxLayout(this); root->setContentsMargins(1,0,0,0); root->setSpacing(0);
    auto* header = new QWidget(this); header->setObjectName("MasterPanelHeader");
    auto* head = new QHBoxLayout(header); head->setContentsMargins(10,6,6,6); head->setSpacing(6);
    auto* title = label(tr("MASTER"),"title",header); head->addWidget(title);
    head->addStretch();
    auto* close = new ui::IconButton(icons::Glyph::ArrowRight,tr("Hide master panel"),header);
    close->setObjectName("HideMasterPanel"); close->setButtonSize(28,28);
    close->setFocusPolicy(Qt::StrongFocus); head->addWidget(close);
    connect(close,&QAbstractButton::clicked,this,&MasterPanel::closeRequested); root->addWidget(header);
    auto* scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body=new QWidget(scroll); body->setObjectName("MasterPanelBody");
    auto* column=new QVBoxLayout(body); column->setContentsMargins(8,8,8,10); column->setSpacing(8);
    m_meter=new MasterMeter(body); column->addWidget(m_meter);
    auto* meterTools=new QHBoxLayout; meterTools->setSpacing(6);
    meterTools->addWidget(label(tr("0 VU"),"caption",body));
    auto* reference=new QComboBox(body); reference->setObjectName("MasterVuReference");
    reference->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    reference->setAccessibleName(tr("VU reference level"));
    for(int value:{-20,-18,-14}) reference->addItem(QString::number(value)+" dBFS",value);
    m_reference=QSettings().value("master/vuReference",-18).toInt();
    const int referenceIndex=reference->findData(m_reference);
    reference->setCurrentIndex(referenceIndex<0?1:referenceIndex);
    m_reference=reference->currentData().toInt();
    connect(reference,&QComboBox::currentIndexChanged,this,[this,reference] {
        m_reference=reference->currentData().toInt(); QSettings().setValue("master/vuReference",m_reference); refreshMeters();
    });
    reference->setToolTip(tr("Sine-wave reference for the VU needles. Does not change audio gain."));
    meterTools->addWidget(reference,1);
    auto* reset=button(tr("Reset"),body); reset->setObjectName("MasterResetMeters");
    reset->setToolTip(tr("Reset integrated loudness and held peak"));
    connect(reset,&QAbstractButton::clicked,this,[this] {
        m_controller->resetMasterLoudness(); m_controller->resetMasterPeakHold(); m_readoutClock.invalidate(); refreshMeters();
    }); meterTools->addWidget(reset); column->addLayout(meterTools);
    auto* metrics=new QWidget(body); auto* grid=new QGridLayout(metrics);
    grid->setContentsMargins(0,0,0,0); grid->setSpacing(6);
    const QStringList names={tr("M · LUFS"),tr("S · LUFS"),tr("I · LUFS"),tr("Peak · dBFS"),tr("RMS · dBFS"),tr("Correlation")};
    const char* metricIds[]={"MasterMomentary","MasterShortTerm","MasterIntegrated","MasterPeak","MasterRms","MasterCorrelation"};
    const QStringList tips={tr("Momentary loudness · 400 ms"),tr("Short-term loudness · 3 s"),
        tr("Integrated loudness since reset; measured during playback"),tr("Highest sample peak since reset"),
        tr("Unweighted stereo RMS level"),tr("Stereo correlation: +1 mono, 0 uncorrelated, −1 opposite polarity")};
    for(int i=0;i<6;++i) {
        auto* cell=new QWidget(metrics); cell->setObjectName("MasterMetric"); cell->setToolTip(tips[i]);
        auto* layout=new QVBoxLayout(cell); layout->setContentsMargins(7,5,7,5); layout->setSpacing(2);
        layout->addWidget(label(names[i],"caption",cell));
        m_values[i]=label("—","value",cell); m_values[i]->setAccessibleName(tips[i]);
        m_values[i]->setObjectName(metricIds[i]);
        m_values[i]->setMinimumWidth(0); layout->addWidget(m_values[i]); grid->addWidget(cell,i/2,i%2);
        grid->setColumnStretch(i%2,1);
    } column->addWidget(metrics);
    auto* output=new ui::ConsoleLevelWell(body); output->setObjectName("MasterOutput");
    output->setMinimumHeight(204);
    output->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Expanding);
    auto* outputColumn=new QVBoxLayout(output); outputColumn->setContentsMargins(6,5,6,5); outputColumn->setSpacing(3);
    auto* balance=new QHBoxLayout; balance->addStretch();
    m_pan=new ui::PanKnob(output); m_pan->setObjectName("MasterBalance"); m_pan->setAutomatable(true);
    m_pan->setAccessibleName(tr("Master balance")); balance->addWidget(m_pan);
    m_panValue=label("C","caption",output); m_panValue->setFixedWidth(28); balance->addWidget(m_panValue);
    balance->addStretch(); outputColumn->addLayout(balance);
    auto* controls=new QHBoxLayout; controls->setSpacing(5); controls->addStretch();
    m_fader=new ui::FaderWidget(output);
    m_fader->setObjectName("MasterOutputFader"); m_fader->setAccessibleName(tr("Master output gain"));
    m_fader->setAutomatable(true);
    m_outputMeter=new ui::LevelMeter(Qt::Vertical,2,output); m_outputMeter->setFixedWidth(18);
    m_outputMeter->setAccessibleName(tr("Master output peak level"));
    ui::configureConsoleLevel(m_fader,m_outputMeter);
    m_fader->setMinimumHeight(132);
    controls->addWidget(m_fader); controls->addWidget(m_outputMeter); controls->addStretch(); outputColumn->addLayout(controls,1);
    auto* readouts=new QHBoxLayout; readouts->setSpacing(3);
    m_gain=label("0.0","gain",output); m_gain->setObjectName("ChannelGainReadout");
    m_gain->setAlignment(Qt::AlignCenter); m_gain->setFixedHeight(20);
    m_peak=button("−∞",output); m_peak->setObjectName("ChannelPeakReadout"); m_peak->setFixedHeight(20);
    m_peak->setToolTip(tr("Highest sample peak since reset")); m_peak->setAccessibleName(tr("Reset held peak"));
    const auto resetPeak=[this] { m_controller->resetMasterPeakHold(); m_outputMeter->clearClip(); refreshMeters(); };
    connect(m_peak,&QAbstractButton::clicked,this,resetPeak);
    connect(m_outputMeter,&ui::LevelMeter::peakResetRequested,this,resetPeak);
    readouts->addWidget(m_gain,1); readouts->addWidget(m_peak,1); outputColumn->insertLayout(1,readouts);
    connect(m_fader,&ui::FaderWidget::gainChanged,this,[this](double value) {
        if(!m_gainBefore) m_gainBefore=m_controller->masterVolume();
        m_controller->setMasterVolumeLive(float(value)); m_gain->setText(number(db(value)));
        m_gain->setToolTip(tr("Master output gain")+": "+number(db(value))+" dB");
    });
    connect(m_fader,&ui::FaderWidget::editFinished,this,[this] {
        if(m_gainBefore) { m_controller->commitMasterVolumeEdit(*m_gainBefore); m_gainBefore.reset(); emit edited(); }
    });
    connect(m_pan,&ui::PanKnob::panChanged,this,[this](double value) {
        if(!m_panBefore) m_panBefore=m_controller->masterPan(); m_controller->setMasterPanLive(float(value));
        m_panValue->setText(ui::formatPan(value));
    });
    connect(m_pan,&ui::PanKnob::editFinished,this,[this] {
        if(m_panBefore) { m_controller->commitMasterPanEdit(*m_panBefore); m_panBefore.reset(); emit edited(); }
    });
    connect(m_fader,&ui::FaderWidget::automateRequested,this,[this] { emit automateRequested(false); });
    connect(m_pan,&ui::PanKnob::automateRequested,this,[this] { emit automateRequested(true); });
    column->addWidget(label(tr("MASTER INSERTS"),"section",body));
    auto* insertWell=new ui::ConsoleLevelWell(body); insertWell->setObjectName("MasterInsertWell");
    m_inserts=new QVBoxLayout(insertWell); m_inserts->setContentsMargins(1,1,1,1); m_inserts->setSpacing(1);
    column->addWidget(insertWell);
    // Give the available height to the console, keeping its lower edge beside
    // the analysis footer instead of leaving an expanding spacer below it.
    column->addWidget(label(tr("OUTPUT"),"section",body)); column->addWidget(output,1);
    scroll->setWidget(body); root->addWidget(scroll,1);
    auto* footer=new QWidget(this); footer->setObjectName("MasterAnalysisFooter");
    footer->setAttribute(Qt::WA_StyledBackground);
    auto* footerColumn=new QVBoxLayout(footer); footerColumn->setContentsMargins(8,4,8,8); footerColumn->setSpacing(6);
    m_analysisToggle=button(tr("Analysis inserts"),footer); m_analysisToggle->setObjectName("MasterAnalysisToggle");
    m_analysisToggle->setCheckable(true); m_analysisToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_analysisToggle->setChecked(QSettings().value("master/analysisExpanded",false).toBool());
    m_analysisToggle->setAccessibleName(tr("Expand analysis inserts")); footerColumn->addWidget(m_analysisToggle);
    m_analysisBody=new QWidget(footer); auto* analysisColumn=new QVBoxLayout(m_analysisBody);
    analysisColumn->setContentsMargins(0,0,0,0); analysisColumn->setSpacing(6);
    auto* explanation=label(tr("Hear the final output. Never change the sound."),"caption",m_analysisBody);
    explanation->setWordWrap(true); analysisColumn->addWidget(explanation);
    auto* analysisWell=new ui::ConsoleLevelWell(m_analysisBody); analysisWell->setObjectName("MasterAnalysisWell");
    m_analysisInserts=new QVBoxLayout(analysisWell); m_analysisInserts->setContentsMargins(1,1,1,1); m_analysisInserts->setSpacing(1);
    analysisColumn->addWidget(analysisWell); footerColumn->addWidget(m_analysisBody);
    connect(m_analysisToggle,&QAbstractButton::toggled,this,[this](bool expanded) {
        m_analysisBody->setVisible(expanded); QSettings().setValue("master/analysisExpanded",expanded); applyTheme();
    }); m_analysisBody->setVisible(m_analysisToggle->isChecked()); root->addWidget(footer);
    m_timer=new ui::FrameTimer(this,this);
    connect(m_timer,&ui::FrameTimer::timeout,this,&MasterPanel::refreshMeters);
    connect(&ThemeManager::instance(),&ThemeManager::changed,this,&MasterPanel::applyTheme);
    applyTheme(); sync();
}

void MasterPanel::applyTheme() {
    const auto& t=th();
    setStyleSheet(QStringLiteral(
        "#MasterPanel { background:%1; border-left:1px solid %2; }"
        "#MasterPanelHeader { background:%3; border-bottom:1px solid %2; }"
        "#MasterPanelBody { background:%1; }"
        "#MasterAnalysisFooter { background:%1; border-top:1px solid %2; }"
        "#MasterMetric { background:%4; border:1px solid %2; border-radius:4px; }"
        "QLabel { background:transparent; border:0; color:%5; }"
        "QLabel[masterRole=title] { font-size:12px; font-weight:600; letter-spacing:1px; }"
        "QLabel[masterRole=caption] { font-size:10px; color:%6; }"
        "QLabel[masterRole=section] { font-size:9px; font-weight:500; color:%6; }"
        "QLabel[masterRole=value] { font-size:17px; font-weight:600; }"
        "QToolButton { padding:3px 6px; }"
        "QToolButton:focus { border:1px solid %7; border-radius:4px; }"
        "#MasterVuReference { font-size:11px; }"
        "#MasterResetMeters { font-size:10px; padding:3px 4px; }"
        "#SlotButton { background:transparent; border:0; padding:0 5px; font-size:10px; font-weight:500; text-align:left; }"
        "#SlotButton::menu-indicator { image:none; width:0; }"
        "#MasterAddEffect, #MasterAddAnalyzer { background:transparent; border:0; padding:0; border-radius:4px; }"
        "#MasterAddEffect:hover, #MasterAddEffect:focus, #MasterAddAnalyzer:hover, #MasterAddAnalyzer:focus { background:%8; }"
        "#MasterAnalysisToggle { background:transparent; border:0; text-align:left; color:%6; padding:0; font-size:10px; }"
        "#MasterAnalysisToggle:hover, #MasterAnalysisToggle:focus { color:%5; }"
        "#SlotOverflow { background:%4; color:%5; border:1px solid %2; padding:0; }"
        "#ChannelGainReadout, #ChannelPeakReadout { background:%9; color:%5; border:1px solid %2; border-radius:3px; font-size:9px; padding:0; }"
        "#ChannelPeakReadout { color:%6; }"
        "#ChannelPeakReadout:hover, #ChannelPeakReadout:focus { background:%8; color:%5; }"
        "#ChannelPeakReadout[clipped=true] { color:%10; }")
        .arg(t.surface.name(),t.separator().name(),t.panelBottom().name(),t.well().name(),
             t.textPrimary.name(),t.textSecondary.name(),t.accent.name(),
             mixColors(t.well(),t.textPrimary,.06).name(),t.wellTop().name(),Theme::record().name()));
    m_analysisToggle->setIcon(icons::icon(m_analysisToggle->isChecked()?icons::Glyph::Chevron:icons::Glyph::ChevronRight,t.textSecondary,14));
    for(const auto* name:{"MasterAddEffect","MasterAddAnalyzer"})
        if(auto* add=findChild<QToolButton*>(name)) add->setIcon(icons::icon(icons::Glyph::Plus,t.textSecondary,14));
    m_meter->update();
}

void MasterPanel::addPlugin(bool analysis,QWidget* anchor) {
    auto* menu=ui::buildPluginMenu(this,m_controller,false,[this,analysis](const auto& descriptor) {
        const auto id=m_controller->addInsert(master,descriptor,size_t(-1),analysis);
        if(id.empty()) { QMessageBox::warning(this,tr("Add plugin"),tr("Could not load %1.").arg(QString::fromStdString(descriptor.name))); return; }
        ui::rememberRecentPlugin(descriptor); sync(); emit edited(); emit structureChanged();
        emit editorRequested(QString::fromLatin1(master),QString::fromStdString(id));
    });
    menu->setAttribute(Qt::WA_DeleteOnClose); menu->popup(anchor->mapToGlobal(QPoint(0,anchor->height())));
}

void MasterPanel::rebuildInserts() {
    clearRows(m_inserts); clearRows(m_analysisInserts);
    const auto* inserts=m_controller->channelInserts(master); if(!inserts) return;
    int counts[2]{};
    for(std::size_t index=0;index<inserts->size();++index) {
        const auto& slot=(*inserts)[index]; if(!slot.isLoaded()) continue;
        const auto id=slot.id; const bool analysis=slot.analysisOnly;
        auto* edit=ChannelStrip::createSlotButton(QString::fromStdString(slot.name),true,this);
        edit->setProperty("insertRow",true); edit->setProperty("bypassed",slot.bypassed);
        edit->setToolTip(QString::fromStdString(slot.name)); edit->setFocusPolicy(Qt::StrongFocus);
        edit->setContextMenuPolicy(Qt::CustomContextMenu);
        auto* power=new ui::IconButton(icons::Glyph::Power,ChannelStrip::tr("Bypass this plugin"),this);
        power->setCheckable(true); power->setChecked(slot.bypassed); power->setActiveColor(Theme::mute());
        power->setFocusPolicy(Qt::StrongFocus);
        connect(power,&QAbstractButton::clicked,this,[this,id](bool on) {
            m_controller->setInsertBypassed(master,id,on); emit edited(); sync();
        });
        auto* open=new ui::IconButton(icons::Glyph::Detach,ChannelStrip::tr("Open the plugin window"),this);
        open->setFocusPolicy(Qt::StrongFocus);
        const auto showEditor=[this,id] { emit editorRequested(master,QString::fromStdString(id)); };
        connect(edit,&QAbstractButton::clicked,this,showEditor); connect(open,&QAbstractButton::clicked,this,showEditor);
        auto* swap=new ui::IconButton(icons::Glyph::Chevron,ChannelStrip::tr("Replace this plugin"),this);
        swap->setFocusPolicy(Qt::StrongFocus);
        connect(swap,&QAbstractButton::clicked,this,[this,id,swap] {
            auto* menu=ui::buildPluginMenu(this,m_controller,false,[this,id](const auto& descriptor) {
                if(!m_controller->replaceInsert(master,id,descriptor)) {
                    QMessageBox::warning(this,tr("Add plugin"),tr("Could not load %1.").arg(QString::fromStdString(descriptor.name))); return;
                }
                ui::rememberRecentPlugin(descriptor); sync(); emit edited(); emit structureChanged();
            });
            menu->setAttribute(Qt::WA_DeleteOnClose); menu->popup(swap->mapToGlobal(QPoint(0,swap->height())));
        });
        auto* row=ChannelStrip::createSlotRow(edit,{power,open,swap},this); row->setObjectName("MasterInsertRow");
        row->setProperty("slotId",QString::fromStdString(id)); row->setProperty("analysisOnly",analysis);
        connect(edit,&QWidget::customContextMenuRequested,this,[this,id,analysis,edit](const QPoint& atPoint) {
            auto* menu=new QMenu(this); menu->setAttribute(Qt::WA_DeleteOnClose);
            connect(menu->addAction(ChannelStrip::tr("Open the plugin window")),&QAction::triggered,this,
                    [this,id] { emit editorRequested(master,QString::fromStdString(id)); });
            ui::addPluginControlsAction(menu,m_controller,master,QString::fromStdString(id));
            const auto* current=m_controller->channelInserts(master);
            std::vector<std::size_t> peers;
            for(std::size_t i=0;i<current->size();++i) if((*current)[i].analysisOnly==analysis && (*current)[i].isLoaded()) peers.push_back(i);
            const auto at=std::find_if(peers.begin(),peers.end(),[&](auto i) { return (*current)[i].id==id; });
            if(at==peers.end()) { menu->deleteLater(); return; }
            const auto move=[this,id](std::size_t to) { m_controller->moveInsert(master,id,to); sync(); emit edited(); emit structureChanged(); };
            auto* up=menu->addAction(tr("Move up")); up->setEnabled(at!=peers.begin());
            if(at!=peers.begin()) connect(up,&QAction::triggered,this,[move,to=*(at-1)] { move(to); });
            auto* down=menu->addAction(tr("Move down")); down->setEnabled(at+1!=peers.end());
            if(at+1!=peers.end()) connect(down,&QAction::triggered,this,[move,to=*(at+1)] { move(to); });
            menu->addSeparator(); connect(menu->addAction(tr("Remove plugin")),&QAction::triggered,this,[this,id] {
                m_controller->removeInsert(master,id); sync(); emit edited(); emit structureChanged();
            }); menu->popup(edit->mapToGlobal(atPoint));
        });
        (analysis?m_analysisInserts:m_inserts)->addWidget(row); ++counts[analysis?1:0];
    }
    for(bool analysis:{false,true}) {
        auto* add=ChannelStrip::createSlotButton({},false,this);
        add->setObjectName(analysis?"MasterAddAnalyzer":"MasterAddEffect");
        add->setFixedHeight(std::max(1,3-counts[analysis?1:0])*20-1);
        add->setToolButtonStyle(Qt::ToolButtonIconOnly); add->setIconSize(QSize(14,14));
        add->setFocusPolicy(Qt::StrongFocus);
        add->setToolTip(analysis?tr("+ Add analyzer"):tr("+ Add effect")); add->setAccessibleName(add->toolTip());
        add->setIcon(icons::icon(icons::Glyph::Plus,th().textSecondary,14));
        connect(add,&QAbstractButton::clicked,this,[this,analysis,add] { addPlugin(analysis,add); });
        (analysis?m_analysisInserts:m_inserts)->addWidget(add);
    }
    m_analysisToggle->setText(tr("Analysis inserts")+QStringLiteral("  ·  %1").arg(counts[1]));
}

void MasterPanel::sync() {
    QString state;
    for(const auto& slot:m_controller->project().masterInserts)
        state+=QString::fromStdString(slot.id+slot.name+slot.uid)+QString::number(slot.bypassed)+QString::number(slot.analysisOnly);
    if(state!=m_chainState || !m_inserts->count()) { m_chainState=state; rebuildInserts(); }
    double gain=m_controller->masterVolume(),pan=m_controller->masterPan();
    if(m_controller->isPlaying()) {
        daw::AutomationTarget target; target.channelId=master; target.kind=daw::AutomationTargetKind::TrackVolume;
        if(const auto value=m_controller->automationValueAtPlayhead(target)) gain=*value;
        target.kind=daw::AutomationTargetKind::TrackPan;
        if(const auto value=m_controller->automationValueAtPlayhead(target)) pan=*value;
    }
    if(!m_gainBefore && !m_fader->isEditing()) { const QSignalBlocker block(m_fader); m_fader->setGain(gain); }
    if(!m_panBefore && !m_pan->isEditing()) { const QSignalBlocker block(m_pan); m_pan->setPan(pan); }
    m_gain->setText(number(db(m_fader->gain())));
    m_gain->setToolTip(tr("Master output gain")+": "+number(db(m_fader->gain()))+" dB");
    m_panValue->setText(ui::formatPan(m_pan->pan()));
    if(isVisible()) refreshMeters();
}

void MasterPanel::refreshMeters() {
    if(!ui::isVisibleInViewport(this)) return;
    const auto levels=m_controller->masterLoudness(); const auto peaks=m_controller->meterSnapshot(master);
    m_meter->setLevels(levels,peaks,m_reference);
    m_outputMeter->setPeaks(peaks.left,peaks.right); m_outputMeter->setPeakHold(peaks.hold);
    m_peak->setText(number(db(peaks.hold)));
    if(m_peak->property("clipped").toBool()!=(peaks.hold>=1)) {
        m_peak->setProperty("clipped",peaks.hold>=1); m_peak->style()->unpolish(m_peak); m_peak->style()->polish(m_peak);
    }
    if(m_readoutClock.isValid() && m_readoutClock.elapsed()<100) return;
    m_readoutClock.start();
    const std::array<QString,6> values={number(levels.momentary),number(levels.shortTerm),number(levels.integrated),
        number(db(peaks.hold)),number(db(std::sqrt((double(levels.rmsLeft)*levels.rmsLeft+double(levels.rmsRight)*levels.rmsRight)*.5))),
        peaks.left+peaks.right>1e-8f?number(levels.correlation,2):QStringLiteral("—")};
    for(int i=0;i<6;++i) if(m_values[i]->text()!=values[i]) {
        m_values[i]->setText(values[i]);
        m_values[i]->setAccessibleName(m_values[i]->parentWidget()->toolTip()+": "+values[i]);
    }
}
void MasterPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); sync(); m_timer->start(); }
void MasterPanel::hideEvent(QHideEvent* e) { m_fader->finishEditing(); m_timer->stop(); QWidget::hideEvent(e); }
