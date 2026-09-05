#include "ProjectDialogs.hpp"

#include "Icons.hpp"
#include "ProjectSerializer.hpp"
#include "Theme.hpp"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace ui {
namespace {

constexpr auto kRecentProjectsSetting = "projects/recent";
constexpr int kCoverSize = 210;

QString normalizedName(QString name) {
    name = name.trimmed();
    if (name.endsWith(QStringLiteral(".vlt"), Qt::CaseInsensitive)) {
        name.chop(4);
        name = name.trimmed();
    }
    return name;
}

QString nameError(const QString& value) {
    const QString name = normalizedName(value);
    if (name.isEmpty()) return ProjectSaveDialog::tr("Enter a project name.");
    if (name == QLatin1String(".") || name == QLatin1String("..") ||
        name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')) ||
        name.contains(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*])")))) {
        return ProjectSaveDialog::tr(
            "The name cannot contain < > : \" / \\ | ? * or end with a dot.");
    }
#ifdef Q_OS_WIN
    static const QRegularExpression reserved(
        QStringLiteral(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch())
        return ProjectSaveDialog::tr("Choose another project name.");
#endif
    return {};
}

QString normalizedPath(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool samePath(const QString& left, const QString& right) {
#ifdef Q_OS_WIN
    return left.compare(right, Qt::CaseInsensitive) == 0;
#else
    return left == right;
#endif
}

QPixmap coverPixmap(const QString& path, const QSize& size) {
    if (path.isEmpty()) return {};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) return {};
    QPixmap scaled = QPixmap::fromImage(image).scaled(
        size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = qMax(0, (scaled.width() - size.width()) / 2);
    const int y = qMax(0, (scaled.height() - size.height()) / 2);
    return scaled.copy(x, y, size.width(), size.height());
}

void showCover(QLabel* label, const QString& path, const QSize& size) {
    const QPixmap artwork = coverPixmap(path, size);
    if (!artwork.isNull()) {
        label->setPixmap(artwork);
        label->setText(QString());
        label->setAccessibleName(ProjectSaveDialog::tr("Project cover"));
        return;
    }
    label->setPixmap(icons::icon(icons::Glyph::Waveform, th().textSecondary, 52)
                         .pixmap(52, 52));
    label->setText(QString());
    label->setAccessibleName(ProjectSaveDialog::tr("No project cover selected"));
}

struct ProjectSummary {
    QString path;
    QString name;
    QString author;
    QString coverPath;
    QDateTime modified;
};

ProjectSummary readSummary(const QString& path) {
    ProjectSummary summary;
    summary.path = normalizedPath(path);
    summary.name = QFileInfo(summary.path).completeBaseName();

    const QString manifest = QString::fromStdString(
        daw::ProjectSerializer::manifestPath(summary.path.toStdString()));
    summary.modified = QFileInfo(manifest).lastModified();
    QFile file(manifest);
    if (!file.open(QIODevice::ReadOnly)) return summary;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return summary;

    const QJsonObject root = document.object();
    summary.name = root.value(QStringLiteral("name")).toString(summary.name);
    summary.author = root.value(QStringLiteral("author")).toString();
    const QString cover = root.value(QStringLiteral("cover")).toString();
    if (!cover.isEmpty()) {
        summary.coverPath = QDir::isAbsolutePath(cover)
            ? cover
            : QDir(QString::fromStdString(
                       daw::ProjectSerializer::mediaPath(
                           summary.path.toStdString())))
                  .filePath(cover);
    }
    return summary;
}

} // namespace

QString ProjectSaveOptions::packagePath() const {
    return QDir(parentDirectory).filePath(normalizedName(name) +
                                          QStringLiteral(".vlt"));
}

ProjectSaveDialog::ProjectSaveDialog(const QString& name, const QString& author,
                                     const QString& coverPath,
                                     const QString& parentDirectory,
                                     QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("ProjectSaveDialog"));
    setWindowTitle(tr("Save Project"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    resize(730, 500);
    setMinimumSize(660, 460);

    auto* title = new QLabel(tr("Save your project"), this);
    title->setObjectName(QStringLiteral("ProjectDialogTitle"));
    QFont titleFont = title->font();
    titleFont.setPixelSize(22);
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto* subtitle = new QLabel(
        tr("Set the project details and choose where its portable VLT package will be saved."),
        this);
    subtitle->setObjectName(QStringLiteral("ProjectDialogSecondary"));
    subtitle->setWordWrap(true);

    m_cover = new QLabel(this);
    m_cover->setObjectName(QStringLiteral("ProjectCover"));
    m_cover->setFixedSize(kCoverSize, kCoverSize);
    m_cover->setAlignment(Qt::AlignCenter);

    auto* chooseCover = new QPushButton(tr("Choose Image…"), this);
    chooseCover->setAccessibleName(tr("Choose project cover image"));
    m_removeCover = new QPushButton(tr("Remove"), this);
    m_removeCover->setAccessibleName(tr("Remove project cover image"));
    auto* coverButtons = new QHBoxLayout;
    coverButtons->setContentsMargins(0, 0, 0, 0);
    coverButtons->setSpacing(8);
    coverButtons->addWidget(chooseCover);
    coverButtons->addWidget(m_removeCover);

    auto* coverColumn = new QVBoxLayout;
    coverColumn->setSpacing(10);
    coverColumn->addWidget(m_cover);
    coverColumn->addLayout(coverButtons);
    coverColumn->addStretch(1);

    m_name = new QLineEdit(normalizedName(name), this);
    m_name->setObjectName(QStringLiteral("ProjectName"));
    m_name->setMaxLength(120);
    m_name->setClearButtonEnabled(true);
    m_name->setAccessibleName(tr("Project name"));

    m_author = new QLineEdit(author, this);
    m_author->setObjectName(QStringLiteral("ProjectAuthor"));
    m_author->setMaxLength(120);
    m_author->setClearButtonEnabled(true);
    m_author->setAccessibleName(tr("Project author"));

    m_location = new QLineEdit(this);
    m_location->setObjectName(QStringLiteral("ProjectLocation"));
    m_location->setReadOnly(true);
    m_location->setAccessibleName(tr("Save location"));
    QString location = parentDirectory;
    if (location.isEmpty()) {
        location = QStandardPaths::writableLocation(
            QStandardPaths::DocumentsLocation);
    }
    m_location->setText(normalizedPath(location));

    auto* browseLocation = new QPushButton(tr("Browse…"), this);
    browseLocation->setAccessibleName(tr("Choose save location"));
    auto* locationRow = new QHBoxLayout;
    locationRow->setContentsMargins(0, 0, 0, 0);
    locationRow->setSpacing(8);
    locationRow->addWidget(m_location, 1);
    locationRow->addWidget(browseLocation);

    auto* locationWidget = new QWidget(this);
    locationWidget->setLayout(locationRow);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(14);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(tr("Project name"), m_name);
    form->addRow(tr("Author"), m_author);
    form->addRow(tr("Save to"), locationWidget);

    auto* destinationLabel = new QLabel(tr("Project package"), this);
    destinationLabel->setObjectName(QStringLiteral("ProjectFieldLabel"));
    m_destination = new QLabel(this);
    m_destination->setObjectName(QStringLiteral("ProjectDestination"));
    m_destination->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_destination->setWordWrap(true);
    m_destination->setAccessibleName(tr("Final project path"));

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("ProjectError"));
    m_error->setWordWrap(true);
    m_error->setAccessibleName(tr("Project name error"));

    auto* fields = new QVBoxLayout;
    fields->setSpacing(10);
    fields->addLayout(form);
    fields->addSpacing(4);
    fields->addWidget(destinationLabel);
    fields->addWidget(m_destination);
    fields->addWidget(m_error);
    fields->addStretch(1);

    auto* body = new QHBoxLayout;
    body->setSpacing(24);
    body->addLayout(coverColumn);
    body->addLayout(fields, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Save, this);
    m_save = buttons->button(QDialogButtonBox::Save);
    m_save->setObjectName(QStringLiteral("ProjectPrimaryButton"));
    m_save->setText(tr("Save Project"));
    m_save->setDefault(true);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(28, 24, 28, 24);
    column->setSpacing(10);
    column->addWidget(title);
    column->addWidget(subtitle);
    column->addSpacing(10);
    column->addLayout(body, 1);
    column->addWidget(buttons);

    connect(m_name, &QLineEdit::textChanged, this,
            &ProjectSaveDialog::updateState);
    connect(chooseCover, &QPushButton::clicked, this,
            &ProjectSaveDialog::chooseCover);
    connect(m_removeCover, &QPushButton::clicked, this,
            [this] { setCoverPath({}); });
    connect(browseLocation, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(
            this, tr("Choose Project Location"), m_location->text());
        if (path.isEmpty()) return;
        m_location->setText(normalizedPath(path));
        updateState();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_save->isEnabled()) accept();
    });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &ProjectSaveDialog::applyTheme);

    setCoverPath(coverPath);
    updateState();
    applyTheme();
    m_name->selectAll();
    m_name->setFocus(Qt::OtherFocusReason);
}

