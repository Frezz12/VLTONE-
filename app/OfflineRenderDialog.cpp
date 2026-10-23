#include "OfflineRenderDialog.hpp"

#include "ChannelStrip.hpp"
#include "ChannelStripPreset.hpp"
#include "ChannelStripPresets.hpp"
#include "PluginEditorWindow.hpp"
#include "SamplerPanel.hpp"
#include "Theme.hpp"
#include "Internal/EqualizerInstance.hpp"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPainter>
#include <QProxyStyle>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStyleFactory>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kPresetPathRole = Qt::UserRole + 1;
constexpr int kClipDurationRole = Qt::UserRole + 2;
constexpr int kClipActiveRole = Qt::UserRole + 3;
constexpr int kClipProgressRole = Qt::UserRole + 4;

class OfflineSelectionStyle final : public QProxyStyle {
public:
    explicit OfflineSelectionStyle(QWidget* owner)
        : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) { setParent(owner); }

    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override {
        if (metric == PM_IndicatorWidth || metric == PM_IndicatorHeight) return 24;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter, const QWidget* widget) const override {
        if (element != PE_IndicatorCheckBox && element != PE_IndicatorItemViewItemCheck) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        const bool marked = option->state & (State_On | State_NoChange);
        const QPointF center = QRectF(option->rect).center();
        const QRectF box(center.x() - 8, center.y() - 8, 16, 16);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (!(option->state & State_Enabled)) painter->setOpacity(0.45);
        painter->setPen(QPen(marked ? th().accent : th().textSecondary, 1.0));
        painter->setBrush(marked ? th().accent : th().background);
        painter->drawRoundedRect(box, 3, 3);
        painter->setPen(QPen(th().accentText(), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (option->state & State_NoChange) {
            painter->drawLine(center + QPointF(-3, 0), center + QPointF(3, 0));
        } else if (option->state & State_On) {
            const QPointF tick[]{center + QPointF(-4, 0), center + QPointF(-1, 3), center + QPointF(4, -3)};
            painter->drawPolyline(tick, 3);
        }
        painter->restore();
    }
};

class OfflineClipDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        if (index.column() != 0) {
            QStyledItemDelegate::paint(painter, option, index);
            if (index.column() == 2 && index.data(kClipActiveRole).toBool()) {
                painter->save();
                const QRectF bar = QRectF(option.rect).adjusted(12, option.rect.height() - 9, -12, -6);
                painter->fillRect(bar, th().separator());
                painter->fillRect(QRectF(bar.topLeft(), QSizeF(bar.width() *
                    index.data(kClipProgressRole).toDouble(), bar.height())), th().accent);
                painter->restore();
            }
            return;
        }
        const QString text = index.data(Qt::DisplayRole).toString();
        const QString title = text.section(QLatin1Char('\n'), 0, 0);
        const QString detail = text.section(QLatin1Char('\n'), 1);
        item.text.clear();
        const auto* style = item.widget ? item.widget->style() : QApplication::style();
        painter->save();
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, item.widget);
        const auto rect = style->subElementRect(QStyle::SE_ItemViewItemText, &item, item.widget)
                              .adjusted(4, 0, -10, 0);
        const auto textRect = rect;
        QFont titleFont = item.font;
        titleFont.setWeight(QFont::DemiBold);
        painter->setFont(titleFont);
        painter->setPen(th().textPrimary);
        const int lineHeight = item.fontMetrics.height();
        painter->drawText(QRect(textRect.left(), rect.center().y() - lineHeight,
                               textRect.width(), lineHeight), Qt::AlignVCenter,
                         QFontMetrics(titleFont).elidedText(title, Qt::ElideRight, textRect.width()));
        painter->setFont(item.font);
        painter->setPen(th().textSecondary);
        painter->drawText(QRect(textRect.left(), rect.center().y() + 2, textRect.width(), lineHeight),
                         Qt::AlignVCenter, item.fontMetrics.elidedText(detail, Qt::ElideRight, textRect.width()));
        painter->restore();
    }
};

QString elapsedText(qint64 milliseconds) {
    const auto seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

/// The preset shelf behaves like a desktop collection: one click applies a
/// card, while the same card can be dragged onto the Audio FX rack. File URLs
/// deliberately reuse ChannelStrip's existing preset drop path.
class OfflinePresetList final : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        auto* data = new QMimeData;
        QList<QUrl> urls;
        for (const QListWidgetItem* item : items) {
            const QString path = item->data(kPresetPathRole).toString();
            if (!path.isEmpty()) urls.push_back(QUrl::fromLocalFile(path));
        }
        data->setUrls(urls);
        return data;
    }

    Qt::DropActions supportedDropActions() const override {
        return Qt::CopyAction;
    }
};

} // namespace

