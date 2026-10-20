#include "AudioEditPanel.hpp"
#include "Controls.hpp"
#include "Theme.hpp"
#include "MediaWorker.hpp"
#include <QActionGroup>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QDir>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QResizeEvent>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <QFile>

using daw::AudioEditFrame;
namespace { std::optional<daw::AudioEditDocument> clipboard;
constexpr double rulerHeight=28;
constexpr double regionStripHeight=26;
// Open editors of the same content retain one immutable peak pyramid. Weak
// entries do not keep large waveforms alive after the last editor closes.
std::mutex peakCacheMutex;
std::unordered_map<std::string,std::weak_ptr<const AudioEditPeaks>> peakCache;
std::shared_ptr<const AudioEditPeaks> cachedPeaks(const daw::AudioEditDocument& doc,
    const daw::engine::SampleBuffer& audio,const daw::audioedit::Continue& keepGoing) {
    {
        std::lock_guard lock(peakCacheMutex);
        if(auto it=peakCache.find(doc.revision);it!=peakCache.end())
            if(auto peaks=it->second.lock())return peaks;
    }
    auto peaks=AudioEditPeaks::build(audio,keepGoing);
    if(peaks) {
        std::lock_guard lock(peakCacheMutex);
        std::erase_if(peakCache,[](const auto& item){return item.second.expired();});
        peakCache[doc.revision]=peaks;
    }
    return peaks;
}
// The editor shows the unprocessed edit document. Map transport time through
// the clip's trim, stretch/warp, loop and source reversal before drawing it.
double clipSourceFrame(const daw::ClipModel& clip,double time,double tempo,const daw::AudioEditDocument& doc){
    const double rate=doc.sampleRate,sourceLength=double(doc.frames)/rate;
    const double stretch=std::max(.001,clip.sampleEdit.stretchTime);
    const double duration=clip.durationSeconds>0?clip.durationSeconds:std::max(0.,sourceLength-clip.offsetSeconds)*stretch;
    double elapsed=time-clip.startSeconds;
    if(clip.muted||elapsed<0||elapsed>=duration)return -1;
    double source;
    if(clip.warp.enabled&&clip.warp.markers.size()>=2)source=daw::warpSourceAt(clip.warp,elapsed*tempo/60.);
    else{
        daw::engine::ClipPlacement timing;
        timing.tapeStartSamples=clip.fadeInMode==daw::ClipFadeMode::Tape?std::llround(clip.fadeInSeconds*rate):0;
        timing.tapeStopSamples=clip.fadeOutMode==daw::ClipFadeMode::Tape?std::llround(clip.fadeOutSeconds*rate):0;
        elapsed=timing.sourceProgressAt(std::llround(elapsed*rate),std::llround(duration*rate))/rate;
        source=clip.offsetSeconds+elapsed/stretch;
        const double end=std::min(sourceLength,clip.offsetSeconds+duration/stretch);
        const double begin=clip.offsetSeconds+(end-clip.offsetSeconds)*clip.sampleEdit.loopStart;
        const double loopEnd=clip.offsetSeconds+(end-clip.offsetSeconds)*clip.sampleEdit.loopEnd;
        const double length=loopEnd-begin;
        if(clip.sampleEdit.loopMode&&length>0){
            const double cycle=length*(clip.sampleEdit.loopMode==2?2:1);
            double phase=std::fmod(source-begin,cycle);if(phase<0)phase+=cycle;
            source=phase<=length?begin+phase:loopEnd-(phase-length);
        }
    }
    double frame=source*rate;if(clip.sampleEdit.reverse)frame=doc.frames-1-frame;
    return frame>=0&&frame<doc.frames?frame:-1;
}
}

