#include "ForumBrowserBridge.hpp"
#include "VltContentInstaller.hpp"
#include "WebBrowserPanel.hpp"
#include "AccountService.hpp"
#include "graphics/BrowserSurface.hpp"
#include <QWebEngineProfile>
#include <QQuickWebEngineProfile>
#include <QWebEngineCookieStore>
#include <QWebEngineDownloadRequest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkCookie>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QFile>
#include <QFileDialog>
#include <QLoggingCategory>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QProgressDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QCoreApplication>
#include <QUuid>

namespace ui {
namespace {
QUrl apiURL(account::Service* service,const QString& suffix) {
    QString origin=service->apiOrigin();while(origin.endsWith('/'))origin.chop(1);
    if(!origin.endsWith("/v1"))origin+="/v1";
    return QUrl(origin+suffix);
}
bool sameOrigin(const QUrl& a,const QUrl& b){return a.scheme()==b.scheme()&&a.host()==b.host()&&a.port()==b.port();}
}
ForumBrowserBridge::ForumBrowserBridge(WebBrowserPanel* panel,account::Service* service)
    :QObject(panel),m_panel(panel),m_account(service),m_network(new QNetworkAccessManager(this)),m_origin("https://forum.vltstudio.ru") {
    m_accountKey=QStringLiteral("initial");
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    auto* profile=panel->browserProfile();
    m_cookies=profile->quick?profile->quick->cookieStore():profile->legacy->cookieStore();
    if(profile->quick)profile->quick->setHttpUserAgent(profile->quick->httpUserAgent()+" VLTONE/Forum");
    else profile->legacy->setHttpUserAgent(profile->legacy->httpUserAgent()+" VLTONE/Forum");
    connect(m_cookies,&QWebEngineCookieStore::cookieAdded,this,[this](const QNetworkCookie& cookie){
        if(cookie.name()!="vlt_web_session"||cookie.domain().remove(0,cookie.domain().startsWith('.')?1:0)!=m_origin.host())return;
        if(m_ready && m_account && !m_account->authenticated()){m_cookies->deleteCookie(cookie,m_origin);return;}
        if(m_cookie==cookie.value())return;m_cookie=cookie.value();
        m_panel->refreshSite(m_origin);
    });
    connect(m_cookies,&QWebEngineCookieStore::cookieRemoved,this,[this](const QNetworkCookie& cookie){
        if(cookie.name()=="vlt_web_session"&&cookie.value()==m_cookie)m_cookie.clear();
    });
    panel->contentDownloadDirectory=[this](QWebEngineDownloadRequest* download){
        const auto id=VltContentInstaller::attachmentId(download->url(),m_origin);
        if(id.isEmpty()||VltContentInstaller::resourceType(download->suggestedFileName()).isEmpty())return QString();
        QTemporaryDir temp(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/forum-download-XXXXXX");
        if(!temp.isValid())return QString();
        temp.setAutoRemove(false);download->setProperty("vltForumStaging",temp.path());download->setProperty("vltForumAttachment",id);return temp.path();
    };
    panel->contentDownloadCompleted=[this](QWebEngineDownloadRequest* request,const QString& path){
        if(request->property("vltForumAttachment").toString().isEmpty())return false;
        installDownloaded(request,path);return true;
    };
    connect(panel,&WebBrowserPanel::externalNavigationRequested,this,[this](const QUrl& target,const QUrl& source){
        if(target==QUrl("vltone://forum/sign-in")&&sameOrigin(source,m_origin))syncAccount(true);
    });
    connect(panel,&WebBrowserPanel::exportForumPackageRequested,this,&ForumBrowserBridge::exportPackage);
    if(!service)return;
    connect(service,&account::Service::authenticatedChanged,this,[this]{syncAccount();});
    connect(service,&account::Service::snapshotChanged,this,[this]{syncAccount();});
    connect(service,&account::Service::logoutFinished,this,[this]{syncAccount();});
    QNetworkRequest request(apiURL(service,"/forum/config"));request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    auto* reply=m_network->get(request);
    connect(reply,&QNetworkReply::finished,this,[this,reply]{
        const auto cfg=QJsonDocument::fromJson(reply->readAll()).object();
        const QUrl origin(cfg["forum_origin"].toString());
        if(reply->error()==QNetworkReply::NoError&&origin.isValid()&&origin.userInfo().isEmpty()&&!origin.hasQuery()&&!origin.hasFragment()&&(origin.path().isEmpty()||origin.path()=="/")&&(origin.scheme()=="https"||(origin.scheme()=="http"&&(origin.host()=="localhost"||origin.host()=="127.0.0.1"))))m_origin=origin;
        reply->deleteLater();m_ready=true;m_cookies->loadAllCookies();syncAccount();
    });
}
ForumBrowserBridge::~ForumBrowserBridge(){for(const auto& cancelled:m_cancellations)cancelled->store(true);m_panel->contentDownloadDirectory={};m_panel->contentDownloadCompleted={};}
void ForumBrowserBridge::clearCookie(){
    QNetworkCookie cookie("vlt_web_session",m_cookie);cookie.setPath("/");
    m_cookies->deleteCookie(cookie,m_origin);m_cookie.clear();m_panel->refreshSite(m_origin);
}
void ForumBrowserBridge::syncAccount(bool explicitLogin){
    if(!m_ready||!m_account)return;
    const QString key=m_account->authenticated()?m_account->snapshot().userId+":"+m_account->snapshot().deviceId:QString();
    if(!explicitLogin&&key==m_accountKey)return;
    m_accountKey=key;const auto generation=++m_generation;clearCookie();
    if(key.isEmpty()){if(explicitLogin)emit m_panel->statusMessage(tr("Sign in to your VLTONE account in the application first."));return;}
    QNetworkRequest request(apiURL(m_account,"/desktop/forum-session"));request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization","Bearer "+m_account->accessToken().toUtf8());request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
    auto* reply=m_network->post(request,"{}");
    connect(reply,&QNetworkReply::finished,this,[this,reply,generation]{
        const auto body=QJsonDocument::fromJson(reply->readAll()).object();
        const bool ok=reply->error()==QNetworkReply::NoError;reply->deleteLater();
        if(generation!=m_generation)return;
        if(!ok||QUrl(body["origin"].toString())!=m_origin||body["cookie_name"]!="vlt_web_session"||body["cookie"].toString().isEmpty()){
            // Let a later explicit attempt or online snapshot retry; token
            // rotation itself never replaces a successfully installed cookie.
            m_accountKey.clear();emit m_panel->statusMessage(tr("Could not sign in to the forum. Try again when online."));return;
        }
        QNetworkCookie cookie("vlt_web_session",body["cookie"].toString().toUtf8());cookie.setPath("/");cookie.setHttpOnly(true);cookie.setSecure(m_origin.scheme()=="https");cookie.setSameSitePolicy(QNetworkCookie::SameSite::Lax);
        cookie.setExpirationDate(QDateTime::fromString(body["expires_at"].toString(),Qt::ISODateWithMs));m_cookies->setCookie(cookie,m_origin);
    });
}
void ForumBrowserBridge::installDownloaded(QWebEngineDownloadRequest* request,const QString& path){
    const QString staging=request->property("vltForumStaging").toString();
    // Own the already-created staging directory across network and worker jobs.
    auto cleanup=std::shared_ptr<QTemporaryDir>(new QTemporaryDir, [staging](QTemporaryDir* p){delete p;QDir(staging).removeRecursively();});
    const QString id=request->property("vltForumAttachment").toString();
    QUrl metadata=m_origin;metadata.setPath("/api/v1/forum/attachments/"+id);
    QNetworkRequest fetch(metadata);fetch.setTransferTimeout(15000);fetch.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    if(!m_cookie.isEmpty())fetch.setRawHeader("Cookie","vlt_web_session="+m_cookie);
    auto* reply=m_network->get(fetch);
    connect(reply,&QNetworkReply::finished,this,[this,reply,path,id,cleanup]{
        const auto attachment=QJsonDocument::fromJson(reply->readAll()).object()["attachment"].toObject();
        const bool valid=reply->error()==QNetworkReply::NoError&&attachment["id"]==id;reply->deleteLater();
        if(!valid){emit m_panel->statusMessage(tr("This forum attachment is no longer available."));return;}
        runInstaller(path,attachment,cleanup);
    });
}
void ForumBrowserBridge::runInstaller(const QString& path,const QJsonObject& metadata,std::shared_ptr<QTemporaryDir> cleanup,bool replace){
    auto cancelled=std::make_shared<std::atomic_bool>(false);m_cancellations.push_back(cancelled);
    auto* progress=new QProgressDialog(tr("Installing %1…").arg(metadata["name"].toString()),tr("Cancel"),0,0,m_panel);
    progress->setWindowModality(Qt::NonModal);progress->setMinimumDuration(250);progress->setAutoClose(false);
    connect(progress,&QProgressDialog::canceled,this,[cancelled]{cancelled->store(true);});
    QPointer<ForumBrowserBridge> guard(this);QPointer<QProgressDialog> dialog(progress);
    auto work=[guard,dialog,path,metadata,cleanup,cancelled,replace]{
        const auto result=VltContentInstaller::install(path,metadata,[cancelled]{return !cancelled->load();},replace);
        QMetaObject::invokeMethod(QCoreApplication::instance(),[guard,dialog,path,metadata,cleanup,cancelled,result]{
            if(dialog){dialog->close();dialog->deleteLater();}if(!guard)return;
            guard->m_cancellations.removeAll(cancelled);
            if(result.languageConflict&&QMessageBox::question(guard->m_panel,tr("Language pack already installed"),tr("Replace the existing language pack with this download?"),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)==QMessageBox::Yes){guard->runInstaller(path,metadata,cleanup,true);return;}
            if(!result.ok)qWarning().noquote()<<"forum installation failed:"<<result.error;
            emit guard->m_panel->statusMessage(result.ok?(result.duplicate?tr("Already installed: %1"):tr("Installed: %1")).arg(metadata["name"].toString()):tr("Installation failed: %1").arg(result.error));
            // The result is also visible in the browser panel itself.
            guard->m_panel->showContentResult(result.ok?tr("Installed in your VLTONE library: %1").arg(metadata["name"].toString()):result.error);
        },Qt::QueuedConnection);
    };
    if(metadata["resource_type"]=="language")QTimer::singleShot(0,this,std::move(work));
    else QThreadPool::globalInstance()->start(std::move(work));
}
void ForumBrowserBridge::exportPackage() {
    const auto folder=QFileDialog::getExistingDirectory(m_panel,tr("Choose a saved project or template"),QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if(folder.isEmpty())return;
    const bool isTemplate=folder.endsWith(".vltt",Qt::CaseInsensitive);
    const QString suffix=isTemplate?".vltt.zip":".vlt.zip";
    QString destination=QFileDialog::getSaveFileName(m_panel,tr("Package for forum"),QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)+"/"+QFileInfo(folder).completeBaseName()+suffix,tr("VLT package (*%1)").arg(suffix));
    if(destination.isEmpty())return;
    if(!destination.endsWith(suffix,Qt::CaseInsensitive)){
        destination+=suffix;
        if(QFileInfo::exists(destination)&&QMessageBox::question(m_panel,tr("Replace package?"),tr("A package with this name already exists. Replace it?"),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
    }
    auto cancelled=std::make_shared<std::atomic_bool>(false);m_cancellations.push_back(cancelled);
    auto* progress=new QProgressDialog(tr("Packing saved project…"),tr("Cancel"),0,0,m_panel);progress->setWindowModality(Qt::NonModal);progress->show();
    connect(progress,&QProgressDialog::canceled,this,[cancelled]{cancelled->store(true);});
    QPointer<ForumBrowserBridge> guard(this);QPointer<QProgressDialog> dialog(progress);
    QThreadPool::globalInstance()->start([guard,dialog,folder,destination,cancelled]{
        QString error;const bool ok=VltContentInstaller::exportPackage(folder,destination,error,[cancelled]{return !cancelled->load();});
        QMetaObject::invokeMethod(QCoreApplication::instance(),[guard,dialog,cancelled,ok,error,destination]{
            if(dialog){dialog->close();dialog->deleteLater();}if(!guard)return;
            guard->m_cancellations.removeAll(cancelled);
            guard->m_panel->showContentResult(ok?tr("Ready to attach to your forum post: %1").arg(destination):error);
        },Qt::QueuedConnection);
    });
}
}
