#include "PluginQuickAdder.hpp"

#include "EngineController.hpp"
#include "PluginFormatPreference.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMessageBox>
#include <QMoveEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStyle>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kCollapsedWidth = 22;
constexpr int kToolbarHeight = 20;
constexpr int kExpandedWidth = 310;
constexpr int kSearchHeight = 20;
constexpr int kVisiblePluginRows = 5;
constexpr int kListTop = 6;
constexpr int kSectionHeight = 20;
constexpr int kPluginHeight = 36;
constexpr int kSide = 6;
constexpr int kOverlayHeight =
    kListTop + kVisiblePluginRows * kPluginHeight + kSide;

const QStringList& sectionOrder() {
    static const QStringList sections = {
        QStringLiteral("Suggested"), QStringLiteral("Recently Used"),
        QStringLiteral("Favorites"), QStringLiteral("Instruments"),
        QStringLiteral("Effects"), QStringLiteral("Utilities")};
    return sections;
}

QString uidOf(const daw::plugins::PluginDescriptor& descriptor) {
    return QString::fromStdString(descriptor.uid);
}

QString formatOf(const daw::plugins::PluginDescriptor& descriptor) {
    return QString::fromLatin1(daw::plugins::toString(descriptor.format).data(),
                               int(daw::plugins::toString(descriptor.format).size()));
}

bool containsAny(const QString& haystack,
                 std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (haystack.contains(QLatin1String(needle), Qt::CaseInsensitive)) return true;
    }
    return false;
}

QString baseSection(const daw::plugins::PluginDescriptor& descriptor) {
    if (descriptor.isInstrument) return QStringLiteral("Instruments");
    const QString category = QString::fromStdString(descriptor.category);
    if (containsAny(category, {"analy", "meter", "utility", "tool", "midi",
                               "generator"})) {
        return QStringLiteral("Utilities");
    }
    return QStringLiteral("Effects");
}

int animationDuration() {
    const QString speed =
        QSettings().value("contextPanel/pluginAnimationSpeed", "normal").toString();
    if (speed == QLatin1String("fast")) return 150;
    if (speed == QLatin1String("slow")) return 400;
    return 250;
}

bool reducedMotion() {
    return QSettings().value("ui/reduceMotion", false).toBool();
}

QColor withAlpha(QColor color, int alpha) {
    color.setAlpha(std::clamp(alpha, 0, 255));
    return color;
}

} // namespace

/// Results live outside the ToolPanel's clipped child hierarchy, but remain a
/// child of the main window. That gives them overlay z-order without creating a
/// second native window or stealing focus from the inline search field.
class PluginQuickAdderOverlay final : public QWidget {
public:
    PluginQuickAdderOverlay(PluginQuickAdder* owner, QWidget* parent)
        : QWidget(parent), m_owner(owner) {
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAccessibleName(PluginQuickAdder::tr("Plugin search results"));
        hide();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        if (m_owner) m_owner->paintOverlay(event);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (m_owner) {
            m_owner->overlayMousePress(event);
            event->accept();
        }
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_owner) m_owner->overlayMouseMove(event);
    }
    void leaveEvent(QEvent*) override {
        if (m_owner) m_owner->overlayLeave();
    }
    void wheelEvent(QWheelEvent* event) override {
        if (m_owner) m_owner->overlayWheel(event);
    }

private:
    PluginQuickAdder* m_owner = nullptr;
};

PluginQuickAdder::PluginQuickAdder(daw::EngineController* controller,
                                   QWidget* parent)
    : QWidget(parent), m_controller(controller), m_accent(th().accent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAccessibleName(tr("Search plugins"));
    setToolTip(tr("Search plugins (%1)")
                   .arg(QKeySequence(QKeySequence::Find)
                            .toString(QKeySequence::NativeText)));

    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("PluginQuickSearch"));
    m_search->setPlaceholderText(tr("Plugin or vendor…"));
    m_search->setAccessibleName(tr("Plugin search"));
    m_search->setClearButtonEnabled(true);
    m_search->addAction(icons::icon(icons::Glyph::Search, th().textSecondary, 15),
                        QLineEdit::LeadingPosition);
    m_search->installEventFilter(this);
    m_search->hide();
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_filter = text.trimmed();
        m_scrollOffset = 0;
        applyFilter();
        if (m_overlay) m_overlay->update();
    });

    m_expandAnim = new QVariantAnimation(this);
    connect(m_expandAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                m_expandProgress = value.toDouble();
                updateGeometryForState();
                update();
            });
    connect(m_expandAnim, &QVariantAnimation::finished, this, [this] {
        if (m_expanded) {
            m_search->show();
            m_search->setFocus(Qt::ShortcutFocusReason);
            showOverlay();
        } else {
            hideOverlay();
            m_search->hide();
            emit searchStateChanged(false);
            setFocus(Qt::OtherFocusReason);
        }
    });

    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        if (!m_accent.isValid()) m_accent = th().accent;
        m_search->actions().constFirst()->setIcon(
            icons::icon(icons::Glyph::Search, th().textSecondary, 15));
        update();
    });

    m_sectionCollapsed.fill(false, sectionOrder().size());
    refreshFavorites();
    refreshRecent();
    rebuildEntries();
    updateGeometryForState();
}