std::shared_ptr<const AudioEditPeaks> AudioEditPeaks::build(const daw::engine::SampleBuffer& audio, const daw::audioedit::Continue& keepGoing) {
    auto out=std::make_shared<AudioEditPeaks>();out->channels.resize(audio.channels());
    for(unsigned ch=0;ch<audio.channels();++ch){
        auto& levels=out->channels[ch];auto& fine=levels.emplace_back();
        for(std::uint64_t i=0;i<audio.frames();i+=64){
            if((i&16383)==0 && keepGoing && !keepGoing())return {};
            Pair p{std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()};for(auto j=i;j<std::min<std::uint64_t>(i+64,audio.frames());++j){const float s=audio.channel(ch)[j];p.low=std::min(p.low,s);p.high=std::max(p.high,s);}fine.push_back(p);
        }
        while(levels.back().size()>1){Level next;const auto& prev=levels.back();
            for(std::size_t i=0;i<prev.size();i+=4){auto p=prev[i];for(auto j=i+1;j<std::min(i+4,prev.size());++j){p.low=std::min(p.low,prev[j].low);p.high=std::max(p.high,prev[j].high);}next.push_back(p);}levels.push_back(std::move(next));}
    }
    return out;
}
AudioEditCanvas::AudioEditCanvas(QWidget* parent):ui::FrameWidget(parent){
    setObjectName("AudioEditCanvas");setFocusPolicy(Qt::StrongFocus);setMouseTracking(true);setMinimumSize(280,180);
    setAccessibleName(tr("Audio waveform editor"));
    setToolTip(tr("Drag the waveform to select. Drag a segment strip to move it. Double-click a segment to select it. Ctrl+wheel zooms at the pointer; middle-drag pans."));
    setAccessibleDescription(toolTip());
}
AudioEditFrame AudioEditCanvas::frame(double px)const{return std::max<AudioEditFrame>(0,std::llround(m_scroll+px*m_framesPerPixel));}
double AudioEditCanvas::x(AudioEditFrame f)const{return (double(f)-m_scroll)/m_framesPerPixel;}
void AudioEditCanvas::setAudio(const daw::AudioEditDocument& doc,std::shared_ptr<const daw::engine::SampleBuffer> audio,std::shared_ptr<const AudioEditPeaks> peaks){
    m_doc=doc;m_audio=std::move(audio);m_peaks=std::move(peaks);m_first=std::min(m_first,doc.frames);m_last=std::min(m_last,doc.frames);
    if(m_initial){m_initial=false;fit();}update();emit viewChanged();
}
void AudioEditCanvas::setSelection(AudioEditFrame first,AudioEditFrame last){m_first=std::clamp(first,AudioEditFrame(0),m_doc.frames);m_last=std::clamp(last,m_first,m_doc.frames);m_cursor=m_first;m_anchor=m_first;update();emit selectionChanged();}
void AudioEditCanvas::fit(bool selected){if(selected&&m_last>m_first){m_scroll=m_first;m_framesPerPixel=std::max(.125,double(m_last-m_first)/std::max(1,width()));}else{m_scroll=0;m_framesPerPixel=std::max(.125,double(m_doc.frames)/std::max(1,width()));}update();emit viewChanged();}
void AudioEditCanvas::showRange(AudioEditFrame first, AudioEditFrame last){m_scroll=std::max<AudioEditFrame>(0,first);m_framesPerPixel=std::max(.125,double(last-first)/std::max(1,width()));update();emit viewChanged();}
void AudioEditCanvas::zoom(double factor,double anchor){if(anchor<0)anchor=width()/2.;const double at=m_scroll+anchor*m_framesPerPixel;m_framesPerPixel=std::clamp(m_framesPerPixel/factor,.125,std::max(1.,double(m_doc.frames)/std::max(1,width())));m_scroll=std::max(0.,at-anchor*m_framesPerPixel);update();emit viewChanged();}
void AudioEditCanvas::setTool(Tool t){cancelGesture();m_tool=t;updatePointer(mapFromGlobal(QCursor::pos()));emit toolChanged(t);}
void AudioEditCanvas::setClipColor(const QColor& color){if(m_clipColor==color)return;m_clipColor=color;update();}
void AudioEditCanvas::setPlayhead(double at){
    if(m_playhead==at)return;
    const auto damage=[this](double f){const double px=(f-m_scroll)/m_framesPerPixel;return px < -8 || px > width()+8?QRect():QRect(int(px)-7,0,15,height());};
    QRegion changed;if(m_playhead>=0)changed+=damage(m_playhead);m_playhead=at;if(at>=0)changed+=damage(at);
    if(!changed.isEmpty())update(changed);
}
void AudioEditCanvas::setEditingEnabled(bool enabled){if(m_editingEnabled==enabled)return;m_editingEnabled=enabled;if(!enabled)cancelGesture();updatePointer(mapFromGlobal(QCursor::pos()));}
void AudioEditCanvas::setAmplitude(double a){m_amplitude=a;update();}
void AudioEditCanvas::scrollTo(double at){m_scroll=std::max(0.,at);update();emit viewChanged();}
void AudioEditCanvas::cancelGesture(){
    if(m_drag==1||m_drag==3){m_first=m_savedFirst;m_last=m_savedLast;m_cursor=m_savedCursor;emit selectionChanged();}
    if(m_drag==4||m_drag==7){m_scroll=m_savedScroll;emit viewChanged();}
    m_drag=0;m_region.clear();m_dragMoved=false;if(mouseGrabber()==this)releaseMouse();update();
}
const daw::AudioEditRegion* AudioEditCanvas::regionAt(AudioEditFrame at)const{
    const auto it=std::upper_bound(m_doc.regions.begin(),m_doc.regions.end(),at,[](auto f,const auto& r){return f<r.start;});
    if(it==m_doc.regions.begin())return nullptr;const auto& r=*std::prev(it);return at<r.start+r.length?&r:nullptr;
}
void AudioEditCanvas::updatePointer(const QPointF& point){
    setToolTip(accessibleDescription());
    if(m_drag==3||m_drag==7){setCursor(Qt::ClosedHandCursor);return;}
    if(!m_editingEnabled){setCursor(Qt::BusyCursor);return;}
    if(point.y()<rulerHeight){setCursor(Qt::IBeamCursor);return;}
    if(point.y()>=height()-regionStripHeight&&regionAt(frame(point.x()))){setCursor(Qt::OpenHandCursor);return;}
    if(m_tool==Tool::Select&&m_last>m_first&&(std::abs(point.x()-x(m_first))<12||std::abs(point.x()-x(m_last))<12)){
        if(point.y()<rulerHeight+16)setToolTip(tr("Drag the top selection handles to create fades. Alt-drag changes the curve."));
        setCursor(Qt::SizeHorCursor);return;
    }
    setCursor(m_tool==Tool::Move?Qt::OpenHandCursor:m_tool==Tool::Slip?Qt::SizeHorCursor:m_tool==Tool::Split?Qt::CrossCursor:Qt::IBeamCursor);
}
void AudioEditCanvas::hideEvent(QHideEvent* e){cancelGesture();ui::FrameWidget::hideEvent(e);}
void AudioEditCanvas::resizeEvent(QResizeEvent* e){
    // Keep the visible time range stable when the inspector opens or the window resizes.
    if(e->oldSize().width()>0&&width()>0)m_framesPerPixel*=double(e->oldSize().width())/width();
    ui::FrameWidget::resizeEvent(e);emit viewChanged();
}
void AudioEditCanvas::drawWave(QPainter& p,int ch,double center,double height){
    if(!m_audio||!m_peaks||ch>=int(m_peaks->channels.size()))return;
    const auto& levels=m_peaks->channels[ch];if(levels.empty())return;
    std::size_t level=0;double bucket=64;while(level+1<levels.size()&&bucket*4<=m_framesPerPixel){++level;bucket*=4;}
    QPainterPath line;
    for(int px=0;px<width();++px){
        const auto a=frame(px),b=std::max(a+1,frame(px+1));if(a>=AudioEditFrame(m_audio->frames()))break;
        float low=std::numeric_limits<float>::infinity(),high=-std::numeric_limits<float>::infinity();
        if(m_framesPerPixel<64){for(auto f=a;f<std::min<AudioEditFrame>(b,m_audio->frames());++f){float s=m_audio->channel(ch)[f];low=std::min(low,s);high=std::max(high,s);}}
        else{const auto& data=levels[level];for(std::size_t i=std::size_t(a/bucket);i<std::min(data.size(),std::size_t(std::ceil(b/bucket)));++i){low=std::min(low,data[i].low);high=std::max(high,data[i].high);}}
        const double y1=center-std::clamp(double(high)*m_amplitude,-1.,1.)*height,y2=center-std::clamp(double(low)*m_amplitude,-1.,1.)*height;
        if(m_framesPerPixel<1){if(px==0)line.moveTo(px,y1);else line.lineTo(px,y1);}else p.drawLine(QPointF(px,y1),QPointF(px,y2));
    }
    if(m_framesPerPixel<1)p.drawPath(line);
}
void AudioEditCanvas::paintEvent(QPaintEvent*){QPainter p(this);paintScene(p,QRegion(rect()));}
void AudioEditCanvas::paintScene(QPainter& p,const QRegion&){
    const auto& t=th();const QColor clip=m_clipColor.isValid()?m_clipColor:Theme::audioAccent();
    const QColor surface=mixColors(clip,t.surface,t.dark?.38:.18);
    const auto linear=[](double c){return c<=.04045?c/12.92:std::pow((c+.055)/1.055,2.4);};
    const double luminance=.2126*linear(surface.redF())+.7152*linear(surface.greenF())+.0722*linear(surface.blueF());
    const QColor ink=luminance>.179?QColor(16,23,30):QColor(Qt::white);
    const double bottom=height()-regionStripHeight;
    const QRectF outline=QRectF(rect()).adjusted(.5,.5,-.5,-.5);
    p.save();p.setRenderHint(QPainter::Antialiasing,true);
    QPainterPath clipping;clipping.addRoundedRect(outline,Theme::cornerRadius,Theme::cornerRadius);p.setClipPath(clipping,Qt::IntersectClip);
    p.fillRect(rect(),m_doc.initialized?surface:t.surface);
    p.fillRect(QRectF(0,0,width(),rulerHeight),t.surface);
    if(!m_doc.initialized){p.setPen(t.textSecondary);p.drawText(rect(),Qt::AlignCenter,tr("Load a sample to edit audio"));p.restore();return;}
    p.setFont(font());const double secondsPerPixel=m_framesPerPixel/m_doc.sampleRate;
    const double wanted=secondsPerPixel*90;const double decade=std::pow(10.,std::floor(std::log10(std::max(1e-8,wanted))));
    double step=decade;for(double multiplier:{1.,2.,5.,10.})if(decade*multiplier>=wanted){step=decade*multiplier;break;}
    const double start=m_scroll/m_doc.sampleRate,end=(m_scroll+width()*m_framesPerPixel)/m_doc.sampleRate;
    QColor grid=ink;grid.setAlpha(22);QColor centerInk=ink;centerInk.setAlpha(55);
    p.setRenderHint(QPainter::Antialiasing,false);
    for(double time=std::ceil(start/(step/4))*(step/4);time<end;time+=step/4){
        const double px=(time*m_doc.sampleRate-m_scroll)/m_framesPerPixel;
        p.setPen(grid);p.drawLine(QPointF(px,rulerHeight),QPointF(px,bottom));
    }
    for(double time=std::ceil(start/step)*step;time<end;time+=step){double px=(time*m_doc.sampleRate-m_scroll)/m_framesPerPixel;p.setPen(centerInk);p.drawLine(QPointF(px,rulerHeight),QPointF(px,bottom));p.setPen(t.textSecondary);p.drawText(QRectF(px+6,0,86,rulerHeight),Qt::AlignVCenter,tr("%1 s").arg(QString::number(time,'f',step<1?std::min(6,int(std::ceil(-std::log10(step)))):1)));}
    const int channels=std::min(2,m_doc.channels);const double channelHeight=(bottom-rulerHeight)/std::max(1,channels);
    const auto wave=[&](int ch){p.setPen(QPen(ink,1));drawWave(p,ch,rulerHeight+channelHeight*(ch+.5),channelHeight*.38);};
    for(int ch=0;ch<channels;++ch){const double center=rulerHeight+channelHeight*(ch+.5);p.setPen(centerInk);p.drawLine(QPointF(0,center),QPointF(width(),center));wave(ch);p.setPen(ink);p.drawText(QRectF(9,rulerHeight+ch*channelHeight+5,40,20),ch==0?(channels==1?tr("Mono"):"L"):"R");}
    if(m_drag==3&&m_dragMoved){
        const auto origin=m_pressFrame-m_anchor;
        const double delta=double(m_moveStart-origin)/m_framesPerPixel;
        const QRectF oldArea(x(origin),rulerHeight,double(m_moveLength)/m_framesPerPixel,bottom-rulerHeight);
        const QRectF movedArea=oldArea.translated(delta,0);
        p.fillRect(oldArea,mixColors(surface,t.surface,.5));p.fillRect(movedArea,surface);
        p.save();p.setClipRect(movedArea,Qt::IntersectClip);p.translate(delta,0);
        for(int ch=0;ch<channels;++ch)wave(ch);
        p.restore();
    }
    p.setRenderHint(QPainter::Antialiasing,true);
    p.fillRect(QRectF(0,bottom,width(),regionStripHeight),mixColors(surface,t.surface,.55));
    const auto visibleFirst=frame(0),visibleLast=frame(width());
    auto region=std::lower_bound(m_doc.regions.begin(),m_doc.regions.end(),visibleFirst,
        [](const auto& r,AudioEditFrame at){return r.start+r.length<at;});
    for(;region!=m_doc.regions.end()&&region->start<=visibleLast;++region){
        const auto& r=*region;const bool moving=m_drag==3&&r.id==m_region;
        const double left=x(moving?m_moveStart:r.start),right=left+double(r.length)/m_framesPerPixel;
        p.setPen(centerInk);p.drawLine(QPointF(left,rulerHeight),QPointF(left,bottom));
        const QRectF strip(std::max(1.,left+2),bottom+3,std::max(1.,std::min(double(width()-2),right-2)-std::max(1.,left+2)),regionStripHeight-6);
        p.setPen(QPen(moving?ink:centerInk,1));p.setBrush(mixColors(surface,ink,moving?.22:.09));p.drawRoundedRect(strip,4,4);
        if(strip.width()>38){
            p.setPen(QPen(ink,1.4,Qt::SolidLine,Qt::RoundCap));
            for(int dx:{0,4})for(int dy:{-3,2})p.drawPoint(QPointF(strip.left()+8+dx,strip.center().y()+dy));
            p.setPen(ink);p.drawText(strip.adjusted(23,0,-4,0),Qt::AlignVCenter,QString::number(std::distance(m_doc.regions.begin(),region)+1));
        }
    }
    if(m_last>m_first){QColor color=ink;color.setAlpha(28);p.fillRect(QRectF(x(m_first),rulerHeight,x(m_last)-x(m_first),bottom-rulerHeight),color);p.setPen(QPen(ink,1));for(auto f:{m_first,m_last})p.drawLine(QPointF(x(f),rulerHeight),QPointF(x(f),bottom));
        p.setBrush(ink);p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(x(m_first)+2,rulerHeight+2,10,10),3,3);
        p.drawRoundedRect(QRectF(x(m_last)-12,rulerHeight+2,10,10),3,3);
        if(m_drag==5||m_drag==6){
            const bool in=m_drag==5;QPainterPath curve;
            const double from=x(in?m_first:m_last-m_fadeFrames),length=double(m_fadeFrames)/m_framesPerPixel;
            for(int i=0;i<=64;++i){double t=double(i)/64;double amplitude=daw::engine::curve::shapeT(t,daw::engine::curve::Shape::Linear,m_fadeCurve);if(!in)amplitude=1-amplitude;
                const QPointF point(from+length*t,bottom-amplitude*(bottom-rulerHeight-16));if(i==0)curve.moveTo(point);else curve.lineTo(point);}
            p.setPen(QPen(ink,2));p.drawPath(curve);
        }
    }
    p.setPen(QPen(centerInk,1,Qt::DashLine));p.drawLine(QPointF(x(m_cursor),rulerHeight),QPointF(x(m_cursor),bottom));
    if(m_drag==3&&m_dragMoved){p.setPen(QPen(ink,1.5));p.setBrush(Qt::NoBrush);p.drawRect(QRectF(x(m_moveStart),rulerHeight,double(m_moveLength)/m_framesPerPixel,bottom-rulerHeight));}
    if(m_playhead>=0){
        const double px=(m_playhead-m_scroll)/m_framesPerPixel;
        p.setPen(QPen(QColor(0,0,0,110),3));p.drawLine(QPointF(px,rulerHeight),QPointF(px,bottom));
        p.setPen(QPen(Qt::white,1.5));p.drawLine(QPointF(px,rulerHeight),QPointF(px,bottom));
        p.setPen(Qt::NoPen);p.setBrush(t.cursor);p.drawPolygon(QPolygonF{QPointF(px-5,17),QPointF(px+5,17),QPointF(px+5,23),QPointF(px,28),QPointF(px-5,23)});
    }
    p.setBrush(Qt::NoBrush);p.setPen(QPen(t.separator(),1));p.drawRoundedRect(outline,Theme::cornerRadius,Theme::cornerRadius);p.restore();
}
void AudioEditCanvas::mousePressEvent(QMouseEvent* e){
    if(!m_doc.initialized||m_drag)return;
    if(e->button()==Qt::MiddleButton){m_drag=7;m_pressX=e->position().x();m_savedScroll=m_scroll;grabMouse();updatePointer(e->position());e->accept();return;}
    if(e->button()!=Qt::LeftButton||!m_editingEnabled)return;
    m_savedFirst=m_first;m_savedLast=m_last;m_savedCursor=m_cursor;setFocus();const auto at=frame(e->position().x());m_pressFrame=at;m_pressX=e->position().x();m_dragMoved=false;
    const bool strip=e->position().y()>=height()-regionStripHeight;
    const bool ruler=e->position().y()<rulerHeight;
    // The segment strip is a direct handle in every tool; it never starts a range selection.
    if((strip||m_tool==Tool::Move)&&!ruler){
        if(const auto* r=regionAt(at)){m_drag=3;m_region=r->id;m_anchor=at-r->start;m_moveStart=r->start;m_moveLength=r->length;m_first=r->start;m_last=r->start+r->length;m_cursor=m_first;grabMouse();updatePointer(e->position());update();emit selectionChanged();return;}
    }
    if(m_tool==Tool::Split&&!ruler&&!strip){m_cursor=std::min(at,m_doc.frames);emit selectionChanged();emit splitRequested(m_cursor);update();return;}
    if(m_tool==Tool::Select&&m_last>m_first&&e->position().y()>=rulerHeight&&e->position().y()<rulerHeight+16){
        if(std::abs(e->position().x()-(x(m_first)+6))<7)m_drag=5;
        else if(std::abs(e->position().x()-(x(m_last)-6))<7)m_drag=6;
        if(m_drag){m_pressY=e->position().y();m_fadeCurve=0;m_fadeFrames=std::min<AudioEditFrame>(m_last-m_first,std::llround(.02*m_doc.sampleRate));grabMouse();update();return;}
    }
    if(m_tool==Tool::Slip&&!ruler){m_drag=4;m_anchor=at;m_savedScroll=m_scroll;m_slipDelta=0;grabMouse();return;}
    if(m_last>m_first&&std::abs(e->position().x()-x(m_first))<=6){m_drag=1;m_anchor=m_last;}
    else if(m_last>m_first&&std::abs(e->position().x()-x(m_last))<=6){m_drag=1;m_anchor=m_first;}
    else{m_drag=1;m_anchor=(e->modifiers()&Qt::ShiftModifier)?m_first:std::min(at,m_doc.frames);}
    m_cursor=at;
    const auto selectionEnd=std::min(at,m_doc.frames);
    m_first=std::min(m_anchor,selectionEnd);m_last=std::max(m_anchor,selectionEnd);
    setCursor(Qt::IBeamCursor);grabMouse();update();emit selectionChanged();
}
void AudioEditCanvas::mouseMoveEvent(QMouseEvent* e){if(!m_drag){updatePointer(e->position());return;}
    if(m_drag==4||m_drag==7){m_scroll=std::clamp(m_savedScroll+(m_pressX-e->position().x())*m_framesPerPixel,0.,double(m_doc.frames));m_slipDelta=m_scroll-m_savedScroll;update();emit viewChanged();return;}
    if(m_drag==3&&!m_dragMoved){if(std::abs(e->position().x()-m_pressX)<QApplication::startDragDistance())return;m_dragMoved=true;}
    auto at=frame(e->position().x());
    if(e->position().x()<12)m_scroll=std::max(0.,m_scroll-12*m_framesPerPixel);else if(e->position().x()>width()-12)m_scroll+=12*m_framesPerPixel;
    at=frame(e->position().x());
    if(m_drag==1){at=std::min(at,m_doc.frames);m_first=std::min(at,m_anchor);m_last=std::max(at,m_anchor);emit selectionChanged();}
    if(m_drag==3){m_moveStart=std::max<AudioEditFrame>(0,at-m_anchor);m_first=m_moveStart;m_last=m_moveStart+m_moveLength;m_cursor=m_first;emit selectionChanged();}
    if(m_drag==5||m_drag==6){if(e->modifiers()&Qt::AltModifier)m_fadeCurve=std::clamp((m_pressY-e->position().y())/100.,-1.,1.);else m_fadeFrames=std::clamp<AudioEditFrame>(m_drag==5?at-m_first:m_last-at,1,m_last-m_first);}
    update();emit viewChanged();
}
void AudioEditCanvas::mouseReleaseEvent(QMouseEvent* e){
    if(!m_drag||e->button()!=(m_drag==7?Qt::MiddleButton:Qt::LeftButton))return;
    const int drag=m_drag;const auto id=m_region;const auto start=m_moveStart;const bool moved=m_dragMoved&&start!=m_pressFrame-m_anchor;const double delta=m_slipDelta/m_doc.sampleRate;
    // A successful release keeps the preview; cancellation alone restores the press snapshot.
    m_drag=0;m_region.clear();m_dragMoved=false;if(mouseGrabber()==this)releaseMouse();updatePointer(e->position());update();
    if(drag==3&&moved)emit moveRequested(QString::fromStdString(id),start);
    if(drag==4&&delta!=0)emit slipRequested(delta);
    if(drag==5||drag==6)emit fadeRequested(drag==5,m_fadeFrames,m_fadeCurve);
}
void AudioEditCanvas::mouseDoubleClickEvent(QMouseEvent* e){if(e->button()!=Qt::LeftButton||!m_editingEnabled)return;cancelGesture();if(const auto* r=regionAt(frame(e->position().x())))setSelection(r->start,r->start+r->length);}
void AudioEditCanvas::wheelEvent(QWheelEvent* e){const auto delta=e->pixelDelta().isNull()?QPointF(e->angleDelta())/8:QPointF(e->pixelDelta());if(e->modifiers()&(Qt::ControlModifier|Qt::MetaModifier))zoom(std::exp(delta.y()*.012),e->position().x());else scrollTo(m_scroll-(delta.x()!=0?delta.x():delta.y())*m_framesPerPixel*2);e->accept();}
void AudioEditCanvas::keyPressEvent(QKeyEvent* e){
    if(e->key()==Qt::Key_Escape){const bool dragging=m_drag!=0;cancelGesture();if(!dragging)emit editActionRequested("cancel");e->accept();return;}
    if(e->modifiers()==Qt::NoModifier){
        if(e->key()>=Qt::Key_1&&e->key()<=Qt::Key_4){setTool(Tool(e->key()-Qt::Key_1));e->accept();return;}
        if(e->key()==Qt::Key_S){emit editActionRequested("split");e->accept();return;}
        if(e->key()==Qt::Key_Return||e->key()==Qt::Key_Enter){emit editActionRequested("audition");e->accept();return;}
    }
    if(e->key()==Qt::Key_F&&(e->modifiers()==Qt::NoModifier||e->modifiers()==Qt::ShiftModifier)){fit(bool(e->modifiers()&Qt::ShiftModifier));e->accept();return;}
    if(e->key()==Qt::Key_Delete||e->key()==Qt::Key_Backspace){emit editActionRequested("silence");e->accept();return;}
    if(e->matches(QKeySequence::SelectAll)){setSelection(0,m_doc.frames);e->accept();return;}
    for(const auto& pair:std::vector<std::pair<QKeySequence::StandardKey,QString>>{{QKeySequence::Copy,"copy"},{QKeySequence::Cut,"cut"},{QKeySequence::Paste,"paste"},{QKeySequence::Undo,"undo"},{QKeySequence::Redo,"redo"}})if(e->matches(pair.first)){emit editActionRequested(pair.second);e->accept();return;}
    if(e->key()==Qt::Key_Plus||e->key()==Qt::Key_Equal){zoom(2);e->accept();return;}if(e->key()==Qt::Key_Minus){zoom(.5);e->accept();return;}
    if(e->key()==Qt::Key_Left||e->key()==Qt::Key_Right){const auto delta=e->key()==Qt::Key_Left?-1:1;m_cursor=std::max<AudioEditFrame>(0,m_cursor+delta);const auto boundary=std::min(m_cursor,m_doc.frames);if(e->modifiers()&Qt::ShiftModifier){m_first=std::min(m_anchor,boundary);m_last=std::max(m_anchor,boundary);}else{m_anchor=boundary;m_first=m_last=boundary;}update();emit selectionChanged();e->accept();return;}
    e->ignore();
}
void AudioEditCanvas::contextMenuEvent(QContextMenuEvent* e){QMenu menu(this);for(const auto& item:std::vector<std::pair<QString,QString>>{{tr("Cut"),"cut"},{tr("Copy"),"copy"},{tr("Paste"),"paste"},{tr("Silence selection"),"silence"},{tr("Reverse selection"),"reverse"},{tr("Split at cursor"),"split"},{tr("Select all"),"all"}}){auto* a=menu.addAction(item.first);connect(a,&QAction::triggered,this,[this,id=item.second]{emit editActionRequested(id);});}menu.exec(e->globalPos());}

