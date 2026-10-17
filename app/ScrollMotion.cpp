#include "ScrollMotion.hpp"
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QEvent>
#include <QScrollBar>
#include <QSettings>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace ui {
namespace {
constexpr auto motionName = "vltScrollMotion";
constexpr auto zoomMotionName = "vltZoomMotion";
ScrollMotion* motion(QWidget* owner, bool zoom = false) {
    return owner ? owner->findChild<ScrollMotion*>(QLatin1String(zoom ? zoomMotionName : motionName),Qt::FindDirectChildrenOnly) : nullptr;
}
class ScrollFilter final : public QObject {
public:
    explicit ScrollFilter(QObject* parent) : QObject(parent) {}
    bool eventFilter(QObject* object, QEvent* event) override {
        auto* widget=qobject_cast<QWidget*>(object);
        if (!widget) return false;
        if (event->type()==QEvent::Show) {
            if (auto* view=qobject_cast<QAbstractItemView*>(widget)) {
                view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
                view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
            }
        }
        if (event->type()==QEvent::MouseButtonPress || event->type()==QEvent::KeyPress)
            for(auto* parent=widget;parent;parent=parent->parentWidget()) {
                ScrollMotion::cancel(parent);
                ScrollMotion::cancelZoom(parent);
            }
        if (event->type()!=QEvent::Wheel) return false;
        auto* wheel=static_cast<QWheelEvent*>(event);
        if (wheel->modifiers()&(Qt::ControlModifier|Qt::AltModifier)) {
            for(auto* parent=widget;parent;parent=parent->parentWidget())ScrollMotion::cancel(parent);
            return false;
        }
        QAbstractScrollArea* area=qobject_cast<QAbstractScrollArea*>(widget);
        if (!area) {
            auto* parent=qobject_cast<QAbstractScrollArea*>(widget->parentWidget());
            if (parent && parent->viewport()==widget) area=parent;
        }
        if (!area) {
            if(auto* bar=qobject_cast<QScrollBar*>(widget))
                for(auto* parent=bar->parentWidget();parent;parent=parent->parentWidget())
                    if((area=qobject_cast<QAbstractScrollArea*>(parent))) break;
        }
        if (!area) return false;
        auto* x=area->horizontalScrollBar();
        auto* y=area->verticalScrollBar();
        const bool precise=!wheel->pixelDelta().isNull() || wheel->phase()==Qt::ScrollMomentum;
        QPointF delta;
        if (!wheel->pixelDelta().isNull()) delta=-QPointF(wheel->pixelDelta());
        else delta={-wheel->angleDelta().x()/120.0*QApplication::wheelScrollLines()*x->singleStep(),
                    -wheel->angleDelta().y()/120.0*QApplication::wheelScrollLines()*y->singleStep()};
        // Device racks opt in; other scroll areas keep their usual wheel axis.
        if((wheel->modifiers()&Qt::ShiftModifier) || area->property("horizontalWheelScroll").toBool()) {
            if(delta.x()==0) delta.setX(delta.y());
            delta.setY(0);
        }
        if(auto* bar=qobject_cast<QScrollBar*>(widget)) {
            if(bar->orientation()==Qt::Horizontal) {
                if(delta.x()==0) delta.setX(delta.y());
                delta.setY(0);
            } else delta.setX(0);
        }
        if(delta.isNull()) return false;
        // Preserve wheel propagation when this panel cannot move on either axis.
        const auto canMove=[](QScrollBar* bar,double by) {
            return by<0 ? bar->value()>bar->minimum() : by>0 && bar->value()<bar->maximum();
        };
        if(!canMove(x,delta.x())&&!canMove(y,delta.y())) {
            ScrollMotion::cancel(area);return false;
        }
        ScrollMotion::scroll(area,delta,precise,
            [x,y]{return QPointF(x->value(),y->value());},
            [x,y](QPointF p){x->setValue(int(std::lround(p.x())));y->setValue(int(std::lround(p.y())));});
        wheel->accept();
        return true;
    }
};
}
ScrollPreferences::ScrollPreferences() : QObject(qApp) {
    QSettings settings;
    m_enabled=settings.value("ui/smoothScroll",true).toBool();
    m_reduced=settings.value("ui/reduceMotion",false).toBool();
    m_speed=std::clamp(settings.value("ui/scrollSpeed",100).toInt(),50,200);
    m_strength=std::clamp(settings.value("ui/scrollSmoothing",65).toInt(),0,100);
}
ScrollPreferences& ScrollPreferences::instance() {
    static auto* prefs=new ScrollPreferences;
    return *prefs;
}
void ScrollPreferences::setEnabled(bool value) {
    if(m_enabled==value)return;
    m_enabled=value;QSettings().setValue("ui/smoothScroll",value);emit changed();
}
void ScrollPreferences::setReducedMotion(bool value) {
    if(m_reduced==value)return;
    m_reduced=value;QSettings().setValue("ui/reduceMotion",value);emit changed();
}
void ScrollPreferences::setSpeed(int value) {
    value=std::clamp(value,50,200);if(m_speed==value)return;
    m_speed=value;QSettings().setValue("ui/scrollSpeed",value);emit changed();
}
void ScrollPreferences::setStrength(int value) {
    value=std::clamp(value,0,100);if(m_strength==value)return;
    m_strength=value;QSettings().setValue("ui/scrollSmoothing",value);emit changed();
}
void ScrollMotion::install() {
    static const auto* filter=[] {
        auto* filter=new ScrollFilter(qApp);qApp->installEventFilter(filter);return filter;
    }();
    Q_UNUSED(filter);
}
ScrollMotion::ScrollMotion(QWidget* owner, Read read, Write write, bool logarithmic)
    : QObject(owner),m_read(std::move(read)),m_write(std::move(write)),m_frames(owner,this),
      m_easing(QEasingCurve::BezierSpline),m_logarithmic(logarithmic) {
    setObjectName(QLatin1String(logarithmic ? zoomMotionName : motionName));
    // The shared strong ease-out: a short, non-bouncing deceleration.
    m_easing.addCubicBezierSegment({.23,1.},{.32,1.},{1.,1.});
    owner->installEventFilter(this);
    connect(&m_frames,&FrameTimer::timeout,this,&ScrollMotion::advance);
    connect(&ScrollPreferences::instance(),&ScrollPreferences::changed,this,[this] {
        if(!ScrollPreferences::instance().effectiveEnabled()) stop();
    });
}
void ScrollMotion::scroll(QWidget* owner,QPointF delta,bool precise,Read read,Write write) {
    if(!owner || delta.isNull()) return;
    cancelZoom(owner);
    auto* animation=motion(owner);
    if(!animation) animation=new ScrollMotion(owner,std::move(read),std::move(write));
    else {animation->m_read=std::move(read);animation->m_write=std::move(write);}
    if(animation->m_pan) animation->stop();
    animation->move(delta,precise);
}
void ScrollMotion::pan(QWidget* owner,QPointF delta,Read read,Write write,Constrain constrain) {
    if(!owner || delta.isNull()) return;
    cancelZoom(owner);
    auto* animation=motion(owner);
    if(!animation) animation=new ScrollMotion(owner,std::move(read),std::move(write));
    else {animation->m_read=std::move(read);animation->m_write=std::move(write);}
    if(!animation->m_pan) animation->stop();
    animation->movePan(delta,constrain);
}
void ScrollMotion::finishPan(QWidget* owner) {
    if(auto* animation=motion(owner); animation && animation->m_pan) {
        // The release owns the final pointer position. There is no inertial
        // tail, and a stalled frame clock cannot shorten a drag.
        if(animation->m_read()==animation->m_actual) animation->apply(animation->m_target);
        animation->stop();
    }
}
void ScrollMotion::cancel(QWidget* owner) {if(auto* animation=motion(owner))animation->stop();}
void ScrollMotion::cancelZoom(QWidget* owner) {if(auto* animation=motion(owner,true))animation->stop();}
bool ScrollMotion::isZooming(QWidget* owner) {
    const auto* animation=motion(owner,true);
    return animation && animation->m_frames.isActive();
}
void ScrollMotion::zoom(QWidget* owner,QPointF factors,bool precise,Read read,Write write) {
    if(!owner || !std::isfinite(factors.x()) || !std::isfinite(factors.y()) ||
       factors.x()<=0 || factors.y()<=0 || factors==QPointF(1,1)) return;
    const auto scale=read();
    if(!std::isfinite(scale.x()) || !std::isfinite(scale.y()) || scale.x()<=0 || scale.y()<=0) return;
    cancel(owner);
    auto logRead=[read=std::move(read)] {
        const auto value=read();return QPointF(std::log(value.x()),std::log(value.y()));
    };
    auto logWrite=[write=std::move(write)](QPointF value) {
        write({std::exp(value.x()),std::exp(value.y())});
    };
    auto* animation=motion(owner,true);
    if(!animation) animation=new ScrollMotion(owner,std::move(logRead),std::move(logWrite),true);
    else {animation->m_read=std::move(logRead);animation->m_write=std::move(logWrite);}
    animation->move({std::log(factors.x()),std::log(factors.y())},precise);
}
void ScrollMotion::stop() {
    m_frames.stop();
    m_pan=false;
    if(m_initialized)m_position=m_target=m_actual=m_read();
}
bool ScrollMotion::eventFilter(QObject*,QEvent* event) {
    if(event->type()==QEvent::Hide || event->type()==QEvent::Resize ||
       event->type()==QEvent::MouseButtonPress || event->type()==QEvent::KeyPress ||
       event->type()==QEvent::WindowDeactivate || event->type()==QEvent::UngrabMouse) stop();
    return false;
}
void ScrollMotion::apply(QPointF position) {
    m_write(position);m_actual=m_read();m_position=position;
    // Setter rejection means a real boundary, rather than integer rounding.
    const double tolerance=m_logarithmic ? 1e-10 : 1.;
    if(std::abs(m_actual.x()-position.x())>tolerance){m_position.setX(m_actual.x());m_target.setX(m_actual.x());}
    if(std::abs(m_actual.y()-position.y())>tolerance){m_position.setY(m_actual.y());m_target.setY(m_actual.y());}
}
void ScrollMotion::move(QPointF delta,bool precise) {
    const auto actual=m_read();
    if(!m_initialized || actual!=m_actual) {
        m_frames.stop();
        m_position=m_target=m_actual=actual;m_initialized=true;
    }
    auto& preferences=ScrollPreferences::instance();
    if(precise || !preferences.effectiveEnabled()) {
        m_frames.stop();m_target=m_position+delta;
        apply(m_target);return;
    }
    // Input may arrive between late frames. Sample the old trajectory now,
    // rather than repeatedly retargeting from its last presented position.
    auto position=m_position;
    if(m_frames.isActive())
        position=m_from+(m_target-m_from)*m_easing.valueForProgress(progress());
    // Reverse from the displayed position: catching up the old direction
    // here would make the viewport jump against the user's new input.
    if((m_target.x()-m_position.x())*delta.x()<0) {
        m_target.setX(m_position.x());position.setX(m_position.x());
    }
    if((m_target.y()-m_position.y())*delta.y()<0) {
        m_target.setY(m_position.y());position.setY(m_position.y());
    }
    m_target+=delta;
    // The owner clamps its actual range. Graphics canvases legitimately
    // scroll through negative scene coordinates on both axes.
    const double direct=std::max(.15,1.-preferences.strength()/100.);
    m_duration=.15*100./preferences.speed();
    m_motionClock.start();
    apply(position+delta*direct);
    m_from=m_position;
    m_frames.start();
}
void ScrollMotion::movePan(QPointF delta,const Constrain& constrain) {
    const auto actual=m_read();
    if(!m_initialized || actual!=m_actual) {
        m_frames.stop();
        m_position=m_target=m_actual=actual;m_initialized=true;
    }
    m_pan=true;
    auto position=m_position;
    if(m_frames.isActive())
        position=m_from+(m_target-m_from)*m_easing.valueForProgress(progress());
    m_target=constrain(m_target+delta);
    auto& preferences=ScrollPreferences::instance();
    if(!preferences.effectiveEnabled()) {
        m_frames.stop();apply(m_target);return;
    }
    // Most movement follows the hand immediately; interpolate only the small
    // remainder between input samples. Never discard it on a reversal.
    const double direct=1.-.5*preferences.strength()/100.;
    m_duration=.028;
    apply(position+(m_target-position)*direct);
    m_from=m_position;
    m_motionClock.start();
    m_frames.start();
}
double ScrollMotion::progress() const {
    return std::min(1.,double(m_motionClock.nsecsElapsed())/1e9/m_duration);
}
void ScrollMotion::advance() {
    if(m_read()!=m_actual) {stop();return;}
    const double elapsed=progress();
    const double fraction=m_easing.valueForProgress(elapsed);
    apply(m_from+(m_target-m_from)*fraction);
    if(elapsed>=1.)m_frames.stop();
}
} // namespace ui