PluginQuickAdder::~PluginQuickAdder() {
    qApp->removeEventFilter(this);
    delete m_overlay;
    m_overlay = nullptr;
}

void PluginQuickAdder::setTrackId(const QString& trackId) {
    if (m_trackId == trackId && m_clipId.isEmpty()) return;
    m_trackId = trackId;
    m_clipId.clear();
    rebuildEntries();
    if (m_overlay) m_overlay->update();
}

void PluginQuickAdder::setClipTarget(const QString& trackId,
                                     const QString& clipId) {
    if (m_trackId == trackId && m_clipId == clipId) return;
    m_trackId = trackId;
    m_clipId = clipId;
    setAccessibleName(tr("Search Clip FX"));
    setToolTip(tr("Search Clip FX (%1)")
                   .arg(QKeySequence(QKeySequence::Find)
                            .toString(QKeySequence::NativeText)));
    rebuildEntries();
    if (m_overlay) m_overlay->update();
}

void PluginQuickAdder::setAccentColor(const QColor& color) {
    m_accent = color.isValid() ? color : th().accent;
    update();
}

QSize PluginQuickAdder::sizeHint() const {
    const double eased = m_expandProgress;
    return QSize(int(std::lerp(double(kCollapsedWidth), double(kExpandedWidth), eased)),
                 kToolbarHeight);
}

QSize PluginQuickAdder::minimumSizeHint() const { return sizeHint(); }

int PluginQuickAdder::preferredHeight() const { return sizeHint().height(); }

void PluginQuickAdder::openSearch() {
    if (m_expanded && m_search->isVisible()) {
        m_search->setFocus(Qt::ShortcutFocusReason);
        m_search->selectAll();
        showOverlay();
        return;
    }
    rebuildEntries();
    animateTo(true);
}

void PluginQuickAdder::closeSearch() {
    if (!m_expanded && m_expandProgress <= 0.0) return;
    animateTo(false);
}

void PluginQuickAdder::animateTo(bool expanded) {
    if (m_expanded == expanded && m_expandAnim->state() != QAbstractAnimation::Running)
        return;
    m_expanded = expanded;
    m_expandAnim->stop();
    m_expandAnim->setStartValue(m_expandProgress);
    m_expandAnim->setEndValue(expanded ? 1.0 : 0.0);
    m_expandAnim->setDuration(reducedMotion() ? 0 : animationDuration());
    m_expandAnim->setEasingCurve(expanded ? QEasingCurve::OutCubic
                                          : QEasingCurve::InCubic);
    if (expanded) {
        qApp->installEventFilter(this);
        emit searchStateChanged(true);
        m_search->show();
        m_search->raise();
        m_search->setFocus(Qt::ShortcutFocusReason);
    } else {
        qApp->removeEventFilter(this);
        hideOverlay();
    }
    setAccessibleName(expanded ? tr("Plugin search, expanded") : tr("Search plugins"));
    emit sizeChanged();
    m_expandAnim->start();
}

void PluginQuickAdder::collapseImmediatelyForEditor() {
    // Opening a native editor is already unambiguous confirmation. More
    // importantly, release the application-wide mouse filter and remove the
    // results overlay before MainWindow creates a native child surface. The
    // first editor used to be born while this overlay was still completing a
    // 180 ms confirmation plus a 250 ms close animation; later editors did not
    // pay that one-time native/input ordering cost.
    if (m_expandAnim) m_expandAnim->stop();
    if (m_confirmAnim) m_confirmAnim->stop();
    qApp->removeEventFilter(this);
    m_expanded = false;
    m_expandProgress = 0.0;
    m_confirmIndex = -1;
    m_confirmProgress = 0.0;
    hideOverlay();
    if (m_search) m_search->hide();
    updateGeometryForState();
    setAccessibleName(tr("Search plugins"));
    emit searchStateChanged(false);
    update();
}