AudioEditPanel::AudioEditPanel(daw::EngineController* controller,daw::EngineController::AudioEditTarget target,QWidget* parent)
    :QWidget(parent),m_controller(controller),m_target(std::move(target)),m_generation(std::make_shared<std::atomic<std::uint64_t>>(0)){
    setObjectName("AudioEditPanel");auto* layout=new QVBoxLayout(this);layout->setContentsMargins(4,4,4,4);layout->setSpacing(7);
    m_auditionPath="sample-editor:"+QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    auto* toolbar=new QToolBar(this);toolbar->setObjectName("AudioEditToolbar");toolbar->setIconSize(QSize(18,18));toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);layout->addWidget(toolbar);
    m_canvas=new AudioEditCanvas(this);auto* modes=new QActionGroup(this);modes->setExclusive(true);
    m_flatten=new QPushButton(tr("Create editable comp version"),this);
    m_flatten->setObjectName("sampleEditor.flatten");m_flatten->hide();
    m_flatten->setToolTip(tr("Render the comp for editing. Original takes remain in version history."));
    layout->addWidget(m_flatten);
    connect(m_flatten,&QPushButton::clicked,this,[this]{
        if(m_busy||m_target.instrument||m_controller->hasCloudProjectBinding())return;
        const auto* clip=m_controller->audioClip(m_target.trackId,m_target.objectId);if(!clip||clip->takes.empty())return;
        const auto source=*clip;
        const auto revision=m_controller->projectRevision();
        const auto project=m_controller->projectGeneration();
        const auto directory=m_controller->recordDirectory();const auto rate=m_controller->sampleRate();
        const auto generation=++*m_generation;const auto token=m_generation;QPointer<AudioEditPanel> guard(this);
        m_busy=true;m_flatten->setEnabled(false);m_status->setText(tr("Preparing audio…"));
        QThreadPool::globalInstance()->start([guard,source,revision,project,directory,rate,generation,token]{
            daw::EngineController::PreparedCompEdit result;
            try{result=daw::EngineController::prepareCompAudioEdit(source,rate,directory,[token,generation]{return token->load()==generation;});}catch(...){}
            const auto discard=[&]{for(const auto& s:result.document.sources)QFile::remove(QString::fromStdString(s.filePath));};
            if(!guard||token->load()!=generation){discard();return;}
            QMetaObject::invokeMethod(guard,[guard,result=std::move(result),revision,project,generation,token]() mutable {
                const auto discard=[&]{for(const auto& s:result.document.sources)QFile::remove(QString::fromStdString(s.filePath));};
                if(!guard||token->load()!=generation){discard();return;}
                guard->m_busy=false;guard->m_flatten->setEnabled(true);
                if(guard->m_controller->projectGeneration()!=project||guard->m_controller->projectRevision()!=revision||
                    !guard->m_controller->adoptCompAudioEdit(guard->m_target,result)) {
                    discard();guard->m_status->setText(tr("The edit could not be applied."));return;
                }
                emit guard->projectEdited();emit guard->liveEdited();guard->refresh();
            },Qt::QueuedConnection);
        });
    });
    const auto button=[&](QToolBar* bar,icons::Glyph glyph,const QString& text,const QString& id,bool range=false,bool edit=false){
        auto* a=new QAction(text,this);a->setObjectName("sampleEditor."+id);a->setProperty("glyph",int(glyph));a->setToolTip(text);bar->addAction(a);
        if(auto* b=qobject_cast<QToolButton*>(bar->widgetForAction(a))){b->setAccessibleName(text);b->setFocusPolicy(Qt::StrongFocus);}
        if(range)m_rangeActions.push_back(a);if(edit)m_editActions.push_back(a);
        connect(a,&QAction::triggered,this,[this,id]{action(id);});return a;
    };
    const auto tool=[&](const QString& text,const QString& tip,icons::Glyph glyph,AudioEditCanvas::Tool mode,bool visible){
        auto* a=new QAction(text,this);a->setProperty("glyph",int(glyph));a->setToolTip(tip);a->setObjectName("sampleEditor.tool"+QString::number(int(mode)+1));a->setCheckable(true);modes->addAction(a);
        if(visible){toolbar->addAction(a);toolbar->widgetForAction(a)->setAccessibleName(text);}
        a->setChecked(mode==m_canvas->tool());connect(a,&QAction::triggered,this,[this,mode]{m_canvas->setTool(mode);m_canvas->setFocus();});
        connect(m_canvas,&AudioEditCanvas::toolChanged,a,[a,mode](auto current){a->setChecked(current==mode);});return a;
    };
    tool(tr("Select"),tr("Select a range (1)"),icons::Glyph::Pointer,AudioEditCanvas::Tool::Select,true);
    tool(tr("Move"),tr("Move a segment (2)"),icons::Glyph::ResizeHorizontal,AudioEditCanvas::Tool::Move,true);
    tool(tr("Split"),tr("Split, then move (3)"),icons::Glyph::Knife,AudioEditCanvas::Tool::Split,true);
    auto* slip=tool(tr("Slip trim window (4)"),tr("Move the trim window without moving audio"),icons::Glyph::Clock,AudioEditCanvas::Tool::Slip,false);
    toolbar->addSeparator();
    m_undo=button(toolbar,icons::Glyph::Undo,tr("Undo (Ctrl+Z)"),"undo");m_redo=button(toolbar,icons::Glyph::Redo,tr("Redo (Ctrl+Shift+Z)"),"redo");
    toolbar->addSeparator();m_listen=button(toolbar,icons::Glyph::Play,tr("Listen to selection or sample (Enter)"),"audition");m_stop=button(toolbar,icons::Glyph::Stop,tr("Stop listening"),"stop");
    auto* space=new QWidget(toolbar);space->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);toolbar->addWidget(space);
    button(toolbar,icons::Glyph::ZoomOut,tr("Zoom out (−)"),"zoomOut");button(toolbar,icons::Glyph::ZoomIn,tr("Zoom in (+)"),"zoomIn");button(toolbar,icons::Glyph::ZoomFit,tr("Fit sample (F)"),"fit");button(toolbar,icons::Glyph::Crosshair,tr("Fit selection (Shift+F)"),"zoomSelection",true);
    toolbar->addSeparator();
    auto* more=new QToolButton(toolbar);more->setObjectName("sampleEditor.more");more->setToolTip(tr("More editing actions"));more->setAccessibleName(more->toolTip());more->setPopupMode(QToolButton::InstantPopup);more->setProperty("glyph",int(icons::Glyph::Edit));more->setFocusPolicy(Qt::StrongFocus);
    auto* menu=new QMenu(more);more->setMenu(menu);toolbar->addWidget(more);
    for(const auto& item:std::vector<std::pair<QString,QString>>{{tr("Cut"),"cut"},{tr("Copy"),"copy"},{tr("Paste"),"paste"},{tr("Split at cursor (S)"),"split"},{tr("Trim to selection"),"trim"},{tr("Fit clip to content"),"fitClip"}}){
        auto* a=menu->addAction(item.first);a->setObjectName("sampleEditor."+item.second);
        if(item.second=="cut"||item.second=="copy"||item.second=="trim")m_rangeActions.push_back(a);
        if(item.second!="copy")m_editActions.push_back(a);
        connect(a,&QAction::triggered,this,[this,id=item.second]{action(id);});
    }
    menu->addSeparator();menu->addAction(slip);
    auto* details=button(toolbar,icons::Glyph::Inspector,tr("Selection settings"),"details");details->setCheckable(true);
    auto* body=new QHBoxLayout;body->setSpacing(8);auto* waveColumn=new QVBoxLayout;waveColumn->setSpacing(0);waveColumn->addWidget(m_canvas,1);m_scroll=new QScrollBar(Qt::Horizontal,this);waveColumn->addWidget(m_scroll);body->addLayout(waveColumn,1);
    auto* inspector=new QWidget(this);inspector->setObjectName("AudioEditInspector");auto* fields=new QFormLayout(inspector);fields->setContentsMargins(10,8,10,8);fields->setSpacing(7);fields->setRowWrapPolicy(QFormLayout::WrapAllRows);
    auto* heading=new QLabel(tr("Selection settings"),inspector);QFont titleFont=heading->font();titleFont.setBold(true);heading->setFont(titleFont);fields->addRow(heading);
    const auto spin=[&](const QString& label,const QString& name,double lo,double hi,int decimals){auto* value=new QDoubleSpinBox(inspector);value->setObjectName("sampleEditor."+name);value->setRange(lo,hi);value->setDecimals(decimals);value->setKeyboardTracking(false);value->setAccessibleName(label);fields->addRow(label,value);return value;};
    m_start=spin(tr("Start, s"),"start",0,86400,6);m_end=spin(tr("End, s"),"end",0,86400,6);m_length=spin(tr("Length, s"),"length",0,86400,6);
    for(auto* field:{m_start,m_end,m_length})connect(field,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,field](double){if(m_sync||!m_doc.initialized)return;const double first=m_start->value();const double last=field==m_length?first+m_length->value():m_end->value();m_canvas->setSelection(std::llround(first*m_doc.sampleRate),std::llround(last*m_doc.sampleRate));});
    m_gain=new ui::Knob({},inspector);m_gain->setAccessibleName(tr("Selection gain"));m_gain->setObjectName("sampleEditor.gain");m_gain->setRange(-60,24);m_gain->setDefaultValue(0);m_gain->setBipolar(true);m_gain->setCompact(true);m_gain->setValue(0);m_gain->setFormatter([](double db){return QString::number(db,'f',1)+" dB";});auto* gainRow=new QWidget(inspector);auto* gainLayout=new QHBoxLayout(gainRow);gainLayout->setContentsMargins(0,0,0,0);
    m_gainValue=new QDoubleSpinBox(gainRow);m_gainValue->setObjectName("sampleEditor.gainValue");m_gainValue->setRange(-60,24);m_gainValue->setDecimals(2);m_gainValue->setSuffix(" dB");m_gainValue->setKeyboardTracking(false);m_gainValue->setAccessibleName(tr("Selection gain"));
    gainLayout->addWidget(m_gain);gainLayout->addWidget(m_gainValue,1);fields->addRow(tr("Selection gain"),gainRow);
    connect(m_gainValue,qOverload<double>(&QDoubleSpinBox::valueChanged),m_gain,&ui::Knob::editValue);
    connect(m_gain,&ui::Knob::valueChanged,this,[this](double value){QSignalBlocker block(m_gainValue);m_gainValue->setValue(value);});
    connect(m_gain,&ui::Knob::valueChanged,this,[this](double db){if(m_sync||m_canvas->last()<=m_canvas->first())return;if(!m_gainBase&&(m_busy||!currentRevision()))return;if(!m_gainBase){m_gainBase=m_doc;m_gainFinished=false;setProperty("gainFirst",qlonglong(m_canvas->first()));setProperty("gainLast",qlonglong(m_canvas->last()));}auto doc=*m_gainBase;if(db==0 || daw::audioedit::gain(doc,property("gainFirst").toLongLong(),property("gainLast").toLongLong(),db))prepare(std::move(doc),tr("Change Selection Gain"),true);});
    connect(m_gain,&ui::Knob::editFinished,this,&AudioEditPanel::finishGain);
    m_fadeLength=spin(tr("Fade, ms"),"fadeLength",0.001,86400000,3);m_fadeLength->setValue(20);m_curve=spin(tr("Curve"),"fadeCurve",-1,1,2);m_curve->setSingleStep(.1);
    m_normalize=spin(tr("Peak, dBFS"),"peak",-60,0,1);m_normalize->setValue(-1);
    auto* amplitude=spin(tr("View gain"),"amplitude",.125,16,2);amplitude->setValue(1);amplitude->setToolTip(tr("Visual amplitude only; does not change the sound"));connect(amplitude,qOverload<double>(&QDoubleSpinBox::valueChanged),m_canvas,&AudioEditCanvas::setAmplitude);
    m_inspector=new QScrollArea(this);m_inspector->setObjectName("AudioEditInspectorScroll");
    m_inspector->setWidgetResizable(true);m_inspector->setFrameShape(QFrame::NoFrame);
    m_inspector->setFixedWidth(218);m_inspector->setWidget(inspector);body->addWidget(m_inspector);m_inspector->hide();layout->addLayout(body,1);
    connect(details,&QAction::toggled,m_inspector,&QWidget::setVisible);
    auto* selectionBar=new QToolBar(this);selectionBar->setObjectName("AudioEditSelectionBar");selectionBar->setIconSize(QSize(18,18));selectionBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_selectionInfo=new QLabel(this);m_selectionInfo->setObjectName("sampleEditor.selectionInfo");m_selectionInfo->setMinimumWidth(100);m_selectionInfo->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);selectionBar->addWidget(m_selectionInfo);
    button(selectionBar,icons::Glyph::FadeIn,tr("Fade in"),"fadeIn",true,true);button(selectionBar,icons::Glyph::FadeOut,tr("Fade out"),"fadeOut",true,true);
    button(selectionBar,icons::Glyph::Rewind,tr("Reverse selection"),"reverse",true,true);button(selectionBar,icons::Glyph::ResizeVertical,tr("Normalize selection"),"normalize",true,true);button(selectionBar,icons::Glyph::Eraser,tr("Silence selection (Delete)"),"silence",true,true);layout->addWidget(selectionBar);
    m_hint=new QLabel(this);m_hint->setMinimumWidth(0);m_hint->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);layout->addWidget(m_hint);
    const auto hint=[this](AudioEditCanvas::Tool mode){
        m_hint->setText(mode==AudioEditCanvas::Tool::Move?tr("Drag a segment to move · Double-click to select · Esc to cancel"):
            mode==AudioEditCanvas::Tool::Split?tr("Click to split · Then drag either part to move it"):
            mode==AudioEditCanvas::Tool::Slip?tr("Drag to shift the trim window · Esc to cancel"):
            tr("Drag wave to select · Drag bottom strip to move · Ctrl+wheel to zoom"));m_hint->setToolTip(m_hint->text());
    };connect(m_canvas,&AudioEditCanvas::toolChanged,this,hint);hint(m_canvas->tool());
    auto* statusRow=new QHBoxLayout;m_status=new QLabel(this);m_status->setMinimumWidth(0);m_status->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);statusRow->addWidget(m_status,1);m_links=new QLabel(this);statusRow->addWidget(m_links);layout->addLayout(statusRow);
    connect(m_canvas,&AudioEditCanvas::selectionChanged,this,&AudioEditPanel::syncSelection);
    connect(m_canvas,&AudioEditCanvas::viewChanged,this,[this]{QSignalBlocker block(m_scroll);
        m_scrollExtent=std::max({1.,double(m_doc.frames)*1.25-m_canvas->span(),m_canvas->scroll()});
        m_scroll->setRange(0,1000000);m_scroll->setPageStep(std::max(1,int(m_canvas->span()/m_scrollExtent*1000000)));
        m_scroll->setValue(int(m_canvas->scroll()/m_scrollExtent*1000000));});
    connect(m_scroll,&QScrollBar::valueChanged,this,[this](int value){m_canvas->scrollTo(double(value)/1000000*m_scrollExtent);});
    connect(m_canvas,&AudioEditCanvas::editActionRequested,this,&AudioEditPanel::action);
    connect(m_canvas,&AudioEditCanvas::splitRequested,this,&AudioEditPanel::splitAt);
    connect(m_canvas,&AudioEditCanvas::moveRequested,this,[this](const QString& id,qint64 at){edit(tr("Move Audio Fragment"),[id,at](auto& doc){return daw::audioedit::move(doc,id.toStdString(),at);});});
    connect(m_canvas,&AudioEditCanvas::fadeRequested,this,[this](bool in,qint64 frames,double curve){
        m_fadeLength->setValue(1000.*frames/m_doc.sampleRate);m_curve->setValue(curve);
        const auto first=m_canvas->first(),last=m_canvas->last();
        edit(in?tr("Fade In Selection"):tr("Fade Out Selection"),[first,last,in,frames,curve](auto& doc){return daw::audioedit::fade(doc,first,last,in,frames,curve);});
    });
    connect(m_canvas,&AudioEditCanvas::slipRequested,this,[this](double seconds){if(m_controller->slipAudioEdit(m_target,seconds)){emit projectEdited();emit liveEdited();refresh();}});
    connect(&ThemeManager::instance(),&ThemeManager::changed,this,&AudioEditPanel::updateAppearance);
    m_poll=new QTimer(this);m_poll->setInterval(100);connect(m_poll,&QTimer::timeout,this,&AudioEditPanel::refresh);
    m_frameTimer=new ui::FrameTimer(m_canvas,this);connect(m_frameTimer,&ui::FrameTimer::timeout,this,&AudioEditPanel::refreshPlayhead);
    updateAppearance();syncSelection();
}
AudioEditPanel::~AudioEditPanel(){++*m_generation;if(m_controller->previewPath()==m_auditionPath)m_controller->stopPreview();if(property("gainTransaction").toBool())m_controller->cancelAudioEdit();}
void AudioEditPanel::showEvent(QShowEvent* e){QWidget::showEvent(e);m_poll->start();refresh();}
void AudioEditPanel::hideEvent(QHideEvent* e){m_poll->stop();m_frameTimer->stop();QWidget::hideEvent(e);}
void AudioEditPanel::keyPressEvent(QKeyEvent* e){if(e->key()==Qt::Key_Escape){action("cancel");e->accept();return;}e->ignore();}
void AudioEditPanel::refresh(){
    if(!m_controller)return;
    refreshPlayhead();updateActions();
    if(isVisible()&&(m_controller->isPlaying()||m_controller->previewPlaying()||(m_target.instrument&&m_canvas->playhead()>=0)))m_frameTimer->start();else m_frameTimer->stop();
    if(const auto* track=m_controller->project().findTrack(m_target.trackId))m_canvas->setClipColor(colorFromRgb(track->color));
    if(m_busy||m_gainBase)return;
    const auto* clip=m_target.instrument?nullptr:m_controller->audioClip(m_target.trackId,m_target.objectId);
    m_flatten->setVisible(clip&&!clip->takes.empty());
    m_flatten->setEnabled(!m_controller->hasCloudProjectBinding());
    if(!m_target.instrument){const auto count=m_controller->linkedClipCount({m_target.trackId,m_target.objectId});m_links->setText(count>1?tr("Linked: %1").arg(count):QString());}
    // Read the revision before copying a potentially large edit document.
    if (clip && clip->audioEdit.initialized && m_audio && m_doc.revision == clip->audioEdit.revision) return;
    const auto doc=m_controller->audioEditDocument(m_target);if(!doc.initialized){m_status->setText(tr("Load a sample. For takes, first create a flattened version."));return;}
    if(m_doc.revision==doc.revision&&m_audio)return;
    prepare(doc,{},false);
}
void AudioEditPanel::prepare(daw::AudioEditDocument doc,const QString& label,bool commit){
    const auto generation=++*m_generation;const auto token=m_generation;const auto expected=m_controller->audioEditDocument(m_target).revision;const auto projectGeneration=m_controller->projectGeneration();QPointer<AudioEditPanel> guard(this);m_busy=true;m_status->setText(tr("Preparing audio…"));
    updateActions();
    // Reuse decoded audio for the initial display; subsequent renders use the
    // immutable source references, never the previously processed output.
    auto current=commit?std::shared_ptr<const daw::engine::SampleBuffer>{}:m_controller->cachedAudioEditSamples(m_target);
    QThreadPool::globalInstance()->start([guard,token,generation,doc=std::move(doc),label,commit,current,expected,projectGeneration]() mutable {
        if(token->load()!=generation)return;
        std::shared_ptr<const daw::engine::SampleBuffer> audio;std::shared_ptr<const AudioEditPeaks> peaks;QString error;
        try{audio=current?current:daw::EngineController::renderAudioEdit(doc,[token,generation]{return token->load()==generation;});if(audio&&token->load()==generation)peaks=cachedPeaks(doc,*audio,[token,generation]{return token->load()==generation;});if(!audio)error=tr("Audio could not be prepared. The previous version is unchanged.");}catch(const std::exception& e){error=QString::fromUtf8(e.what());}
        if(token->load()!=generation||!guard)return;
        QMetaObject::invokeMethod(guard,[guard,token,generation,doc=std::move(doc),audio,peaks,label,commit,error,expected,projectGeneration]() mutable {
            if(!guard||token->load()!=generation)return;auto* self=guard.data();self->m_busy=false;
            if(self->m_controller->projectGeneration()!=projectGeneration||self->m_controller->audioEditDocument(self->m_target).revision!=expected){if(self->property("gainTransaction").toBool())self->m_controller->cancelAudioEdit();self->m_gainBase.reset();self->setProperty("gainTransaction",false);self->m_status->setText(tr("The sample changed while audio was being prepared. Please retry."));self->updateActions();return;}
            if(!audio){if(self->property("gainTransaction").toBool()){self->m_controller->cancelAudioEdit();self->setProperty("gainTransaction",false);}self->m_status->setText(error);self->m_gainBase.reset();self->updateActions();return;}
            if(commit){
                bool applied=false;
                if(self->m_gainBase){
                    // The first completed preview opens the transaction; later
                    // knob updates replace its endpoint instead of accumulating.
                    if(!self->property("gainTransaction").toBool()){
                        if(!self->m_controller->beginAudioEdit(self->m_target)){self->m_gainBase.reset();self->m_status->setText(tr("The edit could not be applied."));self->updateActions();return;}
                        self->setProperty("gainTransaction",true);
                    }
                    applied=self->m_controller->updateAudioEdit(doc,audio);
                }else applied=self->m_controller->setAudioEdit(self->m_target,doc,audio,label.toStdString());
                if(!applied){if(self->property("gainTransaction").toBool())self->m_controller->cancelAudioEdit();self->m_gainBase.reset();self->setProperty("gainTransaction",false);self->m_status->setText(tr("The edit could not be applied."));self->updateActions();return;}
                if(!self->m_gainBase)emit self->projectEdited();emit self->liveEdited();
            }
            const bool firstView=!self->m_doc.initialized;self->m_doc=std::move(doc);self->m_audio=audio;self->m_peaks=peaks;self->m_canvas->setAudio(self->m_doc,audio,peaks);
            if(firstView&&!self->m_target.instrument) {
                if(const auto* clip=self->m_controller->audioClip(self->m_target.trackId,self->m_target.objectId)) {
                    const double last=clip->warp.enabled&&!clip->warp.empty()?clip->warp.markers.back().sourceSeconds:
                        clip->offsetSeconds+clip->durationSeconds/std::max(.001,clip->sampleEdit.stretchTime);
                    if(last>clip->offsetSeconds)self->m_canvas->showRange(std::llround(clip->offsetSeconds*self->m_doc.sampleRate),std::llround(last*self->m_doc.sampleRate));
                }
            }
            self->syncSelection();self->m_status->setText(tr("%1 Hz · %2 channels · %3 s").arg(self->m_doc.sampleRate).arg(self->m_doc.channels).arg(double(self->m_doc.frames)/self->m_doc.sampleRate,0,'f',3));
            if(self->m_gainBase&&self->m_gainFinished)self->finishGain();self->updateActions();
        },Qt::QueuedConnection);
    });
}
bool AudioEditPanel::currentRevision(){
    if(m_doc.initialized && m_controller->audioEditDocument(m_target).revision==m_doc.revision)return true;
    // A linked peer may have changed between polling ticks. Refresh first so
    // an operation can never restore this panel's obsolete audio document.
    refresh();return false;
}
void AudioEditPanel::updateAppearance(){
    const auto& t=th();
    setStyleSheet(QStringLiteral(
        "QToolBar#AudioEditToolbar, QToolBar#AudioEditSelectionBar { background: %1; border: 1px solid %2; border-radius: 8px; padding: 3px; spacing: 2px; }"
        "QToolBar#AudioEditToolbar QToolButton, QToolBar#AudioEditSelectionBar QToolButton { min-width: 30px; max-width: 30px; min-height: 30px; max-height: 30px; padding: 0; border: 1px solid transparent; border-radius: 6px; background: transparent; }"
        "QToolBar#AudioEditToolbar QToolButton:hover, QToolBar#AudioEditSelectionBar QToolButton:hover { background: %3; }"
        "QToolBar#AudioEditToolbar QToolButton:checked { background: %4; }"
        "QToolBar#AudioEditToolbar QToolButton::menu-indicator { image: none; }"
        "QWidget#AudioEditInspector { background: %1; border-radius: 8px; }")
        .arg(t.surface.name(),t.separator().name(),t.surfaceElevated.name(),mixColors(t.surface,t.accent,.22).name()));
    for(auto* a:findChildren<QAction*>())if(a->property("glyph").isValid())a->setIcon(icons::icon(icons::Glyph(a->property("glyph").toInt()),t.textPrimary));
    for(auto* b:findChildren<QToolButton*>())if(b->property("glyph").isValid())b->setIcon(icons::icon(icons::Glyph(b->property("glyph").toInt()),t.textPrimary));
    for(auto* label:{m_status,m_links,m_hint}){auto palette=label->palette();palette.setColor(QPalette::WindowText,t.textSecondary);label->setPalette(palette);}
    m_canvas->update();
}
void AudioEditPanel::refreshPlayhead(){
    double at=-1;
    if(m_doc.initialized){
        if(m_controller->previewPlaying()&&m_controller->previewPath()==m_auditionPath)
            at=(m_auditionStart+m_controller->previewPositionSeconds())*m_doc.sampleRate;
        else if(!m_target.instrument){
            if(const auto* clip=m_controller->audioClip(m_target.trackId,m_target.objectId))
                at=clipSourceFrame(*clip,m_controller->presentationPositionSeconds(),m_controller->tempo(),m_doc);
        }else{
            const double seconds=m_controller->samplerSnapshot(m_target.trackId,m_target.objectId).playbackPositionSeconds;
            if(seconds>=0){
                at=seconds*m_doc.sampleRate;
                if(m_controller->insertParameter(m_target.trackId,m_target.objectId,"pre.reverse")>=.5)at=m_doc.frames-1-at;
                if(at<0||at>=m_doc.frames)at=-1;
            }
        }
    }
    m_canvas->setPlayhead(at);
}
void AudioEditPanel::splitAt(AudioEditFrame at){
    if(m_busy||m_gainBase||!m_audio||!m_doc.initialized||m_controller->hasCloudProjectBinding()||!currentRevision())return;
    auto doc=m_doc;if(!daw::audioedit::split(doc,at))return;
    // A split only changes region boundaries. Reusing immutable samples and
    // peaks makes cutting immediate, even for a long recording.
    if(!m_controller->setAudioEdit(m_target,doc,m_audio,tr("Split Sample").toStdString()))return;
    {std::lock_guard lock(peakCacheMutex);peakCache[doc.revision]=m_peaks;}
    m_doc=std::move(doc);m_canvas->setAudio(m_doc,m_audio,m_peaks);m_canvas->setSelection(at,at);m_canvas->setTool(AudioEditCanvas::Tool::Move);
    updateActions();emit projectEdited();emit liveEdited();
}
void AudioEditPanel::edit(const QString& label,Edit transform){if(m_controller->hasCloudProjectBinding()){m_status->setText(tr("This collaboration server does not support audio editing yet."));return;}if(m_busy||m_gainBase||!m_doc.initialized||!currentRevision())return;auto doc=m_doc;if(transform(doc))prepare(std::move(doc),label,true);}
void AudioEditPanel::syncSelection(){m_sync=true;const double rate=m_doc.sampleRate;{QSignalBlocker a(m_start),b(m_end),c(m_length);m_start->setValue(m_canvas->first()/rate);m_end->setValue(m_canvas->last()/rate);m_length->setValue((m_canvas->last()-m_canvas->first())/rate);}m_sync=false;
    m_selectionInfo->setText(m_canvas->last()>m_canvas->first()?tr("%1 – %2 s  ·  %3 ms").arg(m_canvas->first()/rate,0,'f',3).arg(m_canvas->last()/rate,0,'f',3).arg((m_canvas->last()-m_canvas->first())*1000./rate,0,'f',1):tr("Cursor  %1 s").arg(m_canvas->cursor()/rate,0,'f',3));
    m_selectionInfo->setToolTip(m_selectionInfo->text());updateActions();}
