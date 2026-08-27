#include "MixerView.h"

#include <QFrame>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
// Шкала фейдера в дБ. Ниже kMinDb cчитаем тишиной, как на реальных пультах.
constexpr int kMinDb = -60;
constexpr int kMaxDb = 6;

float dbToLinear(int db)
{
    return db <= kMinDb ? 0.0f : std::pow(10.0f, static_cast<float>(db) / 20.0f);
}

int linearToDb(float linear)
{
    if (linear <= 0.0f)
        return kMinDb;
    const int db = static_cast<int>(std::lround(20.0 * std::log10(linear)));
    return std::clamp(db, kMinDb, kMaxDb);
}

QString dbText(int db)
{
    if (db <= kMinDb)
        return QStringLiteral("−∞");        // −∞
    return db > 0 ? QStringLiteral("+%1").arg(db) : QString::number(db);
}
} // namespace

MixerView::MixerView(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    stripLayout_ = new QHBoxLayout();
    stripLayout_->setSpacing(8);
    root->addLayout(stripLayout_);
    root->addStretch(1);

    placeholder_ = new QLabel(tr("Откройте аудиофайл — появится полоcа микшера"), this);
    placeholder_->setStyleSheet(QStringLiteral("color: #6a6f76;"));
    stripLayout_->addWidget(placeholder_);
}

void MixerView::setSession(std::shared_ptr<daw::model::Session> session)
{
    session_ = std::move(session);
    clearStrips();

    if (!session_ || session_->trackCount() == 0) {
        placeholder_ = new QLabel(tr("Откройте аудиофайл — появится полоcа микшера"), this);
        placeholder_->setStyleSheet(QStringLiteral("color: #6a6f76;"));
        stripLayout_->addWidget(placeholder_);
        return;
    }

    for (int t = 0; t < session_->trackCount(); ++t) {
        const auto* track = session_->track(t);
        if (!track)
            continue;
        stripLayout_->addWidget(buildStrip(t, *track));
    }
}

void MixerView::clearStrips()
{
    placeholder_ = nullptr;
    while (QLayoutItem* item = stripLayout_->takeAt(0)) {
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
}

QWidget* MixerView::buildStrip(int trackIndex, const daw::model::Track& track)
{
    auto* strip = new QFrame(this);
    strip->setFrameShape(QFrame::StyledPanel);
    strip->setFixedWidth(96);
    strip->setStyleSheet(QStringLiteral(
        "QFrame { background: #26282b; border: 1px solid #1c1e20; border-radius: 4px; }"));

    auto* col = new QVBoxLayout(strip);
    col->setContentsMargins(6, 6, 6, 6);
    col->setSpacing(6);

    auto* name = new QLabel(QString::fromStdString(track.name()), strip);
    name->setAlignment(Qt::AlignCenter);
    name->setStyleSheet(QStringLiteral("color: #c8ccd2; border: none;"));
    name->setWordWrap(false);
    QFontMetrics fm(name->font());
    name->setText(fm.elidedText(QString::fromStdString(track.name()),
                                Qt::ElideRight, 80));
    name->setToolTip(QString::fromStdString(track.name()));
    col->addWidget(name);

    // ---- Mute / Solo ------------------------------------------------------
    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(4);

    auto* mute = new QPushButton(QStringLiteral("M"), strip);
    mute->setCheckable(true);
    mute->setChecked(track.isMuted());
    mute->setFixedSize(34, 24);
    mute->setToolTip(tr("Заглушить дорожку"));
    mute->setStyleSheet(QStringLiteral(
        "QPushButton { border: 1px solid #3a3d42; border-radius: 3px; color: #b0b4ba; }"
        "QPushButton:checked { background: #c0703a; color: #fff; border-color: #c0703a; }"));

    auto* solo = new QPushButton(QStringLiteral("S"), strip);
    solo->setCheckable(true);
    solo->setChecked(track.isSoloed());
    solo->setFixedSize(34, 24);
    solo->setToolTip(tr("Соло — звучат только cолирующие дорожки"));
    solo->setStyleSheet(QStringLiteral(
        "QPushButton { border: 1px solid #3a3d42; border-radius: 3px; color: #b0b4ba; }"
        "QPushButton:checked { background: #c0a83a; color: #201c00; border-color: #c0a83a; }"));

    buttons->addWidget(mute);
    buttons->addWidget(solo);
    col->addLayout(buttons);

    // ---- Фейдер -----------------------------------------------------------
    auto* fader = new QSlider(Qt::Vertical, strip);
    fader->setRange(kMinDb, kMaxDb);
    fader->setValue(linearToDb(track.gain()));
    fader->setToolTip(tr("Громкоcть дорожки"));
    col->addWidget(fader, 1, Qt::AlignHCenter);

    auto* dbLabel = new QLabel(dbText(fader->value()), strip);
    dbLabel->setAlignment(Qt::AlignCenter);
    dbLabel->setStyleSheet(QStringLiteral("color: #8a9096; border: none;"));
    col->addWidget(dbLabel);

    // ---- Связи ------------------------------------------------------------
    connect(fader, &QSlider::valueChanged, this, [this, trackIndex, dbLabel](int db) {
        dbLabel->setText(dbText(db));
        emit trackGainChanged(trackIndex, dbToLinear(db));
    });
    connect(mute, &QPushButton::toggled, this, [this, trackIndex](bool on) {
        emit trackMuteChanged(trackIndex, on);
    });
    connect(solo, &QPushButton::toggled, this, [this, trackIndex](bool on) {
        emit trackSoloChanged(trackIndex, on);
    });

    return strip;
}