void PluginQuickAdder::updateGeometryForState() {
    const QSize wanted = sizeHint();
    setMinimumSize(wanted);
    setMaximumSize(wanted);
    resize(wanted);
    m_search->setGeometry(0, 0, std::max(1, width()), kSearchHeight);
    const double fieldOpacity = std::clamp((m_expandProgress - 0.12) / 0.42, 0.0, 1.0);
    m_search->setVisible(m_expanded && fieldOpacity > 0.04);
    m_search->setStyleSheet(QString(R"(
        QLineEdit#PluginQuickSearch {
            color: %1;
            background: %2;
            border: 1px solid %3;
            border-top-color: %4;
            border-radius: 6px;
            padding: 0 24px 0 2px;
            selection-background-color: %5;
            font-size: 11px;
        }
        QLineEdit#PluginQuickSearch:focus { border: 1px solid %6; }
    )")
        .arg(th().textPrimary.name(QColor::HexArgb),
             withAlpha(th().well(), int(225 * fieldOpacity)).name(QColor::HexArgb),
             withAlpha(th().ink(), int(54 * fieldOpacity)).name(QColor::HexArgb),
             withAlpha(th().ink(), int(92 * fieldOpacity)).name(QColor::HexArgb),
             withAlpha(m_accent, 110).name(QColor::HexArgb),
             withAlpha(m_accent, 190).name(QColor::HexArgb)));
    updateGeometry();
    positionOverlay();
    emit sizeChanged();
}

void PluginQuickAdder::refreshFavorites() {
    m_favorites.clear();
    const QStringList saved =
        QSettings().value("contextPanel/pluginFavorites").toStringList();
    for (const QString& uid : saved) m_favorites.push_back(uid);
}

void PluginQuickAdder::refreshRecent() {
    m_recent.clear();
    const QStringList saved = QSettings().value("contextPanel/pluginRecent").toStringList();
    for (const QString& uid : saved.mid(0, 5)) m_recent.push_back(uid);
}

void PluginQuickAdder::refreshSuggested() {
    m_suggested.clear();
    for (const Entry& entry : m_all) {
        if (entry.descriptor.isInstrument) continue;
        const QString blob = searchText(entry);
        if (containsAny(blob, {"equalizer", " eq", "compress", "dynamic",
                               "analy", "reverb"})) {
            m_suggested.push_back(uidOf(entry.descriptor));
            if (m_suggested.size() == 5) break;
        }
    }
    if (m_suggested.isEmpty()) {
        for (const Entry& entry : m_all) {
            m_suggested.push_back(uidOf(entry.descriptor));
            if (m_suggested.size() == 5) break;
        }
    }
}

void PluginQuickAdder::rebuildEntries() {
    refreshFavorites();
    refreshRecent();
    m_all.clear();
    if (m_controller) {
        std::vector<daw::plugins::PluginDescriptor> plugins =
            m_controller->pluginManager().plugins();
        plugins = daw::preferredPluginVariants(std::move(plugins),
                                               ui::preferredPluginFormat());
        std::sort(plugins.begin(), plugins.end(), [](const auto& a, const auto& b) {
            const int byName = QString::compare(QString::fromStdString(a.name),
                                                QString::fromStdString(b.name),
                                                Qt::CaseInsensitive);
            if (byName != 0) return byName < 0;
            return a.vendor < b.vendor;
        });
        m_all.reserve(plugins.size());
        for (const auto& descriptor : plugins) {
            if (!m_clipId.isEmpty() && descriptor.isInstrument) continue;
            Entry entry;
            entry.descriptor = descriptor;
            entry.section = baseSection(descriptor);
            entry.favorite = m_favorites.contains(uidOf(descriptor));
            entry.recent = m_recent.contains(uidOf(descriptor));
            m_all.push_back(std::move(entry));
        }
    }
    refreshSuggested();
    for (Entry& entry : m_all)
        entry.suggested = m_suggested.contains(uidOf(entry.descriptor));
    applyFilter();
}

QString PluginQuickAdder::searchText(const Entry& entry) const {
    QString blob = QStringLiteral("%1 %2 %3 %4 %5")
                       .arg(QString::fromStdString(entry.descriptor.name),
                            QString::fromStdString(entry.descriptor.vendor),
                            QString::fromStdString(entry.descriptor.category),
                            formatOf(entry.descriptor), entry.section);
    const QString lower = blob.toLower();
    if (containsAny(lower, {"compress", "dynamic"})) blob += QStringLiteral(" comp compressor");
    if (containsAny(lower, {"equaliz", "filter", " eq"})) blob += QStringLiteral(" eq tone");
    if (containsAny(lower, {"synth", "instrument"})) blob += QStringLiteral(" analog synth keys");
    if (containsAny(lower, {"satur", "tape", "tube"})) blob += QStringLiteral(" analog colour");
    if (containsAny(lower, {"analy", "meter"})) blob += QStringLiteral(" utility spectrum");
    return blob;
}

