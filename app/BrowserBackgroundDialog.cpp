#include "BrowserBackgroundDialog.hpp"
#include "WebPrefs.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

namespace {
bool save(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(bytes) == bytes.size() && file.commit();
}
QByteArray read(const QString& path, qsizetype limit) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.size() <= limit
               ? file.readAll()
               : QByteArray{};
}
QIcon thumbnailIcon(const QByteArray& bytes) {
    QPixmap pixmap;
    pixmap.loadFromData(bytes);
    return QIcon(
        pixmap.scaled(180, 110, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
} // namespace
BrowserBackgroundDialog::BrowserBackgroundDialog(const QUrl& apiOrigin,
                                                 QWidget* parent)
    : QDialog(parent), m_network(new QNetworkAccessManager(this)),
      m_origin(apiOrigin) {
    setWindowTitle(tr("Browser background collection"));
    resize(700, 540);
    setMinimumSize(360, 340);
    m_cache = QDir(ui::webprefs::profileStoragePath())
                  .filePath("Backgrounds/" +
                            QString::fromLatin1(QCryptographicHash::hash(
                                                    m_origin.toEncoded(),
                                                    QCryptographicHash::Sha256)
                                                    .toHex()
                                                    .left(16)));
    QDir().mkpath(m_cache);
    auto* column = new QVBoxLayout(this);
    auto* hint = new QLabel(
        tr("Choose an image for the start page. Your selected "
           "background is saved on this computer and works offline."),
        this);
    hint->setWordWrap(true);
    column->addWidget(hint);
    m_list = new QListWidget(this);
    m_list->setAccessibleName(tr("Available browser backgrounds"));
    m_list->setViewMode(QListView::IconMode);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setIconSize(QSize(180, 110));
    m_list->setGridSize(QSize(205, 150));
    m_list->setSpacing(6);
    m_list->setWordWrap(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    column->addWidget(m_list, 1);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    column->addWidget(m_status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_apply =
        buttons->addButton(tr("Use background"), QDialogButtonBox::AcceptRole);
    m_apply->setEnabled(false);
    m_refresh = buttons->addButton(tr("Refresh collection"),
                                   QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_apply, &QPushButton::clicked, this,
            &BrowserBackgroundDialog::choose);
    connect(m_refresh, &QPushButton::clicked, this,
            &BrowserBackgroundDialog::refresh);
    connect(m_list, &QListWidget::currentRowChanged, this,
            [this](int row) { m_apply->setEnabled(row >= 0 && !m_choosing); });
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this] { choose(); });
    column->addWidget(buttons);
    showCatalog(read(QDir(m_cache).filePath("catalog.json"), 512 * 1024));
    QTimer::singleShot(0, this, &BrowserBackgroundDialog::refresh);
}
BrowserBackgroundDialog::~BrowserBackgroundDialog() {
    // Stop queued downloads before members and child widgets are destroyed.
    for (auto* reply : m_network->findChildren<QNetworkReply*>()) {
        reply->disconnect(this);
        reply->abort();
    }
}

QUrl BrowserBackgroundDialog::endpoint(const QString& suffix) const {
    QUrl url = m_origin;
    QString path = url.path();
    while (path.endsWith('/'))
        path.chop(1);
    url.setPath(path + "/browser-backgrounds" + suffix);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}
QString BrowserBackgroundDialog::cachedPath(const QJsonObject& item,
                                            bool thumbnail) const {
    const QString hash = item.value("sha256").toString();
    const QString suffix = thumbnail ? ".thumb.jpg"
                           : item.value("mime_type").toString() == "image/png"
                               ? ".png"
                               : ".jpg";
    return QDir(m_cache).filePath(hash + suffix);
}
bool BrowserBackgroundDialog::validImage(const QByteArray& data,
                                         const QString& digest) const {
    if (data.isEmpty() || data.size() > 20 * 1024 * 1024)
        return false;
    if (!digest.isEmpty() &&
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() !=
            digest.toLatin1())
        return false;
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const QSize size = reader.size();
    return (reader.format() == "jpeg" || reader.format() == "png") &&
           size.width() > 0 && size.height() > 0 && size.width() <= 12000 &&
           size.height() <= 12000 &&
           qint64(size.width()) * size.height() <= 16'000'000 &&
           reader.canRead();
}
void BrowserBackgroundDialog::fetch(
    const QUrl& url, qsizetype limit,
    std::function<void(QByteArray, QString)> done) {
    QNetworkRequest request(url);
    request.setTransferTimeout(20000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    auto* reply = m_network->get(request);
    reply->setReadBufferSize(limit + 1);
    auto data = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, data, limit] {
        data->append(reply->readAll());
        if (data->size() > limit)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [reply, data, limit, done = std::move(done)] {
                data->append(reply->readAll());
                const bool ok =
                    reply->error() == QNetworkReply::NoError &&
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                            .toInt() == 200 &&
                    data->size() <= limit;
                const QString error = reply->errorString();
                reply->deleteLater();
                done(ok ? *data : QByteArray{}, ok ? QString() : error);
            });
}
void BrowserBackgroundDialog::refresh() {
    if (m_choosing || m_refreshing)
        return;
    m_refreshing = true;
    m_refresh->setEnabled(false);
    m_status->setText(tr("Loading collection…"));
    fetch(endpoint(""), 512 * 1024, [this](QByteArray bytes, QString error) {
        m_refreshing = false;
        if (m_choosing)
            return;
        m_refresh->setEnabled(true);
        if (!error.isEmpty() || !showCatalog(bytes)) {
            m_status->setText(tr("The collection is unavailable. Check your "
                                 "connection or VPN and try "
                                 "again. Previously downloaded backgrounds are "
                                 "still available."));
            return;
        }
        save(QDir(m_cache).filePath("catalog.json"), bytes);
        m_status->setText(
            m_items.isEmpty()
                ? tr("No backgrounds have been published yet.")
                : tr("Select a background and click Use background."));
    });
}
bool BrowserBackgroundDialog::showCatalog(const QByteArray& bytes) {
    const auto doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject() || !doc.object().value("backgrounds").isArray())
        return false;
    const auto list = doc.object()["backgrounds"].toArray();
    if (list.size() > 100)
        return false;
    QList<QJsonObject> items;
    const QRegularExpression idPattern(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    const QRegularExpression hashPattern("^[0-9a-f]{64}$");
    for (const auto& value : list) {
        const auto item = value.toObject();
        if (!idPattern.match(item["id"].toString()).hasMatch() ||
            !hashPattern.match(item["sha256"].toString()).hasMatch() ||
            (item["mime_type"] != "image/png" &&
             item["mime_type"] != "image/jpeg"))
            return false;
        items.append(item);
    }
    const QString selected =
        m_list->currentItem()
            ? m_list->currentItem()->data(Qt::UserRole).toString()
            : QString();
    const int generation = ++m_generation;
    m_items = items;
    m_list->clear();
    for (const auto& item : m_items) {
        const auto id = item["id"].toString();
        auto* row =
            new QListWidgetItem(item["title"].toString().left(160), m_list);
        row->setToolTip(row->text());
        row->setData(Qt::UserRole, id);
        row->setSizeHint(QSize(198, 140));
        if (id == selected ||
            cachedPath(item, false) == ui::webprefs::startPageBackgroundPath())
            m_list->setCurrentItem(row);
        const auto cached = read(cachedPath(item, true), 1024 * 1024);
        if (validImage(cached)) {
            row->setIcon(thumbnailIcon(cached));
            continue;
        }
        fetch(endpoint("/" + id + "/thumbnail"), 1024 * 1024,
              [this, generation, item, id](QByteArray data, QString error) {
                  if (generation != m_generation || !error.isEmpty() ||
                      !validImage(data))
                      return;
                  save(cachedPath(item, true), data);
                  for (int i = 0; i < m_list->count(); ++i)
                      if (m_list->item(i)->data(Qt::UserRole).toString() == id)
                          m_list->item(i)->setIcon(thumbnailIcon(data));
              });
    }
    return true;
}
void BrowserBackgroundDialog::choose() {
    const int row = m_list->currentRow();
    if (m_choosing || row < 0 || row >= m_items.size())
        return;
    const auto item = m_items[row];
    const QString path = cachedPath(item, false);
    if (validImage(read(path, 20 * 1024 * 1024), item["sha256"].toString())) {
        m_selectedPath = path;
        accept();
        return;
    }
    m_choosing = true;
    m_apply->setEnabled(false);
    m_list->setEnabled(false);
    m_refresh->setEnabled(false);
    m_status->setText(tr("Downloading background…"));
    fetch(endpoint("/" + item["id"].toString() + "/image"), 20 * 1024 * 1024,
          [this, item, path](QByteArray data, QString error) {
              m_choosing = false;
              m_apply->setEnabled(m_list->currentRow() >= 0);
              m_list->setEnabled(true);
              m_refresh->setEnabled(true);
              if (!error.isEmpty() ||
                  !validImage(data, item["sha256"].toString()) ||
                  !save(path, data)) {
                  m_status->setText(tr(
                      "The background could not be downloaded or saved. Your "
                      "current background has not changed. Try again."));
                  return;
              }
              m_selectedPath = path;
              accept();
          });
}
