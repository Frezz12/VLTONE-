#include "ClipLibraryView.hpp"
#include "ClipLibraryDrag.hpp"
#include "EngineController.hpp"
#include "PreviewLoader.hpp"
#include "Theme.hpp"
#include "Icons.hpp"
#include "WaveformPaint.hpp"
#include <QContextMenuEvent>
#include <QDrag>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
QString kindName(daw::ClipKind kind) {
    switch (kind) {
    case daw::ClipKind::Midi: return QStringLiteral("MIDI");
    case daw::ClipKind::Audio: return ClipLibraryView::tr("Audio");
    case daw::ClipKind::Pattern: return QStringLiteral("Pattern");
    case daw::ClipKind::Automation: return ClipLibraryView::tr("Automation");
    }
    return {};
}

// Use the audible comp, including punch-in offsets, for recorded MIDI clips.
template <typename Visitor>
void visitNotes(const daw::ClipModel& clip, double tempo, Visitor&& visitor) {
    const double bps = tempo / 60.0;
    const auto slice = [&](const auto& notes, double offset, double from, double to) {
        for (const auto& note : notes) {
            const double begin = std::max(from * bps, note.startBeats + offset * bps);
            const double end = std::min(to * bps, note.startBeats + note.lengthBeats + offset * bps);
            if (end > begin) visitor(note, begin, end);
        }
    };
    if (clip.takes.empty()) slice(clip.notes, -clip.offsetSeconds, 0, clip.durationSeconds);
    else for (const auto& segment : clip.comp)
        if (const auto* take = daw::findTake(clip, segment.takeId); take && !take->muted)
            slice(take->notes, take->clipOffsetSeconds, segment.startSeconds, segment.endSeconds);
}

template <typename Visitor>
void visitAudioSlices(const daw::ClipModel& clip, Visitor&& visitor) {
    const double stretch = std::max(.001, clip.sampleEdit.stretchTime);
    if (clip.takes.empty()) {
        visitor(clip.filePath, 0.0, clip.durationSeconds, clip.offsetSeconds, stretch, clip.gain, clip.sampleEdit.reverse);
    } else for (const auto& segment : clip.comp) {
        const auto* take = daw::findTake(clip, segment.takeId);
        if (!take || take->muted) continue;
        const double from = std::max(segment.startSeconds, take->clipOffsetSeconds);
        const double to = std::min({segment.endSeconds, clip.durationSeconds,
            take->lengthSeconds > 0 ? take->clipOffsetSeconds + take->lengthSeconds * stretch : clip.durationSeconds});
        if (to > from) visitor(take->filePath, from, to,
            take->offsetSeconds + (from - take->clipOffsetSeconds) / stretch, stretch, take->gain, false);
    }
}