OfflineRenderDialog::OfflineRenderDialog(
    daw::EngineController& controller,
    std::vector<daw::EngineController::ClipAddress> clips,
    QWidget* parent)
    : QDialog(parent), m_controller(controller), m_clips(std::move(clips)),
      m_scratch() {
    setWindowTitle(tr("Offline Render"));
    setModal(true);
    setObjectName(QStringLiteral("OfflineRenderDialog"));
    resize(980, 640);
    setMinimumSize(820, 560);

    const audio::Result ready =
        m_scratch.initialize(controller.sampleRate(),
                             controller.bufferSizeFrames(), false);
    if (ready) m_chainTrackId = m_scratch.addTrack(daw::TrackKind::Audio,
                                                   "Offline Processing");
    m_scratch.setPluginRetiringCallback(
        [this](const std::string& channelId, const std::string& slotId) {
            const QString channel = QString::fromStdString(channelId);
            const QString slot = QString::fromStdString(slotId);
            if (channel != QString::fromStdString(m_chainTrackId)) return;
            QDialog* window = m_editors.value(slot);
            if (!window) return;
            for (PluginEditorWindow* editor :
                 window->findChildren<PluginEditorWindow*>())
                editor->detachFromPlugin();
            window->close();
        });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("OfflineHeader"));
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(24, 20, 24, 18);
    headerLayout->setSpacing(6);
    auto* title = new QLabel(tr("Render clips"), header);
    title->setObjectName(QStringLiteral("OfflineRenderTitle"));
    headerLayout->addWidget(title);
    auto* description = new QLabel(tr("One effect chain. A separate result for every clip."), header);
    description->setObjectName(QStringLiteral("OfflineRenderDescription"));
    description->setWordWrap(true);
    headerLayout->addWidget(description);
    root->addWidget(header);

    auto* content = new QHBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    auto* clipsBox = new QWidget(this);
    auto* clipsLayout = new QVBoxLayout(clipsBox);
    clipsLayout->setContentsMargins(24, 20, 24, 20);
    clipsLayout->setSpacing(12);
    auto* clipsHeading = new QHBoxLayout;
    auto* clipsTitle = new QLabel(tr("Audio clips"), clipsBox);
    clipsTitle->setProperty("offlineSection", true);
    clipsHeading->addWidget(clipsTitle);
    clipsHeading->addStretch();
    m_clipSummary = new QLabel(clipsBox);
    m_clipSummary->setObjectName(QStringLiteral("OfflineClipSummary"));
    m_clipSummary->setAccessibleName(tr("Render selection"));
    clipsHeading->addWidget(m_clipSummary);
    clipsLayout->addLayout(clipsHeading);

    auto* selectionRow = new QHBoxLayout;
    m_selectAll = new QCheckBox(tr("Select all"), clipsBox);
    m_selectAll->setObjectName(QStringLiteral("OfflineSelectAll"));
    m_selectAll->setMinimumHeight(28);
    m_selectAll->setToolTip(tr("Select or deselect all clips"));
    m_selectAll->setChecked(true);
    // Selection uses a checkbox; the application's switch style means on/off.
    m_selectAll->setStyle(new OfflineSelectionStyle(m_selectAll));
    selectionRow->addWidget(m_selectAll);
    selectionRow->addStretch();
    m_selectionDuration = new QLabel(clipsBox);
    m_selectionDuration->setObjectName(QStringLiteral("OfflineSelectionDuration"));
    selectionRow->addWidget(m_selectionDuration);
    clipsLayout->addLayout(selectionRow);

    m_clipList = new QTreeWidget(clipsBox);
    m_clipList->setAccessibleName(tr("Clips to process"));
    m_clipList->setObjectName(QStringLiteral("OfflineRenderClips"));
    m_clipList->setHeaderLabels({tr("Clip / Track"), tr("Length"), tr("Status")});
    m_clipList->headerItem()->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    m_clipList->headerItem()->setTextAlignment(2, Qt::AlignCenter);
    m_clipList->setRootIsDecorated(false);
    m_clipList->setIndentation(0);
    m_clipList->setItemsExpandable(false);
    m_clipList->setSelectionMode(QAbstractItemView::NoSelection);
    m_clipList->setItemDelegate(new OfflineClipDelegate(m_clipList));
    m_clipList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_clipList->setTextElideMode(Qt::ElideRight);
    m_clipList->setUniformRowHeights(true);
    m_clipList->header()->setStretchLastSection(false);
    m_clipList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_clipList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_clipList->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_clipList->header()->resizeSection(2, 136);
    m_clipList->setStyle(new OfflineSelectionStyle(m_clipList));
    for (const auto& address : m_clips) {
        const daw::TrackModel* track = m_controller.project().findTrack(address.trackId);
        if (!track) continue;
        const auto clip = std::find_if(track->clips.begin(), track->clips.end(),
            [&](const daw::ClipModel& item) { return item.id == address.clipId; });
        if (clip == track->clips.end()) continue;
        QString clipName = QString::fromStdString(clip->name);
        if (clipName.isEmpty())
            clipName = QFileInfo(QString::fromStdString(clip->filePath)).completeBaseName();
        if (clipName.isEmpty()) clipName = tr("Audio clip");
        const double duration = m_controller.clipPlaybackDuration(*clip);
        const double end = clip->startSeconds + duration;
        auto* item = new QTreeWidgetItem(m_clipList);
        item->setText(0, tr("%1\n%2 · %3–%4 s")
            .arg(clipName, QString::fromStdString(track->name))
            .arg(clip->startSeconds, 0, 'f', 3).arg(end, 0, 'f', 3));
        item->setText(1, tr("%1 s").arg(duration, 0, 'f', 3));
        item->setText(2, tr("Ready"));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(2, Qt::AlignCenter);
        item->setData(0, Qt::UserRole, int(&address - m_clips.data()));
        item->setData(0, kClipDurationRole, duration);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Checked);
        item->setSizeHint(0, QSize(0, std::max(58, fontMetrics().height() * 2 + 20)));
        item->setToolTip(0, item->text(0));
    }
    m_clipList->setMinimumHeight(180);
    clipsLayout->addWidget(m_clipList, 1);
    content->addWidget(clipsBox, 1);

    auto* effectsScroll = new QScrollArea(this);
    effectsScroll->setObjectName(QStringLiteral("OfflineEffectsScroll"));
    effectsScroll->setWidgetResizable(true);
    effectsScroll->setFixedWidth(320);
    effectsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* effects = new QWidget(effectsScroll);
    effects->setObjectName(QStringLiteral("OfflineEffectsColumn"));
    auto* effectsLayout = new QVBoxLayout(effects);
    effectsLayout->setContentsMargins(20, 20, 20, 20);
    effectsLayout->setSpacing(12);
    auto* effectsTitle = new QLabel(tr("Effect chain"), effects);
    effectsTitle->setProperty("offlineSection", true);
    effectsLayout->addWidget(effectsTitle);
    auto* effectsHint = new QLabel(tr("Applied to every checked clip"), effects);
    effectsHint->setProperty("offlineHint", true);
    effectsHint->setWordWrap(true);
    effectsLayout->addWidget(effectsHint);

    m_rackHost = new QWidget(effects);
    m_rackHost->setAccessibleName(tr("Offline processing inserts"));
    m_rackLayout = new QVBoxLayout(m_rackHost);
    m_rackLayout->setContentsMargins(0, 0, 0, 0);
    m_rackLayout->setAlignment(Qt::AlignTop);
    effectsLayout->addWidget(m_rackHost);

    auto* presetHeader = new QHBoxLayout;
    auto* presetTitle = new QLabel(tr("Presets"), effects);
    presetTitle->setProperty("offlineSection", true);
    m_savePreset = new QPushButton(tr("Save…"), effects);
    m_savePreset->setObjectName(QStringLiteral("OfflinePresetSave"));
    m_savePreset->setAutoDefault(false);
    m_savePreset->setMinimumHeight(28);
    presetHeader->addWidget(presetTitle);
    presetHeader->addStretch();
    presetHeader->addWidget(m_savePreset);
    effectsLayout->addLayout(presetHeader);
    m_presets = new OfflinePresetList(effects);
    m_presets->setObjectName(QStringLiteral("OfflinePresetCards"));
    m_presets->setAccessibleName(tr("Offline render presets"));
    m_presets->setViewMode(QListView::IconMode);
    m_presets->setFlow(QListView::LeftToRight);
    m_presets->setMovement(QListView::Static);
    m_presets->setResizeMode(QListView::Adjust);
    m_presets->setWrapping(true);
    m_presets->setWordWrap(true);
    m_presets->setUniformItemSizes(true);
    m_presets->setGridSize(QSize(124, 36));
    m_presets->setSpacing(2);
    m_presets->setDragEnabled(true);
    m_presets->setDragDropMode(QAbstractItemView::DragOnly);
    m_presets->setDefaultDropAction(Qt::CopyAction);
    m_presets->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_presets->setFixedHeight(80);
    effectsLayout->addWidget(m_presets);

    auto* optionsTitle = new QLabel(tr("Render options"), effects);
    optionsTitle->setProperty("offlineSection", true);
    effectsLayout->addWidget(optionsTitle);
    m_includeTail = new QCheckBox(tr("Include effect tails"), effects);
    m_includeTail->setObjectName(QStringLiteral("OfflineIncludeTail"));
    m_includeTail->setMinimumHeight(28);
    QSettings settings;
    m_includeTail->setChecked(settings.value(QStringLiteral("offlineRender/includeTail"), false).toBool());
    m_includeTail->setToolTip(tr("Keep the decay of reverb and delay after each clip (up to 30 seconds)."));
    effectsLayout->addWidget(m_includeTail);
    m_autoRender = new QPushButton(tr("Auto render on changes"), effects);
    m_autoRender->setObjectName(QStringLiteral("OfflineAutoRender"));
    m_autoRender->setAccessibleName(tr("Auto Render"));
    m_autoRender->setToolTip(tr("Render the selected clips whenever the offline effect chain changes"));
    m_autoRender->setCheckable(true);
    m_autoRender->setAutoDefault(false);
    m_autoRender->setMinimumHeight(32);
    effectsLayout->addWidget(m_autoRender);
    effectsLayout->addStretch();
    effectsScroll->setWidget(effects);
    content->addWidget(effectsScroll);
    root->addLayout(content, 1);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("OfflineRenderProgress"));
    m_progress->setAccessibleName(tr("Offline render progress"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setFixedHeight(3);
    m_progress->setTextVisible(false);
    root->addWidget(m_progress);

    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("OfflineFooter"));
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(24, 16, 24, 18);
    footerLayout->setSpacing(16);
    auto* statusLayout = new QVBoxLayout;
    statusLayout->setSpacing(5);
    m_status = new QLabel(tr("Ready"), footer);
    m_status->setObjectName(QStringLiteral("OfflineRenderStatus"));
    m_status->setAccessibleName(tr("Offline render status"));
    m_status->setWordWrap(true);
    statusLayout->addWidget(m_status);
    m_renderDetails = new QLabel(tr("Original audio stays in History."), footer);
    m_renderDetails->setObjectName(QStringLiteral("OfflineRenderDetails"));
    m_renderDetails->setWordWrap(true);
    statusLayout->addWidget(m_renderDetails);
    footerLayout->addLayout(statusLayout, 1);
    m_progressPercent = new QLabel(QStringLiteral("0%"), footer);
    m_progressPercent->setObjectName(QStringLiteral("OfflineProgressPercent"));
    m_progressPercent->setAccessibleName(tr("Offline render percentage"));
    m_progressPercent->setMinimumWidth(56);
    m_progressPercent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_progressPercent->hide();
    footerLayout->addWidget(m_progressPercent);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, footer);
    m_closeButton = m_buttons->button(QDialogButtonBox::Cancel);
    m_closeButton->setObjectName(QStringLiteral("OfflineRenderClose"));
    m_closeButton->setText(tr("Close"));
    m_closeButton->setMinimumHeight(38);
    m_renderButton = m_buttons->addButton(tr("Render clips"), QDialogButtonBox::AcceptRole);
    m_renderButton->setObjectName(QStringLiteral("OfflineRenderStart"));
    m_renderButton->setDefault(true);
    m_renderButton->setMinimumSize(150, 38);
    footerLayout->addWidget(m_buttons);
    root->addWidget(footer);
    setTabOrder(m_selectAll, m_clipList);
    setTabOrder(m_clipList, m_savePreset);
    setTabOrder(m_savePreset, m_presets);
    setTabOrder(m_presets, m_includeTail);
    setTabOrder(m_includeTail, m_autoRender);
    setTabOrder(m_autoRender, m_renderButton);
    setTabOrder(m_renderButton, m_closeButton);

    connect(m_savePreset, &QPushButton::clicked, this,
            &OfflineRenderDialog::savePreset);
    const auto applyCard = [this](QListWidgetItem* item) {
        if (item) loadPreset(item->data(kPresetPathRole).toString());
    };
    connect(m_presets, &QListWidget::itemClicked, this, applyCard);
    connect(m_presets, &QListWidget::itemActivated, this, applyCard);
    connect(m_autoRender, &QPushButton::toggled, this, [this](bool on) {
        if (on) scheduleAutoRender();
    });
    connect(m_renderButton, &QPushButton::clicked, this,
            &OfflineRenderDialog::startRender);
    connect(m_buttons, &QDialogButtonBox::rejected, this,
            &OfflineRenderDialog::reject);
    connect(m_clipList, &QTreeWidget::itemChanged, this,
            &OfflineRenderDialog::updateRenderAvailability);
    connect(m_selectAll, &QCheckBox::clicked, this, [this](bool checked) {
        {
            const QSignalBlocker block(m_clipList);
            for (int row = 0; row < m_clipList->topLevelItemCount(); ++row)
                m_clipList->topLevelItem(row)->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        }
        updateRenderAvailability();
    });

    m_autoRenderTimer = new QTimer(this);
    m_autoRenderTimer->setSingleShot(true);
    connect(m_autoRenderTimer, &QTimer::timeout, this,
            &OfflineRenderDialog::startRender);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &OfflineRenderDialog::applyTheme);

    reloadPresets();
    rebuildRack();
    updateRenderAvailability();
    applyTheme();
    if (!ready || m_chainTrackId.empty()) {
        m_renderButton->setEnabled(false);
        m_autoRender->setEnabled(false);
        m_status->setText(tr("Could not create the offline plugin rack"));
    }
}

