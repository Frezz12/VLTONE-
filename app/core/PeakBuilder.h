#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "daw/graph/PeakFile.h"
#include "daw/model/Source.h"

class PeakBuilderWorker : public QObject {
    Q_OBJECT
public:
    explicit PeakBuilderWorker(QObject* parent = nullptr);

public slots:
    void build(std::shared_ptr<daw::model::Source> source, QString audioPath);

signals:
    void peakReady(QString audioPath, std::shared_ptr<daw::graph::PeakFile> peakFile);
    void buildError(QString audioPath, QString error);
};

class PeakBuilder : public QObject {
    Q_OBJECT
public:
    explicit PeakBuilder(QObject* parent = nullptr);
    ~PeakBuilder() override;

    void start(std::shared_ptr<daw::model::Source> source, const QString& audioPath);

signals:
    void peakReady(const QString& audioPath, std::shared_ptr<daw::graph::PeakFile> peakFile);

private:
    PeakBuilderWorker* worker_ = nullptr;
    QThread* thread_ = nullptr;
};
