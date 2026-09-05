#include "NotebookWindow.hpp"

#include "Controls.hpp"
#include "GlassPanel.hpp"
#include "Icons.hpp"
#include "NotebookPrefs.hpp"
#include "Theme.hpp"

#include "EngineController.hpp"

#include <QAbstractItemView>
#include <QBuffer>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHash>
#include <QHideEvent>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <algorithm>

namespace {

class NotebookPage final : public QWebEnginePage {
public:
    NotebookPage(QWebEngineProfile* profile, QObject* parent)
        : QWebEnginePage(profile, parent) {}

protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType type,
                                 bool isMainFrame) override {
        if (!isMainFrame) return true;
        if (type == NavigationTypeReload) return true;
        if (type == NavigationTypeLinkClicked) return false;
        return url.isLocalFile() || url.scheme() == QLatin1String("about");
    }

    QWebEnginePage* createWindow(WebWindowType) override { return nullptr; }
};

QString jsArray(const QString& value) {
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}

QString htmlEscaped(QString value) {
    return value.replace(QLatin1Char('&'), QStringLiteral("&amp;"))
        .replace(QLatin1Char('<'), QStringLiteral("&lt;"))
        .replace(QLatin1Char('>'), QStringLiteral("&gt;"))
        .replace(QLatin1Char('"'), QStringLiteral("&quot;"));
}

QString cssQuoted(QString value) {
    return value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
        .replace(QLatin1Char('\''), QStringLiteral("\\'"))
        .replace(QLatin1Char('\n'), QStringLiteral(" "));
}

bool isVideo(const QString& path) {
    static const QStringList suffixes = {
        QStringLiteral("mp4"), QStringLiteral("m4v"),
        QStringLiteral("webm"), QStringLiteral("ogv"),
        QStringLiteral("ogg"), QStringLiteral("mov")};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

bool isAnimated(const QString& path) {
    return isVideo(path) ||
           QFileInfo(path).suffix().compare(QStringLiteral("gif"),
                                             Qt::CaseInsensitive) == 0;
}

QString editorPagePath() {
    return QDir(ui::notebookprefs::dataDirectory())
        .filePath(QStringLiteral("editor.html"));
}

QStringList timedTextFontFamilies() {
    static QHash<QString, int> loadedFonts;
    for (const QString& path : ui::notebookprefs::customFontFiles()) {
        if (!loadedFonts.contains(path))
            loadedFonts.insert(path, QFontDatabase::addApplicationFont(path));
    }
    QStringList families = QFontDatabase::families();
    families.sort(Qt::CaseInsensitive);
    return families;
}

} // namespace

