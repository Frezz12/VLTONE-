#include "ReverbWidgets.hpp"
#include "Theme.hpp"
#include <QAccessibleWidget>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace rv = daw::plugins::reverb;
namespace reverbUi {
QColor background() { return th().dark ? QColor("#15171c") : QColor("#e8e9ee"); }
QColor surface() { return th().dark ? QColor("#1b1e25") : QColor("#f4f4f7"); }
QColor ink() { return th().dark ? QColor("#e8e8ed") : QColor("#292d3b"); }
QColor muted() { return th().dark ? QColor("#9295a5") : QColor("#656a7d"); }
QColor violet() { return th().dark ? QColor("#b5a1ed") : QColor("#7756b2"); }
QColor turquoise() { return th().dark ? QColor("#8ccecc") : QColor("#337f88"); }
QColor opacity(QColor c, int a) { c.setAlpha(a); return c; }
QFont font(const QFont& base,int pixels,bool medium) { auto f=base;f.setPixelSize(pixels);f.setWeight(medium?QFont::DemiBold:QFont::Normal);return f; }
void spaceGlyph(QPainter& p,const QRectF& bounds,unsigned mode,const QColor& color) {
    p.save();p.translate(bounds.center());const double s=std::min(bounds.width(),bounds.height())/32;p.scale(s,s);
    p.setPen(QPen(color,1.1));p.setBrush(Qt::NoBrush);
    if(mode==0) {
        for(int i=0;i<3;++i){const double w=13-i*3.8;QPainterPath path;path.moveTo(-w,12);path.lineTo(-w,-1);path.cubicTo(-w,-16,w,-16,w,-1);path.lineTo(w,12);p.drawPath(path);}
        p.drawLine(QPointF(-15,12),QPointF(15,12));
    } else if(mode==1 || mode==2) {
        const QPolygonF front{{-12,-5},{3,-12},{13,-6},{-2,1},{-12,-5},{-12,9},{-2,15},{13,7},{13,-6}};
        p.drawPolyline(front);p.drawLine(QPointF(-2,1),QPointF(-2,15));
        if(mode==2){p.drawLine(QPointF(3,-12),QPointF(3,3));p.drawLine(QPointF(3,3),QPointF(-12,9));p.drawLine(QPointF(3,3),QPointF(13,7));}
    } else if(mode==3) {
        for(int i=0;i<3;++i){p.save();p.translate(0,i*4-4);p.rotate(-22);p.drawEllipse(QRectF(-15,-6,30,12));p.restore();}
    } else if(mode==4) {
        for(int i=0;i<4;++i){p.setPen(QPen(opacity(color,220-i*40),1));p.drawArc(QRectF(-4-i*4,-4-i*4,8+i*8,8+i*8),25*16,130*16);p.drawArc(QRectF(-4-i*4,-4-i*4,8+i*8,8+i*8),205*16,130*16);}
        p.setBrush(color);p.setPen(Qt::NoPen);p.drawEllipse(QPointF(0,0),2,2);
    } else {
        for(const auto pt:{QPointF(-7,5),QPointF(7,-7),QPointF(9,10)}){const double r=pt.x()<0?9:5;QPainterPath star;star.moveTo(pt+QPointF(0,-r));star.quadTo(pt,pt+QPointF(r,0));star.quadTo(pt,pt+QPointF(0,r));star.quadTo(pt,pt+QPointF(-r,0));star.quadTo(pt,pt+QPointF(0,-r));p.drawPath(star);}
    }
    p.restore();
}
}
using namespace reverbUi;

ReverbValue::ReverbValue(QWidget* parent):QDoubleSpinBox(parent) {
    setButtonSymbols(NoButtons);setFrame(false);setAlignment(Qt::AlignRight);setKeyboardTracking(false);
    setProperty("reverbValue",true);setMinimumWidth(62);setFixedHeight(30);setFont(reverbUi::font(font(),13,true));
    connect(this,&QDoubleSpinBox::valueChanged,this,[this]{update();});
    connect(&ThemeManager::instance(),&ThemeManager::changed,this,[this]{update();});
}
void ReverbValue::paintScene(QPainter& p,const QRegion&) {
    p.setRenderHint(QPainter::Antialiasing);const bool focus=hasFocus()||lineEdit()->hasFocus();
    const QRectF r=QRectF(rect()).adjusted(.5,.5,-.5,-.5);
    p.setPen(QPen(opacity(focus?violet():muted(),focus?220:45),1));p.setBrush(opacity(ink(),th().dark?6:12));p.drawRoundedRect(r,5,5);
    const double f=(value()-minimum())/std::max(.001,maximum()-minimum());
    p.setPen(QPen(opacity(violet(),focus?200:90),1));p.drawLine(QPointF(6,height()-2),QPointF(6+(width()-12)*f,height()-2));
}
void ReverbValue::paintEvent(QPaintEvent*) {QPainter p(this);paintScene(p,QRegion(rect()));}
void ReverbValue::resizeEvent(QResizeEvent* e){QDoubleSpinBox::resizeEvent(e);lineEdit()->setGeometry(rect().adjusted(7,3,-7,-4));}
bool ReverbValue::event(QEvent* e){if(e->type()==QEvent::FocusIn||e->type()==QEvent::FocusOut||e->type()==QEvent::Enter||e->type()==QEvent::Leave)update();return QDoubleSpinBox::event(e);}