void PluginQuickAdder::applyFilter() {
    m_visible.clear();
    const QString scope =
        QSettings().value("contextPanel/pluginSearchScope", "all").toString();

    auto allowed = [&](const Entry& entry) {
        if (scope == QLatin1String("favorites") && !entry.favorite) return false;
        if (scope == QLatin1String("instruments") && !entry.descriptor.isInstrument)
            return false;
        if (scope == QLatin1String("effects") && entry.descriptor.isInstrument)
            return false;
        return true;
    };
    if (m_filter.isEmpty()) {
        // The idle state is intentionally predictable: the first five allowed
        // plugins in the alphabetically sorted catalogue, without category
        // headers consuming one of the five compact rows.
        for (const Entry& entry : m_all) {
            if (!allowed(entry)) continue;
            m_visible.push_back({&entry, QString()});
            if (m_visible.size() == kVisiblePluginRows) break;
        }
    } else {
        // Keep autocomplete equally dense while typing: one globally
        // alphabetical stream, with category/vendor still searchable in the
        // metadata rather than taking vertical space as section headers.
        for (const Entry& entry : m_all) {
            if (!allowed(entry) ||
                !searchText(entry).contains(m_filter, Qt::CaseInsensitive)) {
                continue;
            }
            m_visible.push_back({&entry, QString()});
        }
    }

    m_highlight = m_visible.empty() ? -1 : 0;
    m_scrollOffset = std::clamp(m_scrollOffset, 0, maximumScrollOffset());
    update();
    if (m_overlay) m_overlay->update();
}

int PluginQuickAdder::sectionIndex(const QString& section) const {
    return sectionOrder().indexOf(section);
}

std::vector<PluginQuickAdder::HitRow> PluginQuickAdder::hitRows() const {
    std::vector<HitRow> rows;
    const QRect viewport = listViewport();
    int y = viewport.top() - m_scrollOffset;
    QString last;
    for (int index = 0; index < int(m_visible.size()); ++index) {
        const VisibleItem& item = m_visible[std::size_t(index)];
        if (!item.section.isEmpty() && item.section != last) {
            last = item.section;
            rows.push_back({HitRow::Kind::Section,
                            QRect(viewport.left(), y, viewport.width(), kSectionHeight),
                            item.section, -1});
            y += kSectionHeight;
        }
        const int section = sectionIndex(item.section);
        if (section >= 0 && section < m_sectionCollapsed.size() &&
            m_sectionCollapsed[section]) {
            continue;
        }
        rows.push_back({HitRow::Kind::Plugin,
                        QRect(viewport.left(), y, viewport.width(), kPluginHeight),
                        item.section, index});
        y += kPluginHeight;
    }
    return rows;
}

int PluginQuickAdder::maximumScrollOffset() const {
    int contentBottom = kListTop;
    for (const HitRow& row : hitRows()) contentBottom = std::max(contentBottom, row.rect.bottom() + m_scrollOffset);
    return std::max(0, contentBottom - listViewport().bottom());
}

QRect PluginQuickAdder::listViewport() const {
    const int overlayWidth = m_overlay ? m_overlay->width() : kExpandedWidth;
    const int overlayHeight = m_overlay ? m_overlay->height() : kOverlayHeight;
    return QRect(kSide, kListTop, std::max(1, overlayWidth - 2 * kSide),
                 std::max(1, overlayHeight - kListTop - kSide));
}

const PluginQuickAdder::Entry* PluginQuickAdder::visibleAt(int index) const {
    if (index < 0 || index >= int(m_visible.size())) return nullptr;
    return m_visible[std::size_t(index)].entry;
}

int PluginQuickAdder::visibleCount() const { return int(m_visible.size()); }

void PluginQuickAdder::setHighlight(int index) {
    if (m_visible.empty()) {
        m_highlight = -1;
        return;
    }
    m_highlight = std::clamp(index, 0, int(m_visible.size()) - 1);
    scrollHighlightIntoView();
    if (m_overlay) m_overlay->update();
}

void PluginQuickAdder::scrollHighlightIntoView() {
    const QRect viewport = listViewport();
    for (const HitRow& row : hitRows()) {
        if (row.kind != HitRow::Kind::Plugin || row.visibleIndex != m_highlight) continue;
        if (row.rect.top() < viewport.top())
            m_scrollOffset = std::max(0, m_scrollOffset - (viewport.top() - row.rect.top()));
        else if (row.rect.bottom() > viewport.bottom())
            m_scrollOffset = std::min(maximumScrollOffset(),
                                      m_scrollOffset + row.rect.bottom() - viewport.bottom());
        break;
    }
}