NotebookWindow::NotebookWindow(daw::EngineController* controller,
                               QWidget* parent)
    : QDialog(parent, Qt::Window), m_controller(controller) {
    setWindowTitle(tr("Notebook"));
    setObjectName(QStringLiteral("NotebookWindow"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(680, 480);
    resize(920, 700);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    buildToolbar();
    column->addWidget(m_toolbar);

    auto* profile = new QWebEngineProfile(this);
    profile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    profile->setPersistentCookiesPolicy(
        QWebEngineProfile::NoPersistentCookies);
    m_view = new QWebEngineView(this);
    auto* page = new NotebookPage(profile, m_view);
    m_view->setPage(page);
    page->settings()->setAttribute(
        QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    page->settings()->setAttribute(
        QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows,
                                   false);
    auto* channel = new QWebChannel(page);
    channel->registerObject(QStringLiteral("notebook"), this);
    page->setWebChannel(channel);
    auto* content = new QHBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    content->addWidget(m_view, 1);
    buildTimedTextPanel();
    content->addWidget(m_timedTextPanel);
    column->addLayout(content, 1);

    connect(m_view, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (!ok) {
            setSaveStatus(tr("Could not open notebook"), true);
            return;
        }
        m_view->page()->runJavaScript(
            QStringLiteral("typeof QWebChannel==='function' && typeof qt==='object'"),
            [this](const QVariant& ready) {
                if (!ready.toBool()) {
                    setSaveStatus(tr("Notebook editor could not start"), true);
                    return;
                }
                if (isVisible() && m_backgroundPlaying)
                    m_view->page()->runJavaScript(
                        QStringLiteral("setBackgroundMotion(true);"));
            });
    });

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(420);
    connect(m_saveTimer, &QTimer::timeout, this, &NotebookWindow::saveNow);
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(90);
    connect(m_reloadTimer, &QTimer::timeout, this,
            &NotebookWindow::renderDocument);

    QFile contentFile(ui::notebookprefs::contentFilePath());
    if (contentFile.open(QIODevice::ReadOnly))
        m_content = QString::fromUtf8(contentFile.readAll());

    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        applyTheme();
        renderDocument();
    });
    applyTheme();
    reloadSettings();

    const QByteArray geometry =
        QSettings().value(QStringLiteral("notebook/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
}

NotebookWindow::~NotebookWindow() { saveNow(); }

void NotebookWindow::buildToolbar() {
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("NotebookToolbar"));
    auto* column = new QVBoxLayout(m_toolbar);
    column->setContentsMargins(12, 8, 12, 8);
    column->setSpacing(6);

    auto* header = new QHBoxLayout;
    header->setSpacing(6);
    auto* title = new QLabel(tr("NOTEBOOK"), m_toolbar);
    title->setObjectName(QStringLiteral("NotebookTitle"));
    header->addWidget(title);
    header->addStretch(1);
    m_saveStatus = new QLabel(tr("Saved locally"), m_toolbar);
    m_saveStatus->setObjectName(QStringLiteral("NotebookSaveStatus"));
    m_saveStatus->setAccessibleName(tr("Notebook save status"));
    header->addWidget(m_saveStatus);

    m_timedTextEditorButton = new ui::IconButton(
        icons::Glyph::Clock, tr("Edit timed text"), m_toolbar);
    m_timedTextEditorButton->setObjectName(
        QStringLiteral("NotebookTimedTextEditorButton"));
    m_timedTextEditorButton->setAccessibleName(tr("Edit timed text"));
    m_timedTextEditorButton->setButtonSize(28, 28);
    m_timedTextEditorButton->setCheckable(true);
    connect(m_timedTextEditorButton, &QAbstractButton::toggled, this,
            [this](bool visible) {
                if (m_timedTextPanel) m_timedTextPanel->setVisible(visible);
                updateTimedTextButtons();
            });
    header->addWidget(m_timedTextEditorButton);

    m_timedTextPlaybackButton = new ui::IconButton(
        icons::Glyph::Power, tr("Show timed text on the timeline"), m_toolbar);
    m_timedTextPlaybackButton->setObjectName(
        QStringLiteral("NotebookTimedTextPlaybackButton"));
    m_timedTextPlaybackButton->setAccessibleName(
        tr("Show timed text on the timeline"));
    m_timedTextPlaybackButton->setButtonSize(28, 28);
    m_timedTextPlaybackButton->setCheckable(true);
    m_timedTextPlaybackButton->setChecked(
        ui::notebookprefs::timedTextEnabled());
    connect(m_timedTextPlaybackButton, &QAbstractButton::toggled, this,
            [this](bool enabled) {
                ui::notebookprefs::setTimedTextEnabled(enabled);
                updateTimedTextButtons();
                emit timedTextChanged();
            });
    header->addWidget(m_timedTextPlaybackButton);

    m_motionButton = new ui::IconButton(
        icons::Glyph::Pause, tr("Pause animated background"), m_toolbar);
    m_motionButton->setAccessibleName(tr("Pause animated background"));
    m_motionButton->setButtonSize(28, 28);
    m_motionButton->setCheckable(true);
    connect(m_motionButton, &QAbstractButton::toggled, this, [this](bool playing) {
        m_backgroundPlaying = playing;
        if (m_view)
            m_view->page()->runJavaScript(
                QStringLiteral("setBackgroundMotion(%1);")
                    .arg(playing ? QStringLiteral("true")
                                 : QStringLiteral("false")));
        updateMotionButton();
    });
    header->addWidget(m_motionButton);

    auto* settings = new ui::IconButton(
        icons::Glyph::Gear, tr("Notebook settings"), m_toolbar);
    settings->setAccessibleName(tr("Notebook settings"));
    settings->setButtonSize(28, 28);
    connect(settings, &QAbstractButton::clicked, this,
            &NotebookWindow::settingsRequested);
    header->addWidget(settings);
    column->addLayout(header);

    auto* formats = new QHBoxLayout;
    formats->setSpacing(5);
    const auto iconButton = [this, formats](icons::Glyph glyph,
                                            const QString& name,
                                            const auto& callback) {
        auto* button = new ui::IconButton(glyph, name, m_toolbar);
        button->setAccessibleName(name);
        button->setButtonSize(28, 28);
        connect(button, &QAbstractButton::clicked, this, callback);
        formats->addWidget(button);
        return button;
    };
    iconButton(icons::Glyph::Undo, tr("Undo"),
               [this] { runCommand(QStringLiteral("undo")); });
    iconButton(icons::Glyph::Redo, tr("Redo"),
               [this] { runCommand(QStringLiteral("redo")); });

    m_block = new QComboBox(m_toolbar);
    m_block->setAccessibleName(tr("Paragraph style"));
    m_block->addItem(tr("Paragraph"), QStringLiteral("p"));
    m_block->addItem(tr("Heading 1"), QStringLiteral("h1"));
    m_block->addItem(tr("Heading 2"), QStringLiteral("h2"));
    m_block->addItem(tr("Quote"), QStringLiteral("blockquote"));
    m_block->setMinimumWidth(110);
    connect(m_block, &QComboBox::currentIndexChanged, this, [this](int index) {
        runCommand(QStringLiteral("formatBlock"),
                   m_block->itemData(index).toString());
    });
    formats->addWidget(m_block);

    m_font = new QComboBox(m_toolbar);
    m_font->setEditable(true);
    m_font->setInsertPolicy(QComboBox::NoInsert);
    m_font->setAccessibleName(tr("Text font"));
    m_font->setMinimumWidth(150);
    connect(m_font, &QComboBox::textActivated, this, [this](const QString& font) {
        runCommand(QStringLiteral("fontName"), font);
    });
    formats->addWidget(m_font, 1);

    m_size = new QComboBox(m_toolbar);
    m_size->setEditable(true);
    m_size->setInsertPolicy(QComboBox::NoInsert);
    m_size->addItems({QStringLiteral("12"), QStringLiteral("14"),
                      QStringLiteral("16"), QStringLiteral("18"),
                      QStringLiteral("24"), QStringLiteral("32"),
                      QStringLiteral("48")});
    m_size->setCurrentText(QStringLiteral("16"));
    m_size->setAccessibleName(tr("Text size"));
    m_size->setFixedWidth(62);
    connect(m_size, &QComboBox::textActivated, this, [this](const QString& size) {
        bool ok = false;
        const int pixels = size.toInt(&ok);
        if (ok) runCommand(QStringLiteral("fontSizePx"),
                           QString::number(std::clamp(pixels, 8, 96)));
    });
    formats->addWidget(m_size);

    const auto textButton = [this, formats](const QString& text,
                                            const QString& name,
                                            const QString& command) {
        auto* button = new QToolButton(m_toolbar);
        button->setText(text);
        button->setToolTip(name);
        button->setAccessibleName(name);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setFixedSize(28, 28);
        connect(button, &QToolButton::clicked, this,
                [this, command] { runCommand(command); });
        formats->addWidget(button);
        return button;
    };
    auto* bold = textButton(QStringLiteral("B"), tr("Bold"),
                            QStringLiteral("bold"));
    QFont boldFont = bold->font();
    boldFont.setBold(true);
    bold->setFont(boldFont);
    auto* italic = textButton(QStringLiteral("I"), tr("Italic"),
                              QStringLiteral("italic"));
    QFont italicFont = italic->font();
    italicFont.setItalic(true);
    italic->setFont(italicFont);
    auto* underline = textButton(QStringLiteral("U"), tr("Underline"),
                                 QStringLiteral("underline"));
    QFont underlineFont = underline->font();
    underlineFont.setUnderline(true);
    underline->setFont(underlineFont);

    auto* textColor = textButton(QStringLiteral("A"), tr("Text color"),
                                 QString());
    disconnect(textColor, nullptr, this, nullptr);
    connect(textColor, &QToolButton::clicked, this, [this] {
        const QColor color = QColorDialog::getColor(th().textPrimary, this,
                                                     tr("Text color"));
        if (color.isValid())
            runCommand(QStringLiteral("foreColor"), color.name());
    });
    auto* highlight = textButton(QStringLiteral("H"), tr("Highlight color"),
                                 QString());
    disconnect(highlight, nullptr, this, nullptr);
    connect(highlight, &QToolButton::clicked, this, [this] {
        const QColor color = QColorDialog::getColor(th().accentHighlight, this,
                                                     tr("Highlight color"));
        if (color.isValid())
            runCommand(QStringLiteral("hiliteColor"), color.name());
    });

    textButton(QStringLiteral("•"), tr("Bulleted list"),
               QStringLiteral("insertUnorderedList"));
    textButton(QStringLiteral("1."), tr("Numbered list"),
               QStringLiteral("insertOrderedList"));
    iconButton(icons::Glyph::Image, tr("Insert image"),
               [this] { chooseImage(); });
    iconButton(icons::Glyph::Eraser, tr("Clear formatting"),
               [this] { runCommand(QStringLiteral("removeFormat")); });
    column->addLayout(formats);
}

void NotebookWindow::buildTimedTextPanel() {
    m_timedTextPanel = new QWidget(this);
    m_timedTextPanel->setObjectName(QStringLiteral("NotebookTimedTextPanel"));
    m_timedTextPanel->setMinimumWidth(290);
    m_timedTextPanel->setMaximumWidth(360);
    auto* column = new QVBoxLayout(m_timedTextPanel);
    column->setContentsMargins(12, 12, 12, 12);
    column->setSpacing(8);

    auto* title = new QLabel(tr("TIMED TEXT"), m_timedTextPanel);
    title->setObjectName(QStringLiteral("NotebookTimedTextTitle"));
    column->addWidget(title);
    auto* help = new QLabel(
        tr("Place the caret in a notebook line, then stamp it at the current "
           "playhead position. You can edit both time and text below."),
        m_timedTextPanel);
    help->setObjectName(QStringLiteral("NotebookTimedTextHelp"));
    help->setWordWrap(true);
    column->addWidget(help);

    auto* stamp = new QPushButton(
        tr("Stamp selected line at playhead"), m_timedTextPanel);
    stamp->setObjectName(QStringLiteral("NotebookTimedTextStampButton"));
    stamp->setAccessibleName(tr("Stamp selected notebook line at playhead"));
    stamp->setToolTip(
        tr("Add the selected notebook line at the current project time"));
    connect(stamp, &QPushButton::clicked, this,
            &NotebookWindow::captureCurrentLine);
    column->addWidget(stamp);

    m_timedTextTable = new QTableWidget(0, 2, m_timedTextPanel);
    m_timedTextTable->setHorizontalHeaderLabels({tr("Time"), tr("Text")});
    m_timedTextTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    m_timedTextTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    m_timedTextTable->verticalHeader()->hide();
    m_timedTextTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_timedTextTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_timedTextTable->setAlternatingRowColors(true);
    m_timedTextTable->setAccessibleName(tr("Timed notebook lines"));
    m_timedTextTable->setToolTip(
        tr("Double-click a time or line to edit it"));
    connect(m_timedTextTable, &QTableWidget::itemChanged, this,
            [this] { saveTimedTextTable(); });
    column->addWidget(m_timedTextTable, 1);

    auto* editRow = new QHBoxLayout;
    editRow->setSpacing(6);
    m_setCueTime = new QPushButton(tr("Set to playhead"), m_timedTextPanel);
    m_setCueTime->setAccessibleName(
        tr("Set selected line time to playhead"));
    m_deleteCue = new QPushButton(tr("Delete"), m_timedTextPanel);
    m_deleteCue->setAccessibleName(tr("Delete selected timed line"));
    m_setCueTime->setEnabled(false);
    m_deleteCue->setEnabled(false);
    connect(m_setCueTime, &QPushButton::clicked, this,
            &NotebookWindow::setSelectedCueToPlayhead);
    connect(m_deleteCue, &QPushButton::clicked, this,
            &NotebookWindow::deleteSelectedCue);
    connect(m_timedTextTable, &QTableWidget::currentCellChanged, this,
            [this](int row) {
                const bool selected = row >= 0;
                m_setCueTime->setEnabled(selected);
                m_deleteCue->setEnabled(selected);
            });
    editRow->addWidget(m_setCueTime);
    editRow->addWidget(m_deleteCue);
    column->addLayout(editRow);

    auto* fontLabel = new QLabel(tr("Timeline font"), m_timedTextPanel);
    column->addWidget(fontLabel);
    m_timedTextFont = new QComboBox(m_timedTextPanel);
    m_timedTextFont->setAccessibleName(tr("Timed text font"));
    m_timedTextFont->setToolTip(
        tr("The default uses the transport position display font"));
    connect(m_timedTextFont, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                if (m_loadingTimedText || index < 0) return;
                ui::notebookprefs::setTimedTextFontFamily(
                    m_timedTextFont->itemData(index).toString());
                emit timedTextChanged();
            });
    column->addWidget(m_timedTextFont);

    m_timedTextStatus = new QLabel(tr("Timings are saved locally"),
                                   m_timedTextPanel);
    m_timedTextStatus->setObjectName(QStringLiteral("NotebookTimedTextStatus"));
    m_timedTextStatus->setAccessibleName(tr("Timed text save status"));
    m_timedTextStatus->setWordWrap(true);
    column->addWidget(m_timedTextStatus);
    m_timedTextPanel->hide();
}

