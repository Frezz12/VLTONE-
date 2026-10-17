#include "Vlt1073Panel.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include <QAccessible>
#include <QAccessibleWidget>
#include <QAbstractButton>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QHideEvent>
#include <QInputDialog>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>
#include <numbers>

namespace eq = daw::plugins::vlt1073;
namespace {
constexpr double kWidth = 252, kHeight = 594;
const QColor ink("#282a28"), marking("#d3d5ce");
QPointF polar(QPointF c,double r,double degrees) {
    const double a = degrees * std::numbers::pi / 180;
    return c + QPointF(r * std::sin(a),-r * std::cos(a));
}
double micAngle(double gain) {
    // The switch has an OFF detent between the 50 and 55 dB ranges.
    return 165+(gain-20)*3+(gain>=55?15:0);
}
QRectF frequencyLabel(unsigned parameter,unsigned index) {
    const double y=parameter==7?277:parameter==9?374:470;
    // Keep side labels outside the gain-dot arc. Bottom frequencies get their
    // own baseline below the gain zero; painted text and click areas agree.
    if(index==0)return {104,y-6,22,12};
    if(index==1)return {102,y+16,24,12};
    if(index==2)return {76,y+34,26,12};
    if(index==3)return {50,y+43,26,12};
    if(index==4)return {24,y+34,26,12};
    if(index==5)return {1,y+16,22,12};
    return {1,y-6,22,12};
}
void label(QPainter& p,QRectF r,const QString& text,double px,const QColor& color,bool bold = false) {
    QFont f=QApplication::font(); f.setPixelSize(int(std::round(px))); f.setWeight(bold ? QFont::DemiBold : QFont::Normal);
    p.save(); p.setFont(f); p.setPen(color); p.drawText(r,Qt::AlignCenter,text); p.restore();
}
double faderY(double db) {
    constexpr std::array<double,9> values{10,5,0,-5,-10,-20,-30,-40,-90};
    constexpr std::array<double,9> positions{145,177,209,241,275,310,343,375,412};
    for (unsigned i=1;i<values.size();++i) if (db>=values[i]) return std::lerp(positions[i-1],positions[i],(values[i-1]-db)/(values[i-1]-values[i]));
    return positions.back();
}
double faderDb(double y) {
    y=std::clamp(y,145.,412.);
    double lo=-90,hi=10;
    for(int i=0;i<16;++i) { double mid=(lo+hi)*.5; if(faderY(mid)>y)lo=mid;else hi=mid; }
    return y>=411.5 ? -90 : (lo+hi)*.5;
}
QString valueText(unsigned i,double v) {
    static const eq::Vlt1073Instance formatter;
    return QString::fromStdString(formatter.parameterText(i,v)).replace("-inf",QString::fromUtf8("−∞"));
}
void screw(QPainter& p,QPointF at,double radius=6) {
    p.setPen(Qt::NoPen); p.setBrush(QColor(0,0,0,120)); p.drawEllipse(at+QPointF(.6,1.4),radius+.9,radius+.9);
    QRadialGradient g(at-QPointF(2,2),radius*2); g.setColorAt(0,QColor("#55564e"));g.setColorAt(.45,QColor("#2e322f"));g.setColorAt(1,QColor("#10191e"));
    p.setPen(QPen(QColor("#62635b"),.45));p.setBrush(g);p.drawEllipse(at,radius,radius);
    p.setPen(QPen(QColor("#101517"),2.1));p.drawLine(at-QPointF(1.7,-1.4),at+QPointF(1.7,-1.4));
}
void dots(QPainter& p,QPointF c,double radius,int count,double first,double last) {
    p.setPen(Qt::NoPen);p.setBrush(marking);
    for(int i=0;i<count;++i)p.drawEllipse(polar(c,radius,std::lerp(first,last,double(i)/(count-1))),1.35,1.35);
}
void knob(QPainter& p,QPointF c,double radius,QColor color,double angle,bool ring=false) {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(5,10,14,110));p.drawEllipse(c+QPointF(.6,1.2),radius+.6,radius+.6);
    QLinearGradient collar(c-QPointF(radius,radius),c+QPointF(radius,radius));
    collar.setColorAt(0,QColor("#929792"));collar.setColorAt(.26,QColor("#727a7f"));collar.setColorAt(.6,QColor("#424c56"));collar.setColorAt(1,QColor("#343c42"));
    p.setBrush(collar);p.setPen(QPen(QColor("#17222a"),.8));p.drawEllipse(c,radius,radius);
    if(ring) {
        QLinearGradient cream(c-QPointF(radius,radius),c+QPointF(radius,radius));cream.setColorAt(0,QColor("#d2d1c2"));cream.setColorAt(.5,QColor("#bfbeaf"));cream.setColorAt(1,QColor("#9b9e92"));
        p.setBrush(cream);p.setPen(QPen(QColor("#d3d4c7"),.5));p.drawEllipse(c,radius-1,radius-1);
        p.setPen(QPen(QColor("#72766c"),.75));
        for(int i=0;i<40;++i)p.drawLine(polar(c,radius-4,i*9),polar(c,radius-2,i*9));
        p.setBrush(QColor("#343b42"));p.setPen(Qt::NoPen);p.drawEllipse(c,radius-8,radius-8);
        p.setPen(QPen(QColor("#272e34"),2.2,Qt::SolidLine,Qt::FlatCap));p.drawLine(polar(c,radius-7,angle),polar(c,radius-.8,angle));
        return;
    }
    const double r=radius==25?22:radius-7;
    QPainterPath flutes;
    for(int i=0;i<96;++i) {const auto pt=polar(c,r+(i%4<2?1.2:0),i*3.75);if(i==0)flutes.moveTo(pt);else flutes.lineTo(pt);}flutes.closeSubpath();
    QLinearGradient edge(c-QPointF(r,r),c+QPointF(r,r));edge.setColorAt(0,color.lighter(124));edge.setColorAt(.4,color);edge.setColorAt(1,color.darker(140));
    p.setBrush(edge);p.setPen(QPen(color.darker(155),.6));p.drawPath(flutes);
    QLinearGradient cap(c-QPointF(7,r),c+QPointF(7,r));cap.setColorAt(0,color.lighter(111));cap.setColorAt(1,color.darker(109));
    p.setBrush(cap);p.setPen(QPen(color.lighter(135),.7));p.drawEllipse(c,r-3,r-3);
    p.setPen(QPen(QColor("#d8d9d2"),1.8,Qt::SolidLine,Qt::FlatCap));p.drawLine(polar(c,r-3,angle),polar(c,r+3,angle));
}
// Two opposed moulded tabs and a round cap, on a separate machined collar.
// Rotate only the outline; the light source stays above and left of the panel.
void rotarySwitch(QPainter& p,QPointF c,double angle,bool blue) {
    constexpr double radius=32.5,capRadius=22;
    p.save();
    p.setPen(Qt::NoPen);p.setBrush(QColor(6,12,18,125));p.drawEllipse(c+QPointF(.6,1.1),radius+.7,radius+.7);
    QLinearGradient metal(c-QPointF(24,29),c+QPointF(26,30));
    metal.setColorAt(0,QColor("#858d90"));metal.setColorAt(.18,QColor("#67727b"));metal.setColorAt(.48,QColor("#47535d"));metal.setColorAt(.72,QColor("#505b64"));metal.setColorAt(1,QColor("#303c45"));
    p.setPen(QPen(QColor("#26333d"),.65));p.setBrush(metal);p.drawEllipse(c,radius,radius);
    p.setBrush(Qt::NoBrush);
    for(int i=0;i<12;++i){
        p.setPen(QPen(i%3==0?QColor(212,220,222,20):QColor(10,20,27,22),.32));
        p.drawEllipse(c,radius-1.2-i*.7,radius-1.2-i*.7);
    }
    p.setPen(QPen(QColor(205,214,215,90),.75));p.drawArc(QRectF(c-QPointF(31.4,31.4),QSizeF(62.8,62.8)),36*16,112*16);
    p.setPen(QPen(QColor(12,22,32,135),.85));p.drawArc(QRectF(c-QPointF(31.5,31.5),QSizeF(63,63)),205*16,120*16);

    QPainterPath shape;shape.addEllipse(QPointF(),capRadius,capRadius);
    QPainterPath tabs;tabs.addRoundedRect(QRectF(-4.8,-33,9.6,66),.7,.7);shape=shape.united(tabs);
    QTransform orientation;orientation.translate(c.x(),c.y());orientation.rotate(angle);
    shape=orientation.map(shape);
    p.setBrush(QColor(0,3,8,130));p.setPen(Qt::NoPen);p.drawPath(QTransform::fromTranslate(.55,1).map(shape));
    const QColor body=blue?QColor("#172d47"):QColor("#590e14");
    QLinearGradient moulding(c-QPointF(15,24),c+QPointF(20,25));
    moulding.setColorAt(0,body.lighter(145));moulding.setColorAt(.17,body.lighter(114));moulding.setColorAt(.7,body);moulding.setColorAt(1,body.darker(145));
    p.setBrush(moulding);p.setPen(QPen(body.darker(155),.9));p.drawPath(shape);
    p.setBrush(Qt::NoBrush);p.setPen(QPen(blue?QColor("#34475c"):QColor("#792328"),.75));
    p.drawArc(QRectF(c-QPointF(21,21),QSizeF(42,42)),35*16,125*16);

    QLinearGradient face(c-QPointF(8,22),c+QPointF(9,24));
    face.setColorAt(0,blue?QColor("#1c334f"):QColor("#66151b"));face.setColorAt(.65,body);face.setColorAt(1,body.darker(108));
    p.setBrush(face);p.setPen(QPen(body.darker(160),.9));p.drawEllipse(c,19.7,19.7);
    p.setPen(QPen(blue?QColor(68,86,106,120):QColor(134,53,54,105),.55));p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(c-QPointF(19.3,19.3),QSizeF(38.6,38.6)),215*16,100*16);
    // The index is painted into the tab, not floating outside its edge.
    p.setPen(QPen(QColor("#dddcd3"),1.9,Qt::SolidLine,Qt::FlatCap));
    p.drawLine(polar(c,23.4,angle),polar(c,31.2,angle));
    p.restore();
}
void switchCap(QPainter& p,QPointF c,bool on) {
    QRectF slot(c.x()-12,c.y()-4.5,24,9);
    p.setPen(QPen(QColor("#62645c"),.7));p.setBrush(QColor("#101511"));p.drawRoundedRect(slot,4,4);
    QRectF cap(c.x()+(on?-.5:-10),c.y()-4.5,11,9);
    QLinearGradient metal(cap.topLeft(),cap.bottomLeft());metal.setColorAt(0,QColor("#f3f0df"));metal.setColorAt(.35,QColor("#d4d5c9"));metal.setColorAt(.7,QColor("#9c9f94"));metal.setColorAt(1,QColor("#676d63"));
    p.setBrush(metal);p.setPen(QPen(QColor("#33392f"),.6));p.drawRoundedRect(cap,3,3);
}
}