void PluginQuickAdder::toggleFavorite(const Entry& entry) {
    const QString uid = uidOf(entry.descriptor);
    QStringList saved = QSettings().value("contextPanel/pluginFavorites").toStringList();
    if (saved.contains(uid)) saved.removeAll(uid);
    else saved.prepend(uid);
    QSettings().setValue("contextPanel/pluginFavorites", saved);
    rebuildEntries();
}

void PluginQuickAdder::rememberRecent(
    const daw::plugins::PluginDescriptor& descriptor) {
    QStringList saved = QSettings().value("contextPanel/pluginRecent").toStringList();
    const QString uid = uidOf(descriptor);
    saved.removeAll(uid);
    saved.prepend(uid);
    while (saved.size() > 5) saved.removeLast();
    QSettings().setValue("contextPanel/pluginRecent", saved);
}

void PluginQuickAdder::insertCurrent(bool openEditor, bool keepOpen) {
    const Entry* entry = visibleAt(m_highlight);
    if (!entry || !m_controller || m_trackId.isEmpty()) return;
    const QString position =
        QSettings().value("contextPanel/pluginInsertPosition", "end").toString();
    const size_t index = position == QLatin1String("start") ? 0 : size_t(-1);
    const std::string id = [&] {
            m_loading = true;
            setCursor(Qt::WaitCursor);
            if (m_overlay) {
                m_overlay->update();
                m_overlay->repaint();
            }
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const std::string loaded = m_clipId.isEmpty()
                ? m_controller->addInsert(m_trackId.toStdString(),
                                          entry->descriptor, index)
                : m_controller->addClipFxInsert(m_trackId.toStdString(),
                                                m_clipId.toStdString(),
                                                entry->descriptor, index);
            QApplication::restoreOverrideCursor();
            unsetCursor();
            m_loading = false;
            if (m_overlay) m_overlay->update();
            return loaded;
        }();
    if (id.empty()) {
        QApplication::beep();
        QMessageBox::warning(
            this, tr("Plugin could not be loaded"),
            tr("%1 is still listed by the last plugin scan, but its module "
               "could not create an instance. Rescan plugins in Settings and "
               "check that the plugin is installed and licensed.")
                .arg(QString::fromStdString(entry->descriptor.name)));
        emit pluginInserted(QString(), false);
        return;
    }

    rememberRecent(entry->descriptor);
    if (openEditor && !keepOpen) {
        collapseImmediatelyForEditor();
    } else {
        m_confirmIndex = m_highlight;
        if (m_confirmAnim) m_confirmAnim->stop();
        auto* flash = new QVariantAnimation(this);
        flash->setStartValue(1.0);
        flash->setEndValue(0.0);
        flash->setDuration(reducedMotion() ? 0 : 180);
        connect(flash, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& value) {
                    m_confirmProgress = value.toDouble();
                    if (m_overlay) m_overlay->update();
                });
        connect(flash, &QVariantAnimation::finished, this, [this, keepOpen] {
            m_confirmIndex = -1;
            m_confirmProgress = 0.0;
            if (keepOpen) rebuildEntries();
            else closeSearch();
        });
        m_confirmAnim = flash;
        flash->start(QAbstractAnimation::DeleteWhenStopped);
    }

    const QString insertId = QString::fromStdString(id);
    // Present before notifying views that rebuild themselves: a synchronous
    // tracksChanged handler may replace this adder, while the editor only
    // needs the successfully created model that already exists above.
    if (openEditor) emit editorRequested(insertId);
    emit pluginInserted(insertId, openEditor);
}

void PluginQuickAdder::showOverlay() {
    if (!m_expanded || !window()) return;
    if (!m_overlay) {
        m_overlay = new PluginQuickAdderOverlay(this, window());
    }
    positionOverlay();
    m_overlay->show();
    m_overlay->raise();
    m_overlay->update();
}

void PluginQuickAdder::hideOverlay() {
    if (m_overlay) m_overlay->hide();
}

void PluginQuickAdder::positionOverlay() {
    if (!m_overlay || !window()) return;
    QWidget* root = window();
    const QPoint belowField = mapTo(root, QPoint(0, height() + 3));
    // Match the search field pixel-for-pixel; only clamp as a last resort for
    // a window narrower than the expanded Context Panel itself.
    const int overlayWidth = std::min(width(), std::max(220, root->width() - 16));
    const int left = std::clamp(belowField.x(), 8,
                                std::max(8, root->width() - overlayWidth - 8));
    const int available = std::max(100, root->height() - belowField.y() - 44);
    const int overlayHeight = std::min(kOverlayHeight, available);
    m_overlay->setGeometry(left, belowField.y(), overlayWidth, overlayHeight);
    m_scrollOffset = std::clamp(m_scrollOffset, 0, maximumScrollOffset());
}