ReverbModeButton::ReverbModeButton(unsigned mode,QWidget* parent):QAbstractButton(parent),m_mode(mode) {
    setCheckable(true);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);setMinimumSize(138,45);setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Expanding);
    setText(QCoreApplication::translate("ReverbPanel",rv::modeName(mode).data()));setAccessibleName(text());
}
void ReverbModeButton::paintScene(QPainter& p,const QRegion&) {
    p.setRenderHint(QPainter::Antialiasing);const QRectF r=QRectF(rect()).adjusted(.5,.5,-.5,-.5);const bool on=isChecked();
    if(on||underMouse()||hasFocus()){
        QLinearGradient g(r.topLeft(),r.topRight());g.setColorAt(0,mixColors(surface(),violet(),on?.17:.06));g.setColorAt(1,surface());
        p.setBrush(g);p.setPen(QPen(opacity(on?violet():muted(),on?85:40),1));p.drawRoundedRect(r,7,7);
    }
    if(on){p.setPen(QPen(violet(),2,Qt::SolidLine,Qt::RoundCap));p.drawLine(QPointF(1,height()*.3),QPointF(1,height()*.7));}
    spaceGlyph(p,QRectF(12,(height()-30)/2.,30,30),m_mode,on?violet():muted());
    p.setFont(reverbUi::font(font(),12,on));p.setPen(on?ink():muted());p.drawText(QRectF(54,0,width()-63,height()),Qt::AlignVCenter|Qt::AlignLeft,text());
    if(hasFocus()){p.setBrush(Qt::NoBrush);p.setPen(QPen(violet(),1,Qt::DashLine));p.drawRoundedRect(r.adjusted(3,3,-3,-3),4,4);}
}
void ReverbModeButton::paintEvent(QPaintEvent*){QPainter p(this);paintScene(p,QRegion(rect()));}
void ReverbModeButton::enterEvent(QEnterEvent* e){update();QAbstractButton::enterEvent(e);}
void ReverbModeButton::leaveEvent(QEvent* e){update();QAbstractButton::leaveEvent(e);}

ReverbPresetButton::ReverbPresetButton(QWidget* parent):QToolButton(parent) {setFixedHeight(44);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);}
void ReverbPresetButton::paintScene(QPainter& p,const QRegion&) {
    p.setRenderHint(QPainter::Antialiasing);const QRectF r=QRectF(rect()).adjusted(.5,.5,-.5,-.5);
    p.setPen(QPen(opacity(hasFocus()?violet():muted(),hasFocus()?180:45),1));p.setBrush(surface());p.drawRoundedRect(r,6,6);
    p.setFont(reverbUi::font(font(),9,true));p.setPen(muted());p.drawText(QRectF(12,5,width()-38,13),Qt::AlignLeft,QCoreApplication::translate("ReverbPanel","PRESET"));
    p.setFont(reverbUi::font(font(),13,true));p.setPen(ink());p.drawText(QRectF(12,20,width()-38,18),Qt::AlignLeft,fontMetrics().elidedText(text(),Qt::ElideRight,width()-38));
    p.setPen(QPen(muted(),1.2,Qt::SolidLine,Qt::RoundCap));p.drawPolyline(QPolygonF{{width()-19.,20},{width()-15.,24},{width()-11.,20}});
}
void ReverbPresetButton::paintEvent(QPaintEvent*){QPainter p(this);paintScene(p,QRegion(rect()));}