void AudioEditPanel::updateActions(){
    const bool selected=m_canvas->last()>m_canvas->first();
    const bool ready=m_doc.initialized&&!m_busy&&!m_gainBase;
    const bool editable=ready&&!m_controller->hasCloudProjectBinding()&&!m_controller->isRecording()&&m_controller->sharedEditingAllowed();
    for(auto* a:m_rangeActions)a->setEnabled(selected&&ready);
    for(auto* a:m_editActions)a->setEnabled(editable&&(std::find(m_rangeActions.begin(),m_rangeActions.end(),a)==m_rangeActions.end()||selected));
    m_undo->setEnabled(editable&&m_controller->canUndo());m_redo->setEnabled(editable&&m_controller->canRedo());
    m_listen->setEnabled(m_audio&&!m_busy);m_stop->setEnabled(m_auditionPending||(m_controller->previewPlaying()&&m_controller->previewPath()==m_auditionPath));
    m_gain->setEnabled(selected&&(editable||m_gainBase.has_value()));m_gainValue->setEnabled(m_gain->isEnabled());
    m_canvas->setEditingEnabled(ready);for(auto* field:{m_start,m_end,m_length})field->setEnabled(ready);
}
void AudioEditPanel::finishGain(){m_gainFinished=true;if(m_busy)return;if(property("gainTransaction").toBool()){if(m_controller->commitAudioEdit(tr("Change Selection Gain").toStdString()))emit projectEdited();setProperty("gainTransaction",false);}m_gainBase.reset();m_sync=true;m_gain->setValue(0);{QSignalBlocker block(m_gainValue);m_gainValue->setValue(0);}m_sync=false;updateActions();}
void AudioEditPanel::cancelPending(){++*m_generation;m_busy=false;if(property("gainTransaction").toBool())m_controller->cancelAudioEdit();m_gainBase.reset();setProperty("gainTransaction",false);m_doc.revision.clear();m_canvas->cancelGesture();m_sync=true;m_gain->setValue(0);{QSignalBlocker block(m_gainValue);m_gainValue->setValue(0);}m_sync=false;refresh();}
void AudioEditPanel::action(const QString& id){
    if(id=="details")return;
    if(id=="cancel"){if(m_busy||m_gainBase)cancelPending();else m_canvas->setTool(AudioEditCanvas::Tool::Select);return;}if(id=="stop"){setProperty("auditionSerial",property("auditionSerial").toULongLong()+1);m_auditionPending=false;if(m_controller->previewPath()==m_auditionPath)m_controller->stopPreview();refreshPlayhead();updateActions();return;}if(id=="zoomIn"){m_canvas->zoom(2);return;}if(id=="zoomOut"){m_canvas->zoom(.5);return;}if(id=="fit"){m_canvas->fit();return;}if(id=="zoomSelection"){m_canvas->fit(true);return;}if(id=="all"){m_canvas->setSelection(0,m_doc.frames);return;}
    if((m_busy||m_gainBase)&&id!="audition"&&id!="copy")return;
    if(id=="undo"||id=="redo"){if(!m_controller->hasCloudProjectBinding()&&m_controller->sharedEditingAllowed()&&!m_controller->isRecording()){if(id=="undo")m_controller->undo();else m_controller->redo();refresh();emit projectEdited();emit liveEdited();}return;}
    if(!m_doc.initialized||!currentRevision())return;
    const auto first=m_canvas->first(),last=m_canvas->last();
    if(id=="copy"||id=="cut"){if(last<=first)return;clipboard=daw::audioedit::copy(m_doc,first,last);if(id=="copy")return;}
    if(id=="paste"){if(!clipboard)return;const auto copied=*clipboard;const auto at=last>first?first:m_canvas->cursor();edit(tr("Paste Audio"),[copied,at,last](auto& d){return daw::audioedit::paste(d,copied,at,last);});return;}
    if(id=="split"){splitAt(m_canvas->cursor());return;}
    if(id=="fitClip"){if(m_controller->setAudioEditWindow(m_target,0,double(m_doc.frames)/m_doc.sampleRate,tr("Fit Clip to Content").toStdString())){emit projectEdited();emit liveEdited();}return;}
    if(id=="audition"){
        if(!m_audio)return;const auto a=last>first?first:0,b=last>first?last:m_doc.frames;const auto source=m_audio;QPointer<AudioEditPanel> guard(this);const auto serial=property("auditionSerial").toULongLong()+1;setProperty("auditionSerial",serial);
        if(b<=a||a<0||b>AudioEditFrame(source->frames()))return;
        if(a==0&&b==AudioEditFrame(source->frames())){m_auditionPending=false;m_auditionStart=0;if(m_controller->previewBuffer(source,m_auditionPath,false)){refresh();m_frameTimer->start();m_stop->setEnabled(true);}return;}
        m_auditionPending=true;updateActions();const auto revision=m_doc.revision;const auto project=m_controller->projectGeneration();
        QThreadPool::globalInstance()->start([guard,source,a,b,serial,revision,project]{auto out=std::make_shared<daw::engine::SampleBuffer>(source->channels(),b-a,source->sampleRate());for(unsigned ch=0;ch<source->channels();++ch)std::copy_n(source->channel(ch)+a,b-a,out->writableChannel(ch));if(guard)QMetaObject::invokeMethod(guard,[guard,out,serial,a,revision,project]{if(guard&&guard->property("auditionSerial").toULongLong()==serial){guard->m_auditionPending=false;bool started=false;if(guard->m_controller->projectGeneration()==project&&guard->m_doc.revision==revision){guard->m_auditionStart=double(a)/out->sampleRate();started=guard->m_controller->previewBuffer(out,guard->m_auditionPath,false);}guard->refresh();if(started){guard->m_frameTimer->start();guard->m_stop->setEnabled(true);}}},Qt::QueuedConnection);});return;
    }
    if(last<=first){m_status->setText(tr("Select an audio range first."));return;}
    if(id=="trim"){if(m_controller->setAudioEditWindow(m_target,first/m_doc.sampleRate,last/m_doc.sampleRate,tr("Trim to Selection").toStdString())){emit projectEdited();emit liveEdited();}return;}
    if(id=="silence"||id=="cut")edit(tr("Silence Selection"),[first,last](auto& d){return daw::audioedit::silence(d,first,last);});
    if(id=="reverse")edit(tr("Reverse Selection"),[first,last](auto& d){return daw::audioedit::reverse(d,first,last);});
    if(id=="fadeIn"||id=="fadeOut"){const bool in=id=="fadeIn";const auto length=AudioEditFrame(std::llround(m_fadeLength->value()*.001*m_doc.sampleRate));const double curve=m_curve->value();edit(in?tr("Fade In Selection"):tr("Fade Out Selection"),[first,last,in,length,curve](auto& d){return daw::audioedit::fade(d,first,last,in,length,curve);});}
    if(id=="normalize"&&m_audio){const auto source=m_audio;const double db=m_normalize->value();const auto original=m_doc;const auto generation=++*m_generation;const auto token=m_generation;QPointer<AudioEditPanel> guard(this);m_busy=true;m_status->setText(tr("Measuring peak…"));updateActions();QThreadPool::globalInstance()->start([guard,source,db,original,first,last,generation,token]{const double peak=daw::audioedit::peak(*source,first,last);if(guard)QMetaObject::invokeMethod(guard,[guard,db,original,first,last,peak,generation,token]{if(!guard||token->load()!=generation)return;guard->m_busy=false;guard->updateActions();if(guard->m_controller->audioEditDocument(guard->m_target).revision!=original.revision){guard->refresh();return;}if(peak<=1e-15){guard->m_status->setText(tr("The selection is silent."));return;}auto d=original;if(daw::audioedit::gain(d,first,last,std::clamp(db-20*std::log10(peak),-240.,240.)))guard->prepare(std::move(d),tr("Normalize Selection"),true);},Qt::QueuedConnection);});}
}