void OfflineRenderDialog::reject() {
    if (m_rendering) {
        m_cancelRequested = true;
        m_status->setText(tr("Cancelling…"));
        m_closeButton->setEnabled(false);
        return;
    }
    closeEditors();
    QDialog::reject();
}

OfflineRenderDialog::~OfflineRenderDialog() {
    closeEditors();
    m_scratch.setPluginRetiringCallback({});
    m_scratch.shutdown();
}

void OfflineRenderDialog::rebuildRack() {
    if (m_rack) {
        m_rackLayout->removeWidget(m_rack);
        m_rack->deleteLater();
        m_rack = nullptr;
    }
    if (m_chainTrackId.empty()) return;
    m_rack = new ChannelStrip(&m_scratch,
                              QString::fromStdString(m_chainTrackId), false,
                              m_rackHost, true);
    m_rack->setInspectorCompact(true);
    m_rack->setEnabled(!m_rendering);
    m_rackLayout->addWidget(m_rack);
    QWidget* previous = m_clipList;
    for (auto* button : m_rack->findChildren<QAbstractButton*>()) {
        if (button->focusPolicy() == Qt::NoFocus) continue;
        setTabOrder(previous, button);
        previous = button;
    }
    setTabOrder(previous, m_savePreset);
    connect(m_rack, &ChannelStrip::editorRequested, this,
            [this](const QString&, const QString& insertId) {
                openEditor(insertId);
            });
    connect(m_rack, &ChannelStrip::structureChanged, this,
            [this] { rackChanged(false); }, Qt::QueuedConnection);
    updateRenderAvailability();
}