void NotebookWindow::reloadTimedTextTable() {
    if (!m_timedTextTable) return;
    m_loadingTimedText = true;
    m_timedTextTable->setRowCount(0);
    const QVector<ui::notebookprefs::TimedCue> cues =
        ui::notebookprefs::timedCues();
    for (const auto& cue : cues) {
        const int row = m_timedTextTable->rowCount();
        m_timedTextTable->insertRow(row);
        m_timedTextTable->setItem(row, 0,
            new QTableWidgetItem(ui::notebookprefs::timedCueTimeText(
                cue.seconds)));
        m_timedTextTable->setItem(row, 1, new QTableWidgetItem(cue.text));
    }
    m_loadingTimedText = false;
    m_setCueTime->setEnabled(false);
    m_deleteCue->setEnabled(false);
}

void NotebookWindow::saveTimedTextTable() {
    if (m_loadingTimedText || !m_timedTextTable) return;
    QVector<ui::notebookprefs::TimedCue> cues;
    cues.reserve(m_timedTextTable->rowCount());
    for (int row = 0; row < m_timedTextTable->rowCount(); ++row) {
        QTableWidgetItem* timeItem = m_timedTextTable->item(row, 0);
        QTableWidgetItem* textItem = m_timedTextTable->item(row, 1);
        double seconds = 0.0;
        if (!timeItem || !ui::notebookprefs::parseTimedCueTime(
                             timeItem->text(), seconds)) {
            m_timedTextStatus->setProperty("error", true);
            m_timedTextStatus->setText(
                tr("Use seconds or the format mm:ss.mmm."));
            m_timedTextTable->setCurrentCell(row, 0);
            if (timeItem) m_timedTextTable->editItem(timeItem);
            m_timedTextStatus->style()->unpolish(m_timedTextStatus);
            m_timedTextStatus->style()->polish(m_timedTextStatus);
            return;
        }
        const QString text = textItem ? textItem->text().simplified() : QString();
        if (text.isEmpty()) {
            m_timedTextStatus->setProperty("error", true);
            m_timedTextStatus->setText(tr("Timed text cannot be empty."));
            m_timedTextTable->setCurrentCell(row, 1);
            if (textItem) m_timedTextTable->editItem(textItem);
            m_timedTextStatus->style()->unpolish(m_timedTextStatus);
            m_timedTextStatus->style()->polish(m_timedTextStatus);
            return;
        }
        cues.push_back({seconds, text});
    }

    QString error;
    if (!ui::notebookprefs::saveTimedCues(std::move(cues), &error)) {
        m_timedTextStatus->setProperty("error", true);
        m_timedTextStatus->setText(tr("Could not save timed text."));
    } else {
        m_timedTextStatus->setProperty("error", false);
        m_timedTextStatus->setText(tr("Timings are saved locally"));
        reloadTimedTextTable();
        emit timedTextChanged();
    }
    m_timedTextStatus->style()->unpolish(m_timedTextStatus);
    m_timedTextStatus->style()->polish(m_timedTextStatus);
}

