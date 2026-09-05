#include "BrowserSettingsPage.hpp"

#include "BrowserPrefs.hpp"
#include "Controls.hpp"
#include "WebPrefs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QUrl>

BrowserSettingsPage::BrowserSettingsPage(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(16, 16, 16, 16);
    column->setSpacing(8);

    column->addWidget(ui::sectionLabel(tr("FOLDERS"), this));
    auto* hint = new QLabel(
        tr("The browser shows these folders and nothing else. Their contents "
           "are read as you open them."),
        this);
    hint->setWordWrap(true);
    column->addWidget(hint);

    m_folders = new QListWidget(this);
    m_folders->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_folders, &QListWidget::currentRowChanged, this,
            [this](int row) { m_remove->setEnabled(row >= 0); });
    column->addWidget(m_folders, 1);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(8);
    auto* add = new QPushButton(tr("Add Folder…"), this);
    connect(add, &QPushButton::clicked, this, &BrowserSettingsPage::addFolder);
    m_remove = new QPushButton(tr("Remove"), this);
    m_remove->setEnabled(false);
    connect(m_remove, &QPushButton::clicked, this,
            &BrowserSettingsPage::removeSelectedFolder);
    buttons->addWidget(add);
    buttons->addWidget(m_remove);
    buttons->addStretch(1);
    column->addLayout(buttons);

    column->addWidget(ui::separatorLine(Qt::Horizontal, 0, this));
    column->addWidget(ui::sectionLabel(tr("PANEL"), this));

    auto* form = new QFormLayout();
    form->setSpacing(8);

    m_side = new QComboBox(this);
    m_side->addItems({tr("Left of the inspector"), tr("Right of the window")});
    m_side->setCurrentIndex(ui::browserprefs::onLeft() ? 0 : 1);
    connect(m_side, &QComboBox::currentIndexChanged, this, [this](int index) {
        ui::browserprefs::setOnLeft(index == 0);
        emit changed();
    });
    form->addRow(tr("Side"), m_side);

    m_autoPreview = new QCheckBox(tr("Play a file as soon as it is selected"), this);
    m_autoPreview->setChecked(ui::browserprefs::autoPreview());
    connect(m_autoPreview, &QCheckBox::toggled, this, [this](bool on) {
        ui::browserprefs::setAutoPreview(on);
        emit changed();
    });
    form->addRow(QString(), m_autoPreview);

    m_loop = new QCheckBox(tr("Repeat the preview instead of playing it once"), this);
    m_loop->setChecked(ui::browserprefs::previewLoop());
    connect(m_loop, &QCheckBox::toggled, this, [this](bool on) {
        ui::browserprefs::setPreviewLoop(on);
        emit changed();
    });
    form->addRow(QString(), m_loop);

    m_gain = new QSlider(Qt::Horizontal, this);
    m_gain->setRange(0, 200);   // percent of the recorded level
    m_gain->setValue(int(ui::browserprefs::previewGain() * 100.0f));
    connect(m_gain, &QSlider::valueChanged, this, [this](int value) {
        ui::browserprefs::setPreviewGain(float(value) / 100.0f);
        emit changed();
    });
    form->addRow(tr("Preview level"), m_gain);

    m_midiTempo = new QComboBox(this);
    m_midiTempo->addItems({tr("Ask each time"), tr("Keep the project's tempo"),
                           tr("Use the file's tempo")});
    using Policy = ui::browserprefs::MidiTempo;
    const Policy policy = ui::browserprefs::midiTempoPolicy();
    m_midiTempo->setCurrentIndex(policy == Policy::Keep    ? 1
                                 : policy == Policy::Adopt ? 2
                                                           : 0);
    connect(m_midiTempo, &QComboBox::currentIndexChanged, this, [this](int index) {
        ui::browserprefs::setMidiTempoPolicy(index == 1   ? Policy::Keep
                                             : index == 2 ? Policy::Adopt
                                                          : Policy::Ask);
        emit changed();
    });
    form->addRow(tr("Imported MIDI tempo"), m_midiTempo);

    column->addLayout(form);

    column->addWidget(ui::separatorLine(Qt::Horizontal, 0, this));
    column->addWidget(ui::sectionLabel(tr("WEB BROWSER"), this));
    auto* webHint = new QLabel(
        tr("The integrated browser keeps its own cookies and opens this page "
           "when Home is pressed. Leave it blank for the built-in VLT start "
           "page."),
        this);
    webHint->setWordWrap(true);
    column->addWidget(webHint);

    auto* webForm = new QFormLayout();
    webForm->setSpacing(8);
    m_webHome = new QLineEdit(this);
    m_webHome->setClearButtonEnabled(true);
    const QString storedHome = ui::webprefs::homeUrl();
    if (storedHome != QLatin1String(ui::webprefs::kStartUrl))
        m_webHome->setText(storedHome);
    m_webHome->setPlaceholderText(tr("VLT start page"));
    m_webHome->setAccessibleName(tr("Web browser home page"));
    connect(m_webHome, &QLineEdit::editingFinished, this, [this] {
        const QString typed = m_webHome->text().trimmed();
        QString value = typed;
        if (!value.isEmpty() && !value.contains(QStringLiteral("://")))
            value.prepend(QStringLiteral("https://"));
        const QUrl url = value.isEmpty()
                             ? QUrl(QLatin1String(ui::webprefs::kStartUrl))
                             : QUrl::fromUserInput(value);
        const QString scheme = url.scheme().toLower();
        const bool valid = url.isValid() &&
            (scheme == QLatin1String("http") || scheme == QLatin1String("https") ||
             url.toString() == QLatin1String(ui::webprefs::kStartUrl));
        if (!valid) {
            m_webHome->setToolTip(tr("Enter an HTTP or HTTPS address"));
            m_webHome->setText(ui::webprefs::homeUrl() ==
                                       QLatin1String(ui::webprefs::kStartUrl)
                                   ? QString()
                                   : ui::webprefs::homeUrl());
            return;
        }
        m_webHome->setToolTip(QString());
        ui::webprefs::setHomeUrl(url.toString());
        if (url.toString() != QLatin1String(ui::webprefs::kStartUrl))
            m_webHome->setText(url.toString());
        emit changed();
    });
    webForm->addRow(tr("Home page"), m_webHome);

    auto* backgroundRow = new QWidget(this);
    auto* backgroundLayout = new QHBoxLayout(backgroundRow);
    backgroundLayout->setContentsMargins(0, 0, 0, 0);
    backgroundLayout->setSpacing(6);
    m_webBackground = new QLineEdit(backgroundRow);
    m_webBackground->setReadOnly(true);
    m_webBackground->setPlaceholderText(tr("No custom background"));
    m_webBackground->setAccessibleName(tr("Start page background file"));
    const QString backgroundPath = ui::webprefs::startPageBackgroundPath();
    m_webBackground->setText(QDir::toNativeSeparators(backgroundPath));
    m_webBackground->setToolTip(backgroundPath);
    auto* chooseBackground = new QPushButton(tr("Choose…"), backgroundRow);
    chooseBackground->setAccessibleName(tr("Choose start page background"));
    m_clearWebBackground = new QPushButton(tr("Clear"), backgroundRow);
    m_clearWebBackground->setEnabled(!backgroundPath.isEmpty());
    backgroundLayout->addWidget(m_webBackground, 1);
    backgroundLayout->addWidget(chooseBackground);
    backgroundLayout->addWidget(m_clearWebBackground);
    webForm->addRow(tr("Start background"), backgroundRow);

    connect(chooseBackground, &QPushButton::clicked, this, [this] {
        const QString current = ui::webprefs::startPageBackgroundPath();
        const QString selected = QFileDialog::getOpenFileName(
            this, tr("Choose a start page background"), current,
            tr("Background media (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.avif "
               "*.mp4 *.m4v *.webm *.ogv *.ogg *.mov);;Images (*.png *.jpg "
               "*.jpeg *.webp *.bmp *.gif *.avif);;Videos (*.mp4 *.m4v *.webm "
               "*.ogv *.ogg *.mov)"));
        if (selected.isEmpty()) return;
        if (!ui::webprefs::setStartPageBackgroundPath(selected)) {
            m_webBackground->setToolTip(
                tr("Choose a supported image, GIF or video"));
            return;
        }
        const QString stored = ui::webprefs::startPageBackgroundPath();
        m_webBackground->setText(QDir::toNativeSeparators(stored));
        m_webBackground->setToolTip(stored);
        m_clearWebBackground->setEnabled(true);
        emit changed();
    });
    connect(m_clearWebBackground, &QPushButton::clicked, this, [this] {
        ui::webprefs::clearStartPageBackground();
        m_webBackground->clear();
        m_webBackground->setToolTip(QString());
        m_clearWebBackground->setEnabled(false);
        emit changed();
    });

    auto* backgroundHint = new QLabel(
        tr("Images, GIFs and videos stay on this computer. Animated backgrounds "
           "stop when reduced motion is enabled."),
        this);
    backgroundHint->setWordWrap(true);
    webForm->addRow(QString(), backgroundHint);

    m_webBookmarksBar =
        new QCheckBox(tr("Show the bookmarks bar"), this);
    m_webBookmarksBar->setChecked(ui::webprefs::bookmarksBarVisible());
    connect(m_webBookmarksBar, &QCheckBox::toggled, this, [this](bool visible) {
        ui::webprefs::setBookmarksBarVisible(visible);
        emit changed();
    });
    webForm->addRow(QString(), m_webBookmarksBar);
    column->addLayout(webForm);
    column->addStretch(1);

    refreshFolders();
}

void BrowserSettingsPage::refreshFolders() {
    m_folders->clear();
    m_folders->addItems(ui::browserprefs::folders());
    m_remove->setEnabled(m_folders->currentRow() >= 0);
}

void BrowserSettingsPage::addFolder() {
    const QString folder = QFileDialog::getExistingDirectory(
        this, tr("Add a folder to the browser"), QString());
    if (folder.isEmpty()) return;
    if (!ui::browserprefs::addFolder(folder)) return;
    refreshFolders();
    emit changed();
}

void BrowserSettingsPage::removeSelectedFolder() {
    QListWidgetItem* item = m_folders->currentItem();
    if (!item) return;
    ui::browserprefs::removeFolder(item->text());
    refreshFolders();
    emit changed();
}