// The face is painted once by its parent. These native widgets own focus,
// accessibility and hit testing, including disjoint concentric hit regions.
class Vlt1073Control final : public ui::Knob {
    Q_DECLARE_TR_FUNCTIONS(Vlt1073Control)
public:
    unsigned index;
    QRectF designRect;
    bool annulus=false;
    explicit Vlt1073Control(unsigned i,QWidget* parent):ui::Knob({},parent),index(i) {
        const auto& p=eq::parameterTable()[i];setRange(p.minValue,p.maxValue);setDefaultValue(p.defaultValue);setBare(60);setAutomatable(true);
        setMinimumSize(0,0);setMaximumSize(QWIDGETSIZE_MAX,QWIDGETSIZE_MAX);
        setFocusPolicy(Qt::StrongFocus);setAccessibleName(QString::fromStdString(p.name));setCursor(Qt::SizeVerCursor);
        setObjectName(QString::fromStdString(p.id));
        setAccessibleDescription(tr("Drag vertically; Shift for precision; Enter to enter a value; Escape to cancel. Right-click for automation."));
        connect(this,&ui::Knob::valueChanged,this,[this](double v){announce(v);parentWidget()->update();});
    }
    double step() const { return index==2||index==3?5:eq::parameterTable()[index].isStepped?1:.1; }
    double minimum() const {return eq::parameterTable()[index].minValue;}
    double maximum() const {return eq::parameterTable()[index].maxValue;}
    bool editing() const {return m_dragging;}
    void finishEditing() {bool was=m_dragging;m_dragging=false;if(m_readoutVisible){ui::ValueBubble::dismiss();m_readoutVisible=false;}if(QWidget::mouseGrabber()==this)releaseMouse();if(was)emit editFinished();}
    void edit(double v,bool finish=true) {
        if(!isEnabled()||!std::isfinite(v))return;
        v=std::clamp(v,minimum(),maximum());
        if(eq::parameterTable()[index].isStepped)v=std::round(v/step())*step();
        if(value()!=v){setValue(v);emit valueChanged(v);}if(finish)emit editFinished();
    }
    void refresh(double v) {if(!editing()&&v!=value()){setValue(v);announce(v);}setToolTip(accessibleName()+": "+valueText(index,value()));}
    void enterValue() {
        finishEditing();synchronize();QInputDialog dialog(this);dialog.setWindowTitle(accessibleName());dialog.setObjectName("Vlt1073ValueEditor");
        if(index==7||index==9||index==11) {
            QStringList values;for(int i=0;i<=int(maximum());++i)values<<valueText(index,i);
            dialog.setComboBoxItems(values);dialog.setTextValue(valueText(index,value()));dialog.setLabelText(tr("Frequency"));
            if(dialog.exec()==QDialog::Accepted)edit(values.indexOf(dialog.textValue()));
        } else {
            dialog.setInputMode(QInputDialog::DoubleInput);dialog.setDoubleRange(minimum(),maximum());dialog.setDoubleDecimals(eq::parameterTable()[index].isStepped?0:2);
            dialog.setDoubleStep(step());dialog.setDoubleValue(value());dialog.setLabelText(index==14?tr("Level in dB (−90 = −∞)"):QString::fromStdString(eq::parameterTable()[index].unit));
            if(dialog.exec()==QDialog::Accepted)edit(dialog.doubleValue());
        }
    }
protected:
    void paintEvent(QPaintEvent*) override {}
    void mousePressEvent(QMouseEvent* e) override {
        if(e->button()!=Qt::LeftButton)return;finishEditing();synchronize();setFocus(Qt::MouseFocusReason);m_dragging=true;m_start=value();m_accumulator=value();m_y=e->globalPosition().y();showValue();e->accept();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if(!m_dragging)return;
        const double delta=m_y-e->globalPosition().y();m_y=e->globalPosition().y();
        if(maximum()-minimum()==1)return;
        const double fine=e->modifiers().testFlag(Qt::ShiftModifier)?4:1;
        if(index==14)m_accumulator=faderDb(faderY(m_accumulator)-delta/fine/(height()/designRect.height()));
        else m_accumulator=std::clamp(m_accumulator+delta*(maximum()-minimum())/(160*fine),minimum(),maximum());
        edit(m_accumulator,false);showValue();e->accept();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if(e->button()!=Qt::LeftButton||!m_dragging)return;
        if(maximum()-minimum()==1)edit(1-value(),false);else mouseMoveEvent(e);
        finishEditing();e->accept();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override {finishEditing();if(ui::automationCreationMode())emit automateRequested();else edit(eq::parameterTable()[index].defaultValue);}
    void wheelEvent(QWheelEvent* e) override {e->ignore();}
    void keyPressEvent(QKeyEvent* e) override {
        if(!m_dragging)synchronize();
        switch(e->key()) {
        case Qt::Key_Return:case Qt::Key_Enter:enterValue();break;
        case Qt::Key_Escape:if(m_dragging){edit(m_start,false);finishEditing();}break;
        case Qt::Key_Space:if(maximum()-minimum()==1)edit(1-value());break;
        case Qt::Key_Up:case Qt::Key_Right:edit(value()+step()*(e->modifiers().testFlag(Qt::ShiftModifier)&&step()<1?.1:1));break;
        case Qt::Key_Down:case Qt::Key_Left:edit(value()-step()*(e->modifiers().testFlag(Qt::ShiftModifier)&&step()<1?.1:1));break;
        case Qt::Key_Home:edit(minimum());break;
        case Qt::Key_End:edit(maximum());break;
        default:ui::Knob::keyPressEvent(e);return;
        }if(e->key()!=Qt::Key_Escape&&e->key()!=Qt::Key_Return&&e->key()!=Qt::Key_Enter)showValue();e->accept();
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        finishEditing();QMenu menu(this);auto* numeric=menu.addAction(tr("Enter value…"));auto* automate=menu.addAction(tr("Create automation"));automate->setObjectName("Vlt1073Automate");auto* reset=menu.addAction(tr("Reset to default"));
        auto* selected=menu.exec(e->globalPos());if(selected==numeric)enterValue();else if(selected==automate)emit automateRequested();else if(selected==reset)edit(eq::parameterTable()[index].defaultValue);
    }
    bool event(QEvent* e) override {
        if(e->type()==QEvent::UngrabMouse||e->type()==QEvent::Hide||e->type()==QEvent::WindowDeactivate||e->type()==QEvent::FocusOut)finishEditing();
        if(e->type()==QEvent::FocusIn){const auto reason=static_cast<QFocusEvent*>(e)->reason();if(reason==Qt::TabFocusReason||reason==Qt::BacktabFocusReason||reason==Qt::ShortcutFocusReason)showValue();}
        return ui::Knob::event(e);
    }
private:
    // An undo can arrive between timer ticks. Start input from the current
    // processor value, never from the previous painted position.
    void synchronize(){refresh(static_cast<Vlt1073Panel*>(parentWidget())->read(index));}
    void announce(double v) {QAccessibleValueChangeEvent e(this,v);QAccessible::updateAccessibility(&e);setToolTip(accessibleName()+": "+valueText(index,v));if(m_readoutVisible)showValue();}
    void showValue() {
        // Keep the readout beside the moving cap, not above the full fader slot.
        const int y=index==14?int((faderY(value())-28-designRect.y())*height()/designRect.height()):0;
        ui::ValueBubble::showFor(this,{width()/2,y},valueText(index,value()));m_readoutVisible=true;
    }
    bool m_dragging=false;
    bool m_readoutVisible=false;
    double m_start=0,m_accumulator=0,m_y=0;
};
namespace {
class AccessibleControl final:public QAccessibleWidget,public QAccessibleValueInterface {
public:
    explicit AccessibleControl(Vlt1073Control* w):QAccessibleWidget(w,QAccessible::Slider){}
    Vlt1073Control* control()const{return static_cast<Vlt1073Control*>(object());}
    void* interface_cast(QAccessible::InterfaceType t)override{return t==QAccessible::ValueInterface?static_cast<QAccessibleValueInterface*>(this):QAccessibleWidget::interface_cast(t);}
    QString text(QAccessible::Text t)const override{return t==QAccessible::Value?valueText(control()->index,control()->value()):QAccessibleWidget::text(t);}
    QVariant currentValue()const override{return control()->value();}
    void setCurrentValue(const QVariant& v)override{bool ok=false;double d=v.toDouble(&ok);if(ok)control()->edit(d);}
    QVariant maximumValue()const override{return control()->maximum();}
    QVariant minimumValue()const override{return control()->minimum();}
    QVariant minimumStepSize()const override{return control()->step();}
};
QAccessibleInterface* accessibleFactory(const QString&,QObject* o){if(auto* c=dynamic_cast<Vlt1073Control*>(o))return new AccessibleControl(c);return nullptr;}
class FaceButton final:public QAbstractButton {
public:
    FaceButton(QString name,QWidget* parent):QAbstractButton(parent){setText(name);setAccessibleName(name);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);setToolTip(name);}
protected:
    void paintEvent(QPaintEvent*)override{}
};
}
Vlt1073Panel::Vlt1073Panel(daw::EngineController* controller,QString channel,QString insert,QWidget* parent)
    :QWidget(parent),m_controller(controller),m_channel(channel.toStdString()),m_insert(insert.toStdString()) {
    setObjectName("Vlt1073Panel");setAccessibleName(tr("VLT 1073 preamplifier and equalizer"));setMinimumSize(252,594);resize(sizeHint());setAttribute(Qt::WA_OpaquePaintEvent);
    static const bool registered=[] {QAccessible::installFactory(accessibleFactory);return true;}();(void)registered;
    const std::array<QRectF,16> bounds{{{87,27,32,20},{90,110,29,22},{32,51,62,62},{32,51,62,62},{172,7,36,23},{172,39,36,23},
        {36,153,54,54},{16,230,94,94},{41,255,44,44},{16,327,94,94},{41,352,44,44},{29,436,68,68},{22,529,41,24},{64,529,41,24},{165,109,49,341},{169,494,40,43}}};
    for(unsigned i=0;i<m_controls.size();++i){
        auto* c=new Vlt1073Control(i,this);m_controls[i]=c;c->designRect=bounds[i];c->annulus=i==7||i==9;
        c->setValue(read(i));
        connect(c,&ui::Knob::valueChanged,this,[this,i](double v){write(i,v);});
        connect(c,&ui::Knob::editFinished,this,[this,i]{finish(i);});
        connect(c,&ui::Knob::automateRequested,this,[this,i]{finishAll();emit automationRequested(QString::fromStdString(eq::parameterTable()[i].id));});
    }
    const auto addLabel=[this](QString text,QRectF rect,unsigned i,double value){
        auto* button=new FaceButton(text,this);button->setObjectName(QString("Vlt1073_%1_%2").arg(QString::fromStdString(eq::parameterTable()[i].id)).arg(value));
        connect(button,&QAbstractButton::clicked,this,[this,i,value]{m_controls[i]->edit(value);});m_labels.push_back({button,rect});
    };
    addLabel("MIC",{4,117,34,18},0,1);addLabel("LINE",{82,27,36,17},0,0);
    for(unsigned parameter:{7u,9u,11u})for(unsigned i=0;i<(parameter==7?7u:5u);++i)
        addLabel(valueText(parameter,i),frequencyLabel(parameter,i),parameter,i);
    m_power=new FaceButton(tr("Power"),this);m_power->setObjectName("Vlt1073Power");m_power->setCheckable(true);
    connect(m_power,&QAbstractButton::toggled,this,[this]{update();});
    connect(m_power,&QAbstractButton::clicked,this,[this](bool on){if(available()){m_controller->setInsertBypassed(m_channel,m_insert,!on);emit projectEdited();refresh();}});
    m_timer=new QTimer(this);m_timer->setObjectName("Vlt1073RefreshTimer");m_timer->setInterval(33);connect(m_timer,&QTimer::timeout,this,&Vlt1073Panel::refresh);
    layoutControls();refresh();
}
Vlt1073Panel::~Vlt1073Panel(){m_timer->stop();finishAll();}
bool Vlt1073Panel::available()const{return m_controller&&m_controller->hasInsert(m_channel,m_insert,eq::Vlt1073Instance::uid());}
double Vlt1073Panel::read(unsigned i)const{const auto& p=eq::parameterTable()[i];return available()?m_controller->insertParameter(m_channel,m_insert,p.id):p.defaultValue;}
void Vlt1073Panel::write(unsigned i,double v){if(!available())return;if(!m_gestures[i])m_gestures[i]=read(i);m_controller->setInsertParameter(m_channel,m_insert,eq::parameterTable()[i].id,v);refresh();update();}
void Vlt1073Panel::finish(unsigned i){if(!m_gestures[i])return;if(available()&&read(i)!=*m_gestures[i]){m_controller->commitInsertParameterEdit(m_channel,m_insert,eq::parameterTable()[i].id,*m_gestures[i],"Change VLT 1073");emit projectEdited();}m_gestures[i].reset();}
void Vlt1073Panel::finishAll(){for(auto* c:m_controls)if(c)c->finishEditing();for(unsigned i=0;i<m_controls.size();++i)finish(i);}
void Vlt1073Panel::refresh(){
    bool changed=false;const bool enabled=available();
    for(unsigned i=0;i<m_controls.size();++i){auto* c=m_controls[i];const double v=read(i);changed|=c->value()!=v;c->refresh(v);c->setEnabled(enabled);}
    const bool mic=read(0)>.5;m_controls[2]->setVisible(mic);m_controls[3]->setVisible(!mic);
    // The physical switches retain their positions in LINE; the DSP applies
    // MIC Z and PAD only to the microphone input.
    for(const auto& l:m_labels)l.widget->setEnabled(enabled);
    const auto* model=enabled?m_controller->insertModel(m_channel,m_insert):nullptr;const bool on=model&&!model->bypassed;
    changed|=m_power->isChecked()!=on;m_power->setChecked(on);m_power->setEnabled(enabled);if(changed)update();
}
QRectF Vlt1073Panel::faceRect()const{const double scale=std::min(width()/kWidth,height()/kHeight);return {(width()-kWidth*scale)/2,(height()-kHeight*scale)/2,kWidth*scale,kHeight*scale};}
void Vlt1073Panel::layoutControls(){
    const auto r=faceRect();const double s=r.width()/kWidth;
    const auto geometry=[&](QWidget* w,QRectF b){w->setGeometry(QRectF(r.topLeft()+b.topLeft()*s,b.size()*s).toAlignedRect());};
    for(auto* c:m_controls){geometry(c,c->designRect);if(c->annulus){QRegion outer(c->rect(),QRegion::Ellipse);const int inset=int(c->width()*.265);QRegion inner(c->rect().adjusted(inset,inset,-inset,-inset),QRegion::Ellipse);c->setMask(outer-inner);}}
    for(const auto& l:m_labels)geometry(l.widget,l.rect);geometry(m_power,{171,557,37,25});
}
void Vlt1073Panel::resizeEvent(QResizeEvent* e){QWidget::resizeEvent(e);m_face={};layoutControls();}
void Vlt1073Panel::showEvent(QShowEvent* e){QWidget::showEvent(e);refresh();m_timer->start();}
void Vlt1073Panel::hideEvent(QHideEvent* e){m_timer->stop();finishAll();QWidget::hideEvent(e);}
void Vlt1073Panel::changeEvent(QEvent* e){if(e->type()==QEvent::FontChange||e->type()==QEvent::PaletteChange||e->type()==QEvent::DevicePixelRatioChange){m_face={};update();}QWidget::changeEvent(e);}
void Vlt1073Panel::cacheFace(){
    const double dpr=devicePixelRatioF();if(!m_face.isNull()&&m_face.size()==size()*dpr&&m_face.devicePixelRatio()==dpr)return;
    m_face=QPixmap(size()*dpr);m_face.setDevicePixelRatio(dpr);m_face.fill(QColor("#181d22"));QPainter p(&m_face);p.setRenderHint(QPainter::Antialiasing);
    const auto rect=faceRect();p.translate(rect.topLeft());p.scale(rect.width()/kWidth,rect.height()/kHeight);
    QLinearGradient navy(0,0,115,594);navy.setColorAt(0,QColor("#303b45"));navy.setColorAt(.55,QColor("#2c3842"));navy.setColorAt(1,QColor("#28343e"));p.fillRect(QRectF(0,0,127,594),navy);
    QLinearGradient silver(128,0,252,594);silver.setColorAt(0,QColor("#c6c7c2"));silver.setColorAt(.48,QColor("#bfc1bb"));silver.setColorAt(1,QColor("#b4b8b2"));p.fillRect(QRectF(128,0,124,594),silver);
    // Fine enamel grain and aluminium brushing. Cache with the face, so the
    // surface never crawls or changes with a control repaint.
    QImage grain(504,1188,QImage::Format_ARGB32_Premultiplied);
    std::uint32_t noise=1073;
    for(int y=0;y<grain.height();++y){
        auto* row=reinterpret_cast<QRgb*>(grain.scanLine(y));
        for(int x=0;x<grain.width();++x){
            noise=1664525*noise+1013904223;
            const bool light=noise&0x10000;
            const int alpha=int(noise>>29)+(x<254?3:1);
            row[x]=light?qRgba(alpha,alpha,alpha,alpha):qRgba(0,0,0,alpha);
        }
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform);p.drawImage(QRectF(0,0,252,594),grain);
    for(int i=0;i<1000;++i){
        noise=1664525*noise+1013904223;const double y=(noise%11880)*.05;
        noise=1664525*noise+1013904223;const double x=129+(noise%11600)*.01;
        const double length=2+(noise>>24)*.085;
        p.setPen(QPen(i%3?QColor(44,51,49,10):QColor(245,246,239,20),.18));
        p.drawLine(QPointF(x,y),QPointF(std::min(250.,x+length),y));
    }
    p.setPen(QPen(QColor(226,230,228,60),.6));p.drawLine(QPointF(129,1.8),QPointF(250,1.8));p.drawLine(QPointF(1.8,2),QPointF(1.8,592));
    p.setPen(QPen(QColor("#090f12"),1.5));p.drawLine(127,0,127,594);p.drawRect(QRectF(.8,.8,250.4,592.4));
    p.setPen(QPen(QColor("#565951"),1));p.drawLine(128,76,251,76);p.drawLine(128,484,251,484);
    for(auto c:{QPointF(20,13),QPointF(105,13),QPointF(20,580),QPointF(105,580),QPointF(189,87),QPointF(189,473)})screw(p,c,c.x()==189?6:5.5);
    p.setBrush(Qt::NoBrush);p.setPen(QPen(marking,1.1));p.drawEllipse(QPointF(21,36),9,9);label(p,{12,30,18,12},"VLT",6.6,marking,true);
    label(p,{47,16,32,15},"dB",9,marking);label(p,{85,29,31,14},"LINE",8.5,marking);label(p,{5,118,29,16},"MIC.",8.5,marking);
    for(int gain=20;gain<=80;gain+=5){const double a=micAngle(gain);const auto c=polar({63,82},42,a);if(gain%10==0)label(p,{c.x()-11,c.y()-6,22,12},gain==20||gain==80?QString("−%1").arg(gain):QString::number(gain),8.5,marking);p.setPen(Qt::NoPen);p.setBrush(marking);p.drawEllipse(polar({63,82},34.5,a),1.6,1.6);}
    for(int gain=-10;gain<=20;gain+=5){double a=105-gain*3;auto c=polar({63,82},43,a);if(gain%10==0)label(p,{c.x()-13,c.y()-6,26,12},gain==0?"0":QString("%1%2").arg(gain>0?"−":"+").arg(std::abs(gain)),8.5,marking);p.setBrush(marking);p.setPen(Qt::NoPen);p.drawEllipse(polar({63,82},34.5,a),1.6,1.6);}
    label(p,{88,113,30,12},"OFF",8,marking);label(p,{2,75,25,14},"OFF",8,marking);
    for(double cy:{180.,277.,374.}){dots(p,{63,cy},39,23,222,498);label(p,{20,cy-33,13,10},"+",10,marking);label(p,{93,cy-33,13,10},"−",10,marking);label(p,{58,cy+32,10,10},"0",7,marking);}
    const std::array<QString,7> midLabels{"OFF","0·36","0·7","1·6","3·2","4·8","7·2"};
    for(unsigned i=0;i<7;++i)label(p,frequencyLabel(7,i),midLabels[i],8,marking);
    for(unsigned i=0;i<5;++i)label(p,frequencyLabel(9,i),i?QString::number(eq::kLowHz[i],'f',0):"OFF",8,marking);
    for(unsigned i=0;i<5;++i)label(p,frequencyLabel(11,i),i?QString::number(eq::kHighPassHz[i],'f',0):"OFF",8,marking);
    label(p,{2,310,21,14},"kHz",8.5,marking);label(p,{3,407,19,14},"Hz",8.5,marking);label(p,{3,503,19,14},"Hz",8.5,marking);
    // Original engraved filter-response symbols.
    p.setPen(QPen(marking,1));for(double y:{216.,313.,410.,507.}){p.drawLine(QPointF(102,y),QPointF(111,y));p.drawLine(QPointF(111,y),QPointF(115,y-3));p.drawLine(QPointF(115,y-3),QPointF(120,y-3));}
    label(p,{157,10,18,13},"LO",8.5,ink);label(p,{205,10,18,13},"HI",8.5,ink);label(p,{160,26,59,14},"MIC Z",8.7,ink);
    label(p,{156,43,18,13},"0",8.5,ink);label(p,{205,43,22,13},"−20",8.5,ink);label(p,{172,58,34,14},"PAD",9,ink);
    p.setPen(QPen(QColor("#393c34"),1));p.drawLine(164,143,164,412);p.drawLine(214,143,214,412);p.drawLine(160,143,164,143);p.drawLine(214,143,218,143);p.drawLine(160,412,164,412);p.drawLine(214,412,218,412);
    for(double db:{10.,5.,0.,-5.,-10.,-20.,-30.,-40.,-90.}){double y=faderY(db);const QString s=db==-90?QString::fromUtf8("∞"):QString::number(std::abs(db),'f',0);label(p,{143,y-8,18,16},s,11,ink);label(p,{218,y-8,18,16},s,11,ink);}
    p.setPen(QPen(QColor("#707166"),1));p.setBrush(QColor("#111710"));p.drawRoundedRect(QRectF(185.7,111,6.6,339),3,3);
    for(double y:{101.,458.}){QRadialGradient g({188,y-1},5);g.setColorAt(0,QColor("#c2b56b"));g.setColorAt(.5,QColor("#8b8040"));g.setColorAt(1,QColor("#484b2b"));p.setBrush(g);p.setPen(QPen(QColor("#a4a37a"),.5));p.drawEllipse(QPointF(189,y),3.7,4.5);}
    label(p,{171,484,36,15},"0",9,ink);label(p,{164,531,21,15},"−24",8.5,ink);label(p,{191,531,38,15},"12 dB",8.5,ink);label(p,{160,544,59,14},"OUTPUT",8.5,ink);
    label(p,{152,564,24,14},"OFF",8.5,ink);label(p,{205,564,22,14},"ON",8.5,ink);label(p,{160,577,59,13},"POWER",8.3,ink);
    label(p,{5,556,56,17},"VLT 1073",8,marking);
    QRadialGradient plug({60,576},17);plug.setColorAt(0,QColor("#e0e1d6"));plug.setColorAt(.6,QColor("#adb6b4"));plug.setColorAt(1,QColor("#555f65"));p.setBrush(plug);p.setPen(QPen(QColor("#101c25"),1.3));p.drawEllipse(QPointF(63,580),10,10);
}
void Vlt1073Panel::paintEvent(QPaintEvent*){
    cacheFace();QPainter p(this);p.drawPixmap(0,0,m_face);p.setRenderHint(QPainter::Antialiasing);const auto r=faceRect();p.translate(r.topLeft());p.scale(r.width()/kWidth,r.height()/kHeight);
    const auto v=[this](unsigned i){return m_controls[i]->value();};
    const double inputAngle=v(1)<.5?150:v(0)>.5?micAngle(v(2)):105-v(3)*3;
    rotarySwitch(p,{63,82},inputAngle,false);
    knob(p,{63,180},33,QColor("#7f858a"),180+v(6)*7.7);
    knob(p,{63,277},33,QColor(),90+v(7)*30,true);knob(p,{63,277},25,QColor("#818893"),180+v(8)*6.9);
    knob(p,{63,374},33,QColor(),90+v(9)*30,true);knob(p,{63,374},25,QColor("#818893"),180+v(10)*7.7);
    rotarySwitch(p,{63,470},90+v(11)*30,true);
    for(unsigned i=0;i<2;++i){QRectF b(22+i*42,530,39,22);QLinearGradient g(b.topLeft(),b.bottomLeft());bool on=v(12+i)>.5;g.setColorAt(0,QColor(on?"#ebe4c4":"#c8c5b0"));g.setColorAt(1,QColor(on?"#babba0":"#8d9487"));p.setBrush(g);p.setPen(QPen(QColor("#111d25"),1.4));p.drawRect(b);label(p,b,i?"PHASE":"EQL",9,ink);}
    switchCap(p,{189,18},v(4)>.5);switchCap(p,{189,51},v(5)>.5);switchCap(p,{189,569},m_power->isChecked());
    const double fy=faderY(v(14));const QRectF cap(170,fy-28,38,56);p.setPen(Qt::NoPen);p.setBrush(QColor(0,0,0,125));p.drawRoundedRect(cap.translated(.7,1.6),1,1);
    QLinearGradient g(cap.topLeft(),cap.bottomLeft());g.setColorAt(0,QColor("#141817"));g.setColorAt(.08,QColor("#2a302d"));g.setColorAt(.4,QColor("#252b29"));g.setColorAt(.76,QColor("#292f2d"));g.setColorAt(1,QColor("#111715"));p.setBrush(g);p.setPen(QPen(QColor("#0b100d"),.7));p.drawRoundedRect(cap,1,1);
    for(int y=5;y<53;y+=4){p.setPen(QPen(QColor("#4c534e"),.55));p.drawLine(QPointF(171.5,cap.y()+y),QPointF(206.5,cap.y()+y));p.setPen(QPen(QColor("#131916"),.75));p.drawLine(QPointF(171.5,cap.y()+y+1),QPointF(206.5,cap.y()+y+1));}
    knob(p,{189,517},19,QColor("#222622"),v(15)<0?v(15)/24*135:v(15)/12*135);
}