void OfflineRenderDialog::rackChanged(bool settle) {
    rebuildRack();
    scheduleAutoRender(settle ? 240 : 0);
}

void OfflineRenderDialog::scheduleAutoRender(int delayMs) {
    if (!m_autoRender || !m_autoRender->isChecked()) return;
    if (m_rendering) {
        m_autoRenderPending = true;
        return;
    }
    updateRenderAvailability();
    if (!m_renderButton->isEnabled()) return;
    m_autoRenderTimer->start(std::max(0, delayMs));
}

void OfflineRenderDialog::updateRenderAvailability() {
    int checked = 0;
    double seconds = 0.0;
    for (int row = 0; row < m_clipList->topLevelItemCount(); ++row) {
        const auto* item = m_clipList->topLevelItem(row);
        if (item->checkState(0) != Qt::Checked) continue;
        ++checked;
        seconds += item->data(0, kClipDurationRole).toDouble();
    }
    m_clipSummary->setText(
        tr("%1 of %2 selected").arg(checked).arg(m_clipList->topLevelItemCount()));
    m_selectionDuration->setText(tr("%1 s selected").arg(seconds, 0, 'f', 3));
    const QSignalBlocker block(m_selectAll);
    m_selectAll->setCheckState(checked == 0 ? Qt::Unchecked :
        checked == m_clipList->topLevelItemCount() ? Qt::Checked : Qt::PartiallyChecked);
    const auto* inserts = m_scratch.channelInserts(m_chainTrackId);
    const bool hasInserts = inserts && !inserts->empty();
    const bool enabled = inserts && std::any_of(inserts->begin(), inserts->end(),
        [](const auto& slot) { return !slot.bypassed; });
    m_renderButton->setEnabled(!m_rendering && checked > 0 && enabled);
    m_renderButton->setText(tr("Render clips (%1)").arg(checked));
    m_savePreset->setEnabled(!m_rendering && hasInserts);
    m_renderButton->setAccessibleName(
        tr("Render to Clips — %1 selected").arg(checked));
    if (!m_rendering)
        m_status->setText(!checked ? tr("Check the clips to render")
                                  : enabled ? tr("Ready to render")
                                            : tr("Add or enable an effect to render"));
}

void OfflineRenderDialog::openEditor(const QString& insertId) {
    if (insertId.isEmpty()) return;
    if (QDialog* open = m_editors.value(insertId)) {
        open->show();
        open->raise();
        open->activateWindow();
        return;
    }
    if (!m_scratch.hasInsert(m_chainTrackId, insertId.toStdString())) {
        QMessageBox::information(this, tr("Offline Render"),
                                 tr("This plugin is not available."));
        return;
    }

    const auto* model = m_scratch.insertModel(m_chainTrackId,
                                               insertId.toStdString());
    auto* window = new QDialog(this);
    window->setObjectName(QStringLiteral("OfflinePluginEditorWindow"));
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowModality(Qt::NonModal);
    window->setWindowTitle(model ? QString::fromStdString(model->name)
                                 : tr("Plugin"));
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* editor = new PluginEditorWindow(
        &m_scratch, QString::fromStdString(m_chainTrackId), insertId, window);
    layout->addWidget(editor);
    editor->installEventFilter(this);
    window->resize(720, 480);
    m_editors.insert(insertId, window);
    connect(window, &QDialog::finished, this, [this, insertId, editor] {
        editor->detachFromPlugin();
        m_editors.remove(insertId);
        QTimer::singleShot(0, this, &OfflineRenderDialog::rebuildRack);
    });
    connect(editor, &PluginEditorWindow::closing, window, &QDialog::reject);
    connect(editor, &PluginEditorWindow::projectEdited, this,
            [this] { rackChanged(true); }, Qt::QueuedConnection);
    connect(editor, &PluginEditorWindow::nestedPluginEditorRequested, this,
            [this](const QString&, const QString& id) { openEditor(id); });
    editor->prepareNativeHostHierarchy();
    window->show();
    editor->initializeEditor();
}