void NotebookWindow::captureCurrentLine() {
    if (!m_view || !m_controller) return;
    const double seconds =
        std::max(0.0, m_controller->presentationPositionSeconds());
    m_view->page()->runJavaScript(
        QStringLiteral("currentNotebookLine();"),
        [this, seconds](const QVariant& result) {
            const QString text = result.toString().simplified().left(500);
            if (text.isEmpty()) {
                m_timedTextStatus->setProperty("error", true);
                m_timedTextStatus->setText(
                    tr("Place the caret in a non-empty notebook line first."));
                m_timedTextStatus->style()->unpolish(m_timedTextStatus);
                m_timedTextStatus->style()->polish(m_timedTextStatus);
                return;
            }
            m_loadingTimedText = true;
            const int row = m_timedTextTable->rowCount();
            m_timedTextTable->insertRow(row);
            m_timedTextTable->setItem(
                row, 0, new QTableWidgetItem(
                            ui::notebookprefs::timedCueTimeText(seconds)));
            m_timedTextTable->setItem(row, 1, new QTableWidgetItem(text));
            m_timedTextTable->selectRow(row);
            m_loadingTimedText = false;
            saveTimedTextTable();
        });
}

void NotebookWindow::setSelectedCueToPlayhead() {
    if (!m_controller || !m_timedTextTable) return;
    const int row = m_timedTextTable->currentRow();
    if (row < 0) return;
    m_loadingTimedText = true;
    m_timedTextTable->item(row, 0)->setText(
        ui::notebookprefs::timedCueTimeText(
            std::max(0.0, m_controller->presentationPositionSeconds())));
    m_loadingTimedText = false;
    saveTimedTextTable();
}