ProjectSaveOptions ProjectSaveDialog::options() const {
    return {normalizedName(m_name->text()), m_author->text().trimmed(),
            m_coverPath, m_location->text()};
}

void ProjectSaveDialog::chooseCover() {
    const QString initial = m_coverPath.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
        : QFileInfo(m_coverPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose Project Cover"), initial,
        tr("Images (*.png *.jpg *.jpeg *.webp *.bmp);;All Files (*)"));
    if (!path.isEmpty()) setCoverPath(path);
}

void ProjectSaveDialog::setCoverPath(const QString& path) {
    m_coverPath = path.isEmpty() ? QString() : normalizedPath(path);
    showCover(m_cover, m_coverPath, m_cover->size());
    m_removeCover->setEnabled(!m_coverPath.isEmpty());
}

void ProjectSaveDialog::updateState() {
    const QString error = nameError(m_name->text());
    m_error->setText(error);
    m_error->setVisible(!error.isEmpty());
    const ProjectSaveOptions value = options();
    m_destination->setText(QDir::toNativeSeparators(value.packagePath()));
    m_save->setEnabled(error.isEmpty() && QDir(value.parentDirectory).exists());
}

void ProjectSaveDialog::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#ProjectSaveDialog, #ProjectOpenDialog { background: %1; color: %2; }
#ProjectDialogTitle { color: %2; }
#ProjectDialogSecondary, #ProjectCardSecondary { color: %3; }
#ProjectCover, #ProjectCardCover {
    background: %4; border: 1px solid %5; border-radius: 12px;
}
#ProjectSaveDialog QLineEdit {
    min-height: 30px; color: %2; background: %4;
    border: 1px solid %5; border-radius: 7px; padding: 0 9px;
}
#ProjectSaveDialog QLineEdit:focus { border-color: %6; }
#ProjectDestination {
    color: %3; background: %4; border: 1px solid %5;
    border-radius: 7px; padding: 9px;
}
#ProjectError { color: %7; }
#ProjectPrimaryButton {
    min-height: 32px; color: white; background: %6;
    border: 1px solid %6; border-radius: 8px; padding: 0 18px;
    font-weight: 600;
}
#ProjectPrimaryButton:disabled { color: %3; background: %4; border-color: %5; }
)")
        .arg(t.background.name(), t.textPrimary.name(), t.textSecondary.name(),
             t.well().name(), t.separator().name(), t.accent.name(),
             Theme::record().name()));
    showCover(m_cover, m_coverPath, m_cover->size());
}