void PluginQuickAdder::paintEvent(QPaintEvent*) {
    if (m_expandProgress > 0.55) return; // the line edit owns the expanded state

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const double opacity = std::clamp(1.0 - m_expandProgress / 0.55, 0.0, 1.0);
    p.setOpacity(opacity);
    const QRectF hit = QRectF(1, 0, kCollapsedWidth - 2, kToolbarHeight);
    if (m_hover || hasFocus()) {
        p.setPen(QPen(withAlpha(m_accent, hasFocus() ? 190 : 95), 1.0));
        p.setBrush(withAlpha(m_accent, m_pressed ? 42 : 24));
        p.drawRoundedRect(hit, 5, 5);
    }
    icons::paint(p, icons::Glyph::Search, QRectF(4, 3, 14, 14),
                 m_hover || hasFocus() ? th().textPrimary : th().textSecondary);
}

void PluginQuickAdder::paintOverlay(QPaintEvent*) {
    if (!m_overlay) return;
    QPainter p(m_overlay);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF panel = QRectF(m_overlay->rect()).adjusted(0.5, 0.5, -0.5, -1.5);
    p.setPen(QPen(th().separator(), 1.0));
    p.setBrush(th().well());
    p.drawRoundedRect(panel, 9, 9);

    const QRect viewport = listViewport();
    p.save();
    p.setClipRect(viewport);

    if (m_visible.empty()) {
        icons::paint(p, icons::Glyph::Search,
                     QRectF(viewport.center().x() - 13, viewport.top() + 54, 26, 26),
                     withAlpha(th().textSecondary, 130));
        QFont emptyFont = p.font();
        emptyFont.setPixelSize(13);
        p.setFont(emptyFont);
        p.setPen(th().textSecondary);
        p.drawText(viewport.adjusted(12, 92, -12, -12),
                   Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                   m_filter.isEmpty()
                       ? tr("No plugins are available. Scan plugins in Settings.")
                       : tr("No plugins match “%1”. Try a name, vendor, or tag.")
                             .arg(m_filter));
    }

    for (const HitRow& hit : hitRows()) {
        if (!hit.rect.intersects(viewport)) continue;
        if (hit.kind == HitRow::Kind::Section) {
            QLinearGradient divider(hit.rect.left(), 0, hit.rect.right(), 0);
            divider.setColorAt(0.0, Qt::transparent);
            divider.setColorAt(0.15, withAlpha(th().ink(), 30));
            divider.setColorAt(0.85, withAlpha(th().ink(), 30));
            divider.setColorAt(1.0, Qt::transparent);
            p.fillRect(QRect(hit.rect.left(), hit.rect.bottom() - 1,
                             hit.rect.width(), 1), divider);
            QFont sectionFont = p.font();
            sectionFont.setPixelSize(10);
            sectionFont.setWeight(QFont::DemiBold);
            sectionFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);
            p.setFont(sectionFont);
            p.setPen(th().textSecondary);
            p.drawText(hit.rect.adjusted(10, 0, -34, 0),
                       Qt::AlignVCenter | Qt::AlignLeft,
                       hit.section.toUpper());
            const int section = sectionIndex(hit.section);
            const bool collapsed = section >= 0 && section < m_sectionCollapsed.size() &&
                                   m_sectionCollapsed[section];
            icons::paint(p, collapsed ? icons::Glyph::ChevronRight
                                      : icons::Glyph::Chevron,
                         QRectF(hit.rect.right() - 27, hit.rect.center().y() - 8, 16, 16),
                         th().textSecondary);
            continue;
        }

        const Entry* entry = visibleAt(hit.visibleIndex);
        if (!entry) continue;
        const bool active = hit.visibleIndex == m_highlight;
        if (active) {
            const QColor selection = mixColors(th().well(), m_accent, 0.20);
            p.setPen(QPen(mixColors(th().separator(), m_accent, 0.55), 0.8));
            p.setBrush(selection);
            p.drawRoundedRect(QRectF(hit.rect).adjusted(1, 2, -1, -2), 7, 7);
        }
        if (hit.visibleIndex == m_confirmIndex && m_confirmProgress > 0.0) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(m_accent, int(180 * m_confirmProgress)));
            p.drawRoundedRect(QRectF(hit.rect).adjusted(1, 2, -1, -2), 7, 7);
        }

        const QRect iconRect(hit.rect.left() + 6, hit.rect.top() + 6, 24, 24);
        QLinearGradient tile(iconRect.topLeft(), iconRect.bottomRight());
        tile.setColorAt(0, withAlpha(m_accent, 80));
        tile.setColorAt(1, withAlpha(th().well(), 220));
        p.setPen(QPen(withAlpha(th().ink(), 40), 0.8));
        p.setBrush(tile);
        p.drawRoundedRect(iconRect, 6, 6);
        const icons::Glyph glyph = entry->descriptor.isInstrument
                                       ? icons::Glyph::Synth
                                       : (entry->section == QLatin1String("Utilities")
                                              ? icons::Glyph::Eq
                                              : icons::Glyph::Plugin);
        icons::paint(p, glyph, QRectF(iconRect).adjusted(6, 6, -6, -6),
                     th().textPrimary);

        const int textLeft = iconRect.right() + 7;
        const int starLeft = hit.rect.right() - 24;
        QFont nameFont = p.font();
        nameFont.setPixelSize(11);
        nameFont.setWeight(QFont::DemiBold);
        p.setFont(nameFont);
        p.setPen(th().textPrimary);
        const QString name = QFontMetrics(nameFont).elidedText(
            QString::fromStdString(entry->descriptor.name), Qt::ElideRight,
            std::max(10, starLeft - textLeft - 6));
        p.drawText(QRect(textLeft, hit.rect.top() + 2,
                         starLeft - textLeft - 4, 16),
                   Qt::AlignVCenter | Qt::AlignLeft, name);

        QFont metaFont = p.font();
        metaFont.setPixelSize(9);
        metaFont.setWeight(QFont::Normal);
        p.setFont(metaFont);
        p.setPen(th().textSecondary);
        QString vendor = QString::fromStdString(entry->descriptor.vendor).trimmed();
        if (vendor.isEmpty()) vendor = tr("Unknown vendor");
        QString meta = vendor + QStringLiteral("  ·  ") + formatOf(entry->descriptor);
        meta = QFontMetrics(metaFont).elidedText(meta, Qt::ElideRight,
                                                std::max(10, starLeft - textLeft - 6));
        p.drawText(QRect(textLeft, hit.rect.top() + 17,
                         starLeft - textLeft - 4, 15),
                   Qt::AlignVCenter | Qt::AlignLeft, meta);

        icons::paint(p, icons::Glyph::Star,
                     QRectF(starLeft + 4, hit.rect.center().y() - 7, 14, 14),
                     entry->favorite ? m_accent : withAlpha(th().textSecondary, 150));

        QLinearGradient hairline(hit.rect.left(), 0, hit.rect.right(), 0);
        hairline.setColorAt(0, Qt::transparent);
        hairline.setColorAt(0.12, withAlpha(th().ink(), 12));
        hairline.setColorAt(0.88, withAlpha(th().ink(), 12));
        hairline.setColorAt(1, Qt::transparent);
        p.fillRect(QRect(hit.rect.left(), hit.rect.bottom(), hit.rect.width(), 1),
                   hairline);
    }

    const int maxScroll = maximumScrollOffset();
    if (maxScroll > 0) {
        const int trackHeight = viewport.height() - 8;
        const int thumbHeight = std::max(28, int(trackHeight *
            (double(viewport.height()) / (viewport.height() + maxScroll))));
        const int travel = trackHeight - thumbHeight;
        const int thumbY = viewport.top() + 4 +
                           int(double(m_scrollOffset) / maxScroll * travel);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(m_accent, m_hover ? 145 : 82));
        p.drawRoundedRect(QRectF(viewport.right() - 3, thumbY, 2, thumbHeight), 1, 1);
    }
    p.restore();

    if (m_loading) {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(th().well(), 205));
        p.drawRoundedRect(panel.adjusted(5, 5, -5, -5), 7, 7);
        QFont loadingFont = p.font();
        loadingFont.setPixelSize(12);
        loadingFont.setWeight(QFont::DemiBold);
        p.setFont(loadingFont);
        p.setPen(th().textPrimary);
        p.drawText(panel.toRect(), Qt::AlignCenter, tr("Loading plugin…"));
    }
}

