#include "ReverbPanel.hpp"
#include "ReverbWidgets.hpp"
#include "EngineController.hpp"
#include "RackParameterBinding.hpp"
#include "Theme.hpp"
#include <QAbstractButton>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace rv = daw::plugins::reverb;
namespace {
constexpr double tau = 2 * std::numbers::pi;
const Theme& theme() { return ThemeManager::instance().theme(); }
QColor violet() { return reverbUi::violet(); }
QColor cyan() { return reverbUi::turquoise(); }
QColor canvas() { return reverbUi::background(); }
QColor well() { return reverbUi::surface(); }
QColor alpha(QColor c,int a) { c.setAlpha(a); return c; }
QString modeText(unsigned i) { return QCoreApplication::translate("ReverbPanel",rv::modeName(i).data()); }
QFont fontAt(const QFont& source,int size,bool bold=false) { QFont f(source);f.setPixelSize(size);f.setWeight(bold?QFont::DemiBold:QFont::Normal);return f; }
struct UserPreset { QString name; rv::Values values; };
QList<UserPreset> userPresets() {
    QList<UserPreset> out;
    for(const auto& item:QSettings().value("reverb/userPresets.v1").toList()) {
        const auto map=item.toMap();const auto values=map.value("values").toList();
        UserPreset p{map.value("name").toString(),{}};
        if(p.name.isEmpty() || p.name.size()>64 || values.size()!=rv::kParameterCount)continue;
        bool valid=true;
        for(const auto& info:rv::parameterTable()) {
            bool ok=false;const double v=values[int(info.index)].toDouble(&ok);
            valid &= ok && std::isfinite(v) && v>=info.minValue && v<=info.maxValue && (!info.isStepped || std::round(v)==v);
            p.values[info.index]=v;
        }
        if(valid)out.push_back(p);
    }
    return out;
}
}