namespace {
class SliderAccessibility final:public QAccessibleWidget,public QAccessibleValueInterface {
public:
    explicit SliderAccessibility(ReverbSlider* w):QAccessibleWidget(w,QAccessible::Slider){}
    ReverbSlider* slider() const{return static_cast<ReverbSlider*>(widget());}
    void* interface_cast(QAccessible::InterfaceType t) override {return t==QAccessible::ValueInterface?static_cast<QAccessibleValueInterface*>(this):QAccessibleWidget::interface_cast(t);}
    QVariant currentValue() const override{return slider()->plainValue();}
    void setCurrentValue(const QVariant& v) override {slider()->editPlainValue(v.toDouble());slider()->finishEditing();}
    QVariant maximumValue() const override{return slider()->parameter().maxValue;}
    QVariant minimumValue() const override{return slider()->parameter().minValue;}
    QVariant minimumStepSize() const override{return (slider()->parameter().maxValue-slider()->parameter().minValue)/1000.;}
    QString text(QAccessible::Text t) const override{return t==QAccessible::Value?slider()->displayText():QAccessibleWidget::text(t);}
};
}
ReverbSlider::ReverbSlider(const daw::plugins::ParameterInfo& info,QWidget* parent):QSlider(Qt::Horizontal,parent),m_info(info),m_number(new ReverbValue(this)) {
    static const bool installed=[] {QAccessible::installFactory([](const QString&,QObject* object)->QAccessibleInterface*{if(auto* slider=qobject_cast<ReverbSlider*>(object))return new SliderAccessibility(slider);return nullptr;});return true;}();(void)installed;
    setRange(0,10000);setFocusPolicy(Qt::StrongFocus);setMinimumSize(150,92);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);setMouseTracking(true);setCursor(Qt::SizeHorCursor);
    setAccessibleName(QCoreApplication::translate("ReverbPanel",info.name.c_str()));setObjectName(QStringLiteral("ReverbSlider.")+QString::fromStdString(info.id));
    setToolTip(tr("Drag to shape the parameter. Shift for fine adjustment. Double-click resets; Enter edits the value."));
    m_logarithmic=info.id=="decay"||info.id=="lowCut"||info.id=="highCut";
    m_number->setObjectName(QStringLiteral("ReverbNumber.")+QString::fromStdString(info.id));m_number->setAccessibleName(accessibleName());
    m_number->setRange(info.minValue,info.maxValue);m_number->setDecimals(info.id=="decay"?2:info.unit=="Hz"?0:1);
    m_number->setSuffix(" "+QString::fromStdString(info.unit));m_number->setSingleStep(info.id=="decay"?.05:1);
    setPlainValue(info.defaultValue);
    connect(m_number,&QDoubleSpinBox::valueChanged,this,[this](double v){if(!m_syncing)commit(v);});
    connect(m_number,&QDoubleSpinBox::editingFinished,this,&ReverbSlider::finishEditing);
    connect(this,&QSlider::valueChanged,this,[this](int v){if(!m_syncing){commit(fromFraction(v/10000.));if(!m_dragging)finishEditing();}});
}
double ReverbSlider::fraction(double v) const {return m_logarithmic?std::log(v/m_info.minValue)/std::log(m_info.maxValue/m_info.minValue):(v-m_info.minValue)/(m_info.maxValue-m_info.minValue);}
double ReverbSlider::fromFraction(double f) const {f=std::clamp(f,0.,1.);return m_logarithmic?m_info.minValue*std::pow(m_info.maxValue/m_info.minValue,f):std::lerp(m_info.minValue,m_info.maxValue,f);}
void ReverbSlider::setPlainValue(double v) {
    if(isEditing())return;v=std::clamp(v,m_info.minValue,m_info.maxValue);if(m_plain==v)return;m_plain=v;m_syncing=true;
    setValue(int(std::lround(fraction(m_plain)*10000)));m_number->setValue(m_plain);m_syncing=false;update();
}
double ReverbSlider::plainValue() const{return m_plain;}
QString ReverbSlider::displayText() const{return m_number->text();}
bool ReverbSlider::isEditing() const{return m_dragging||m_number->hasFocus();}
void ReverbSlider::commit(double v) {
    if(!isEnabled())return;v=std::clamp(v,m_info.minValue,m_info.maxValue);if(std::abs(v-m_plain)<1.e-9)return;
    m_plain=v;m_dirty=true;m_syncing=true;setValue(int(std::lround(fraction(v)*10000)));m_number->setValue(v);m_syncing=false;
    emit plainValueChanged(v);QAccessibleValueChangeEvent accessible(this,v);QAccessible::updateAccessibility(&accessible);update();
}
void ReverbSlider::editPlainValue(double v){commit(v);}
void ReverbSlider::finishEditing(){m_dragging=false;setSliderDown(false);if(m_dirty){m_dirty=false;emit editFinished();}update();}
void ReverbSlider::setDuckGain(double v){if(std::abs(v-m_duckGain)>.001){m_duckGain=v;update();}}
QRectF ReverbSlider::trackRect() const{return QRectF(14,height()-18,width()-28,3);}
QPointF ReverbSlider::handlePoint() const{const auto r=trackRect();return {r.left()+r.width()*fraction(m_plain),r.center().y()};}
void ReverbSlider::dragTo(QPointF pos,Qt::KeyboardModifiers mods){m_dragFraction=std::clamp(m_dragFraction+(pos.x()-m_lastPointer.x())/trackRect().width()*(mods.testFlag(Qt::ShiftModifier)?.1:1.),0.,1.);m_lastPointer=pos;commit(fromFraction(m_dragFraction));}
void ReverbSlider::mousePressEvent(QMouseEvent* e){if(e->button()!=Qt::LeftButton)return;setFocus(Qt::MouseFocusReason);m_dragging=true;setSliderDown(true);m_lastPointer=e->position();m_dragFraction=fraction(m_plain);if(std::abs(e->position().x()-handlePoint().x())>12&&!e->modifiers().testFlag(Qt::ShiftModifier)){m_dragFraction=std::clamp((e->position().x()-trackRect().left())/trackRect().width(),0.,1.);commit(fromFraction(m_dragFraction));}e->accept();update();}
void ReverbSlider::mouseMoveEvent(QMouseEvent* e){if(m_dragging){dragTo(e->position(),e->modifiers());e->accept();}}
void ReverbSlider::mouseReleaseEvent(QMouseEvent* e){if(m_dragging&&e->button()==Qt::LeftButton){dragTo(e->position(),e->modifiers());finishEditing();e->accept();}}
void ReverbSlider::mouseDoubleClickEvent(QMouseEvent* e){if(e->button()==Qt::LeftButton){commit(m_info.defaultValue);finishEditing();e->accept();}}
void ReverbSlider::keyPressEvent(QKeyEvent* e){
    if(e->key()==Qt::Key_Return||e->key()==Qt::Key_Enter||e->key()==Qt::Key_F2){m_number->setFocus();m_number->selectAll();e->accept();return;}
    double v=m_plain;const double step=e->modifiers().testFlag(Qt::ShiftModifier)?.001:.01;
    switch(e->key()){case Qt::Key_Left:case Qt::Key_Down:v=fromFraction(fraction(v)-step);break;case Qt::Key_Right:case Qt::Key_Up:v=fromFraction(fraction(v)+step);break;case Qt::Key_Home:v=m_info.defaultValue;break;case Qt::Key_End:v=m_info.maxValue;break;default:QSlider::keyPressEvent(e);return;}
    commit(v);finishEditing();e->accept();
}
void ReverbSlider::contextMenuEvent(QContextMenuEvent* e){finishEditing();QMenu menu(this);const auto* reset=menu.addAction(tr("Reset"));const auto* automate=menu.addAction(tr("Create automation"));const auto* choice=menu.exec(e->globalPos());if(choice==reset){commit(m_info.defaultValue);finishEditing();}else if(choice==automate)emit automationRequested(QString::fromStdString(m_info.id));}
void ReverbSlider::enterEvent(QEnterEvent* e){update();QSlider::enterEvent(e);}
void ReverbSlider::leaveEvent(QEvent* e){update();QSlider::leaveEvent(e);}
bool ReverbSlider::event(QEvent* e){if(e->type()==QEvent::Hide||e->type()==QEvent::WindowDeactivate||e->type()==QEvent::UngrabMouse||e->type()==QEvent::FocusOut)finishEditing();return QSlider::event(e);}
void ReverbSlider::resizeEvent(QResizeEvent* e){QSlider::resizeEvent(e);m_number->setGeometry(width()-100,32,86,28);}
void ReverbSlider::paintEvent(QPaintEvent*){QPainter p(this);paintScene(p,QRegion(rect()));}
void ReverbSlider::paintScene(QPainter& p,const QRegion&) {
    p.setRenderHint(QPainter::Antialiasing);if(!isEnabled())p.setOpacity(.35);
    const QRectF r=QRectF(rect()).adjusted(.5,.5,-.5,-.5);const bool active=m_dragging||hasFocus()||m_number->hasFocus();
    const bool cyan=m_info.id=="preDelay"||m_info.id=="width"||m_info.id=="highCut";
    const auto accent=cyan?turquoise():violet();
    QLinearGradient panel(r.topLeft(),r.bottomLeft());panel.setColorAt(0,mixColors(surface(),accent,active?.07:.025));panel.setColorAt(1,background());
    p.setBrush(panel);p.setPen(QPen(opacity(active?accent:muted(),active?155:34),1));p.drawRoundedRect(r,8,8);
    p.setFont(reverbUi::font(font(),11,true));p.setPen(ink());
    const QRectF caption(14,8,width()-28,18);p.drawText(caption,Qt::AlignVCenter|Qt::AlignLeft,p.fontMetrics().elidedText(accessibleName(),Qt::ElideRight,int(caption.width())));
    const QRectF diagram(14,34,width()-128,height()-64);const double f=fraction(m_plain);const auto id=m_info.id;
    p.save();p.setClipRect(diagram.adjusted(-1,-3,1,3));
    const double y=diagram.center().y(),w=diagram.width(),h=diagram.height();
    if(id=="width"||id=="mix"||id=="earlyLate") {
        const double spread=id=="width"?(.05+f*.4):.14;const double radius=std::min(h*.44,13.);
        for(int side:{-1,1}){const QPointF c(diagram.center().x()+side*w*spread,y);p.setPen(QPen(opacity(side<0?turquoise():violet(),170),1.3));p.setBrush(opacity(side<0?turquoise():violet(),id!="width"?int((side<0?1-f:f)*75):20));p.drawEllipse(c,radius*1.4,radius);}
        p.setPen(QPen(opacity(accent,70),1,Qt::DashLine));p.drawLine(diagram.left(),y,diagram.right(),y);
    } else {
        for(int layer=0;layer<(id=="motion"?3:1);++layer){QPainterPath line,fill;fill.moveTo(diagram.bottomLeft());
            for(int i=0;i<=100;++i){const double x=i/100.;double z=0;
                if(id=="bloom")z=std::pow(x,.3+f*3)*std::exp(-x*1.8)*2.4;
                else if(id=="motion")z=.5+f*.44*std::sin(x*std::numbers::pi*(2+f*3)+layer*1.5);
                else if(id=="duck")z=.7-.63*f*std::exp(-std::pow((x-.37)/.16,4));
                else if(id=="preDelay")z=x<.1+f*.55?0:std::exp(-(x-.1-f*.55)*5)*(.65+.25*std::cos(x*95));
                else if(id=="diffusion")z=(.5+.3*std::sin(x*(25+f*110)))*std::exp(-x*.8);
                else if(id=="lowCut"||id=="highCut"){const double cut=id=="lowCut"?x:1-x;z=1/(1+std::exp(-(cut-f*.7-.1)*18));}
                else if(id=="shimmerAmount")z=.5+.3*std::sin(x*40)*(1-f)+.3*f*std::sin(x*80);
                else z=std::exp(-x*(6-4.5*f));
                const QPointF pt(diagram.left()+x*w,diagram.bottom()-std::clamp(z,0.,1.)*h);if(i==0)line.moveTo(pt);else line.lineTo(pt);fill.lineTo(pt);
            }
            fill.lineTo(diagram.bottomRight());fill.closeSubpath();if(layer==0){QLinearGradient g(diagram.topLeft(),diagram.bottomLeft());g.setColorAt(0,opacity(accent,48));g.setColorAt(1,opacity(accent,0));p.fillPath(fill,g);}
            p.setPen(QPen(opacity(layer==1?turquoise():accent,180-layer*45),1.1));p.setBrush(Qt::NoBrush);p.drawPath(line);
        }
        if(id=="duck"){p.setPen(QPen(turquoise(),2));p.drawLine(QPointF(diagram.right()-3,diagram.bottom()),QPointF(diagram.right()-3,diagram.bottom()-h*m_duckGain));}
    }
    p.restore();const auto track=trackRect();const auto handle=handlePoint();
    p.setPen(QPen(opacity(muted(),45),2,Qt::SolidLine,Qt::RoundCap));p.drawLine(track.topLeft(),track.topRight());
    p.setPen(QPen(accent,2,Qt::SolidLine,Qt::RoundCap));p.drawLine(track.topLeft(),QPointF(handle.x(),track.top()));
    for(int i=0;i<=10;++i){const double x=track.left()+track.width()*i/10.;p.setPen(QPen(opacity(muted(),i%5?40:100),1));p.drawLine(QPointF(x,track.top()+7),QPointF(x,track.top()+(i%5?9:11)));}
    p.setBrush(background());p.setPen(QPen(accent,1.5));p.drawRoundedRect(QRectF(handle.x()-4,track.top()-5,8,10),2,2);
    p.setPen(QPen(ink(),1));p.drawLine(QPointF(handle.x(),track.top()-2),QPointF(handle.x(),track.top()+2));
}
