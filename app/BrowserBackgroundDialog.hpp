#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QUrl>
#include <functional>
class QLabel;
class QListWidget;
class QPushButton;
class QNetworkAccessManager;

class BrowserBackgroundDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit BrowserBackgroundDialog(const QUrl& apiOrigin,
                                     QWidget* parent = nullptr);
    ~BrowserBackgroundDialog() override;
    QString selectedPath() const { return m_selectedPath; }

  private:
    void refresh();
    bool showCatalog(const QByteArray& bytes);
    void choose();
    void fetch(const QUrl& url, qsizetype limit,
               std::function<void(QByteArray, QString)> done);
    QUrl endpoint(const QString& suffix) const;
    QString cachedPath(const QJsonObject& item, bool thumbnail) const;
    bool validImage(const QByteArray& data, const QString& digest = {}) const;
    QNetworkAccessManager* m_network;
    QListWidget* m_list;
    QLabel* m_status;
    QPushButton* m_apply;
    QPushButton* m_refresh;
    QUrl m_origin;
    QString m_cache;
    QString m_selectedPath;
    QList<QJsonObject> m_items;
    int m_generation = 0;
    bool m_choosing = false;
    bool m_refreshing = false;
};
