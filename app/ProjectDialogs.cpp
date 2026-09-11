#include "ProjectDialogs.hpp"

#include "Icons.hpp"
#include "ProjectTemplates.hpp"
#include "ProjectSerializer.hpp"
#include "Theme.hpp"
#include "TimelineBackgroundPrefs.hpp"

#include <QApplication>
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
#include <QListWidget>
#include <QLocale>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QMovie>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVideoWidget>
#include <QVBoxLayout>

namespace ui {
namespace {

constexpr auto kRecentProjectsSetting = "projects/recent";
constexpr int kCoverSize = 210;
constexpr QSize kTemplatePreviewSize(420, 244);

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

QString normalizedTemplateName(QString name) {
    name = name.trimmed();
    if (name.endsWith(QStringLiteral(".vltt"), Qt::CaseInsensitive)) {
        name.chop(5);
        name = name.trimmed();
    }
    return name;
}

QString templateNameError(const QString& value) {
    const QString name = normalizedTemplateName(value);
    if (name.isEmpty())
        return ProjectTemplateSaveDialog::tr("Enter a template name.");
    return projecttemplates::filePathForName(name).isEmpty()
        ? ProjectTemplateSaveDialog::tr(
              "The name cannot contain < > : \" / \\ | ? * or end with a dot.")
        : QString();
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

QPixmap croppedPixmap(const QString& path, const QSize& size) {
    if (path.isEmpty() || size.isEmpty()) return {};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) return {};
    const QPixmap scaled = QPixmap::fromImage(image).scaled(
        size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    return scaled.copy(std::max(0, (scaled.width() - size.width()) / 2),
                       std::max(0, (scaled.height() - size.height()) / 2),
                       size.width(), size.height());
}

QPixmap templateThumbnail(const projecttemplates::ArtworkInfo& artwork,
                          const QSize& size) {
    QPixmap thumbnail(size);
    thumbnail.fill(th().well());
    QPainter painter(&thumbnail);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto kind = timelinebackgroundprefs::mediaKind(artwork.path);
    const QPixmap image = kind == timelinebackgroundprefs::MediaKind::Video
        ? QPixmap() : croppedPixmap(artwork.path, size);
    if (!image.isNull()) painter.drawPixmap(0, 0, image);
    else {
        const icons::Glyph glyph = kind == timelinebackgroundprefs::MediaKind::Video
            ? icons::Glyph::Play : icons::Glyph::Layers;
        icons::paint(painter, glyph,
                     QRectF(QPointF(), QSizeF(size)).adjusted(
                         size.width() * 0.32, size.height() * 0.24,
                         -size.width() * 0.32, -size.height() * 0.24),
                     th().textSecondary);
    }
    if (kind == timelinebackgroundprefs::MediaKind::Video) {
        painter.setBrush(QColor(0, 0, 0, 118));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QRectF(size.width() - 29, size.height() - 29, 22, 22));
        icons::paint(painter, icons::Glyph::Play,
                     QRectF(size.width() - 24, size.height() - 24, 12, 12),
                     Qt::white);
    }
    return thumbnail;
}

class TemplateMediaPreview final : public QFrame {
public:
    explicit TemplateMediaPreview(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName(QStringLiteral("TemplateMediaPreview"));
        setMinimumSize(260, 160);
        setAccessibleName(
            ProjectTemplateOpenDialog::tr("Template artwork preview"));

        m_placeholder = new QLabel(this);
        m_placeholder->setAlignment(Qt::AlignCenter);
        m_placeholder->setPixmap(
            icons::icon(icons::Glyph::Layers, th().textSecondary, 56)
                .pixmap(56, 56));
        m_image = new QLabel(this);
        m_image->setAlignment(Qt::AlignCenter);
        m_video = new QVideoWidget(this);
        m_video->setAspectRatioMode(Qt::KeepAspectRatioByExpanding);

        m_stack = new QStackedLayout(this);
        m_stack->setContentsMargins(0, 0, 0, 0);
        m_stack->addWidget(m_placeholder);
        m_stack->addWidget(m_image);
        m_stack->addWidget(m_video);
        m_stack->setCurrentWidget(m_placeholder);
    }