bool OfflineRenderDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize) {
        if (auto* editor = qobject_cast<PluginEditorWindow*>(watched)) {
            if (auto* window = qobject_cast<QDialog*>(editor->parentWidget())) {
                const QSize requested =
                    static_cast<QResizeEvent*>(event)->size();
                if (editor->isEmbedded() && !editor->canResizeNativeEditor()) {
                    window->setFixedSize(requested);
                } else {
                    window->resize(requested);
                }
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void OfflineRenderDialog::closeEditors() {
    const auto windows = m_editors.values();
    m_editors.clear();
    for (const QPointer<QDialog>& window : windows) {
        if (!window) continue;
        window->disconnect(this);
        for (PluginEditorWindow* editor :
             window->findChildren<PluginEditorWindow*>()) {
            editor->disconnect(this);
            editor->detachFromPlugin();
        }
        delete window;
    }
}

void OfflineRenderDialog::reloadPresets() {
    const QString current = m_presets->currentItem()
                                ? m_presets->currentItem()
                                      ->data(kPresetPathRole)
                                      .toString()
                                : QString();
    m_presets->clear();
    for (const QString& file : ui::channelstrippresets::offlineFiles()) {
        daw::EngineController::ChannelSnapshot preset;
        const audio::Result loaded =
            daw::ChannelStripPreset::load(preset, file.toStdString());
        QString name = loaded
                           ? QString::fromStdString(preset.sourceName).trimmed()
                           : QString();
        if (name.isEmpty() && loaded && !preset.inserts.empty())
            name = QString::fromStdString(preset.inserts.front().model.name);
        if (name.isEmpty())
            name = ui::channelstrippresets::displayName(file);
        auto* item = new QListWidgetItem(name, m_presets);
        item->setData(kPresetPathRole, file);
        item->setTextAlignment(Qt::AlignCenter);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                       Qt::ItemIsDragEnabled);
        QStringList plugins;
        if (loaded) {
            for (const auto& slot : preset.inserts)
                plugins << QString::fromStdString(slot.model.name);
        }
        item->setToolTip(
            plugins.isEmpty()
                ? tr("Click to load. Drag onto Audio FX.")
                : tr("%1\n\nClick to load. Drag onto Audio FX.")
                      .arg(plugins.join(QStringLiteral(" · "))));
    }
    if (m_presets->count() == 0) {
        auto* empty = new QListWidgetItem(tr("No saved presets"), m_presets);
        empty->setTextAlignment(Qt::AlignCenter);
        empty->setFlags(Qt::NoItemFlags);
    }
    m_presets->setFixedHeight(m_presets->count() > 2 ? 80 : 44);
    for (int row = 0; row < m_presets->count(); ++row) {
        if (m_presets->item(row)->data(kPresetPathRole).toString() == current) {
            m_presets->setCurrentRow(row);
            break;
        }
    }
}

void OfflineRenderDialog::loadPreset(const QString& path) {
    if (path.isEmpty()) return;
    daw::EngineController::ChannelSnapshot preset;
    const audio::Result result =
        daw::ChannelStripPreset::load(preset, path.toStdString());
    if (!result) {
        QMessageBox::critical(this, tr("Load preset"),
                              QString::fromStdString(result.message()));
        return;
    }
    if (!m_scratch.pasteChannelInserts(m_chainTrackId, preset)) {
        QMessageBox::warning(this, tr("Load preset"),
                             tr("This preset has no plugins to load."));
        return;
    }
    rackChanged(false);
}

void OfflineRenderDialog::savePreset() {
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("Save offline chain"), tr("Preset name"), QLineEdit::Normal,
        QString(), &accepted);
    if (!accepted) return;
    const QString path =
        ui::channelstrippresets::offlineFilePathForName(name);
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("Save preset"),
                             tr("Enter a valid preset name."));
        return;
    }
    auto snapshot = m_scratch.copyChannelStrip(m_chainTrackId, true);
    snapshot.sourceName = name.toStdString();
    const audio::Result result =
        daw::ChannelStripPreset::save(snapshot, path.toStdString());
    if (!result) {
        QMessageBox::critical(this, tr("Save preset"),
                              QString::fromStdString(result.message()));
        return;
    }
    reloadPresets();
    for (int row = 0; row < m_presets->count(); ++row) {
        if (m_presets->item(row)->data(kPresetPathRole).toString() == path) {
            m_presets->setCurrentRow(row);
            break;
        }
    }
}