void NotebookWindow::deleteSelectedCue() {
    if (!m_timedTextTable) return;
    const int row = m_timedTextTable->currentRow();
    if (row < 0) return;
    m_loadingTimedText = true;
    m_timedTextTable->removeRow(row);
    m_loadingTimedText = false;
    saveTimedTextTable();
}

void NotebookWindow::refreshTimedTextFontChoices() {
    if (!m_timedTextFont) return;
    const QString selected = ui::notebookprefs::timedTextFontFamily();
    m_loadingTimedText = true;
    m_timedTextFont->clear();
    m_timedTextFont->addItem(tr("Transport display (default)"), QString());
    for (const QString& family : timedTextFontFamilies())
        m_timedTextFont->addItem(family, family);
    int index = selected.isEmpty() ? 0 : m_timedTextFont->findData(selected);
    if (index < 0) index = 0;
    m_timedTextFont->setCurrentIndex(index);
    m_loadingTimedText = false;
}

void NotebookWindow::updateTimedTextButtons() {
    if (m_timedTextEditorButton) {
        const bool visible = m_timedTextEditorButton->isChecked();
        const QString name = visible ? tr("Close timed text editor")
                                     : tr("Edit timed text");
        m_timedTextEditorButton->setToolTip(name);
        m_timedTextEditorButton->setAccessibleName(name);
    }
    if (m_timedTextPlaybackButton) {
        const bool enabled = m_timedTextPlaybackButton->isChecked();
        const QString name = enabled
                                 ? tr("Hide timed text from the timeline")
                                 : tr("Show timed text on the timeline");
        m_timedTextPlaybackButton->setToolTip(name);
        m_timedTextPlaybackButton->setAccessibleName(name);
    }
}

void NotebookWindow::reloadSettings() {
    const QString background = ui::notebookprefs::backgroundPath();
    const bool reduceMotion =
        QSettings().value(QStringLiteral("ui/reduceMotion"), false).toBool();
    m_backgroundPlaying = isAnimated(background) &&
                          ui::notebookprefs::animatedBackgroundsEnabled() &&
                          !reduceMotion;
    updateMotionButton();

    const QString selectedFont = m_font ? m_font->currentText() : QString();
    if (m_font) {
        m_font->clear();
        m_font->addItems(QFontDatabase::families());
        for (const QString& path : ui::notebookprefs::customFontFiles()) {
            const QString name = QFileInfo(path).completeBaseName();
            if (m_font->findText(name) < 0) m_font->addItem(name);
        }
        const int selected = m_font->findText(selectedFont);
        if (selected >= 0) m_font->setCurrentIndex(selected);
        else m_font->setCurrentText(font().family());
    }
    reloadTimedTextTable();
    refreshTimedTextFontChoices();
    if (m_timedTextPlaybackButton) {
        const QSignalBlocker block(m_timedTextPlaybackButton);
        m_timedTextPlaybackButton->setChecked(
            ui::notebookprefs::timedTextEnabled());
    }
    updateTimedTextButtons();
    m_reloadTimer->start();
}