bool ProjectSaveDialog::checkForTest() const {
    return m_name && m_author && m_location && m_cover && m_destination &&
           m_error && m_save && !m_name->accessibleName().isEmpty() &&
           !m_author->accessibleName().isEmpty() &&
           !m_location->accessibleName().isEmpty() && m_save->isEnabled() &&
           m_cover->size() == QSize(kCoverSize, kCoverSize);
}

ProjectOpenDialog::ProjectOpenDialog(const QStringList& projectPaths,
                                     QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("ProjectOpenDialog"));
    setWindowTitle(tr("Open Project"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    resize(780, 570);
    setMinimumSize(620, 420);

    auto* title = new QLabel(tr("Your projects"), this);
    title->setObjectName(QStringLiteral("ProjectDialogTitle"));
    QFont titleFont = title->font();
    titleFont.setPixelSize(22);
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto* subtitle = new QLabel(
        tr("Recently opened and saved projects are kept here for quick access."),
        this);
    subtitle->setObjectName(QStringLiteral("ProjectDialogSecondary"));
    subtitle->setWordWrap(true);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("ProjectLibraryScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_projectList = new QWidget(scroll);
    m_projectList->setObjectName(QStringLiteral("ProjectLibrary"));
    auto* projects = new QVBoxLayout(m_projectList);
    projects->setContentsMargins(0, 0, 0, 0);
    projects->setSpacing(10);

    if (projectPaths.isEmpty()) {
        auto* empty = new QLabel(
            tr("No recent projects yet. Open one from disk and it will appear here."),
            m_projectList);
        empty->setObjectName(QStringLiteral("ProjectEmptyState"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        empty->setMinimumHeight(180);
        projects->addWidget(empty);
    } else {
        for (const QString& path : projectPaths) {
            const ProjectSummary summary = readSummary(path);
            auto* card = new QFrame(m_projectList);
            card->setObjectName(QStringLiteral("ProjectCard"));
            card->setToolTip(QDir::toNativeSeparators(summary.path));

            auto* cover = new QLabel(card);
            cover->setObjectName(QStringLiteral("ProjectCardCover"));
            cover->setFixedSize(92, 92);
            cover->setAlignment(Qt::AlignCenter);
            showCover(cover, summary.coverPath, cover->size());

            auto* name = new QLabel(summary.name, card);
            name->setObjectName(QStringLiteral("ProjectCardName"));
            QFont nameFont = name->font();
            nameFont.setPixelSize(16);
            nameFont.setBold(true);
            name->setFont(nameFont);

            const QString author = summary.author.isEmpty()
                ? tr("Author not specified")
                : tr("By %1").arg(summary.author);
            auto* authorLabel = new QLabel(author, card);
            authorLabel->setObjectName(QStringLiteral("ProjectCardSecondary"));

            QString modified;
            if (summary.modified.isValid()) {
                modified = tr("Modified %1").arg(
                    QLocale().toString(summary.modified, QLocale::ShortFormat));
            }
            auto* details = new QLabel(modified, card);
            details->setObjectName(QStringLiteral("ProjectCardSecondary"));

            auto* pathLabel = new QLabel(
                QDir::toNativeSeparators(summary.path), card);
            pathLabel->setObjectName(QStringLiteral("ProjectCardPath"));
            pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            pathLabel->setWordWrap(true);

            auto* text = new QVBoxLayout;
            text->setSpacing(3);
            text->addWidget(name);
            text->addWidget(authorLabel);
            if (!modified.isEmpty()) text->addWidget(details);
            text->addStretch(1);
            text->addWidget(pathLabel);

            auto* open = new QPushButton(tr("Open"), card);
            open->setObjectName(QStringLiteral("ProjectPrimaryButton"));
            open->setAccessibleName(tr("Open project %1").arg(summary.name));
            connect(open, &QPushButton::clicked, this, [this, path] {
                m_selectedPath = path;
                accept();
            });

            auto* row = new QHBoxLayout(card);
            row->setContentsMargins(12, 12, 12, 12);
            row->setSpacing(14);
            row->addWidget(cover);
            row->addLayout(text, 1);
            row->addWidget(open, 0, Qt::AlignVCenter);
            projects->addWidget(card);
        }
    }
    projects->addStretch(1);
    scroll->setWidget(m_projectList);

    m_browse = new QPushButton(tr("Browse on Disk…"), this);
    m_browse->setAccessibleName(tr("Browse for another project"));
    auto* cancel = new QPushButton(tr("Cancel"), this);
    connect(m_browse, &QPushButton::clicked, this, &ProjectOpenDialog::browse);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_browse);
    buttons->addStretch(1);
    buttons->addWidget(cancel);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(26, 22, 26, 22);
    column->setSpacing(10);
    column->addWidget(title);
    column->addWidget(subtitle);
    column->addSpacing(8);
    column->addWidget(scroll, 1);
    column->addLayout(buttons);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &ProjectOpenDialog::applyTheme);
    applyTheme();
}

void ProjectOpenDialog::browse() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open VLT Project"), QString(),
        tr("VLT Project (*.vlt);;Project Template (*.vltt);;"
           "Legacy Project (project.json);;All Files (*)"));
    if (path.isEmpty()) return;
    m_selectedPath = path;
    accept();
}