    ~TemplateMediaPreview() override { clearPlayback(); }

    void setSource(const QString& path) {
        if (m_path == path) return;
        clearPlayback();
        m_path = path;
        m_sourcePixmap = {};
        const auto kind = timelinebackgroundprefs::mediaKind(path);
        if (kind == timelinebackgroundprefs::MediaKind::Video) {
            m_player = new QMediaPlayer(this);
            m_player->setVideoOutput(m_video);
            m_player->setLoops(QMediaPlayer::Infinite);
            m_player->setSource(QUrl::fromLocalFile(path));
            m_stack->setCurrentWidget(m_video);
            m_player->play();
            return;
        }
        if (kind == timelinebackgroundprefs::MediaKind::AnimatedImage) {
            m_movie = new QMovie(path, QByteArray(), this);
            connect(m_movie, &QMovie::frameChanged, this,
                    [this] { showMovieFrame(); });
            m_stack->setCurrentWidget(m_image);
            m_movie->start();
            return;
        }
        if (kind == timelinebackgroundprefs::MediaKind::Image) {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            m_sourcePixmap = QPixmap::fromImage(reader.read());
            refreshImage();
            m_stack->setCurrentWidget(
                m_sourcePixmap.isNull() ? m_placeholder : m_image);
            return;
        }
        m_stack->setCurrentWidget(m_placeholder);
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QFrame::resizeEvent(event);
        refreshImage();
        showMovieFrame();
    }

private:
    void clearPlayback() {
        if (m_movie) {
            m_movie->stop();
            delete m_movie;
            m_movie = nullptr;
        }
        if (m_player) {
            m_player->stop();
            m_player->setVideoOutput(nullptr);
            delete m_player;
            m_player = nullptr;
        }
        m_video->setVisible(false);
        m_image->clear();
        if (m_stack) m_stack->setCurrentWidget(m_placeholder);
    }