class CardDelegate final : public QStyledItemDelegate {
public:
    explicit CardDelegate(ClipLibraryView* view) : QStyledItemDelegate(view), m_view(view) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(120, int(std::lround(112 * m_view->zoom())));
    }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const auto* saved = m_view->entry(index);
        if (!saved || saved->tracks.empty() || saved->tracks.front().clips.empty()) return;
        const auto& clip = saved->tracks.front().clips.front();
        const auto& theme = ThemeManager::instance().theme();
        const double z = m_view->zoom();
        const QRectF card = QRectF(option.rect).adjusted(5, 3, -5, -3);
        const QColor color = QColor::fromRgb(clip.color);
        p->save(); p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(theme.separator(), 1)); p->setBrush(theme.surfaceElevated);
        p->drawRoundedRect(card, Theme::cornerRadius, Theme::cornerRadius);
        if (option.state & QStyle::State_Selected) {
            QColor wash = color; wash.setAlpha(theme.dark ? 37 : 24);
            p->setPen(Qt::NoPen); p->setBrush(wash);
            p->drawRoundedRect(card, Theme::cornerRadius, Theme::cornerRadius);
        } else if (option.state & QStyle::State_MouseOver) {
            p->setPen(Qt::NoPen); p->setBrush(theme.ink(9));
            p->drawRoundedRect(card, Theme::cornerRadius, Theme::cornerRadius);
        }
        p->setPen(Qt::NoPen); p->setBrush(color);
        p->drawRoundedRect(QRectF(card.left() + 9*z, card.top() + 10*z, 3*z, 13*z), 1.5, 1.5);
        QFont title = option.font; title.setPixelSize(int(11*z)); title.setWeight(QFont::DemiBold);
        p->setFont(title); p->setPen(theme.textPrimary);
        const QRectF titleRect = card.adjusted(18*z, 6*z, -9*z, 0);
        p->drawText(QRectF(titleRect.x(), titleRect.y(), titleRect.width(), 22*z),
            Qt::AlignVCenter, QFontMetrics(title).elidedText(QString::fromStdString(saved->name), Qt::ElideRight, int(titleRect.width())));
        QFont small = option.font; small.setPixelSize(int(9*z)); p->setFont(small); p->setPen(theme.textSecondary);
        const double beats = clip.durationSeconds * saved->tempo / 60.0;
        QString detail = kindName(clip.kind) + QStringLiteral(" · ") + ClipLibraryView::tr("%1 beats").arg(beats, 0, 'g', 3);
        if (!clip.inserts.empty()) detail += QStringLiteral(" · %1 FX").arg(clip.inserts.size());
        p->drawText(QRectF(card.left()+9*z, card.top()+28*z, card.width()-18*z, 14*z),
            Qt::AlignVCenter, QFontMetrics(small).elidedText(detail, Qt::ElideRight, int(card.width()-18*z)));
        const QRectF preview(card.left()+8*z, card.top()+48*z, card.width()-16*z, card.height()-56*z);
        QColor fill = color; fill.setAlpha(theme.dark ? 27 : 21);
        p->setBrush(fill); p->setPen(Qt::NoPen); p->drawRoundedRect(preview, 4, 4);
        p->setClipRect(preview.adjusted(1,1,-1,-1));
        QColor grid = theme.textSecondary; grid.setAlpha(32); p->setPen(grid);
        for (int i = 1; i < 4; ++i) {
            const double x = preview.left()+preview.width()*i/4;
            p->drawLine(QPointF(x, preview.top()), QPointF(x, preview.bottom()));
        }
        const QRectF content = preview.adjusted(4,5,-4,-5);
        if (clip.kind == daw::ClipKind::Midi || clip.kind == daw::ClipKind::Pattern) {
            if (beats <= 0) { p->restore(); return; }
            int low = 127, high = 0;
            for (const auto& track : saved->tracks) for (const auto& part : track.clips)
                visitNotes(part, saved->tempo, [&](const auto& note, double, double) {
                    low = std::min(low, int(note.pitch)); high = std::max(high, int(note.pitch));
                });
            low = std::min(low, high); high = std::max(high, low+11);
            for (const auto& track : saved->tracks) for (const auto& part : track.clips) {
                const double offset = (part.startSeconds-clip.startSeconds)*saved->tempo/60;
                visitNotes(part, saved->tempo, [&](const auto& note, double begin, double end) {
                    const double from = std::clamp(begin + offset, 0.0, beats);
                    const double to = std::clamp(end + offset, 0.0, beats);
                    if (to <= from) return;
                    QColor ink = theme.dark ? color.lighter(135) : color.darker(130);
                    ink.setAlpha(110+int(145.0*note.velocity/127.0));
                    p->setBrush(ink); p->setPen(Qt::NoPen);
                    p->drawRoundedRect(QRectF(content.left()+from/beats*content.width(),
                        content.bottom()-(note.pitch-low+1.0)/(high-low+1)*content.height(),
                        std::max(2.0,(to-from)/beats*content.width()), std::max(2.0,content.height()/(high-low+1))), 1, 1);
                });
            }
        } else if (clip.kind == daw::ClipKind::Audio) {
            bool ready = false;
            visitAudioSlices(clip, [&](const auto& file, double from, double to, double source, double stretch, float gain, bool reversed) {
                const auto* peaks = m_view->controller()->waveforms().cached(file);
                if (!peaks || !peaks->isValid() || clip.durationSeconds <= 0) return;
                const double pixels = content.width() / clip.durationSeconds;
                const QRectF slice(content.left() + from * pixels, content.top(), (to-from) * pixels, content.height());
                ui::PeakPaint how;
                how.sourceStartSeconds = source; how.secondsPerPixel = 1.0 / (pixels * stretch);
                how.clipLeft = content.left(); how.clipRight = content.right();
                how.reversed = reversed; how.gain = gain;
                how.color = theme.dark ? color.lighter(145) : color.darker(125);
                ui::paintPeaks(*p, peaks, slice, how); ready = true;
            });
            if (!ready) {
                p->setPen(theme.textSecondary);
                p->drawText(content, Qt::AlignCenter, ClipLibraryView::tr("Waveform preview"));
            }
        } else {
            p->setPen(QPen(theme.dark ? color.lighter(145) : color.darker(125), 1.6));
            QPainterPath path;
            for (std::size_t i=0;i<clip.automation.points.size();++i) {
                const auto& point=clip.automation.points[i];
                const QPointF at(content.left()+point.beats/std::max(.001,beats)*content.width(),
                    content.bottom()-std::clamp(point.value,0.0,1.0)*content.height());
                if (!i) path.moveTo(at); else path.lineTo(at);
            }
            p->drawPath(path);
        }
        p->restore();
    }