QString NotebookWindow::pageHtml() const {
    const Theme& theme = th();
    const QString background = ui::notebookprefs::backgroundPath();
    const QString backgroundUrl =
        background.isEmpty() ? QString() : QUrl::fromLocalFile(background).toString();
    const bool video = isVideo(background);
    const bool gif = QFileInfo(background).suffix().compare(
                         QStringLiteral("gif"), Qt::CaseInsensitive) == 0;
    const double visibility =
        double(ui::notebookprefs::backgroundVisibility()) / 100.0;

    QString fontFaces;
    for (const QString& path : ui::notebookprefs::customFontFiles()) {
        const QString alias = cssQuoted(QFileInfo(path).completeBaseName());
        const QString url = cssQuoted(QUrl::fromLocalFile(path).toString());
        fontFaces += QStringLiteral(
                         "@font-face{font-family:'%1';src:url('%2');font-display:swap;}")
                         .arg(alias, url);
    }

    QString media;
    if (!backgroundUrl.isEmpty()) {
        if (video) {
            media = QStringLiteral(
                        "<video id=\"backgroundMedia\" class=\"background-media\" "
                        "src=\"%1\" muted loop playsinline aria-hidden=\"true\"></video>")
                        .arg(htmlEscaped(backgroundUrl));
        } else {
            media = QStringLiteral(
                        "<img id=\"backgroundMedia\" class=\"background-media\" "
                        "src=\"%1\" alt=\"\" aria-hidden=\"true\">"
                        "<canvas id=\"backgroundStill\" class=\"background-media\" "
                        "aria-hidden=\"true\"></canvas>")
                        .arg(htmlEscaped(backgroundUrl));
        }
    }

    const QByteArray initial = m_content.toUtf8().toBase64();
    const QString transparencyOverride = ui::GlassPanel::reduceTransparency()
        ? QStringLiteral(".paper{background:var(--surface);backdrop-filter:none}")
        : QString();
    return QStringLiteral(R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src file: data:; media-src file:; font-src file:; style-src 'unsafe-inline'; script-src 'unsafe-inline' qrc:; object-src 'none'; connect-src 'none'">
<style>
%1
:root{color-scheme:%2;--accent:%3;--text:%4;--muted:%5;--surface:%6;--page:%7;}
*{box-sizing:border-box}html,body{height:100%;margin:0;overflow:hidden}
body{background:var(--page);color:var(--text);font:16px/1.58 system-ui,-apple-system,"Segoe UI",sans-serif}
.background-media{position:fixed;inset:0;width:100%;height:100%;object-fit:cover;opacity:%8;pointer-events:none}
#backgroundStill{display:none}
.scrim{position:fixed;inset:0;background:linear-gradient(180deg,rgba(0,0,0,.08),rgba(0,0,0,.20));pointer-events:none}
.stage{height:100%;padding:clamp(18px,4vw,52px);overflow:auto}
.paper{width:min(860px,100%);min-height:100%;margin:0 auto;padding:clamp(24px,5vw,64px);border:1px solid color-mix(in srgb,var(--accent) 25%,transparent);border-radius:20px;background:var(--surface);background:color-mix(in srgb,var(--surface) 84%,transparent);box-shadow:0 18px 55px rgba(0,0,0,.20);backdrop-filter:blur(18px) saturate(125%)}
#editor{min-height:calc(100vh - 170px);outline:none;overflow-wrap:anywhere;white-space:normal}
#editor:empty::before{content:attr(data-placeholder);color:var(--muted);pointer-events:none}
#editor:focus-visible{box-shadow:inset 3px 0 var(--accent);padding-left:12px}
#editor img{display:block;max-width:100%;height:auto;margin:18px auto;border-radius:12px;box-shadow:0 8px 24px rgba(0,0,0,.20)}
#editor blockquote{margin:18px 0;padding:8px 18px;border-left:3px solid var(--accent);color:var(--muted)}
#editor h1,#editor h2{line-height:1.18}#editor p{margin:.65em 0}
@media (prefers-reduced-transparency:reduce){.paper{background:var(--surface);backdrop-filter:none}}
@media (prefers-reduced-motion:reduce){*{scroll-behavior:auto!important}}
%17
</style></head><body>
%9<div class="scrim"></div><div class="stage"><main class="paper">
<article id="editor" contenteditable="true" role="textbox" aria-multiline="true" aria-label="%10" data-placeholder="%11" spellcheck="true"></article>
</main></div>
<script src="qrc:///qtwebchannel/qwebchannel.js"></script>
<script>
const initialB64='%12';
const initialHtml=new TextDecoder().decode(Uint8Array.from(atob(initialB64),c=>c.charCodeAt(0)));
const editor=document.getElementById('editor');
const media=document.getElementById('backgroundMedia');
const still=document.getElementById('backgroundStill');
const backgroundIsVideo=%13, backgroundIsGif=%14;
let bridge=null, savedRange=null, sendTimer=0, gifSource=media&&media.src;
const allowed=new Set(['P','DIV','BR','B','STRONG','I','EM','U','S','STRIKE','FONT','SPAN','OL','UL','LI','H1','H2','H3','BLOCKQUOTE','IMG','HR','PRE','CODE','TABLE','TBODY','THEAD','TR','TD','TH']);
function sanitize(html){
 const t=document.createElement('template');t.innerHTML=html;
 for(const el of [...t.content.querySelectorAll('*')]){
  if(!allowed.has(el.tagName)){el.replaceWith(...el.childNodes);continue}
  for(const a of [...el.attributes]){
   const n=a.name.toLowerCase(),v=a.value;
   const ok=(el.tagName==='IMG'&&n==='src'&&(v.startsWith('file:')||v.startsWith('data:image/')))||(el.tagName==='IMG'&&n==='alt')||(el.tagName==='FONT'&&['face','color','size'].includes(n))||(n==='style'&&!/url\s*\(|expression\s*\(/i.test(v));
   if(!ok)el.removeAttribute(a.name);
  }
 }
 return t.innerHTML;
}
function rememberSelection(){const s=getSelection();if(s.rangeCount&&editor.contains(s.anchorNode))savedRange=s.getRangeAt(0).cloneRange()}
function restoreSelection(){editor.focus();if(!savedRange)return;const s=getSelection();s.removeAllRanges();s.addRange(savedRange)}
function currentNotebookLine(){
 if(!savedRange)return '';
 const selected=savedRange.toString().trim();if(selected)return selected;
 let node=savedRange.startContainer;if(node.nodeType===Node.TEXT_NODE)node=node.parentElement;
 while(node&&node.parentElement&&node.parentElement!==editor)node=node.parentElement;
 return node&&editor.contains(node)?(node.innerText||node.textContent||'').trim():'';
}
function applyCommand(command,value=''){
 restoreSelection();document.execCommand('styleWithCSS',false,true);
 if(command==='fontSizePx'){
  document.execCommand('fontSize',false,'7');
  for(const n of editor.querySelectorAll('font[size="7"]')){n.removeAttribute('size');n.style.fontSize=value+'px'}
 }else document.execCommand(command,false,value);
 rememberSelection();queueContent();
}
function insertNotebookImage(url,alt){restoreSelection();document.execCommand('insertHTML',false,'<img src="'+url.replaceAll('&','&amp;').replaceAll('"','&quot;')+'" alt="'+alt.replaceAll('&','&amp;').replaceAll('"','&quot;')+'">');queueContent()}
function sendContent(){clearTimeout(sendTimer);if(bridge)bridge.receiveContent(sanitize(editor.innerHTML))}
function queueContent(){clearTimeout(sendTimer);sendTimer=setTimeout(sendContent,120)}
function pauseGif(){
 if(!media||!still)return;
 if(!media.complete||!media.naturalWidth){media.addEventListener('load',pauseGif,{once:true});return}
 still.width=media.naturalWidth;still.height=media.naturalHeight;
 still.getContext('2d').drawImage(media,0,0,still.width,still.height);
 still.style.display='block';media.dataset.paused='1';media.removeAttribute('src');
}
function setBackgroundMotion(play){
 if(!media)return;
 if(backgroundIsVideo){play?media.play().catch(()=>bridge&&bridge.reportMediaError()):media.pause()}
 else if(backgroundIsGif){if(play){media.dataset.paused='0';still.style.display='none';media.src=gifSource}else pauseGif()}
}
function importImageFile(file){if(!file||!file.type.startsWith('image/'))return;const r=new FileReader();r.onload=()=>bridge&&bridge.importPastedImage(r.result,file.name||'%15');r.readAsDataURL(file)}
editor.innerHTML=sanitize(initialHtml);
document.addEventListener('selectionchange',rememberSelection);
editor.addEventListener('input',queueContent);
editor.addEventListener('blur',sendContent);
editor.addEventListener('paste',e=>{
 const image=[...e.clipboardData.items].find(i=>i.kind==='file'&&i.type.startsWith('image/'));
 if(image){e.preventDefault();importImageFile(image.getAsFile());return}
 e.preventDefault();const rich=e.clipboardData.getData('text/html');const plain=e.clipboardData.getData('text/plain');document.execCommand(rich?'insertHTML':'insertText',false,rich?sanitize(rich):plain);queueContent();
});
editor.addEventListener('dragover',e=>{if([...e.dataTransfer.items].some(i=>i.kind==='file'&&i.type.startsWith('image/')))e.preventDefault()});
editor.addEventListener('drop',e=>{const image=[...e.dataTransfer.files].find(f=>f.type.startsWith('image/'));if(image){e.preventDefault();importImageFile(image)}});
if(media)media.addEventListener('error',()=>media.dataset.paused!=='1'&&bridge&&bridge.reportMediaError());
new QWebChannel(qt.webChannelTransport,channel=>{bridge=channel.objects.notebook;setBackgroundMotion(%16);editor.focus()});
window.addEventListener('beforeunload',sendContent);
</script></body></html>)HTML")
        .arg(fontFaces,
             theme.background.lightnessF() < 0.5 ? QStringLiteral("dark")
                                                 : QStringLiteral("light"),
             theme.accent.name(), theme.textPrimary.name(),
             theme.textSecondary.name(), theme.surfaceElevated.name(),
             theme.background.name(), QString::number(visibility, 'f', 2), media,
             htmlEscaped(tr("Notebook editor")),
             htmlEscaped(tr("Start writing…")), QString::fromLatin1(initial),
             video ? QStringLiteral("true") : QStringLiteral("false"),
             gif ? QStringLiteral("true") : QStringLiteral("false"),
             htmlEscaped(tr("Pasted image")),
             m_backgroundPlaying ? QStringLiteral("true")
                                 : QStringLiteral("false"),
             transparencyOverride);
}

void NotebookWindow::renderDocument() {
    if (!m_view) return;
    QDir().mkpath(ui::notebookprefs::dataDirectory());
    QSaveFile page(editorPagePath());
    if (!page.open(QIODevice::WriteOnly)) {
        setSaveStatus(tr("Could not open notebook"), true);
        return;
    }
    page.write(pageHtml().toUtf8());
    if (!page.commit()) {
        setSaveStatus(tr("Could not open notebook"), true);
        return;
    }
    m_view->load(QUrl::fromLocalFile(editorPagePath()));
}

void NotebookWindow::applyTheme() {
    const Theme& theme = th();
    const QColor chrome = theme.toolbarBackground;
    const QColor edge = theme.accent;
    m_toolbar->setStyleSheet(QStringLiteral(R"CSS(
#NotebookToolbar { background: %1; border-bottom: 1px solid %2; }
#NotebookTitle { color: %3; font-size: 11px; font-weight: 700; letter-spacing: 1.4px; }
#NotebookSaveStatus { color: %4; font-size: 11px; }
#NotebookSaveStatus[error="true"] { color: %5; }
QComboBox, QToolButton { background: %6; color: %3; border: 1px solid %7; border-radius: 7px; padding: 3px 7px; }
QComboBox:focus, QToolButton:focus { border: 2px solid %2; }
QToolButton:hover { background: %8; }
)CSS")
                                 .arg(chrome.name(), edge.name(),
                                      theme.textPrimary.name(),
                                      theme.textSecondary.name(),
                                      Theme::mute().name(),
                                      theme.surfaceElevated.name(),
                                      theme.separator().name(),
                                      theme.accentHighlight.name()));
    if (m_timedTextPanel) {
        QColor panel = theme.toolbarBackground;
        panel.setAlpha(246);
        QColor alternate = theme.surfaceElevated;
        alternate.setAlpha(190);
        QColor selection = theme.accent;
        selection.setAlpha(110);
        m_timedTextPanel->setStyleSheet(QStringLiteral(R"CSS(
#NotebookTimedTextPanel { background: %1; border-left: 1px solid %2; }
#NotebookTimedTextTitle { color: %3; font-size: 11px; font-weight: 700; letter-spacing: 1.3px; }
#NotebookTimedTextHelp, #NotebookTimedTextStatus { color: %4; font-size: 11px; }
#NotebookTimedTextStatus[error="true"] { color: %5; }
QTableWidget { background: %6; alternate-background-color: %7; color: %3; border: 1px solid %2; border-radius: 8px; gridline-color: %2; }
QTableWidget::item:selected { background: %8; color: %3; }
QHeaderView::section { background: %7; color: %4; border: 0; border-bottom: 1px solid %2; padding: 5px; }
QComboBox:focus, QPushButton:focus { border: 2px solid %9; }
)CSS")
                                             .arg(
                                                 panel.name(QColor::HexArgb),
                                                 theme.separator().name(),
                                                 theme.textPrimary.name(),
                                                 theme.textSecondary.name(),
                                                 Theme::mute().name(),
                                                 theme.surfaceElevated.name(),
                                                 alternate.name(QColor::HexArgb),
                                                 selection.name(QColor::HexArgb),
                                                 theme.accent.name()));
    }
}

void NotebookWindow::runCommand(const QString& command, const QString& value) {
    if (!m_view) return;
    m_view->page()->runJavaScript(
        QStringLiteral("applyCommand(%1[0],%2[0]);")
            .arg(jsArray(command), jsArray(value)));
}

void NotebookWindow::chooseImage() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Insert an image"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.avif)"));
    if (path.isEmpty()) return;
    QImageReader reader(path);
    if (!reader.canRead()) {
        setSaveStatus(tr("This image could not be read"), true);
        return;
    }
    insertImageFile(path, QFileInfo(path).completeBaseName());
}

