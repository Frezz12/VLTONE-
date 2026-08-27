#pragma once

#include "GlassPanel.hpp"

#include <QPointer>
#include <QString>

#include <functional>
#include <vector>

class PianoRollView;
class QAbstractAnimation;
class QHBoxLayout;

/// The piano roll's contribution to the application's shared context strip:
/// one floating plate that shows what can be done to the notes selected right
/// now, and nothing else.
///
/// It shares the arrangement panel's parent, placement and profile settings,
/// but remains a separate data adapter because `ContextPanel` is driven by
/// `ui::SelectionModel` (tracks and clips), while a note selection lives in the
/// piano roll's own view and never enters that model.
///
/// Everything it does goes through `PianoRollView`: live setters for the
/// continuous values (velocity, pan, length — dragged per mouse-move, exactly
/// like the note handles themselves) and `applyTransform` for the discrete
/// commands, so each of those is one labelled undo entry. Tools that need
/// parameters do not open their own dialogs; the panel reports the request and
/// the window opens the dialog it already owns.
class NoteContextPanel : public ui::GlassPanel {
    Q_OBJECT
public:
    /// The parameter tools, which the panel cannot run on its own — each has a
    /// dialog owned by `PianoRollWindow`.
    enum class Tool {
        Quantize, Arpeggiator, Chord, Strum, Glue, Articulate, Randomize
    };

    NoteContextPanel(PianoRollView* view, QWidget* parent = nullptr);

    /// Re-read the selection and swap the content only when its kind changes.
    /// Controls resolve note identities lazily, so changing one selected note
    /// for another is a value refresh rather than a widget-tree rebuild.
    void refresh();
    /// Rebuild from scratch — after the tool profiles change, or a new clip.
    void rebuild();
    /// Re-centre the plate after the grid resizes.
    void relayout();

    /// Match the arrangement island's placement policy inside the shared tool
    /// strip. Providers use parent coordinates, just like ContextPanel.
    void setAnchorProvider(std::function<bool(int&)> provider);
    void setBoundsProvider(std::function<bool(int&, int&)> provider);
    /// Y coordinate of the shared context strip when the panel's parent is the
    /// whole application surface rather than ToolPanel itself.
    void setTopProvider(std::function<int()> provider);
    void reloadFollowSetting();

    /// The user's View-menu toggle. An off panel stays hidden however many
    /// notes are selected.
    void setPanelEnabled(bool enabled);
    bool isPanelEnabled() const { return m_enabled; }

signals:
    /// The document changed; the window marks the project dirty and repaints.
    void projectEdited();
    /// A tool with parameters was asked for.
    void toolRequested(NoteContextPanel::Tool tool);

protected:
    void resizeEvent(QResizeEvent*) override;

private:
    /// What the panel is showing. One note and several notes get different
    /// content: a single note can show its own values, a group can only offer
    /// what makes sense applied to all of them at once.
    enum class Context { None, SingleNote, MultiNote };

    Context resolve() const;
    QWidget* buildContent(Context context);
    /// The controls shared by both contexts, plus the ones only a group gets.
    QWidget* buildNotes(bool multiple);
    void transitionTo(QWidget* next, Context context);

    QRect targetGeometry() const;
    void layoutSelf();
    bool toolEnabled(const char* toolId) const;
    QWidget* newRow(QHBoxLayout*& row);
    /// Repaint, mark dirty, and pull the document back into the controls.
    void afterEdit();

    PianoRollView* m_view = nullptr;

    Context m_context = Context::None;
    QWidget* m_content = nullptr;
    QWidget* m_outgoing = nullptr;
    QPointer<QAbstractAnimation> m_transition;
    /// Pushes the document's values into the current controls.
    std::function<void()> m_applyValues;
    bool m_enabled = true;
    bool m_follow = true;
    std::function<bool(int&)> m_anchorProvider;
    std::function<bool(int&, int&)> m_boundsProvider;
    std::function<int()> m_topProvider;
    /// Suppresses control signals while loading, so writing a slider's value
    /// into it doesn't read back as the user having moved it.
    bool m_updating = false;
};