void ProjectOpenDialog::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#ProjectOpenDialog, #ProjectLibrary, #ProjectLibraryScroll,
#ProjectLibraryScroll > QWidget > QWidget { background: %1; color: %2; }
#ProjectDialogTitle, #ProjectCardName { color: %2; }
#ProjectDialogSecondary, #ProjectCardSecondary, #ProjectCardPath { color: %3; }
#ProjectCard {
    background: %4; border: 1px solid %5; border-radius: 12px;
}
#ProjectCardCover {
    background: %6; border: 1px solid %5; border-radius: 9px;
}
#ProjectEmptyState {
    color: %3; background: %4; border: 1px dashed %5;
    border-radius: 12px; padding: 24px;
}
#ProjectPrimaryButton {
    min-height: 32px; color: white; background: %7;
    border: 1px solid %7; border-radius: 8px; padding: 0 18px;
    font-weight: 600;
}
#ProjectPrimaryButton:focus { border: 2px solid %8; }
)")
        .arg(t.background.name(), t.textPrimary.name(), t.textSecondary.name(),
             t.surface.name(), t.separator().name(), t.well().name(),
             t.accent.name(), t.accentHighlight.name()));
}

bool ProjectOpenDialog::checkForTest() const {
    const bool hasCard = m_projectList &&
                         m_projectList->findChild<QPushButton*>(
                             QStringLiteral("ProjectPrimaryButton"));
    const bool hasEmptyState = m_projectList &&
                               m_projectList->findChild<QLabel*>(
                                   QStringLiteral("ProjectEmptyState"));
    return m_browse && m_projectList &&
           !m_browse->accessibleName().isEmpty() &&
           m_projectList->layout() && m_projectList->layout()->count() >= 2 &&
           (hasCard || hasEmptyState);
}