    void showPixmap(const QPixmap& source) {
        if (source.isNull() || size().isEmpty()) return;
        const QPixmap scaled = source.scaled(
            size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        m_image->setPixmap(scaled.copy(
            std::max(0, (scaled.width() - width()) / 2),
            std::max(0, (scaled.height() - height()) / 2), width(), height()));
    }

    void refreshImage() {
        if (!m_sourcePixmap.isNull()) showPixmap(m_sourcePixmap);
    }

    void showMovieFrame() {
        if (m_movie && m_movie->isValid()) showPixmap(m_movie->currentPixmap());
    }

    QString m_path;
    QStackedLayout* m_stack = nullptr;
    QLabel* m_placeholder = nullptr;
    QLabel* m_image = nullptr;
    QVideoWidget* m_video = nullptr;
    QMovie* m_movie = nullptr;
    QMediaPlayer* m_player = nullptr;
    QPixmap m_sourcePixmap;
};

TemplateMediaPreview* mediaPreview(QWidget* widget) {
    return static_cast<TemplateMediaPreview*>(widget);
}

QString mediaDescription(const projecttemplates::ArtworkInfo& artwork) {
    if (artwork.path.isEmpty())
        return ProjectTemplateOpenDialog::tr("Standard template artwork");
    const auto kind = timelinebackgroundprefs::mediaKind(artwork.path);
    const QString type = kind == timelinebackgroundprefs::MediaKind::Video
        ? ProjectTemplateOpenDialog::tr("Video")
        : kind == timelinebackgroundprefs::MediaKind::AnimatedImage
            ? ProjectTemplateOpenDialog::tr("Animated GIF")
            : ProjectTemplateOpenDialog::tr("Image");
    return QStringLiteral("%1  ·  %2").arg(artwork.displayName, type);
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
    const QString projectName = normalizedName(name);
#ifdef Q_OS_MACOS
    return QDir(parentDirectory).filePath(projectName);
#else
    return QDir(parentDirectory).filePath(projectName + QStringLiteral(".vlt"));
#endif
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
        tr("Set the project details and choose where its portable VLTONE project folder will be saved."),
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

    auto* destinationLabel = new QLabel(tr("Project folder"), this);
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
        this, tr("Open VLTONE Project"), QString(),
        tr("VLTONE Project (*.vlt);;Project Template (*.vltt);;"
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

ProjectTemplateSaveDialog::ProjectTemplateSaveDialog(
    const QString& name, const QString& artworkPath, QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("ProjectTemplateSaveDialog"));
    setWindowTitle(tr("Save as Template"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    resize(760, 500);
    setMinimumSize(680, 450);

    auto* title = new QLabel(tr("Create a project template"), this);
    title->setObjectName(QStringLiteral("ProjectDialogTitle"));
    QFont titleFont = title->font();
    titleFont.setPixelSize(22);
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto* subtitle = new QLabel(
        tr("Save the current tracks and routing with artwork that makes the template easy to recognise."),
        this);
    subtitle->setObjectName(QStringLiteral("ProjectDialogSecondary"));
    subtitle->setWordWrap(true);

    m_preview = new TemplateMediaPreview(this);
    m_preview->setObjectName(QStringLiteral("TemplateMediaPreview"));
    m_preview->setFixedSize(kTemplatePreviewSize);

    auto* choose = new QPushButton(tr("Choose Photo, GIF or Video…"), this);
    choose->setAccessibleName(tr("Choose template artwork"));
    m_removeArtwork = new QPushButton(tr("Remove"), this);
    m_removeArtwork->setAccessibleName(tr("Remove template artwork"));
    auto* mediaButtons = new QHBoxLayout;
    mediaButtons->setContentsMargins(0, 0, 0, 0);
    mediaButtons->setSpacing(8);
    mediaButtons->addWidget(choose);
    mediaButtons->addWidget(m_removeArtwork);

    m_mediaName = new QLabel(this);
    m_mediaName->setObjectName(QStringLiteral("TemplateMediaName"));
    m_mediaName->setWordWrap(true);
    m_mediaName->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* artwork = new QVBoxLayout;
    artwork->setContentsMargins(0, 0, 0, 0);
    artwork->setSpacing(8);
    artwork->addWidget(m_preview);
    artwork->addWidget(m_mediaName);
    artwork->addLayout(mediaButtons);

    m_name = new QLineEdit(normalizedTemplateName(name), this);
    m_name->setObjectName(QStringLiteral("TemplateName"));
    m_name->setMaxLength(120);
    m_name->setClearButtonEnabled(true);
    m_name->setAccessibleName(tr("Template name"));

    auto* nameLabel = new QLabel(tr("Template name"), this);
    nameLabel->setObjectName(QStringLiteral("ProjectFieldLabel"));
    m_destination = new QLabel(this);
    m_destination->setObjectName(QStringLiteral("ProjectDestination"));
    m_destination->setWordWrap(true);
    m_destination->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_destination->setAccessibleName(tr("Template package location"));
    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("ProjectError"));
    m_error->setWordWrap(true);

    auto* fields = new QVBoxLayout;
    fields->setSpacing(8);
    fields->addWidget(nameLabel);
    fields->addWidget(m_name);
    fields->addSpacing(10);
    fields->addWidget(new QLabel(tr("Saved in the VLTONE template library"), this));
    fields->addWidget(m_destination);
    fields->addWidget(m_error);
    fields->addStretch(1);

    auto* body = new QHBoxLayout;
    body->setSpacing(24);
    body->addLayout(artwork);
    body->addLayout(fields, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Save, this);
    m_save = buttons->button(QDialogButtonBox::Save);
    m_save->setObjectName(QStringLiteral("ProjectPrimaryButton"));
    m_save->setText(tr("Save Template"));
    m_save->setDefault(true);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(28, 24, 28, 24);
    column->setSpacing(10);
    column->addWidget(title);
    column->addWidget(subtitle);
    column->addSpacing(8);
    column->addLayout(body, 1);
    column->addWidget(buttons);

    connect(m_name, &QLineEdit::textChanged, this,
            &ProjectTemplateSaveDialog::updateState);
    connect(choose, &QPushButton::clicked, this,
            &ProjectTemplateSaveDialog::chooseArtwork);
    connect(m_removeArtwork, &QPushButton::clicked, this,
            [this] { setArtworkPath({}); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_save->isEnabled()) accept();
    });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &ProjectTemplateSaveDialog::applyTheme);

    setArtworkPath(artworkPath);
    updateState();
    applyTheme();
    m_name->selectAll();
    m_name->setFocus(Qt::OtherFocusReason);
}

ProjectTemplateSaveOptions ProjectTemplateSaveDialog::options() const {
    return {normalizedTemplateName(m_name->text()), m_artworkPath};
}

void ProjectTemplateSaveDialog::chooseArtwork() {
    const QString initial = m_artworkPath.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
        : QFileInfo(m_artworkPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose Template Artwork"), initial,
        tr("Photos, GIFs and Videos (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.mp4 *.m4v *.webm *.ogv *.mov *.mkv *.avi);;All Files (*)"));
    if (!path.isEmpty()) setArtworkPath(path);
}

void ProjectTemplateSaveDialog::setArtworkPath(const QString& path) {
    const QString normalized = path.isEmpty()
        ? QString() : normalizedPath(path);
    m_artworkPath = timelinebackgroundprefs::isSupported(normalized)
        ? normalized : QString();
    mediaPreview(m_preview)->setSource(m_artworkPath);
    const projecttemplates::ArtworkInfo info{
        m_artworkPath,
        m_artworkPath.isEmpty() ? QString() : QFileInfo(m_artworkPath).fileName()};
    m_mediaName->setText(mediaDescription(info));
    m_mediaName->setToolTip(m_artworkPath);
    m_removeArtwork->setEnabled(!m_artworkPath.isEmpty());
}

void ProjectTemplateSaveDialog::updateState() {
    const QString error = templateNameError(m_name->text());
    m_error->setText(error);
    m_error->setVisible(!error.isEmpty());
    const QString path = projecttemplates::filePathForName(
        normalizedTemplateName(m_name->text()));
    m_destination->setText(QDir::toNativeSeparators(path));
    m_save->setEnabled(error.isEmpty() && !path.isEmpty());
}

void ProjectTemplateSaveDialog::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#ProjectTemplateSaveDialog { background: %1; color: %2; }
#ProjectDialogTitle { color: %2; }
#ProjectDialogSecondary, #TemplateMediaName { color: %3; }
#TemplateMediaPreview {
    background: %4; border: 1px solid %5; border-radius: 14px;
}
#ProjectTemplateSaveDialog QLineEdit {
    min-height: 32px; color: %2; background: %4;
    border: 1px solid %5; border-radius: 8px; padding: 0 10px;
}
#ProjectTemplateSaveDialog QLineEdit:focus { border-color: %6; }
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
    mediaPreview(m_preview)->setSource({});
    mediaPreview(m_preview)->setSource(m_artworkPath);
}