void NotebookWindow::insertImageFile(const QString& path,
                                     const QString& description) {
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        setSaveStatus(tr("This image could not be read"), true);
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&source)) {
        setSaveStatus(tr("This image could not be read"), true);
        return;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    const QString name = QString::fromLatin1(hash.result().toHex().left(24)) +
                         QLatin1Char('.') + suffix;
    QDir().mkpath(ui::notebookprefs::assetDirectory());
    const QString target = QDir(ui::notebookprefs::assetDirectory()).filePath(name);
    if (!QFileInfo::exists(target) && !QFile::copy(path, target)) {
        setSaveStatus(tr("The image could not be copied"), true);
        return;
    }
    m_view->page()->runJavaScript(
        QStringLiteral("insertNotebookImage(%1[0],%2[0]);")
            .arg(jsArray(QUrl::fromLocalFile(target).toString()),
                 jsArray(description)));
}

void NotebookWindow::receiveContent(const QString& html) {
    if (html == m_content) return;
    m_content = html;
    m_contentDirty = true;
    setSaveStatus(tr("Saving…"));
    m_saveTimer->start();
}

void NotebookWindow::importPastedImage(const QString& dataUrl,
                                       const QString& description) {
    const int comma = dataUrl.indexOf(QLatin1Char(','));
    if (comma < 0 || !dataUrl.left(comma).endsWith(QLatin1String(";base64"))) {
        setSaveStatus(tr("This image could not be read"), true);
        return;
    }
    const QString mime = dataUrl.mid(5, comma - 5 - 7).toLower();
    static const QHash<QString, QString> suffixes = {
        {QStringLiteral("image/png"), QStringLiteral("png")},
        {QStringLiteral("image/jpeg"), QStringLiteral("jpg")},
        {QStringLiteral("image/gif"), QStringLiteral("gif")},
        {QStringLiteral("image/webp"), QStringLiteral("webp")},
        {QStringLiteral("image/bmp"), QStringLiteral("bmp")}};
    const QString suffix = suffixes.value(mime);
    const QByteArray bytes = QByteArray::fromBase64(
        dataUrl.mid(comma + 1).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, suffix.toLatin1());
    if (suffix.isEmpty() || bytes.isEmpty() || bytes.size() > 32 * 1024 * 1024 ||
        !reader.canRead()) {
        setSaveStatus(tr("This image could not be read"), true);
        return;
    }
    QDir().mkpath(ui::notebookprefs::assetDirectory());
    const QString name = QString::fromLatin1(
                             QCryptographicHash::hash(bytes,
                                                      QCryptographicHash::Sha256)
                                 .toHex()
                                 .left(24)) +
                         QLatin1Char('.') + suffix;
    const QString target = QDir(ui::notebookprefs::assetDirectory()).filePath(name);
    if (!QFileInfo::exists(target)) {
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
            !file.commit()) {
            setSaveStatus(tr("The image could not be copied"), true);
            return;
        }
    }
    m_view->page()->runJavaScript(
        QStringLiteral("insertNotebookImage(%1[0],%2[0]);")
            .arg(jsArray(QUrl::fromLocalFile(target).toString()),
                 jsArray(description.isEmpty() ? tr("Pasted image") : description)));
}

