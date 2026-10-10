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
ScrollMotion* motion(QWidget* owner) {
    return owner ? owner->findChild<ScrollMotion*>(QLatin1String(motionName),Qt::FindDirectChildrenOnly) : nullptr;
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
            for(auto* parent=widget;parent;parent=parent->parentWidget())
                ScrollMotion::cancel(parent);
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
        if(wheel->modifiers()&Qt::ShiftModifier) {
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
ScrollMotion::ScrollMotion(QWidget* owner, Read read, Write write)
    : QObject(owner),m_read(std::move(read)),m_write(std::move(write)),m_frames(owner,this),
      m_easing(QEasingCurve::BezierSpline) {
    setObjectName(QLatin1String(motionName));
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
    auto* animation=motion(owner);
    if(!animation) animation=new ScrollMotion(owner,std::move(read),std::move(write));
    else {animation->m_read=std::move(read);animation->m_write=std::move(write);}
    animation->move(delta,precise);
}
void ScrollMotion::cancel(QWidget* owner) {if(auto* animation=motion(owner))animation->stop();}
void ScrollMotion::stop() {
    m_frames.stop();
    if(m_initialized)m_position=m_target=m_actual=m_read();
}
bool ScrollMotion::eventFilter(QObject*,QEvent* event) {
    if(event->type()==QEvent::Hide || event->type()==QEvent::Resize ||
       event->type()==QEvent::MouseButtonPress || event->type()==QEvent::KeyPress ||
       event->type()==QEvent::WindowDeactivate) stop();
    return false;
}
void ScrollMotion::apply(QPointF position) {
    m_write(position);m_actual=m_read();m_position=position;
    // Setter rejection means a real boundary, rather than integer rounding.
    if(std::abs(m_actual.x()-position.x())>1.){m_position.setX(m_actual.x());m_target.setX(m_actual.x());}
    if(std::abs(m_actual.y()-position.y())>1.){m_position.setY(m_actual.y());m_target.setY(m_actual.y());}
}
void ScrollMotion::move(QPointF delta,bool precise) {
    const auto actual=m_read();
    if(!m_initialized || actual!=m_actual) {
        m_position=m_target=m_actual=actual;m_initialized=true;
    }
    auto& preferences=ScrollPreferences::instance();
    if(precise || !preferences.effectiveEnabled()) {
        m_frames.stop();m_target=m_position+delta;
        apply(m_target);return;
    }
    // Reversals discard the old destination before applying the new direction.
    if((m_target.x()-m_position.x())*delta.x()<0)m_target.setX(m_position.x());
    if((m_target.y()-m_position.y())*delta.y()<0)m_target.setY(m_position.y());
    m_target+=delta;
    // The owner clamps its actual range. Graphics canvases legitimately
    // scroll through negative scene coordinates on both axes.
    const double direct=std::max(.15,1.-preferences.strength()/100.);
    apply(m_position+delta*direct);
    m_from=m_position;m_elapsed=0.;m_duration=.15*100./preferences.speed();
    m_frames.start();
}
void ScrollMotion::advance() {
    if(m_read()!=m_actual) {stop();return;}
    m_elapsed=std::min(m_duration,m_elapsed+m_frames.deltaSeconds());
    const double fraction=m_easing.valueForProgress(m_elapsed/m_duration);
    apply(m_from+(m_target-m_from)*fraction);
    if(m_elapsed>=m_duration)m_frames.stop();
}
} // namespace ui