bool ProjectTemplateSaveDialog::checkForTest() const {
    return m_name && m_preview && m_mediaName && m_destination && m_error &&
           m_removeArtwork && m_save && m_save->isEnabled() &&
           !m_name->accessibleName().isEmpty() &&
           m_preview->size() == kTemplatePreviewSize;
}

ProjectTemplateOpenDialog::ProjectTemplateOpenDialog(
    const QStringList& templatePaths, QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("ProjectTemplateOpenDialog"));
    setWindowTitle(tr("New Project from Template"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    resize(940, 610);
    setMinimumSize(760, 500);

    auto* title = new QLabel(tr("Choose a project template"), this);
    title->setObjectName(QStringLiteral("ProjectDialogTitle"));
    QFont titleFont = title->font();
    titleFont.setPixelSize(22);
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto* subtitle = new QLabel(
        tr("Preview your saved setups. A new project remains independent of its template."),
        this);
    subtitle->setObjectName(QStringLiteral("ProjectDialogSecondary"));

    m_templates = new QListWidget(this);
    m_templates->setObjectName(QStringLiteral("ProjectTemplateList"));
    m_templates->setAccessibleName(tr("Project templates"));
    m_templates->setSelectionMode(QAbstractItemView::SingleSelection);
    m_templates->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_templates->setSpacing(4);
    m_templates->setMinimumWidth(320);

    for (const QString& path : templatePaths) {
        const projecttemplates::ArtworkInfo art = projecttemplates::artwork(path);
        auto* item = new QListWidgetItem(m_templates);
        item->setData(Qt::UserRole, path);
        item->setSizeHint(QSize(300, 88));
        item->setToolTip(QDir::toNativeSeparators(path));

        auto* card = new QFrame(m_templates);
        card->setObjectName(QStringLiteral("TemplateListCard"));
        card->setProperty("selected", false);
        card->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* thumbnail = new QLabel(card);
        thumbnail->setObjectName(QStringLiteral("TemplateListThumbnail"));
        thumbnail->setFixedSize(96, 64);
        thumbnail->setPixmap(templateThumbnail(art, thumbnail->size()));
        thumbnail->setAlignment(Qt::AlignCenter);

        auto* itemName = new QLabel(projecttemplates::displayName(path), card);
        itemName->setObjectName(QStringLiteral("TemplateListName"));
        QFont itemFont = itemName->font();
        itemFont.setPixelSize(15);
        itemFont.setBold(true);
        itemName->setFont(itemFont);
        auto* itemMedia = new QLabel(mediaDescription(art), card);
        itemMedia->setObjectName(QStringLiteral("TemplateListMedia"));
        itemMedia->setWordWrap(true);

        auto* itemText = new QVBoxLayout;
        itemText->setContentsMargins(0, 0, 0, 0);
        itemText->setSpacing(4);
        itemText->addStretch(1);
        itemText->addWidget(itemName);
        itemText->addWidget(itemMedia);
        itemText->addStretch(1);
        auto* itemRow = new QHBoxLayout(card);
        itemRow->setContentsMargins(9, 8, 9, 8);
        itemRow->setSpacing(12);
        itemRow->addWidget(thumbnail);
        itemRow->addLayout(itemText, 1);
        m_templates->setItemWidget(item, card);
    }

    m_empty = new QLabel(
        tr("No templates yet\n\nUse File → Save as Template… to create one."),
        this);
    m_empty->setObjectName(QStringLiteral("ProjectEmptyState"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    auto* library = new QWidget(this);
    auto* libraryStack = new QStackedLayout(library);
    libraryStack->setContentsMargins(0, 0, 0, 0);
    libraryStack->addWidget(m_templates);
    libraryStack->addWidget(m_empty);
    if (templatePaths.isEmpty()) libraryStack->setCurrentWidget(m_empty);
    else libraryStack->setCurrentWidget(m_templates);

    auto* details = new QFrame(this);
    details->setObjectName(QStringLiteral("TemplateDetails"));
    m_preview = new TemplateMediaPreview(details);
    m_preview->setObjectName(QStringLiteral("TemplateMediaPreview"));
    m_preview->setMinimumSize(360, 220);
    m_name = new QLabel(details);
    m_name->setObjectName(QStringLiteral("TemplateDetailsName"));
    QFont detailFont = m_name->font();
    detailFont.setPixelSize(20);
    detailFont.setBold(true);
    m_name->setFont(detailFont);
    m_mediaName = new QLabel(details);
    m_mediaName->setObjectName(QStringLiteral("TemplateMediaName"));
    m_mediaName->setWordWrap(true);

    m_create = new QPushButton(tr("Create Project"), details);
    m_create->setObjectName(QStringLiteral("ProjectPrimaryButton"));
    m_create->setAccessibleName(tr("Create project from selected template"));
    m_create->setDefault(true);
    m_delete = new QPushButton(
        icons::icon(icons::Glyph::Trash, th().textPrimary, 16),
        tr("Delete Template"), details);
    m_delete->setObjectName(QStringLiteral("TemplateDeleteButton"));
    m_delete->setAccessibleName(tr("Delete selected template"));
    auto* detailButtons = new QHBoxLayout;
    detailButtons->setContentsMargins(0, 0, 0, 0);
    detailButtons->setSpacing(8);
    detailButtons->addWidget(m_create);
    detailButtons->addWidget(m_delete);
    detailButtons->addStretch(1);

    auto* detailColumn = new QVBoxLayout(details);
    detailColumn->setContentsMargins(16, 16, 16, 16);
    detailColumn->setSpacing(9);
    detailColumn->addWidget(m_preview, 1);
    detailColumn->addWidget(m_name);
    detailColumn->addWidget(m_mediaName);
    detailColumn->addLayout(detailButtons);

    auto* body = new QHBoxLayout;
    body->setSpacing(14);
    body->addWidget(library, 0);
    body->addWidget(details, 1);

    auto* cancel = new QPushButton(tr("Cancel"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(cancel);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(26, 22, 26, 22);
    column->setSpacing(10);
    column->addWidget(title);
    column->addWidget(subtitle);
    column->addSpacing(6);
    column->addLayout(body, 1);
    column->addLayout(bottom);

    connect(m_templates, &QListWidget::currentItemChanged, this,
            [this] { updateSelection(); });
    connect(m_templates, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) {
                if (m_create->isEnabled()) m_create->click();
            });
    connect(m_create, &QPushButton::clicked, this, [this] {
        const QListWidgetItem* item = m_templates->currentItem();
        if (!item) return;
        m_selectedPath = item->data(Qt::UserRole).toString();
        accept();
    });
    connect(m_delete, &QPushButton::clicked, this,
            &ProjectTemplateOpenDialog::deleteSelected);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &ProjectTemplateOpenDialog::applyTheme);

    if (m_templates->count() > 0) m_templates->setCurrentRow(0);
    updateSelection();
    applyTheme();
}

void ProjectTemplateOpenDialog::updateSelection() {
    QListWidgetItem* current = m_templates->currentItem();
    for (int row = 0; row < m_templates->count(); ++row) {
        QWidget* card = m_templates->itemWidget(m_templates->item(row));
        if (!card) continue;
        card->setProperty("selected", m_templates->item(row) == current);
        card->style()->unpolish(card);
        card->style()->polish(card);
    }
    const bool selected = current != nullptr;
    m_create->setEnabled(selected);
    m_delete->setEnabled(selected);
    if (!selected) {
        m_name->setText(tr("Select a template"));
        m_mediaName->clear();
        mediaPreview(m_preview)->setSource({});
        return;
    }
    const QString path = current->data(Qt::UserRole).toString();
    const projecttemplates::ArtworkInfo art = projecttemplates::artwork(path);
    m_name->setText(projecttemplates::displayName(path));
    m_mediaName->setText(mediaDescription(art));
    m_mediaName->setToolTip(art.path);
    mediaPreview(m_preview)->setSource(art.path);
}

void ProjectTemplateOpenDialog::deleteSelected() {
    QListWidgetItem* item = m_templates->currentItem();
    if (!item) return;
    const QString path = item->data(Qt::UserRole).toString();
    const QString name = projecttemplates::displayName(path);
    QMessageBox confirmation(QMessageBox::Warning, tr("Delete Template"),
                             tr("Delete “%1”? This cannot be undone.").arg(name),
                             QMessageBox::NoButton, this);
    QPushButton* confirmDelete = confirmation.addButton(
        tr("Delete"), QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != confirmDelete) return;
    const projecttemplates::ArtworkInfo artwork =
        projecttemplates::artwork(path);
    // Windows keeps a playing media file locked. Release the selected preview
    // before removing its portable package, then restore it if deletion fails.
    mediaPreview(m_preview)->setSource({});
    QString error;
    if (!projecttemplates::remove(path, &error)) {
        mediaPreview(m_preview)->setSource(artwork.path);
        QMessageBox::warning(this, tr("Delete Template Failed"), error);
        return;
    }
    const int row = m_templates->row(item);
    QWidget* card = m_templates->itemWidget(item);
    m_templates->removeItemWidget(item);
    delete card;
    delete m_templates->takeItem(row);
    m_libraryChanged = true;
    if (m_templates->count() > 0)
        m_templates->setCurrentRow(std::min(row, m_templates->count() - 1));
    else if (auto* stack = qobject_cast<QStackedLayout*>(m_empty->parentWidget()->layout()))
        stack->setCurrentWidget(m_empty);
    updateSelection();
}

void ProjectTemplateOpenDialog::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#ProjectTemplateOpenDialog { background: %1; color: %2; }
#ProjectDialogTitle, #TemplateDetailsName, #TemplateListName { color: %2; }
#ProjectDialogSecondary, #TemplateMediaName, #TemplateListMedia { color: %3; }
#ProjectTemplateList { background: transparent; border: none; outline: none; }
#ProjectTemplateList::item { border: none; padding: 0; }
#TemplateListCard {
    background: %4; border: 1px solid %5; border-radius: 12px;
}
#TemplateListCard[selected="true"] {
    background: %6; border-color: %7;
}
#TemplateListThumbnail, #TemplateMediaPreview {
    background: %8; border: 1px solid %5; border-radius: 10px;
}
#TemplateDetails {
    background: %4; border: 1px solid %5; border-radius: 14px;
}
#ProjectEmptyState {
    color: %3; background: %4; border: 1px dashed %5;
    border-radius: 12px; padding: 24px;
}
#ProjectPrimaryButton {
    min-height: 34px; color: white; background: %7;
    border: 1px solid %7; border-radius: 8px; padding: 0 18px;
    font-weight: 600;
}
#ProjectPrimaryButton:disabled { color: %3; background: %8; border-color: %5; }
#TemplateDeleteButton {
    min-height: 34px; color: %2; background: transparent;
    border: 1px solid %5; border-radius: 8px; padding: 0 13px;
}
#TemplateDeleteButton:hover { border-color: %9; color: %9; }
)")
        .arg(t.background.name(), t.textPrimary.name(), t.textSecondary.name(),
             t.surface.name(), t.separator().name(), t.surfaceElevated.name(),
             t.accent.name(), t.well().name(), Theme::record().name()));
    updateSelection();
}