void NotebookWindow::saveNow() {
    if (!m_contentDirty) return;
    QDir().mkpath(ui::notebookprefs::dataDirectory());
    QSaveFile file(ui::notebookprefs::contentFilePath());
    const QByteArray bytes = m_content.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit()) {
        setSaveStatus(tr("Could not save"), true);
        return;
    }
    m_contentDirty = false;
    setSaveStatus(tr("Saved locally"));
}

void NotebookWindow::setSaveStatus(const QString& text, bool error) {
    if (!m_saveStatus) return;
    m_saveStatus->setProperty("error", error);
    m_saveStatus->setText(text);
    m_saveStatus->style()->unpolish(m_saveStatus);
    m_saveStatus->style()->polish(m_saveStatus);
}

void NotebookWindow::reportMediaError() {
    setSaveStatus(tr("Background could not be played"), true);
}

void NotebookWindow::updateMotionButton() {
    if (!m_motionButton) return;
    const bool animated = isAnimated(ui::notebookprefs::backgroundPath());
    m_motionButton->setVisible(animated);
    {
        const QSignalBlocker block(m_motionButton);
        m_motionButton->setChecked(m_backgroundPlaying);
    }
    const QString name = m_backgroundPlaying ? tr("Pause animated background")
                                             : tr("Play animated background");
    m_motionButton->setGlyph(m_backgroundPlaying ? icons::Glyph::Pause
                                                 : icons::Glyph::Play);
    m_motionButton->setToolTip(name);
    m_motionButton->setAccessibleName(name);
}

void NotebookWindow::closeEvent(QCloseEvent* event) {
    if (m_saveTimer) m_saveTimer->stop();
    saveNow();
    QSettings().setValue(QStringLiteral("notebook/geometry"), saveGeometry());
    QDialog::closeEvent(event);
}

void NotebookWindow::hideEvent(QHideEvent* event) {
    if (m_view)
        m_view->page()->runJavaScript(QStringLiteral("setBackgroundMotion(false);"));
    emit visibilityChanged(false);
    QDialog::hideEvent(event);
}

void NotebookWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (m_view && m_backgroundPlaying)
        m_view->page()->runJavaScript(QStringLiteral("setBackgroundMotion(true);"));
    emit visibilityChanged(true);
}