private:
    ClipLibraryView* m_view;
};
}

ClipLibraryView::ClipLibraryView(daw::EngineController* controller, QWidget* parent)
    : QListWidget(parent), m_controller(controller), m_loader(new PreviewLoader(this)) {
    setObjectName(QStringLiteral("ClipLibraryView"));
    setAccessibleName(tr("Saved clips"));
    setItemDelegate(new CardDelegate(this));
    setDragEnabled(true); setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true); setFrameShape(QFrame::NoFrame);
    setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; outline: none; }"));
    connect(this, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        emit restoreRequested(item->data(Qt::UserRole).toString(), true);
    });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] { viewport()->update(); });
    connect(m_loader, &PreviewLoader::loaded, this, [this](const QString& path,
        std::shared_ptr<const daw::engine::SampleBuffer>, daw::WaveformPeaks peaks) {
        m_controller->waveforms().storePrepared(path.toStdString(), std::move(peaks));
        m_loading.clear(); viewport()->update(); queueWaveforms();
    });
    connect(m_loader, &PreviewLoader::failed, this, [this] {
        m_loading.clear(); queueWaveforms();
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { queueWaveforms(); });
}

const daw::ClipLibraryEntry* ClipLibraryView::entry(const QModelIndex& index) const {
    return m_controller->libraryClip(index.data(Qt::UserRole).toString().toStdString());
}

void ClipLibraryView::refresh() {
    QString signature = m_filter;
    for (const auto& saved : m_controller->project().clipLibrary)
        signature += QChar(0x1f)+QString::fromStdString(saved.id)+QChar(0x1e)+QString::fromStdString(saved.name);
    if (signature != m_signature || (count()==0 && !m_controller->project().clipLibrary.empty())) {
        const QString selected = currentItem() ? currentItem()->data(Qt::UserRole).toString() : QString{};
        const int scroll = verticalScrollBar()->value();
        clear(); m_signature = signature;
        m_loader->cancel(); m_loading.clear(); m_requested.clear();
        for (const auto& saved : m_controller->project().clipLibrary) {
            const QString name = QString::fromStdString(saved.name);
            if (!m_filter.isEmpty() && !name.contains(m_filter, Qt::CaseInsensitive)) continue;
            auto* item = new QListWidgetItem(name, this);
            item->setData(Qt::UserRole, QString::fromStdString(saved.id));
            item->setToolTip(tr("%1\nDrag to the timeline, or right-click to restore.").arg(name));
            item->setData(Qt::AccessibleTextRole, name);
        }
        selectEntry(selected); verticalScrollBar()->setValue(scroll);
    }
    if (isVisible()) queueWaveforms();
    viewport()->update();
}
void ClipLibraryView::setFilter(const QString& query) { m_filter=query.trimmed(); refresh(); }
void ClipLibraryView::setZoom(double zoom) { m_zoom=zoom; doItemsLayout(); viewport()->update(); }
void ClipLibraryView::selectEntry(const QString& id) {
    for (int i=0;i<count();++i) if (item(i)->data(Qt::UserRole).toString()==id) { setCurrentRow(i); return; }
}
void ClipLibraryView::showEvent(QShowEvent* event) { QListWidget::showEvent(event); refresh(); }

void ClipLibraryView::queueWaveforms() {
    if (!m_loading.isEmpty() || !isVisible()) return;
    for (int i=0;i<count();++i) {
        if (!viewport()->rect().intersects(visualItemRect(item(i)))) continue;
        const auto* saved = entry(indexFromItem(item(i)));
        if (!saved || saved->tracks.empty() || saved->tracks.front().clips.empty()) continue;
        const auto& clip=saved->tracks.front().clips.front();
        if (clip.kind != daw::ClipKind::Audio) continue;
        visitAudioSlices(clip, [&](const auto& file, double, double, double, double, float, bool) {
            const QString path=QString::fromStdString(file);
            if (!m_loading.isEmpty() || path.isEmpty() || m_requested.contains(path) ||
                m_controller->waveforms().cached(file)) return;
            m_requested.insert(path); m_loading=path; m_loader->request(path);
        });
        if (!m_loading.isEmpty()) break;
    }
}

