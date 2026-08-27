#pragma once

#include <QWidget>

/// The "Context Panel" tab of the settings window: which groups the panel shows
/// for each kind of object, plus the escape hatch from the glass treatment.
///
/// The panel is deliberately narrow — it shows only what applies to what is
/// selected — but "applies" isn't the same for everyone. Someone who never
/// touches clip fades from the panel can take that group out and get a smaller
/// plate; the tools stay reachable everywhere else they already were.
class ContextPanelPage : public QWidget {
    Q_OBJECT
public:
    explicit ContextPanelPage(QWidget* parent = nullptr);

signals:
    /// A profile or the transparency setting changed; the panel should rebuild.
    void changed();
};
