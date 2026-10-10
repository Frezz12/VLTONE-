#include "TimelineWidget.hpp"
#include "EngineController.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

QVector<int> TimelineWidget::arrangementRows() const {
    QVector<int> result;
    if (ui::rulerShowsTime(m_rulerFormat)) result << 0;
    if (m_showChords) result << 1;
    if (m_showSections) result << 2;
    return result;
}

bool TimelineWidget::arrangementRowVisible(int row) const { return arrangementRows().contains(row); }

void TimelineWidget::setArrangementRowVisible(int row, bool visible) {
    if (row < 0 || row > 2 || arrangementRowVisible(row) == visible) return;
    if (row == 0) { emit timeRowVisibilityRequested(visible); return; }
    (row == 1 ? m_showChords : m_showSections) = visible;
    QSettings().setValue(row == 1 ? "timeline/showChords" : "timeline/showSections", visible);
    m_labelDragRow = -1;
    if (!visible && m_labelSelectionRow == row) { m_labelSelectionRow = -1; m_labelSelectionId.clear(); }
    m_staticFrameValid = false;
    layoutNavigationControls();
    clampVerticalScroll();
    emit arrangementRowsChanged();
    update();
}

int TimelineWidget::arrangementRowAt(int y) const {
    if (y < rulerHeight() || y >= tracksTop()) return -1;
    return arrangementRows().value((y - rulerHeight()) / ui::kArrangementRowHeight, -1);
}

const daw::ArrangementLabel* TimelineWidget::arrangementLabelAt(int row, int x) const {
    if (row < 1) return nullptr;
    const double beat = xToSeconds(x) * m_controller->tempo() / 60.0;
    const auto& values = row == 1 ? m_controller->project().chords : m_controller->project().sections;
    for (auto it = values.rbegin(); it != values.rend(); ++it)
        if (beat >= it->startBeats && beat < it->startBeats + it->durationBeats) return &*it;
    return nullptr;
}