QStringList recentProjectPaths() {
    QSettings settings;
    const QStringList stored = settings.value(
        QString::fromLatin1(kRecentProjectsSetting)).toStringList();
    QStringList result;
    for (const QString& path : stored) {
        const QString normalized = normalizedPath(path);
        const QString manifest = QString::fromStdString(
            daw::ProjectSerializer::manifestPath(normalized.toStdString()));
        if (!QFileInfo::exists(manifest)) continue;
        bool duplicate = false;
        for (const QString& existing : result) {
            if (samePath(existing, normalized)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) result.push_back(normalized);
    }
    if (result != stored)
        settings.setValue(QString::fromLatin1(kRecentProjectsSetting), result);
    return result;
}

void rememberRecentProject(const QString& packagePath) {
    if (packagePath.isEmpty()) return;
    const QString normalized = normalizedPath(packagePath);
    QStringList paths = recentProjectPaths();
    for (qsizetype i = paths.size(); i-- > 0;) {
        if (samePath(paths.at(i), normalized)) paths.removeAt(i);
    }
    paths.prepend(normalized);
    QSettings().setValue(QString::fromLatin1(kRecentProjectsSetting), paths);
}

bool checkProjectDialogsForTest(QWidget* parent) {
    ProjectSaveDialog save(QStringLiteral("Demo Project"),
                           QStringLiteral("Demo Author"), {}, QDir::tempPath(),
                           parent);
    ProjectOpenDialog open(
        {QDir::temp().filePath(QStringLiteral("Demo Project.vlt"))}, parent);
    return save.checkForTest() && open.checkForTest() &&
           save.options().packagePath().endsWith(
               QStringLiteral("Demo Project.vlt"));
}

} // namespace ui