bool ProjectTemplateOpenDialog::checkForTest() const {
    return m_templates && m_preview && m_name && m_mediaName && m_empty &&
           m_create && m_delete &&
           !m_templates->accessibleName().isEmpty() &&
           !m_create->accessibleName().isEmpty() &&
           !m_delete->accessibleName().isEmpty();
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
    QTemporaryDir temporary;
    const QString package =
        QDir(temporary.path()).filePath(QStringLiteral("Recording.vltt"));
    QDir().mkpath(package);
    const QString artwork =
        QDir(temporary.path()).filePath(QStringLiteral("studio-cover.png"));
    QImage image(160, 90, QImage::Format_RGB32);
    image.fill(QColor(QStringLiteral("#426b9e")));
    const QString suppliedMedia = qEnvironmentVariable("VLT_TEMPLATE_TEST_MEDIA");
    const QString testMedia = suppliedMedia.isEmpty() ? artwork : suppliedMedia;
    const bool imageSaved = suppliedMedia.isEmpty() ? image.save(artwork)
                                                     : QFileInfo(testMedia).isFile();
    QString artworkError;
    const bool artworkSaved = imageSaved &&
        projecttemplates::installArtwork(package, testMedia, &artworkError);
    const projecttemplates::ArtworkInfo stored =
        projecttemplates::artwork(package);
    ProjectTemplateSaveDialog templateSave(
        QStringLiteral("Recording"), testMedia, parent);
    ProjectTemplateOpenDialog templateOpen({package}, parent);
    bool screenshotSaved = true;
    const QString saveScreenshot =
        qEnvironmentVariable("VLT_TEMPLATE_SAVE_DIALOG_SCREENSHOT");
    if (!saveScreenshot.isEmpty()) {
        templateSave.show();
        QApplication::processEvents();
        screenshotSaved = templateSave.grab().save(saveScreenshot);
        templateSave.hide();
    }
    const QString screenshot =
        qEnvironmentVariable("VLT_TEMPLATE_DIALOG_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        templateOpen.show();
        QApplication::processEvents();
        screenshotSaved = templateOpen.grab().save(screenshot);
        templateOpen.hide();
    }
    const QString expectedProjectFolder =
#ifdef Q_OS_MACOS
        QStringLiteral("Demo Project");
#else
        QStringLiteral("Demo Project.vlt");
#endif
    return save.checkForTest() && open.checkForTest() &&
           templateSave.checkForTest() && templateOpen.checkForTest() &&
           artworkSaved && QFileInfo(stored.path).isFile() &&
           stored.displayName == QFileInfo(testMedia).fileName() &&
           screenshotSaved &&
           QFileInfo(save.options().packagePath()).fileName() ==
               expectedProjectFolder;
}

} // namespace ui