void TimelineWidget::drawArrangementRows(QPainter& p) {
    const auto& theme = ThemeManager::instance().theme();
    const double secondsPerBeat = 60.0 / std::max(1.0, m_controller->tempo());
    int y = rulerHeight();
    for (const int row : arrangementRows()) {
        const QRect band(0, y, width(), ui::kArrangementRowHeight);
        p.save();
        p.setClipRect(band, Qt::IntersectClip);
        p.fillRect(band, theme.surface);
        p.setPen(theme.separator());
        p.drawLine(0, band.bottom(), width(), band.bottom());
        QFont font = p.font(); font.setPixelSize(11); p.setFont(font);
        if (row == 0) {
            const double raw = 75.0 / m_pixelsPerSecond;
            const double steps[] = {.1, .25, .5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
            double step = std::max(raw, 3600.0);
            for (double s : steps) if (s >= raw) { step = s; break; }
            const double last = xToSeconds(width());
            for (double at = std::max(0.0, std::floor(xToSeconds(0) / step) * step); at <= last; at += step) {
                const int x = secondsToX(at);
                p.setPen(theme.gridLineStrong); p.drawLine(x, y + 20, x, y + 26);
                p.setPen(theme.textSecondary);
                p.drawText(x + 5, y + 18, QString::asprintf("%d:%04.1f", int(at) / 60, std::fmod(at, 60.0)));
            }
        } else {
            const auto& values = row == 1 ? m_controller->project().chords : m_controller->project().sections;
            const QColor accent = row == 1 ? theme.accent : Theme::midiAccent();
            for (const auto& original : values) {
                const auto& label = m_labelDragRow == row && m_labelPreview.id == original.id ? m_labelPreview : original;
                const double rawLeft = (label.startBeats * secondsPerBeat - m_scrollSeconds) * m_pixelsPerSecond;
                const double rawRight = ((label.startBeats + label.durationBeats) * secondsPerBeat - m_scrollSeconds) * m_pixelsPerSecond;
                if (rawRight < 0 || rawLeft > width()) continue;
                const double x = std::clamp(rawLeft, -8.0, double(width()) + 8);
                const double right = std::clamp(rawRight, -8.0, double(width()) + 8);
                QRectF box(x + 1, y + 3, std::max(2.0, right - x - 2), ui::kArrangementRowHeight - 6);
                const bool selected = m_labelSelectionRow == row && m_labelSelectionId == QString::fromStdString(label.id);
                QColor fill = accent; fill.setAlpha(selected ? 65 : 35);
                p.setBrush(fill); p.setPen(QPen(accent, selected ? 2 : 1)); p.drawRoundedRect(box, 3, 3);
                p.setPen(theme.textPrimary);
                const auto text = QString::fromStdString(label.text);
                const QRectF caption = box.intersected(QRectF(band)).adjusted(6, 0, -5, 0);
                p.drawText(caption, Qt::AlignVCenter | Qt::AlignLeft,
                    p.fontMetrics().elidedText(text, Qt::ElideRight, std::max(0, int(caption.width()))));
            }
            if (values.empty()) {
                p.setPen(theme.textSecondary);
                p.drawText(band.adjusted(10, 0, -10, 0), Qt::AlignVCenter,
                    row == 1 ? tr("Double-click to add a chord") : tr("Double-click to add a section"));
            }
        }
        p.restore();
        y += ui::kArrangementRowHeight;
    }
}

void TimelineWidget::editArrangementLabel(int row, const QString& id, double seconds) {
    if (row < 1 || row > 2) return;
    if (m_controller->hasCloudProjectBinding() || !m_controller->sharedEditingAllowed()) {
        emit operationStatus(tr("Arrangement labels are available in local projects.")); return;
    }
    const auto revision = m_controller->projectRevision();
    auto values = row == 1 ? m_controller->project().chords : m_controller->project().sections;
    auto found = std::find_if(values.begin(), values.end(), [&](const auto& value) { return value.id == id.toStdString(); });
    const bool existing = found != values.end();
    const double bar = barLengthBeats();
    daw::ArrangementLabel value = existing ? *found : daw::ArrangementLabel{
        daw::newUuid(), row == 1 ? "C" : "Verse",
        std::floor(std::max(0.0, seconds < 0 ? m_controller->positionSeconds() : seconds) * m_controller->tempo() / 60.0 / bar) * bar,
        bar * (row == 1 ? 1.0 : 8.0)};
    QDialog dialog(this);
    dialog.setWindowTitle(row == 1 ? tr("Chord") : tr("Song section"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(QString::fromStdString(value.text), &dialog);
    name->setMaxLength(64);
    name->setPlaceholderText(row == 1 ? tr("e.g. Cmaj7, Am, G/B") : tr("e.g. Intro, Verse, Hook"));
    auto* start = new QDoubleSpinBox(&dialog); start->setRange(1, 1000000); start->setDecimals(3);
    auto* length = new QDoubleSpinBox(&dialog); length->setRange(.001, 1000000); length->setDecimals(3);
    start->setValue(value.startBeats / bar + 1); length->setValue(value.durationBeats / bar);
    name->setAccessibleName(tr("Name")); start->setAccessibleName(tr("Start bar")); length->setAccessibleName(tr("Length in bars"));
    form->addRow(tr("Name"), name); form->addRow(tr("Start bar"), start); form->addRow(tr("Length in bars"), length);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons); dialog.resize(350, dialog.sizeHint().height());
    name->selectAll(); name->setFocus();
    if (dialog.exec() != QDialog::Accepted || m_controller->projectRevision() != revision || name->text().trimmed().isEmpty()) return;
    value.text = name->text().trimmed().toStdString(); value.startBeats = (start->value() - 1) * bar;
    value.durationBeats = length->value() * bar;
    if (existing) *found = value; else values.push_back(value);
    if (m_controller->setArrangementLabels(row == 1, std::move(values))) {
        m_staticFrameValid = false; syncNavigationControls(); emit projectEdited(); update();
    }
}


void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    const int row = arrangementRowAt(event->position().y());
    if (row >= 1 && event->button() == Qt::LeftButton) {
        m_labelDragRow = -1;
        const auto* value = arrangementLabelAt(row, event->position().x());
        editArrangementLabel(row, value ? QString::fromStdString(value->id) : QString{}, xToSeconds(event->position().x()));
        event->accept(); return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

bool TimelineWidget::showArrangementMenu(QContextMenuEvent* event) {
    const int row = arrangementRowAt(event->pos().y());
    if (row < 0) return false;
    QMenu menu(this);
    const auto* label = arrangementLabelAt(row, event->pos().x());
    const QString id = label ? QString::fromStdString(label->id) : QString{};
    QAction* add = nullptr; QAction* edit = nullptr; QAction* remove = nullptr;
    if (row >= 1) {
        add = menu.addAction(row == 1 ? tr("Add chord…") : tr("Add section…"));
        if (label) { edit = menu.addAction(tr("Edit…")); remove = menu.addAction(tr("Delete")); }
        const bool editable = !m_controller->hasCloudProjectBinding() && m_controller->sharedEditingAllowed();
        for (auto* action : {add, edit, remove}) if (action) action->setEnabled(editable);
        menu.addSeparator();
    }
    auto* hide = menu.addAction(tr("Hide row"));
    const auto revision = m_controller->projectRevision();
    auto* chosen = menu.exec(event->globalPos());
    if (chosen == hide) setArrangementRowVisible(row, false);
    else if (chosen && m_controller->projectRevision() == revision) {
        if (chosen == add || chosen == edit) editArrangementLabel(row, chosen == edit ? id : QString{}, xToSeconds(event->pos().x()));
        else if (chosen == remove) {
            auto values = row == 1 ? m_controller->project().chords : m_controller->project().sections;
            std::erase_if(values, [&](const auto& value) { return value.id == id.toStdString(); });
            if (m_controller->setArrangementLabels(row == 1, std::move(values))) {
                m_staticFrameValid = false; syncNavigationControls(); emit projectEdited(); update();
            }
        }
    }
    event->accept(); return true;
}

double TimelineWidget::arrangementDuration() const {
    double end = m_controller->durationSeconds();
    for (const auto* values : {&m_controller->project().chords, &m_controller->project().sections})
        for (const auto& value : *values) end = std::max(end, (value.startBeats + value.durationBeats) * 60.0 / m_controller->tempo());
    return end;
}

// Return true for an active annotation row even if it is empty, so Delete can
// never fall through to an audio clip or track while this row has focus.
bool TimelineWidget::deleteSelectedArrangementLabel() {
    if (m_labelSelectionRow < 1) return false;
    m_labelDragRow = -1;
    auto values = m_labelSelectionRow == 1 ? m_controller->project().chords : m_controller->project().sections;
    std::erase_if(values, [&](const auto& value) { return value.id == m_labelSelectionId.toStdString(); });
    if (m_controller->setArrangementLabels(m_labelSelectionRow == 1, std::move(values))) {
        m_labelSelectionId.clear(); m_staticFrameValid = false;
        syncNavigationControls(); emit projectEdited(); update();
    }
    return true;
}
