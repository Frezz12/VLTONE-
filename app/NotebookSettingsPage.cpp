#include "NotebookSettingsPage.hpp"

#include "Controls.hpp"
#include "NotebookPrefs.hpp"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

NotebookSettingsPage::NotebookSettingsPage(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(16, 16, 16, 16);
    column->setSpacing(10);

    column->addWidget(ui::sectionLabel(tr("APPEARANCE"), this));
    auto* hint = new QLabel(
        tr("Choose a local image, GIF or video for the notebook background. "
           "The file stays on this computer."),
        this);
    hint->setWordWrap(true);
    column->addWidget(hint);

    auto* form = new QFormLayout;
    form->setSpacing(8);

    auto* backgroundRow = new QWidget(this);
    auto* backgroundLayout = new QHBoxLayout(backgroundRow);
    backgroundLayout->setContentsMargins(0, 0, 0, 0);
    backgroundLayout->setSpacing(6);
    m_background = new QLineEdit(backgroundRow);
    m_background->setReadOnly(true);
    m_background->setPlaceholderText(tr("No custom background"));
    m_background->setAccessibleName(tr("Notebook background file"));
    auto* choose = new QPushButton(tr("Choose…"), backgroundRow);
    choose->setAccessibleName(tr("Choose notebook background"));
    m_clearBackground = new QPushButton(tr("Clear"), backgroundRow);
    backgroundLayout->addWidget(m_background, 1);
    backgroundLayout->addWidget(choose);
    backgroundLayout->addWidget(m_clearBackground);
    form->addRow(tr("Background"), backgroundRow);

    connect(choose, &QPushButton::clicked, this, [this] {
        const QString selected = QFileDialog::getOpenFileName(
            this, tr("Choose a notebook background"),
            ui::notebookprefs::backgroundPath(),
            tr("Background media (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.avif "
               "*.mp4 *.m4v *.webm *.ogv *.ogg *.mov);;Images (*.png *.jpg "
               "*.jpeg *.webp *.bmp *.gif *.avif);;Videos (*.mp4 *.m4v *.webm "
               "*.ogv *.ogg *.mov)"));
        if (selected.isEmpty()) return;
        if (!ui::notebookprefs::setBackgroundPath(selected)) {
            m_background->setToolTip(tr("Choose a supported image, GIF or video"));
            return;
        }
        refreshBackground();
        emit changed();
    });
    connect(m_clearBackground, &QPushButton::clicked, this, [this] {
        ui::notebookprefs::clearBackground();
        refreshBackground();
        emit changed();
    });

    auto* visibilityRow = new QWidget(this);
    auto* visibilityLayout = new QHBoxLayout(visibilityRow);
    visibilityLayout->setContentsMargins(0, 0, 0, 0);
    visibilityLayout->setSpacing(8);
    m_visibility = new QSlider(Qt::Horizontal, visibilityRow);
    m_visibility->setRange(0, 100);
    m_visibility->setValue(ui::notebookprefs::backgroundVisibility());
    m_visibility->setAccessibleName(tr("Background visibility"));
    m_visibilityValue = new QLabel(visibilityRow);
    m_visibilityValue->setMinimumWidth(38);
    m_visibilityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    visibilityLayout->addWidget(m_visibility, 1);
    visibilityLayout->addWidget(m_visibilityValue);
    form->addRow(tr("Visibility"), visibilityRow);
    const auto refreshVisibility = [this](int value) {
        m_visibilityValue->setText(QStringLiteral("%1%").arg(value));
    };
    refreshVisibility(m_visibility->value());
    connect(m_visibility, &QSlider::valueChanged, this,
            [this, refreshVisibility](int value) {
                refreshVisibility(value);
                ui::notebookprefs::setBackgroundVisibility(value);
                emit changed();
            });

    auto* animate = new QCheckBox(tr("Play animated GIF and video backgrounds"),
                                  this);
    animate->setChecked(ui::notebookprefs::animatedBackgroundsEnabled());
    connect(animate, &QCheckBox::toggled, this, [](bool enabled) {
        ui::notebookprefs::setAnimatedBackgroundsEnabled(enabled);
    });
    connect(animate, &QCheckBox::toggled, this, &NotebookSettingsPage::changed);
    form->addRow(QString(), animate);
    column->addLayout(form);

    column->addWidget(ui::separatorLine(Qt::Horizontal, 0, this));
    column->addWidget(ui::sectionLabel(tr("CUSTOM TEXT FONTS"), this));
    auto* fontHint = new QLabel(
        tr("Add local font files for text inside the notebook. Removing a font "
           "does not delete the original file."),
        this);
    fontHint->setWordWrap(true);
    column->addWidget(fontHint);

    m_fonts = new QListWidget(this);
    m_fonts->setMinimumHeight(100);
    m_fonts->setAccessibleName(tr("Notebook custom fonts"));
    connect(m_fonts, &QListWidget::currentRowChanged, this,
            [this](int row) { m_removeFont->setEnabled(row >= 0); });
    column->addWidget(m_fonts);

    auto* fontButtons = new QHBoxLayout;
    auto* addFont = new QPushButton(tr("Add Font…"), this);
    m_removeFont = new QPushButton(tr("Remove"), this);
    m_removeFont->setEnabled(false);
    fontButtons->addWidget(addFont);
    fontButtons->addWidget(m_removeFont);
    fontButtons->addStretch(1);
    column->addLayout(fontButtons);

    connect(addFont, &QPushButton::clicked, this, [this] {
        const QStringList selected = QFileDialog::getOpenFileNames(
            this, tr("Add notebook fonts"), QString(),
            tr("Font files (*.ttf *.otf *.ttc *.woff *.woff2)"));
        bool added = false;
        for (const QString& path : selected)
            added = ui::notebookprefs::addCustomFontFile(path) || added;
        if (!added) return;
        refreshFonts();
        emit changed();
    });
    connect(m_removeFont, &QPushButton::clicked, this, [this] {
        auto* item = m_fonts->currentItem();
        if (!item) return;
        ui::notebookprefs::removeCustomFontFile(item->data(Qt::UserRole).toString());
        refreshFonts();
        emit changed();
    });

    column->addStretch(1);
    refreshBackground();
    refreshFonts();
}

void NotebookSettingsPage::refreshBackground() {
    const QString path = ui::notebookprefs::backgroundPath();
    m_background->setText(QDir::toNativeSeparators(path));
    m_background->setToolTip(path);
    m_clearBackground->setEnabled(!path.isEmpty());
}

void NotebookSettingsPage::refreshFonts() {
    m_fonts->clear();
    for (const QString& path : ui::notebookprefs::customFontFiles()) {
        auto* item = new QListWidgetItem(QFileInfo(path).completeBaseName(), m_fonts);
        item->setData(Qt::UserRole, path);
        item->setToolTip(QDir::toNativeSeparators(path));
    }
    m_removeFont->setEnabled(m_fonts->currentRow() >= 0);
}