bool AudioEditPanel::checkForTest() {
    AudioEditCanvas canvas;
    canvas.resize(800,300);
    auto audio=std::make_shared<daw::engine::SampleBuffer>(2,48000,48000);
    for(unsigned ch=0;ch<2;++ch)for(unsigned i=0;i<48000;++i){
        const double pulse=std::fmod(i/48000.+ch*.003,.25);
        audio->writableChannel(ch)[i]=float(.8*std::exp(-pulse*17)*std::sin(i*.038+2*std::sin(i*.002)));
    }
    auto doc=daw::audioedit::fromSource({"ui-source","ui.wav",48000,48000,2});
    auto peaks=AudioEditPeaks::build(*audio);
    daw::ClipModel clip;clip.startSeconds=2;clip.durationSeconds=1;clip.offsetSeconds=.1;clip.sampleEdit.stretchTime=2;
    if(clipSourceFrame(clip,1.9,120,doc)!=-1||std::abs(clipSourceFrame(clip,2.4,120,doc)-14400)>1e-6||clipSourceFrame(clip,3,120,doc)!=-1)return false;
    clip.sampleEdit.reverse=true;
    if(std::abs(clipSourceFrame(clip,2.4,120,doc)-(48000-1-14400))>1e-6)return false;
    clip.sampleEdit.reverse=false;clip.warp.enabled=true;clip.warp.markers={{"a",.1,0,true},{"b",.4,1,false},{"c",.9,2,true}};
    if(std::abs(clipSourceFrame(clip,2.75,120,doc)-31200)>1e-6)return false;
    clip.warp.enabled=false;clip.sampleEdit.loopMode=1;clip.sampleEdit.loopStart=.2;clip.sampleEdit.loopEnd=.6;
    if(std::abs(clipSourceFrame(clip,2.8,120,doc)-14400)>1e-6)return false;
    clip.sampleEdit.loopMode=2;
    if(std::abs(clipSourceFrame(clip,2.8,120,doc)-14400)>1e-6)return false;
    clip.sampleEdit.loopMode=0;clip.muted=true;if(clipSourceFrame(clip,2.4,120,doc)!=-1)return false;
    std::fprintf(stderr,"PASS Audio editor: playhead trim, stretch, reverse, warp, forward/ping-pong loops and clip bounds\n");
    canvas.setAudio(doc,audio,peaks);
    const double anchored=canvas.scroll()+200*canvas.span()/canvas.width();
    canvas.zoom(4,200);
    if(std::abs(anchored-canvas.scroll()-200*canvas.span()/canvas.width())>1e-6)return false;
    canvas.setSelection(12000,24000);canvas.fit(true);
    if(canvas.first()!=12000||canvas.last()!=24000||canvas.scroll()!=12000)return false;
    canvas.zoom(100000,200);
    if(canvas.span()/canvas.width()>=1)return false;
    canvas.fit();
    bool split=false;
    QObject::connect(&canvas,&AudioEditCanvas::splitRequested,[&](qint64 at){split=at==24000;});
    canvas.setTool(AudioEditCanvas::Tool::Split);
    QMouseEvent press(QEvent::MouseButtonPress,QPointF(400,140),QPointF(400,140),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&press);
    if(!split)return false;

    daw::EngineController controller;
    if(!controller.initialize(48000,512,false))return false;
    AudioEditPanel panel(&controller,{});
    panel.resize(720,440);panel.show();QApplication::processEvents();panel.m_poll->stop();
    daw::audioedit::split(doc,12000);daw::audioedit::split(doc,32000);
    panel.m_doc=doc;panel.m_audio=audio;panel.m_peaks=peaks;
    panel.m_canvas->setAudio(doc,audio,peaks);panel.m_canvas->setSelection(12000,32000);
    panel.m_canvas->setClipColor(QColor(65,125,190));panel.m_canvas->setPlayhead(21000);
    panel.syncSelection();panel.m_links->setText(tr("Linked: %1").arg(3));
    panel.m_status->setText(tr("%1 Hz · %2 channels · %3 s").arg(48000).arg(2).arg("1.000"));
    const auto destination=qEnvironmentVariable("DAW_AUDIO_EDITOR_CHECK_DIR");
    if(!destination.isEmpty())QDir().mkpath(destination);
    for(const auto& theme:ThemeManager::instance().presets()) {
        ThemeManager::instance().setThemeId(theme.id,false);QApplication::processEvents();
        if(panel.m_canvas->width()<280||panel.m_canvas->height()<180)return false;
        if(!destination.isEmpty())panel.grab().save(destination+"/sample-editor-"+theme.id+".png");
    }
    panel.findChild<QAction*>("sampleEditor.details")->setChecked(true);QApplication::processEvents();
    if(panel.m_canvas->width()<280)return false;
    if(!destination.isEmpty())panel.grab().save(destination+"/sample-editor-details.png");
    panel.findChild<QAction*>("sampleEditor.details")->setChecked(false);panel.resize(560,360);QApplication::processEvents();
    if(panel.width()!=560||panel.m_canvas->height()<180)return false;
    if(!destination.isEmpty())panel.grab().save(destination+"/sample-editor-compact.png");
    std::fprintf(stderr,"PASS Audio editor: pointer anchored zoom, sample scale, range fit, split tool, stereo layout\n");
    return true;
}
