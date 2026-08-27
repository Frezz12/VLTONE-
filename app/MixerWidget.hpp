#pragma once

#include <QString>
#include <QWidget>
#include <vector>

namespace daw { class EngineController; }
class QHBoxLayout;
class QLabel;
class QScrollArea;
class ChannelStrip;

/// The mixer console: a scrolling row of channel strips with the master strip
/// pinned to the right, under a thin header ("MIXER · n channels · audio
/// online"). It lives in the centre column below the timeline, so it spans from
/// the track headers to the right edge of the window.
class MixerWidget : public QWidget {
    Q_OBJECT
public:
    explicit MixerWidget(daw::EngineController* controller,
                         QWidget* parent = nullptr);

    void rebuild();
    void refreshMeters();
    void refreshAutomationValues();
    /// Re-read every strip's values from the document — the cheap counterpart
    /// to `rebuild`, for when a level or a flag was changed somewhere else.
    void syncFromModel();
    /// The gain a strip is showing, or −1 when there is no such strip.
    double faderGainForTest(const QString& trackId) const;
    void setSelectedTrack(const QString& trackId);

signals:
    void trackSelected(const QString& trackId);
    void edited();
    /// An insert, instrument or routing slot changed. Ordinary value edits do
    /// not emit this, so the shell can keep its existing strip widgets alive.
    void structureChanged();
    void trackRemoved(const QString& trackId);
    void pluginEditorRequested(const QString& channelId, const QString& insertId);
    void openPatternRequested(const QString& patternId);
    void automateControlRequested(const QString& trackId, bool pan);
    void automateMuteRequested(const QString& trackId);
    void automateSendRequested(const QString& trackId, const QString& sendId);

private:
    void applyTheme();
    bool stripIsVisible(const ChannelStrip* strip) const;

    daw::EngineController* m_controller = nullptr;
    QLabel* m_headerCount = nullptr;
    QLabel* m_statusDot = nullptr;
    QLabel* m_statusText = nullptr;
    QWidget* m_header = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_stripsHost = nullptr;
    QHBoxLayout* m_stripsLayout = nullptr;
    QWidget* m_masterHost = nullptr;
    std::vector<ChannelStrip*> m_strips;
    QString m_selectedTrackId;
    bool m_statusInitialized = false;
    bool m_statusOnline = false;
};