ReverbCanvas::ReverbCanvas(bool response,QWidget* parent):ui::FrameWidget(parent),m_response(response) {
    setObjectName(response?"ReverbResponse":"ReverbField");
    setMinimumSize(response?230:320,270);
    setFocusPolicy(Qt::StrongFocus);setMouseTracking(true);setCursor(Qt::CrossCursor);
    setAccessibleName(response?tr("Space response"):tr("Space Field"));
    setToolTip(response?tr("Drag the onset for Pre-delay or the tail handle for Decay. Shift gives fine control."):
        tr("Drag right for a larger space, up for more brightness. Arrow keys adjust; Shift gives fine control. Home resets."));
    for(const auto& info:rv::parameterTable())m_snapshot.state.values[info.index]=info.defaultValue;
    m_animation=new ui::FrameTimer(this,this);
    connect(m_animation,&ui::FrameTimer::timeout,this,[this] {
        m_phase+=m_animation->deltaSeconds()*.12;
        update();
    });
    connect(&ThemeManager::instance(),&ThemeManager::changed,this,[this]{update();});
}
bool ReverbCanvas::animationActive() const { return m_animation->isActive(); }
void ReverbCanvas::setSnapshot(const daw::ReverbSnapshot& state,bool reducedMotion) {
    const bool changed=state.state.values!=m_snapshot.state.values ||
        std::abs(state.telemetry.tail-m_snapshot.telemetry.tail)>1.e-5 ||
        std::abs(state.telemetry.early-m_snapshot.telemetry.early)>1.e-5 ||
        std::abs(state.telemetry.duckGain-m_snapshot.telemetry.duckGain)>1.e-4;
    m_snapshot=state;m_reduced=reducedMotion;
    if(!m_response && !m_reduced && state.state.values[10]<.5 && isVisible() && state.telemetry.tail>1.e-5)m_animation->start();
    else m_animation->stop();
    if(changed)update();
    if(!m_response)setAccessibleDescription(tr("Size %1 percent; brightness %2 percent.")
        .arg(state.state.values[1],0,'f',1).arg(state.state.values[2],0,'f',1));
}
void ReverbCanvas::setEarlyView(bool early) {m_earlyView=early;update();}
QRectF ReverbCanvas::fieldRect() const {return QRectF(rect()).adjusted(m_response?20:32,58,-24,-40);}
QPointF ReverbCanvas::point() const {
    const auto r=fieldRect();return {r.left()+r.width()*m_snapshot.state.values[1]/100,r.bottom()-r.height()*m_snapshot.state.values[2]/100};
}
void ReverbCanvas::movePoint(QPointF p) {
    const auto r=fieldRect();
    m_snapshot.state.values[1]=std::clamp((p.x()-r.left())/std::max(1.,r.width())*100,0.,100.);
    m_snapshot.state.values[2]=std::clamp((r.bottom()-p.y())/std::max(1.,r.height())*100,0.,100.);
    emit valuesChanged(m_snapshot.state.values[1],m_snapshot.state.values[2]);update();
}
void ReverbCanvas::finishGesture() { if(m_dragging){m_dragging=false;if(m_dragParameter>=0)emit parameterGestureFinished(m_dragParameter==3?"preDelay":"decay");else emit gestureFinished();m_dragParameter=-1;} }
void ReverbCanvas::dragTo(QPointF p,Qt::KeyboardModifiers modifiers) {
    const auto delta=(p-m_lastPointer)*(modifiers.testFlag(Qt::ShiftModifier)?.1:1.);
    m_lastPointer=p;
    if(m_dragParameter>=0){const auto& info=rv::parameterTable()[m_dragParameter];const double scale=m_dragParameter==3?1000:1;
        auto& value=m_snapshot.state.values[m_dragParameter];value=std::clamp(value+delta.x()*m_responseTimeMax/fieldRect().width()*scale,info.minValue,info.maxValue);
        emit parameterChanged(QString::fromStdString(info.id),value);update();
    }else movePoint(point()+delta);
}
void ReverbCanvas::mousePressEvent(QMouseEvent* e) {
    if(e->button()!=Qt::LeftButton || !fieldRect().adjusted(-4,-4,4,12).contains(e->position())){QWidget::mousePressEvent(e);return;}
    setFocus(Qt::MouseFocusReason);m_dragging=true;m_lastPointer=e->position();
    if(m_response){const auto& v=m_snapshot.state.values;const double pre=v[3]*.001;
        m_responseTimeMax=m_earlyView?std::max(.4,pre+.3*(.4+v[1]*.014)):pre+v[4]*1.25+rv::bloomSeconds(v);
        const double onset=fieldRect().left()+pre/m_responseTimeMax*fieldRect().width();
        m_dragParameter=m_earlyView||e->position().x()<onset+18?3:4;e->accept();return;
    }
    // A grab on the orb preserves its offset. A click elsewhere places it.
    if(QLineF(point(),e->position()).length()>18)movePoint(e->position());
    e->accept();
}
void ReverbCanvas::mouseMoveEvent(QMouseEvent* e) {if(m_dragging){dragTo(e->position(),e->modifiers());e->accept();}else QWidget::mouseMoveEvent(e);}
void ReverbCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if(m_dragging && e->button()==Qt::LeftButton){dragTo(e->position(),e->modifiers());finishGesture();e->accept();}
    else QWidget::mouseReleaseEvent(e);
}
void ReverbCanvas::keyPressEvent(QKeyEvent* e) {
    if(m_response){const int index=m_earlyView?3:4;auto& v=m_snapshot.state.values[index];const double step=(index==3?1:.05)*(e->modifiers().testFlag(Qt::ShiftModifier)?.1:1.);
        if(e->key()==Qt::Key_Left||e->key()==Qt::Key_Down)v-=step;else if(e->key()==Qt::Key_Right||e->key()==Qt::Key_Up)v+=step;else if(e->key()==Qt::Key_Home)v=rv::parameterTable()[index].defaultValue;else{QWidget::keyPressEvent(e);return;}
        v=std::clamp(v,rv::parameterTable()[index].minValue,rv::parameterTable()[index].maxValue);const auto id=index==3?QStringLiteral("preDelay"):QStringLiteral("decay");emit parameterChanged(id,v);emit parameterGestureFinished(id);update();e->accept();return;
    }
    double x=m_snapshot.state.values[1],y=m_snapshot.state.values[2];
    const double step=e->modifiers().testFlag(Qt::ShiftModifier)?.1:1.;
    switch(e->key()) {
    case Qt::Key_Left:x-=step;break;case Qt::Key_Right:x+=step;break;
    case Qt::Key_Up:y+=step;break;case Qt::Key_Down:y-=step;break;
    case Qt::Key_Home:x=65;y=45;break;
    default:QWidget::keyPressEvent(e);return;
    }
    const auto r=fieldRect();movePoint({r.left()+x*r.width()/100,r.bottom()-y*r.height()/100});
    emit gestureFinished();e->accept();
}
void ReverbCanvas::contextMenuEvent(QContextMenuEvent* e) {
    if(m_response)return;
    finishGesture();QMenu menu(this);
    auto* reset=menu.addAction(tr("Reset space"));
    auto* size=menu.addAction(tr("Automate Size"));auto* brightness=menu.addAction(tr("Automate Brightness"));
    const auto* action=menu.exec(e->globalPos());
    if(action==reset){const auto r=fieldRect();movePoint({r.left()+r.width()*.65,r.bottom()-r.height()*.45});emit gestureFinished();}
    else if(action==size)emit automationRequested("size");else if(action==brightness)emit automationRequested("brightness");
}
bool ReverbCanvas::event(QEvent* e) {
    if(e->type()==QEvent::UngrabMouse || e->type()==QEvent::WindowDeactivate || e->type()==QEvent::FocusOut)finishGesture();
    return ui::FrameWidget::event(e);
}
void ReverbCanvas::hideEvent(QHideEvent* e) {m_animation->stop();finishGesture();QWidget::hideEvent(e);}
void ReverbCanvas::paintEvent(QPaintEvent*) {QPainter p(this);paintScene(p,QRegion(rect()));}
void ReverbCanvas::paintScene(QPainter& p,const QRegion&) {
    p.setRenderHint(QPainter::Antialiasing);const QRectF bounds=QRectF(rect()).adjusted(.5,.5,-.5,-.5);
    QLinearGradient surface(bounds.topLeft(),bounds.bottomRight());surface.setColorAt(0,well());surface.setColorAt(1,canvas());
    p.setBrush(surface);p.setPen(QPen(alpha(reverbUi::muted(),38),1));p.drawRoundedRect(bounds,9,9);
    auto heading=fontAt(font(),10,true);heading.setLetterSpacing(QFont::AbsoluteSpacing,1.6);p.setFont(heading);p.setPen(reverbUi::muted());
    p.drawText(QRectF(18,13,width()-36,20),Qt::AlignLeft|Qt::AlignVCenter,m_response?tr("SPACE RESPONSE"):tr("SPACE FIELD"));
    const auto r=fieldRect();const auto& v=m_snapshot.state.values;
    p.save();p.setClipRect(r.adjusted(-1,-1,1,1));
    if(!m_response) {
        p.setPen(QPen(alpha(reverbUi::muted(),24),1));
        for(int x=0;x<=8;++x)for(int y=0;y<=6;++y){const QPointF a(r.left()+r.width()*x/8,r.top()+r.height()*y/6);p.drawPoint(a);}
        const QPointF center=r.center()+(point()-r.center())*.25;
        const double energy=std::clamp(double(m_snapshot.telemetry.tail)*4,0.,1.);
        const double radius=std::min(r.width()*.48,r.height()*.73)*(.48+v[1]*.0075);
        const double phase=m_phase;
        QRadialGradient halo(center,radius*1.25);halo.setColorAt(0,alpha(violet(),24+int(energy*36)));
        halo.setColorAt(.65,alpha(violet(),9));halo.setColorAt(1,alpha(cyan(),0));
        p.setPen(Qt::NoPen);p.setBrush(halo);p.drawEllipse(center,radius*1.25,radius*1.25);
        // Contour density represents diffusion. Size changes the volume, while
        // the brighter upper layers follow HF damping rather than Decay.
        const int layers=18+int(v[11]*.2);
        for(int layer=0;layer<layers;++layer) {
            const double h=double(layer)/(layers-1)*2-1;
            const double span=std::sqrt(std::max(0.,1-h*h));
            QPainterPath contour;
            for(int sample=0;sample<=100;++sample){
                const double a=sample*tau/100;
                const double warp=1+.075*std::sin(a*3+h*2+phase)+.025*std::cos(a*5+h*4);
                const double x=std::cos(a)*span*radius*warp;
                const double y=h*radius*.63+std::sin(a)*span*radius*.22;
                const QPointF pos=center+QPointF(x+std::sin(h*2+phase)*radius*.07,y);
                if(sample==0)contour.moveTo(pos);else contour.lineTo(pos);
            }
            QLinearGradient wire(center-QPointF(radius,radius),center+QPointF(radius,radius));
            wire.setColorAt(0,alpha(violet(),int(65+v[2]*.85+energy*35)));
            wire.setColorAt(.55,alpha(violet(),int(80+energy*55)));wire.setColorAt(1,alpha(cyan(),150));
            p.setBrush(Qt::NoBrush);p.setPen(QPen(wire,.8));p.drawPath(contour);
        }
        const auto orbCenter=point();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(alpha(reverbUi::muted(),80),1,Qt::DashLine));
        p.drawLine(QPointF(orbCenter.x(),r.bottom()),orbCenter);p.drawLine(QPointF(r.left(),orbCenter.y()),orbCenter);
        QRadialGradient glow(orbCenter,30);glow.setColorAt(0,alpha(violet(),110));glow.setColorAt(1,alpha(violet(),0));
        p.setBrush(glow);p.setPen(Qt::NoPen);p.drawEllipse(orbCenter,30,30);
        p.setBrush(Qt::NoBrush);p.setPen(QPen(alpha(violet(),m_dragging?180:95),1));p.drawEllipse(orbCenter,16,16);
        QRadialGradient orb(orbCenter-QPointF(3,4),15);orb.setColorAt(0,QColor("#fbf9ff"));orb.setColorAt(.5,violet());orb.setColorAt(1,cyan());
        p.setBrush(orb);p.setPen(QPen(theme().dark?QColor("#e0d7f5"):violet(),1));p.drawEllipse(orbCenter,8,8);
    } else {
        const double pre=v[3]*.001,decay=v[4],bloom=rv::bloomSeconds(v);
        const double timeMax=m_dragging?m_responseTimeMax:(m_earlyView?std::max(.4,pre+.3*(.4+v[1]*.014)):pre+decay*1.25+bloom);
        p.setFont(fontAt(font(),9));
        for(int i=1;i<=3;++i){const double y=r.top()+r.height()*i/3.;p.setPen(QPen(alpha(reverbUi::muted(),22),1));p.drawLine(QPointF(r.left(),y),QPointF(r.right(),y));
            p.setPen(alpha(reverbUi::muted(),135));p.drawText(QRectF(r.right()-28,y-14,28,12),Qt::AlignRight,QString::number(-20*i));}
        if(!m_earlyView) {
            for(int layer=8;layer>=0;--layer) {
                QPainterPath line,fill;fill.moveTo(r.bottomLeft());
                for(int i=0;i<=180;++i) {
                    const double t=timeMax*i/180.-pre;
                    const double rise=t>0?1-std::exp(-t/(.012+bloom*.4)):0;
                    const double amplitude=t>0?rise*rise*std::exp(-6.90775528*t/(decay*(1-layer*.055))):0;
                    const QPointF pos(r.left()+r.width()*i/180.,r.bottom()-r.height()*(.88-layer*.035)*std::log1p(amplitude*200)/std::log(201.));
                    if(i==0)line.moveTo(pos);else line.lineTo(pos);fill.lineTo(pos);
                }
                fill.lineTo(r.bottomRight());fill.closeSubpath();
                if(layer==0){QLinearGradient gradient(r.topLeft(),r.bottomLeft());gradient.setColorAt(0,alpha(violet(),45));gradient.setColorAt(1,alpha(cyan(),1));p.fillPath(fill,gradient);}
                p.setPen(QPen(alpha(violet(),layer==0?225:35+layer*8),layer==0?1.5:.8));p.setBrush(Qt::NoBrush);p.drawPath(line);
            }
        }
        for(const auto& tap:rv::reflections(v)) {
            const double x=r.left()+(tap.seconds+pre)/timeMax*r.width();
            p.setPen(QPen(alpha(cyan(),m_earlyView?200:105),m_earlyView?1.5:1));
            p.drawLine(QPointF(x,r.bottom()),QPointF(x,r.bottom()-r.height()*std::min(.95,tap.gain*3.4)));
            if(m_earlyView){p.setBrush(cyan());p.setPen(Qt::NoPen);p.drawEllipse(QPointF(x,r.bottom()-r.height()*std::min(.95,tap.gain*3.4)),2,2);}
        }
        const double onset=r.left()+pre/timeMax*r.width(),end=r.left()+(pre+decay)/timeMax*r.width();
        p.setPen(QPen(alpha(cyan(),130),1,Qt::DashLine));p.drawLine(QPointF(onset,r.top()+8),QPointF(onset,r.bottom()));
        p.setBrush(cyan());p.setPen(Qt::NoPen);p.drawPolygon(QPolygonF{{onset-4,r.bottom()},{onset+4,r.bottom()},{onset,r.bottom()-6}});
        if(!m_earlyView){p.setPen(QPen(violet(),1));p.setBrush(canvas());p.drawEllipse(QPointF(end,r.bottom()-4),4,4);}
        p.setFont(fontAt(font(),20,true));p.setPen(reverbUi::ink());
        p.drawText(r.adjusted(0,-2,-4,0),Qt::AlignRight|Qt::AlignTop,v[10]>.5?QString::fromUtf8("∞"):(m_earlyView?tr("%1 ms").arg(locale().toString(pre*1000,'f',0)):tr("%1 s").arg(locale().toString(decay,'f',2))));
        p.setFont(fontAt(font(),9));p.setPen(reverbUi::muted());p.drawText(r.adjusted(0,24,-4,0),Qt::AlignRight|Qt::AlignTop,m_earlyView?tr("PRE-DELAY"):tr("DECAY"));
    }
    p.restore();
    p.setPen(QPen(alpha(reverbUi::muted(),65),1));p.drawLine(r.bottomLeft(),r.bottomRight());
    p.setFont(fontAt(font(),9));p.setPen(reverbUi::muted());
    if(!m_response){
        for(int i=0;i<=4;++i){const double x=r.left()+r.width()*i/4.;p.drawLine(QPointF(x,r.bottom()),QPointF(x,r.bottom()+4));if(i==0||i==4)p.drawText(QRectF(x-10,r.bottom()+7,20,14),Qt::AlignCenter,QString::number(i*25));}
        p.drawText(QRectF(r.left(),height()-28,r.width(),18),Qt::AlignCenter,tr("SIZE"));
        p.save();p.translate(15,r.center().y());p.rotate(-90);p.drawText(QRectF(-r.height()/2,-9,r.height(),18),Qt::AlignCenter,tr("BRIGHTNESS"));p.restore();
        if(v[10]>.5){p.setPen(violet());p.drawText(QRectF(width()-68,17,50,15),Qt::AlignRight,tr("HELD"));}
    }else{
        const float energy=m_earlyView?m_snapshot.telemetry.early:m_snapshot.telemetry.tail;
        const double reading=20*std::log10(std::max(1.e-5f,energy));
        p.setPen(reverbUi::muted());p.drawText(QRectF(20,height()-29,width()-40,16),Qt::AlignLeft,m_earlyView?tr("EARLY ENERGY"):tr("TAIL ENERGY"));
        p.setPen(m_earlyView?cyan():violet());p.drawText(QRectF(20,height()-29,width()-40,16),Qt::AlignRight,energy<1.e-5f?QString::fromUtf8("−∞ dB"):QString::number(reading,'f',1)+" dB");
    }
    if(hasFocus()){p.setBrush(Qt::NoBrush);p.setPen(QPen(alpha(violet(),150),1));p.drawRoundedRect(bounds.adjusted(2,2,-2,-2),7,7);}
}