void PluginQuickAdder::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    m_pressed = true;
    if (!m_expanded) {
        openSearch();
        event->accept();
        return;
    }
    m_search->setFocus(Qt::MouseFocusReason);
    event->accept();
}

void PluginQuickAdder::overlayMousePress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    event->accept();
    for (const HitRow& row : hitRows()) {
        if (!row.rect.contains(event->position().toPoint())) continue;
        if (row.kind == HitRow::Kind::Section) {
            const int section = sectionIndex(row.section);
            if (section >= 0 && section < m_sectionCollapsed.size()) {
                m_sectionCollapsed[section] = !m_sectionCollapsed[section];
                m_scrollOffset = std::clamp(m_scrollOffset, 0, maximumScrollOffset());
                if (m_overlay) m_overlay->update();
            }
            return;
        }
        setHighlight(row.visibleIndex);
        const Entry* entry = visibleAt(row.visibleIndex);
        if (!entry) return;
        // Only the visible star itself toggles Favorite. The rest of the row,
        // including the right-hand padding, always performs the primary action.
        const QRect starHit(row.rect.right() - 24, row.rect.top(), 24,
                            row.rect.height());
        if (starHit.contains(event->position().toPoint())) {
            toggleFavorite(*entry);
        } else {
            insertCurrent(/*openEditor=*/true,
                          event->modifiers().testFlag(Qt::ShiftModifier));
        }
        event->accept();
        return;
    }
}