void OfflineRenderDialog::startRender() {
    if (m_rendering || !m_renderButton->isEnabled()) return;
    m_autoRenderTimer->stop();
    m_autoRenderPending = false;
    std::vector<daw::EngineController::ClipAddress> checked;
    std::vector<int> rows;
    for (int row = 0; row < m_clipList->topLevelItemCount(); ++row) {
        const auto* item = m_clipList->topLevelItem(row);
        if (item->checkState(0) != Qt::Checked) continue;
        checked.push_back(m_clips.at(std::size_t(item->data(0, Qt::UserRole).toInt())));
        rows.push_back(row);
    }
    QSettings settings;
    settings.setValue(QStringLiteral("offlineRender/includeTail"),
                      m_includeTail->isChecked());
    m_rendering = true;
    m_cancelRequested = false;
    QElapsedTimer elapsed;
    elapsed.start();
    m_progress->setValue(0);
    m_progressPercent->show();
    m_progressPercent->setText(QStringLiteral("0%"));
    m_renderButton->setEnabled(false);
    if (m_rack) m_rack->setEnabled(false);
    for (PluginEditorWindow* editor : findChildren<PluginEditorWindow*>())
        editor->setEnabled(false);
    m_presets->setEnabled(false);
    m_savePreset->setEnabled(false);
    m_autoRender->setEnabled(false);
    m_includeTail->setEnabled(false);
    m_selectAll->setEnabled(false);
    {
        const QSignalBlocker block(m_clipList);
        for (int row = 0; row < m_clipList->topLevelItemCount(); ++row) {
            auto* item = m_clipList->topLevelItem(row);
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            item->setText(2, item->checkState(0) == Qt::Checked ? tr("Queued") : tr("Skipped"));
        }
    }
    m_closeButton->setText(tr("Cancel"));
    m_status->setText(tr("Rendering…"));

    const auto chain = m_scratch.copyChannelStrip(m_chainTrackId, false);
    daw::EngineController::OfflineRenderReport report;
    const audio::Result result = m_controller.renderClipsOffline(
        checked, chain, m_includeTail->isChecked(),
        [this, &report, &rows, &elapsed](const daw::rendering::Progress& progress) {
            using State = daw::EngineController::OfflineRenderReport::ClipProgress::State;
            {
                const QSignalBlocker block(m_clipList);
                for (std::size_t i = 0; i < report.clips.size() && i < rows.size(); ++i) {
                    const auto& clip = report.clips[i];
                    QString state;
                    switch (clip.state) {
                    case State::Pending: state = tr("Queued"); break;
                    case State::Preparing: state = tr("Preparing…"); break;
                    case State::Rendering: state = tr("Rendering · %1%").arg(int(clip.fraction * 100)); break;
                    case State::Complete: state = tr("Rendered"); break;
                    case State::Failed: state = tr("Failed"); break;
                    }
                    auto* item = m_clipList->topLevelItem(rows[i]);
                    item->setText(2, state);
                    item->setData(2, kClipProgressRole, clip.fraction);
                    item->setData(2, kClipActiveRole, clip.state == State::Preparing || clip.state == State::Rendering);
                    item->setData(0, Qt::AccessibleDescriptionRole, state);
                }
            }
            m_clipSummary->setText(tr("%1 / %2 complete").arg(report.completedClips).arg(report.clipCount));
            const int percentage = std::clamp(int(std::round(progress.fraction * 100.0)),
                0, report.completedClips == report.clipCount ? 100 : 99);
            m_progress->setValue(percentage);
            m_progressPercent->setText(QString::number(percentage) +
                                       QLatin1Char('%'));
            if (!m_cancelRequested)
                m_status->setText(report.completedClips == report.clipCount ? tr("Applying results…") :
                    progress.stage == daw::rendering::Progress::Stage::Preparing
                    ? tr("Preparing audio and effects…") : tr("Rendering clips…"));
            m_renderDetails->setText(report.workerCount > 1
                ? tr("%1 elapsed · Up to %2 clips at once").arg(elapsedText(elapsed.elapsed())).arg(report.workerCount)
                : tr("%1 elapsed").arg(elapsedText(elapsed.elapsed())));
            QApplication::processEvents();
            return !m_cancelRequested;
        },
        report);
    m_rendering = false;
    if (m_rack) m_rack->setEnabled(true);
    for (PluginEditorWindow* editor : findChildren<PluginEditorWindow*>())
        editor->setEnabled(true);
    m_presets->setEnabled(true);
    m_autoRender->setEnabled(true);
    m_includeTail->setEnabled(true);
    m_selectAll->setEnabled(true);
    {
        const QSignalBlocker block(m_clipList);
        std::size_t index = 0;
        for (int row = 0; row < m_clipList->topLevelItemCount(); ++row) {
            auto* item = m_clipList->topLevelItem(row);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setData(2, kClipActiveRole, false);
            if (item->checkState(0) == Qt::Checked) {
                const bool failed = index < report.clips.size() && report.clips[index].state ==
                    daw::EngineController::OfflineRenderReport::ClipProgress::State::Failed;
                ++index;
                const QString state = failed ? tr("Failed") :
                    result && !report.cancelled && !m_cancelRequested ? tr("Done") : tr("Not applied");
                item->setText(2, state);
                item->setData(0, Qt::AccessibleDescriptionRole, state);
            }
        }
    }
    m_closeButton->setEnabled(true);
    m_closeButton->setText(tr("Close"));
    updateRenderAvailability();
    m_renderDetails->setText(tr("%1 elapsed").arg(elapsedText(elapsed.elapsed())));
    const bool rerender = m_autoRenderPending;
    m_autoRenderPending = false;
    if (report.cancelled || m_cancelRequested) {
        m_status->setText(tr("Cancelled — the project was not changed"));
        return;
    }
    if (!result) {
        m_status->setText(tr("Offline render failed"));
        QMessageBox::critical(this, tr("Offline Render"),
                              QString::fromStdString(result.message()));
        return;
    }
    m_progress->setValue(100);
    m_progressPercent->setText(QStringLiteral("100%"));
    m_status->setText(report.clipCount == 1 ? tr("Done — clip rendered") :
                     tr("Done — %1 clips rendered").arg(report.clipCount));
    m_renderDetails->setText(tr("%1 elapsed · Original audio is available in History").arg(elapsedText(elapsed.elapsed())));
    m_rendered = true;
    if (rerender) scheduleAutoRender();
}

