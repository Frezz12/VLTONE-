#include "PeakBuilder.h"

#include <QThread>
#include <filesystem>

PeakBuilderWorker::PeakBuilderWorker(QObject* parent)
    : QObject(parent) {}

void PeakBuilderWorker::build(std::shared_ptr<daw::model::Source> source, QString audioPath)
{
    namespace fs = std::filesystem;
    const std::string path = audioPath.toStdString();

    auto peakFile = std::make_shared<daw::graph::PeakFile>();

    const auto peakPath = daw::graph::PeakFile::getPeakPath(path);
    if (fs::exists(peakPath) && peakFile->loadFromFile(peakPath)) {
        emit peakReady(audioPath, std::move(peakFile));
        return;
    }

    if (!peakFile->build(*source, 256)) {
        emit buildError(audioPath, QStringLiteral("PeakFile::build failed"));
        return;
    }

    peakFile->saveToFile(peakPath);
    emit peakReady(audioPath, std::move(peakFile));
}

PeakBuilder::PeakBuilder(QObject* parent)
    : QObject(parent)
{
    thread_ = new QThread(this);
    worker_ = new PeakBuilderWorker();
    worker_->moveToThread(thread_);

    connect(worker_, &PeakBuilderWorker::peakReady,
            this, &PeakBuilder::peakReady);
    connect(worker_, &PeakBuilderWorker::buildError,
            this, [](QString, QString) {});

    connect(thread_, &QThread::finished,
            worker_, &QObject::deleteLater);

    thread_->start();
}

PeakBuilder::~PeakBuilder()
{
    thread_->quit();
    thread_->wait();
}

void PeakBuilder::start(std::shared_ptr<daw::model::Source> source, const QString& audioPath)
{
    QMetaObject::invokeMethod(worker_, "build",
        Qt::QueuedConnection,
        Q_ARG(std::shared_ptr<daw::model::Source>, std::move(source)),
        Q_ARG(QString, audioPath));
}