void PluginQuickAdder::mouseMoveEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    if (!m_hover) {
        m_hover = true;
        update();
    }
}

void PluginQuickAdder::overlayMouseMove(QMouseEvent* event) {
    m_hover = true;
    for (const HitRow& row : hitRows()) {
        if (row.kind == HitRow::Kind::Plugin &&
            row.rect.contains(event->position().toPoint())) {
            if (m_highlight != row.visibleIndex) setHighlight(row.visibleIndex);
            return;
        }
    }
}

void PluginQuickAdder::overlayLeave() {
    m_hover = false;
    if (m_overlay) m_overlay->update();
}

void PluginQuickAdder::overlayWheel(QWheelEvent* event) {
    const int delta = event->angleDelta().y();
    m_scrollOffset = std::clamp(m_scrollOffset - delta / 3, 0,
                                maximumScrollOffset());
    if (m_overlay) m_overlay->update();
    event->accept();
}

void PluginQuickAdder::mouseReleaseEvent(QMouseEvent*) {
    m_pressed = false;
    update();
}

void PluginQuickAdder::leaveEvent(QEvent*) {
    m_hover = false;
    m_pressed = false;
    update();
}

void PluginQuickAdder::keyPressEvent(QKeyEvent* event) {
    if (!m_expanded && (event->key() == Qt::Key_Return ||
                        event->key() == Qt::Key_Enter ||
                        event->key() == Qt::Key_Space)) {
        openSearch();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        closeSearch();
        event->accept();
        return;
    }

    std::vector<int> selectable;
    for (const HitRow& row : hitRows())
        if (row.kind == HitRow::Kind::Plugin) selectable.push_back(row.visibleIndex);
    auto move = [&](int direction) {
        if (selectable.empty()) return;
        auto found = std::find(selectable.begin(), selectable.end(), m_highlight);
        int at = found == selectable.end() ? (direction > 0 ? -1 : int(selectable.size()))
                                           : int(found - selectable.begin());
        at = std::clamp(at + direction, 0, int(selectable.size()) - 1);
        setHighlight(selectable[std::size_t(at)]);
    };

    if (event->key() == Qt::Key_Down ||
        (event->key() == Qt::Key_Tab && !event->modifiers().testFlag(Qt::ShiftModifier))) {
        move(1);
        event->accept();
    } else if (event->key() == Qt::Key_Up ||
               (event->key() == Qt::Key_Backtab) ||
               (event->key() == Qt::Key_Tab &&
                event->modifiers().testFlag(Qt::ShiftModifier))) {
        move(-1);
        event->accept();
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        insertCurrent(/*openEditor=*/true,
                      event->modifiers().testFlag(Qt::ShiftModifier));
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}

bool PluginQuickAdder::eventFilter(QObject* watched, QEvent* event) {
    if (m_expanded && watched == window() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        positionOverlay();
    }
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
            case Qt::Key_Escape:
            case Qt::Key_Down:
            case Qt::Key_Up:
            case Qt::Key_Tab:
            case Qt::Key_Backtab:
            case Qt::Key_Return:
            case Qt::Key_Enter:
                keyPressEvent(key);
                return key->isAccepted();
            default:
                break;
        }
    }
    if (m_expanded && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint global = mouse->globalPosition().toPoint();
        const bool insideInline = rect().contains(mapFromGlobal(global));
        const bool insideOverlay = m_overlay && m_overlay->isVisible() &&
            m_overlay->rect().contains(m_overlay->mapFromGlobal(global));
        if (!insideInline && !insideOverlay) closeSearch();
    }
    return QWidget::eventFilter(watched, event);
}

void PluginQuickAdder::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    if (m_expanded && m_search->isVisible()) m_search->setFocus(event->reason());
}

void PluginQuickAdder::wheelEvent(QWheelEvent* event) {
    QWidget::wheelEvent(event);
}

void PluginQuickAdder::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_search) m_search->setGeometry(0, 0, std::max(1, width()), kSearchHeight);
    positionOverlay();
}

void PluginQuickAdder::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    positionOverlay();
}
