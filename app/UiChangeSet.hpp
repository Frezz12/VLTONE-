#pragma once
#include <QSet>
#include <QStringList>
namespace ui {
struct UiChangeSet {
    QSet<QString> tracks;
    QSet<QString> arrangementTracks;
    bool structure = false;
    bool clipContent = false;
    bool fullArrangement = false;
    bool transport = false;
    void merge(const UiChangeSet& other) {
        tracks.unite(other.tracks); arrangementTracks.unite(other.arrangementTracks);
        structure = structure || other.structure;
        clipContent |= other.clipContent;
        fullArrangement |= other.fullArrangement;
        transport |= other.transport;
    }
    static UiChangeSet values(const QStringList& ids, bool appearance = false) {
        UiChangeSet result;
        for (const auto& id : ids) {
            result.tracks.insert(id);
            if (appearance) result.arrangementTracks.insert(id);
        }
        return result;
    }
};
}