void OfflineRenderDialog::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#OfflineRenderDialog { background: %BG%; color: %TEXT%; }
#OfflineHeader { border-bottom: 1px solid %SEP%; }
#OfflineRenderTitle { color: %TEXT%; font-size: 24px; font-weight: 600; }
QLabel[offlineSection="true"] { color: %TEXT%; font-size: 15px; font-weight: 600; }
QLabel[offlineHint="true"], #OfflineRenderDescription, #OfflineRenderDetails,
#OfflineClipSummary, #OfflineSelectionDuration { color: %TEXT2%; }
#OfflineEffectsColumn, #OfflineEffectsScroll { background: %SURFACE%; }
#OfflineEffectsScroll { border: none; border-left: 1px solid %SEP%; }
#OfflineFooter { background: %SURFACE%; }
#OfflineRenderStatus { color: %TEXT%; font-weight: 600; }
#OfflineProgressPercent { color: %TEXT%; font-size: 22px; font-weight: 600; }
#OfflineRenderProgress { background: %SEP%; border: none; border-radius: 0; }
#OfflineRenderProgress::chunk { background: %ACCENT%; border-radius: 0; }
#OfflineSelectAll, #OfflineIncludeTail { color: %TEXT%; padding: 2px; border: 1px solid transparent; border-radius: 3px; }
#OfflineSelectAll:focus, #OfflineIncludeTail:focus { border-color: %ACCENT%; }
#OfflineRenderClips { background: %BG%; color: %TEXT%; border: 1px solid %SEP%; border-radius: 4px; }
#OfflineRenderClips::item { border: none; border-bottom: 1px solid %SEP%; padding: 4px 8px; }
#OfflineRenderClips::item:hover { background: %HOVER%; }
#OfflineRenderClips:focus { border-color: %ACCENT%; }
#OfflineRenderClips QHeaderView { background: %SURFACE%; }
#OfflineRenderClips QHeaderView::section {
    background: %SURFACE%; color: %TEXT2%; border: none; border-bottom: 1px solid %SEP%;
    padding: 9px 10px; font-weight: 500;
}
#OfflinePresetCards { background: transparent; color: %TEXT%; border: none; }
#OfflinePresetCards::item {
    background: %BG%; color: %TEXT%; border: 1px solid %SEP%;
    border-radius: 4px; padding: 4px; margin: 2px;
}
#OfflinePresetCards::item:hover { background: %HOVER%; border-color: %TEXT2%; }
#OfflinePresetCards::item:selected { background: %SELECTED%; border-color: %ACCENT%; }
#OfflinePresetCards::item:disabled { background: transparent; color: %TEXT2%; border: none; }
#OfflinePresetSave, #OfflineAutoRender, #OfflineRenderStart, #OfflineRenderClose {
    background: %BG%; color: %TEXT%; border: 1px solid %SEP%; border-radius: 5px; padding: 2px 12px;
}
#OfflinePresetSave { background: transparent; color: %TEXT2%; }
#OfflinePresetSave:hover, #OfflineAutoRender:hover, #OfflineRenderClose:hover { background: %HOVER%; }
#OfflinePresetSave:pressed, #OfflineAutoRender:pressed, #OfflineRenderClose:pressed { background: %SELECTED%; }
#OfflineAutoRender:checked { background: %SELECTED%; border-color: %ACCENT%; color: %TEXT%; }
#OfflineRenderStart { background: %ACCENT%; color: %ACCENT_TEXT%; border-color: %ACCENT%; font-weight: 600; }
#OfflineRenderStart:hover { background: %ACCENT_HOVER%; }
#OfflineRenderStart:pressed { background: %ACCENT%; border-color: %TEXT%; }
#OfflineRenderStart:disabled, #OfflinePresetSave:disabled, #OfflineAutoRender:disabled {
    background: %SURFACE%; color: %TEXT2%; border-color: %SEP%;
}
#OfflineRenderStart:focus, #OfflinePresetSave:focus, #OfflineAutoRender:focus,
#OfflineRenderClose:focus { border-color: %TEXT%; }
)").replace("%BG%", t.background.name())
        .replace("%SURFACE%", t.surface.name())
        .replace("%TEXT%", t.textPrimary.name())
        .replace("%TEXT2%", t.textSecondary.name())
        .replace("%SEP%", t.separator().name())
        .replace("%ACCENT%", t.accent.name())
        .replace("%ACCENT_TEXT%", t.accentText().name())
        .replace("%ACCENT_HOVER%", t.accentHighlight.name())
        .replace("%HOVER%", mixColors(t.background, t.accent, 0.08).name())
        .replace("%SELECTED%", mixColors(t.surface, t.accent, 0.18).name()));
}