ReverbPanel::ReverbPanel(daw::EngineController* controller,QString channel,QString slot,QWidget* parent)
    :QWidget(parent),m_controller(controller) {
    setObjectName("ReverbPanel");setAccessibleName(tr("Reverb"));setMinimumSize(840,660);resize(1120,716);
    m_binding=new RackParameterBinding(controller,channel,slot,this);
    auto* root=new QVBoxLayout(this);root->setContentsMargins(18,14,18,16);root->setSpacing(12);
    auto* header=new QHBoxLayout;header->setSpacing(6);
    m_preset=new ReverbPresetButton(this);m_preset->setObjectName("ReverbPreset");m_preset->setAccessibleName(tr("Reverb presets"));
    m_preset->setPopupMode(QToolButton::InstantPopup);m_preset->setMinimumWidth(188);m_preset->setMaximumWidth(278);
    m_preset->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    m_presetMenu=new QMenu(m_preset);m_preset->setMenu(m_presetMenu);
    connect(m_presetMenu,&QMenu::aboutToShow,this,&ReverbPanel::buildPresetMenu);header->addWidget(m_preset,1);
    const auto button=[this,header](const QString& text,const char* name,bool checkable=false){
        auto* b=new QToolButton(this);b->setText(text);b->setObjectName(name);b->setAccessibleName(text);b->setCheckable(checkable);b->setFixedHeight(32);header->addWidget(b);return b;
    };
    header->addSpacing(6);m_a=button("A","ReverbA",true);m_b=button("B","ReverbB",true);m_copy=button(tr("Copy"),"ReverbCopy");
    m_a->setFixedWidth(32);m_b->setFixedWidth(32);
    m_a->setToolTip(tr("Compare settings A"));m_b->setToolTip(tr("Compare settings B"));m_copy->setToolTip(tr("Copy current settings to the other comparison"));
    header->addStretch();
    m_freeze=m_binding->toggle("freeze",tr("Freeze"),this);m_freeze->setObjectName("ReverbFreeze");
    m_freeze->setFixedHeight(32);m_freeze->setMinimumWidth(94);
    m_freeze->setToolTip(tr("Hold the current tail. New input is excluded; space changes take effect when released."));header->addWidget(m_freeze);
    m_advanced=button(tr("Advanced"),"ReverbAdvanced",true);root->addLayout(header);
    auto* stage=new QHBoxLayout;stage->setSpacing(12);
    auto* modes=new QVBoxLayout;modes->setSpacing(4);
    auto* label=new QLabel(tr("SPACES"),this);label->setObjectName("ReverbSection");label->setFixedHeight(20);modes->addWidget(label);
    for(unsigned i=0;i<6;++i) {
        auto* b=new ReverbModeButton(i,this);b->setObjectName(QStringLiteral("ReverbMode.%1").arg(i));b->setFixedWidth(150);
        m_modes[i]=b;modes->addWidget(b,1);
        connect(b,&QAbstractButton::clicked,this,[this,i]{m_binding->write("mode",i);m_binding->finish("mode");refresh();});
    }
    stage->addLayout(modes);
    auto* fieldColumn=new QVBoxLayout;fieldColumn->setSpacing(8);
    m_field=new ReverbCanvas(false,this);fieldColumn->addWidget(m_field,1);
    auto* coordinates=new QHBoxLayout;coordinates->setContentsMargins(8,0,8,0);coordinates->setSpacing(8);
    for(int i=0;i<2;++i) {
        const QString id=i==0?"size":"brightness";
        auto* number=new ReverbValue(this);m_coordinates[i]=number;number->setObjectName(QStringLiteral("ReverbValue.")+id);
        number->setRange(0,100);number->setSuffix(" %");number->setDecimals(1);number->setSingleStep(.1);number->setFixedWidth(80);
        auto* caption=new QLabel(i==0?tr("Size"):tr("Brightness"),this);
        number->setAccessibleName(caption->text());
        caption->setBuddy(number);coordinates->addWidget(caption);coordinates->addWidget(number);if(i==0)coordinates->addStretch();
        connect(number,&QDoubleSpinBox::valueChanged,this,[this,id](double value){m_binding->write(id,value);});
        connect(number,&QDoubleSpinBox::editingFinished,this,[this,id]{m_binding->finish(id);});
    }
    fieldColumn->addLayout(coordinates);stage->addLayout(fieldColumn,3);
    auto* responseColumn=new QVBoxLayout;responseColumn->setSpacing(8);
    m_response=new ReverbCanvas(true,this);responseColumn->addWidget(m_response,1);
    auto* responseOptions=new QHBoxLayout;responseOptions->setContentsMargins(8,0,8,0);responseOptions->setSpacing(6);
    auto* early=new QToolButton(this);auto* tail=new QToolButton(this);
    for(auto* b:{early,tail}){b->setCheckable(true);b->setAutoExclusive(true);b->setFixedHeight(30);responseOptions->addWidget(b);}
    early->setText(tr("Early"));early->setAccessibleName(tr("Show early reflections"));early->setObjectName("ReverbEarly");
    tail->setText(tr("Tail"));tail->setAccessibleName(tr("Show diffuse tail"));tail->setObjectName("ReverbTail");tail->setChecked(true);
    auto* duckMeter=new QLabel(this);duckMeter->setObjectName("ReverbDuckMeter");duckMeter->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    responseOptions->addWidget(duckMeter,1);responseColumn->addLayout(responseOptions);stage->addLayout(responseColumn,2);root->addLayout(stage,1);
    connect(early,&QToolButton::clicked,this,[this]{m_response->setEarlyView(true);});
    connect(tail,&QToolButton::clicked,this,[this]{m_response->setEarlyView(false);});
    m_controls=new QStackedWidget(this);m_controls->setObjectName("ReverbControls");m_controls->setFixedHeight(228);
    const auto controls=[this](std::initializer_list<const char*> first,std::initializer_list<const char*> second){
        auto* page=new QWidget(m_controls);auto* column=new QVBoxLayout(page);column->setContentsMargins(0,0,0,0);column->setSpacing(10);
        for(const auto rowIds:{first,second}){
            auto* row=new QHBoxLayout;row->setSpacing(10);
            for(const char* id:rowIds){
                const auto* info=m_binding->info(id);if(!info)continue;
                auto* slider=new ReverbSlider(*info,page);m_sliders.push_back(slider);row->addWidget(slider,1);
                if(QString::fromUtf8(id)=="shimmerAmount")m_shimmerControl=slider;
                connect(slider,&ReverbSlider::plainValueChanged,this,[this,id](double v){m_binding->write(id,v);});
                connect(slider,&ReverbSlider::editFinished,this,[this,id]{m_binding->finish(id);});
                connect(slider,&ReverbSlider::automationRequested,m_binding,&RackParameterBinding::automate);
            }
            column->addLayout(row,1);
        }
        m_controls->addWidget(page);
    };
    controls({"preDelay","decay","width","mix"},{"bloom","motion","duck"});
    controls({"diffusion","earlyLate","shimmerAmount"},{"lowCut","highCut"});root->addWidget(m_controls);
    connect(m_advanced,&QToolButton::toggled,this,[this](bool on){m_controls->setCurrentIndex(on?1:0);});
    connect(m_field,&ReverbCanvas::valuesChanged,this,[this](double size,double brightness){m_binding->write("size",size);m_binding->write("brightness",brightness);});
    connect(m_field,&ReverbCanvas::gestureFinished,m_binding,&RackParameterBinding::finishAll);
    connect(m_field,&ReverbCanvas::automationRequested,m_binding,&RackParameterBinding::automate);
    connect(m_response,&ReverbCanvas::parameterChanged,m_binding,&RackParameterBinding::write);
    connect(m_response,&ReverbCanvas::parameterGestureFinished,m_binding,&RackParameterBinding::finish);
    connect(m_binding,&RackParameterBinding::automationRequested,this,&ReverbPanel::automationRequested);
    connect(m_binding,&RackParameterBinding::edited,this,[this]{
        if(m_binding->available())m_controller->setInsertPresetReference(m_binding->channel(),m_binding->slot(),"custom","Custom");
        emit projectEdited();refresh();
    });
    for(auto* b:{m_a,m_b})connect(b,&QToolButton::clicked,this,[this,b]{
        if(auto* focus=focusWidget();focus&&isAncestorOf(focus))focus->clearFocus();
        m_binding->finishAll();if(m_binding->available()&&m_controller->switchReverbComparison(m_binding->channel(),m_binding->slot(),b==m_a?'A':'B'))emit projectEdited();refresh();
    });
    connect(m_copy,&QToolButton::clicked,this,[this]{m_binding->finishAll();if(m_binding->available()&&m_controller->copyReverbComparison(m_binding->channel(),m_binding->slot()))emit projectEdited();refresh();});
    for(auto* b:findChildren<QAbstractButton*>())b->setFocusPolicy(Qt::StrongFocus);
    m_timer=new QTimer(this);m_timer->setObjectName("ReverbTelemetryTimer");m_timer->setInterval(33);
    connect(m_timer,&QTimer::timeout,this,&ReverbPanel::refresh);
    connect(&ThemeManager::instance(),&ThemeManager::changed,this,&ReverbPanel::applyTheme);
    applyTheme();
}
ReverbPanel::~ReverbPanel(){m_binding->finishAll();}
void ReverbPanel::showEvent(QShowEvent* e){QWidget::showEvent(e);refresh();m_timer->start();}
void ReverbPanel::hideEvent(QHideEvent* e){m_timer->stop();m_binding->finishAll();QWidget::hideEvent(e);}
void ReverbPanel::paintEvent(QPaintEvent*){QPainter p(this);paintScene(p,QRegion(rect()));}
void ReverbPanel::paintScene(QPainter& p,const QRegion&){p.fillRect(rect(),canvas());}
void ReverbPanel::refresh() {
    if(!isVisible())return;
    const bool available=m_binding->available();
    const auto snapshot=available?m_controller->reverbSnapshot(m_binding->channel(),m_binding->slot()):std::nullopt;
    setEnabled(snapshot.has_value());if(!snapshot)return;
    m_binding->refresh();
    const bool reduced=QSettings().value("ui/reduceMotion",false).toBool();
    m_field->setSnapshot(*snapshot,reduced);m_response->setSnapshot(*snapshot,reduced);
    const auto& state=snapshot->state;
    for(unsigned i=0;i<6;++i)m_modes[i]->setChecked(unsigned(state.values[0])==i);
    m_a->setChecked(state.comparison=='A');m_b->setChecked(state.comparison=='B');
    m_preset->setText(state.presetKind=="custom"?tr("Custom"):QCoreApplication::translate("ReverbPanel",state.presetName.c_str()));
    m_preset->setToolTip(m_preset->text());
    m_shimmerControl->setEnabled(state.values[0]==5);
    for(auto* slider:m_sliders){slider->setPlainValue(state.values[slider->parameter().index]);if(slider->parameter().id=="duck")slider->setDuckGain(snapshot->telemetry.duckGain);}
    for(int i=0;i<2;++i)if(!m_coordinates[i]->hasFocus()){QSignalBlocker blocked(m_coordinates[i]);m_coordinates[i]->setValue(state.values[i+1]);}
    if(auto* label=findChild<QLabel*>("ReverbDuckMeter"))label->setText(tr("Duck %1 dB").arg(20*std::log10(std::max(.0001f,snapshot->telemetry.duckGain)),0,'f',1));
}
void ReverbPanel::applyValues(const rv::Values& values,const QString& kind,const QString& name) {
    if(auto* focused=focusWidget();focused && isAncestorOf(focused))focused->clearFocus();
    m_binding->finishAll();if(!m_binding->available())return;
    const auto snapshot=m_controller->reverbSnapshot(m_binding->channel(),m_binding->slot());if(!snapshot)return;
    auto state=snapshot->state;state.values=values;state.presetKind=kind.toStdString();state.presetName=name.toStdString();
    if(m_controller->applyReverbState(m_binding->channel(),m_binding->slot(),state,"Load Reverb Preset"))emit projectEdited();
    refresh();
}
void ReverbPanel::applyFactoryPreset(unsigned i) {
    const auto presets=rv::factoryPresets();if(i>=presets.size())return;
    applyValues(presets[i].values,"factory",QString::fromUtf8(presets[i].name.data(),int(presets[i].name.size())));
}
void ReverbPanel::buildPresetMenu() {
    m_presetMenu->clear();m_presetMenu->addSection(tr("Factory"));
    for(unsigned i=0;i<6;++i)m_presetMenu->addAction(modeText(i),this,[this,i]{applyFactoryPreset(i);});
    const auto saved=userPresets();
    if(!saved.isEmpty()){m_presetMenu->addSection(tr("User"));for(const auto& p:saved)m_presetMenu->addAction(p.name,this,[this,p]{applyValues(p.values,"user",p.name);});}
    m_presetMenu->addSeparator();m_presetMenu->addAction(tr("Save preset…"),this,&ReverbPanel::saveUserPreset);
}
void ReverbPanel::saveUserPreset() {
    m_binding->finishAll();if(!m_binding->available())return;
    bool ok=false;const auto name=QInputDialog::getText(this,tr("Save Reverb preset"),tr("Preset name"),QLineEdit::Normal,{},&ok).trimmed().left(64);
    if(!ok || name.isEmpty())return;
    const auto state=m_controller->reverbSnapshot(m_binding->channel(),m_binding->slot());if(!state)return;
    auto saved=userPresets();auto it=std::find_if(saved.begin(),saved.end(),[&](const auto& p){return p.name==name;});
    if(it==saved.end())saved.push_back({name,state->state.values});else it->values=state->state.values;
    QVariantList list;for(const auto& p:saved){QVariantList values;for(double v:p.values)values.append(v);list.append(QVariantMap{{"name",p.name},{"values",values}});}
    QSettings().setValue("reverb/userPresets.v1",list);
    m_controller->setInsertPresetReference(m_binding->channel(),m_binding->slot(),"user",name.toStdString());emit projectEdited();refresh();
}
void ReverbPanel::applyTheme() {
    const auto surface=well();
    setStyleSheet(QStringLiteral(
        "#ReverbPanel {color:%1;} #ReverbPanel QLabel {color:%2;font-size:11px;} #ReverbSection {font-size:9px;font-weight:600;letter-spacing:1.8px;padding-left:12px;}"
        "#ReverbPanel QToolButton,#ReverbPanel QPushButton {color:%1;background:%3;border:1px solid %4;border-radius:6px;padding:3px 10px;}"
        "#ReverbPanel QToolButton:hover,#ReverbPanel QPushButton:hover {background:%5;}"
        "#ReverbPanel QToolButton:checked,#ReverbPanel QPushButton:checked {background:%6;border-color:%7;color:%1;}"
        "#ReverbPanel QAbstractButton:focus {border-color:%7;}"
        "#ReverbFreeze:checked {color:%7;border-color:%7;background:%6;}"
        "#ReverbPanel QDoubleSpinBox[reverbValue=\"true\"],#ReverbPanel QDoubleSpinBox QLineEdit {color:%1;background:transparent;border:none;padding:0;selection-background-color:%7;selection-color:%3;}"
    ).arg(reverbUi::ink().name(),reverbUi::muted().name(),surface.name(),mixColors(canvas(),reverbUi::muted(),.22).name(),
          mixColors(surface,violet(),.12).name(),mixColors(surface,violet(),.17).name(),violet().name()));
    for(auto* child:findChildren<QWidget*>())child->update();
    update();
}