void ClipLibraryView::paintEvent(QPaintEvent* event) {
    QListWidget::paintEvent(event);
    if (count()) return;
    QPainter p(viewport()); const auto& theme=ThemeManager::instance().theme();
    const QRectF area=viewport()->rect().adjusted(16,12,-16,-12);
    const QRectF icon(area.center().x()-15, area.top()+28, 30,30);
    icons::paint(p, icons::Glyph::Layers, icon, theme.textSecondary);
    p.setPen(theme.textPrimary); QFont font=p.font(); font.setPixelSize(int(12*m_zoom)); font.setWeight(QFont::DemiBold); p.setFont(font);
    p.drawText(QRectF(area.left(), icon.bottom()+14, area.width(),30), Qt::AlignHCenter|Qt::TextWordWrap, tr("Saved clips"));
    font.setPixelSize(int(10*m_zoom)); font.setWeight(QFont::Normal); p.setFont(font); p.setPen(theme.textSecondary);
    p.drawText(QRectF(area.left(), icon.bottom()+52, area.width(),100), Qt::AlignHCenter|Qt::TextWordWrap,
        m_filter.isEmpty() ? tr("Drag a clip here to keep a copy.\nSaved with this project.") : tr("No matching clips"));
}

void ClipLibraryView::startDrag(Qt::DropActions) {
    if (!currentItem()) return;
    auto* drag=new QDrag(this); auto* mime=new QMimeData;
    mime->setData(ui::cliplibrary::kLibraryMime, currentItem()->data(Qt::UserRole).toString().toUtf8());
    drag->setMimeData(mime); drag->setPixmap(viewport()->grab(visualItemRect(currentItem())));
    drag->setHotSpot(QPoint(20,20)); drag->exec(Qt::CopyAction,Qt::CopyAction); drag->deleteLater();
}

bool ClipLibraryView::populateActions(QMenu& menu) {
    if (!currentItem()) return false;
    const QString id=currentItem()->data(Qt::UserRole).toString();
    menu.addAction(tr("Insert at original position"), this, [this,id] { emit restoreRequested(id,true); });
    menu.addAction(tr("Insert on a new track at playhead"), this, [this,id] { emit restoreRequested(id,false); });
    menu.addSeparator();
    menu.addAction(tr("Rename…"), this, [this,id] {
        const auto* saved=m_controller->libraryClip(id.toStdString()); if (!saved) return;
        bool ok=false; const QString name=QInputDialog::getText(this,tr("Rename saved clip"),tr("Name"),
            QLineEdit::Normal,QString::fromStdString(saved->name),&ok).trimmed();
        if (ok && m_controller->renameLibraryClip(id.toStdString(),name.toStdString())) { refresh(); emit projectEdited(); }
    });
    menu.addAction(tr("Remove from saved clips"), this, [this,id] {
        if (m_controller->removeLibraryClip(id.toStdString())) { refresh(); emit projectEdited(); }
    });
    return true;
}
bool ClipLibraryView::showActions() { QMenu menu(this); if (!populateActions(menu)) return false; menu.exec(QCursor::pos()); return true; }
void ClipLibraryView::contextMenuEvent(QContextMenuEvent* event) {
    if (auto* item=itemAt(event->pos())) setCurrentItem(item);
    showActions(); event->accept();
}
void ClipLibraryView::keyPressEvent(QKeyEvent* event) {
    if (event->key()==Qt::Key_Delete || event->key()==Qt::Key_Backspace) {
        removeSelected();
        event->accept(); return;
    }
    if (event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter) {
        if (currentItem()) emit restoreRequested(currentItem()->data(Qt::UserRole).toString(),true);
        event->accept(); return;
    }
    QListWidget::keyPressEvent(event);
}

bool ClipLibraryView::event(QEvent* event) {
    if (event->type()==QEvent::ShortcutOverride) {
        const auto* key=static_cast<QKeyEvent*>(event);
        if (key->modifiers()==Qt::NoModifier && (key->key()==Qt::Key_Delete ||
            key->key()==Qt::Key_Backspace || key->key()==Qt::Key_Return || key->key()==Qt::Key_Enter)) {
            event->accept(); return true;
        }
    }
    return QListWidget::event(event);
}

void ClipLibraryView::removeSelected() {
    if (currentItem() && m_controller->removeLibraryClip(currentItem()->data(Qt::UserRole).toString().toStdString())) {
        refresh(); emit projectEdited();
    }
}
