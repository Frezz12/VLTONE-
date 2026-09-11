#pragma once

#include "MidiFile.hpp"

#include <QObject>
#include <QString>

#include <memory>

/// Parses browser MIDI selections away from the GUI thread.
///
/// Like the audio PreviewLoader, it owns one worker at a time and keeps only
/// the newest requested path. Rapid arrow-key navigation therefore cannot
/// queue a long line of obsolete files behind the current selection.
class MidiPreviewLoader : public QObject {
    Q_OBJECT
public:
    explicit MidiPreviewLoader(QObject* parent = nullptr);
    ~MidiPreviewLoader() override;

    void request(const QString& path);
    void cancel();

signals:
    void loaded(const QString& path,
                std::shared_ptr<const daw::midifile::File> file);
    void failed(const QString& path, const QString& reason);

private:
    struct State;
    std::shared_ptr<State> m_state;
};