bool OfflineRenderDialog::checkForTest(const QString& screenshotPath) {
    QTemporaryDir dir;
    if (!dir.isValid()) return false;
    qputenv("DAW_PRESET_ROOT", dir.filePath(QStringLiteral("presets")).toUtf8());
    daw::EngineController controller{};
    if (!controller.initialize(48000, 256, false)) return false;
    controller.setRecordDirectory(dir.path().toStdString());
    const auto file = (dir.path() + QStringLiteral("/Vocal.wav")).toStdString();
    audio::platform::AudioFileWriter writer;
    audio::platform::WriteSpec spec;
    spec.encoding = audio::platform::Encoding::Float32;
    if (!writer.open(file, spec, 48000, 2, 12000)) return false;
    std::vector<float> samples(12000);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.2f * std::sin(double(i) * 440.0 * 6.283185307 / 48000.0);
    const float* channels[]{samples.data(), samples.data()};
    if (!writer.write(channels, samples.size()) || !writer.close()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Audio, "Vocal");
    const auto first = controller.importAudio(file, track, 0.0);
    const auto second = controller.importAudio(file, track, 0.5);
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        std::fprintf(stderr, "%s offline UI: %s\n", condition ? "PASS" : "FAIL", message);
        ok &= condition;
    };
    OfflineRenderDialog dialog(controller, {{track, first}, {track, second}});
    dialog.show();
    QApplication::processEvents();
    check(!dialog.m_renderButton->isEnabled() && dialog.m_clipList->topLevelItemCount() == 2,
          "empty rack disables render and lists every selected clip");
    dialog.m_selectAll->click();
    check(dialog.m_clipList->topLevelItem(0)->checkState(0) == Qt::Unchecked &&
              dialog.m_clipList->topLevelItem(1)->checkState(0) == Qt::Unchecked,
          "selection header clears every clip");
    dialog.m_selectAll->click();
    check(dialog.m_clipList->topLevelItem(0)->checkState(0) == Qt::Checked &&
              dialog.m_clipList->topLevelItem(1)->checkState(0) == Qt::Checked,
          "selection header selects every clip");
    dialog.m_clipList->setCurrentItem(dialog.m_clipList->topLevelItem(0), 0);
    QKeyEvent toggleClip(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(dialog.m_clipList, &toggleClip);
    check(dialog.m_clipList->topLevelItem(0)->checkState(0) == Qt::Unchecked &&
              dialog.m_selectionDuration->text() == tr("%1 s selected").arg(0.25, 0, 'f', 3),
          "Space toggles a clip and updates the selected duration");
    QApplication::sendEvent(dialog.m_clipList, &toggleClip);
    const auto effect = dialog.m_scratch.addInsert(dialog.m_chainTrackId,
        daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
    dialog.rebuildRack();
    QApplication::processEvents();
    check(!effect.empty() && dialog.m_renderButton->isEnabled(), "loading an effect enables render");
    const QString preset =
        ui::channelstrippresets::offlineFilePathForName(QStringLiteral("Vocal Chain"));
    auto snapshot = dialog.m_scratch.copyChannelStrip(dialog.m_chainTrackId, true);
    snapshot.sourceName = "Vocal Chain";
    snapshot.volume = 0.25f;
    snapshot.pan = 0.5f;
    const auto savedPreset = daw::ChannelStripPreset::save(
        snapshot, preset.toStdString());
    check(bool(savedPreset), "offline chain template saves");
    if (!savedPreset) return false;
    dialog.reloadPresets();
    check(dialog.m_presets->count() == 1 &&
              dialog.m_presets->item(0)->text() == QStringLiteral("Vocal Chain") &&
              dialog.m_presets->dragEnabled() &&
              dialog.m_presets->item(0)->flags().testFlag(Qt::ItemIsDragEnabled),
          "offline presets are named, clickable drag cards");
    check(dialog.m_scratch.pasteChannelInserts(dialog.m_chainTrackId, {}),
          "draft rack clears before applying the card");
    dialog.rebuildRack();
    dialog.loadPreset(preset);
    const auto loadedStrip =
        dialog.m_scratch.copyChannelStrip(dialog.m_chainTrackId, true);
    check(loadedStrip.inserts.size() == 1 && loadedStrip.volume == 1.0f &&
              loadedStrip.pan == 0.0f,
          "preset card loads plugins without channel-strip settings");
    check(dialog.m_scratch.pasteChannelInserts(dialog.m_chainTrackId, {}),
          "draft rack clears before testing the card drop");
    dialog.rebuildRack();
    QMimeData presetMime;
    presetMime.setUrls({QUrl::fromLocalFile(preset)});
    QDragEnterEvent enter(QPoint(12, 12), Qt::CopyAction, &presetMime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dialog.m_rack, &enter);
    QDropEvent drop(QPointF(12, 12), Qt::CopyAction, &presetMime,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dialog.m_rack, &drop);
    QApplication::processEvents();
    check(enter.isAccepted() && drop.isAccepted() &&
              dialog.m_scratch.channelInserts(dialog.m_chainTrackId)->size() == 1,
          "dragging a preset card onto Audio FX loads its plugins");
    check(!dialog.m_rack->findChild<QToolButton*>(
              QStringLiteral("ChannelStripMenu")),
          "offline Audio FX has no shared channel-strip settings menu");
    const QString loadedId = QString::fromStdString(
        dialog.m_scratch.channelInserts(dialog.m_chainTrackId)->front().id);
    dialog.openEditor(loadedId);
    QApplication::processEvents();
    QDialog* pluginWindow = dialog.m_editors.value(loadedId);
    check(pluginWindow && pluginWindow->isWindow() && pluginWindow->isVisible() &&
              pluginWindow != &dialog,
          "plugin editor opens in its own window");
    if (pluginWindow) pluginWindow->close();
    QApplication::processEvents();
    dialog.m_clipList->topLevelItem(1)->setCheckState(0, Qt::Unchecked);
    check(dialog.m_renderButton->isEnabled() && dialog.m_selectAll->checkState() == Qt::PartiallyChecked,
          "one checked clip remains renderable and header shows partial selection");
    dialog.m_clipList->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
    check(!dialog.m_renderButton->isEnabled(), "zero checked clips disables render");
    dialog.m_clipList->topLevelItem(0)->setCheckState(0, Qt::Checked);
    QApplication::processEvents();
    check(dialog.m_rack->width() <= 280 && dialog.height() <= 640,
          "rack and render controls remain compact");
    if (!screenshotPath.isEmpty()) check(dialog.grab().save(screenshotPath), "dialog screenshot saved");
    dialog.resize(dialog.minimumSize());
    QApplication::processEvents();
    check(dialog.size() == QSize(820, 560) &&
              dialog.rect().contains(QRect(dialog.m_renderButton->mapTo(&dialog, QPoint()), dialog.m_renderButton->size())) &&
              dialog.m_clipList->horizontalScrollBar()->maximum() == 0,
          "minimum window size keeps the render action and table columns visible");
    auto* effectsScroll = dialog.findChild<QScrollArea*>(QStringLiteral("OfflineEffectsScroll"));
    check(effectsScroll && effectsScroll->verticalScrollBar()->maximum() == 0,
          "a short chain and render options fit without scrolling at minimum size");
    if (!screenshotPath.isEmpty())
        check(dialog.grab().save(screenshotPath + ".compact.png"), "compact dialog screenshot saved");
    dialog.resize(980, 640);
    dialog.m_autoRender->setChecked(true);
    QElapsedTimer autoRenderWait;
    autoRenderWait.start();
    while (!dialog.rendered() && autoRenderWait.elapsed() < 5000)
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    check(dialog.rendered() && dialog.isVisible() &&
              dialog.m_progressPercent->text() == QStringLiteral("100%") &&
              dialog.m_clipList->topLevelItem(0)->text(2) == tr("Done") &&
              dialog.m_clipList->topLevelItem(1)->text(2) == tr("Skipped") &&
              controller.audioClip(track, first)->offlineHistory.size() == 2 &&
              controller.audioClip(track, second)->offlineHistory.empty(),
          "Auto Render replaces only checked clips, reports percent, and keeps the dialog open");
    dialog.m_autoRender->setChecked(false);
    if (!screenshotPath.isEmpty())
        check(dialog.grab().save(screenshotPath + ".complete.png"), "completed render screenshot saved");
    QTimer::singleShot(0, &dialog, &OfflineRenderDialog::reject);
    dialog.startRender();
    check(dialog.isVisible() && dialog.m_closeButton->isEnabled() && dialog.m_renderButton->isEnabled() &&
              dialog.m_status->text() == tr("Cancelled — the project was not changed") &&
              controller.audioClip(track, first)->offlineHistory.size() == 2,
          "Cancel leaves the dialog usable and the previous audio version intact");
    dialog.reject();
    OfflineRenderDialog next(controller, {{track, first}});
    check(next.m_scratch.channelInserts(next.m_chainTrackId)->empty() && !next.m_renderButton->isEnabled(),
          "reopening a processed clip starts with a completely empty chain");
    SamplerPanel panel(&controller, SamplerPanel::Context::Clip,
                       QString::fromStdString(track), QString::fromStdString(first));
    panel.resize(900, 540);
    panel.show();
    panel.refresh();
    QApplication::processEvents();
    if (!screenshotPath.isEmpty())
        check(panel.grab().save(screenshotPath + ".sampler.png"), "sampler screenshot saved");
    auto* history = panel.findChild<QToolButton*>(QStringLiteral("SamplerOfflineHistory"));
    check(history && history->isEnabled(), "clip sampler exposes its history dropdown");
    if (history) history->click();
    QApplication::processEvents();
    auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("OfflineHistoryTree"));
    check(tree && tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->childCount() == 1,
          "dropdown lists the original and its rendered version");
    if (tree) {
        if (!screenshotPath.isEmpty())
            check(tree->window()->grab().save(screenshotPath + ".history.png"), "history screenshot saved");
        tree->itemActivated(tree->topLevelItem(0), 0);
        check(controller.audioClip(track, first)->filePath == file, "keyboard activation restores original audio");
    }
    return ok;
}
