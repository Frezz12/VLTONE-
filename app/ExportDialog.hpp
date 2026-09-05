#pragma once

#include "RenderSpec.hpp"

#include <QDialog>
#include <QByteArray>

#include <string>
#include <vector>

class QCheckBox;
class QBoxLayout;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QRadioButton;

namespace daw {
class EngineController;
}
namespace ui {
class SelectionModel;
}

/// The render dialog: what to write, over what stretch of the timeline, in what
/// format, and with which parts of the mixer taken out of the path.
///
/// It owns no rendering logic. Everything it collects goes into one
/// `daw::rendering::Spec`, and `EngineController::renderProject` does the work in
/// a single pass — which is why the mixdown and every stem it writes are
/// guaranteed to be the same render.
///
/// Plugin control operations run on the GUI thread. The controller renders an
/// isolated graph and throttles event-loop progress updates, keeping this
/// modal dialog and its Cancel button responsive.
class ExportDialog : public QDialog {
    Q_OBJECT
public:
    ExportDialog(daw::EngineController& controller,
                 const ui::SelectionModel* selection, QWidget* parent = nullptr,
                 const QString& projectPath = {});

    /// Exercised by --selftest: the dialog builds, its channel list is
    /// populated, and the format menus agree with what this build can write.
    bool checkForTest();
    /// Put the dialog into its stems-and-lossy state for a screenshot — the
    /// half of the layout the default state never shows.
    void stageStemsForShot();

private:
    void buildUi();
    void applyTheme();

    void populateChannels();
    void populateContainers();
    void repopulateEncodings();
    void repopulateSampleRates();

    /// Push the current range mode into the two time fields, and enable them
    /// only for a custom range.
    void syncRangeFields();
    void syncEnabledState();
    void updateSummary();
    /// What is wrong with the current stem selection, in one sentence, or an
    /// empty string. Stems only sum back to the mix when the set is disjoint
    /// and complete; the dialog says so rather than letting the guarantee fail
    /// silently.
    QString stemSelectionWarning() const;

    /// Everything the controls say, as one spec. `resolveRange` has already
    /// turned a selection into a custom range by the time this returns.
    daw::rendering::Spec collectSpec() const;
    /// The bounds of the current arrangement selection, or false when there is
    /// nothing selected to bound.
    bool selectionRange(double& start, double& end) const;

    void startRender();
    bool loadCover(const QString& path);
    void updateCover();
    void openRenderedFolder();
    void reject() override;
    void resizeEvent(QResizeEvent* event) override;

    daw::EngineController& m_controller;
    const ui::SelectionModel* m_selection = nullptr;

    QLineEdit* m_folder = nullptr;
    QPushButton* m_browse = nullptr;
    QLineEdit* m_baseName = nullptr;

    QCheckBox* m_writeMixdown = nullptr;
    QCheckBox* m_writeStems = nullptr;
    QListWidget* m_channels = nullptr;
    QPushButton* m_allChannels = nullptr;
    QPushButton* m_selectedChannels = nullptr;
    QPushButton* m_noChannels = nullptr;

    QRadioButton* m_rangeWhole = nullptr;
    QRadioButton* m_rangeCycle = nullptr;
    QRadioButton* m_rangeSelection = nullptr;
    QRadioButton* m_rangeCustom = nullptr;
    QDoubleSpinBox* m_rangeStart = nullptr;
    QDoubleSpinBox* m_rangeEnd = nullptr;

    QComboBox* m_tail = nullptr;
    QDoubleSpinBox* m_tailSeconds = nullptr;
    QDoubleSpinBox* m_tailSilenceDb = nullptr;
    QDoubleSpinBox* m_tailMaxSeconds = nullptr;
    QDoubleSpinBox* m_preRoll = nullptr;

    QCheckBox* m_bypassInserts = nullptr;
    QCheckBox* m_bypassMaster = nullptr;
    QCheckBox* m_ignoreMuteSolo = nullptr;
    QCheckBox* m_preFaderStems = nullptr;
    QCheckBox* m_dither = nullptr;

    QComboBox* m_container = nullptr;
    QComboBox* m_encoding = nullptr;
    QComboBox* m_quality = nullptr;
    QComboBox* m_sampleRate = nullptr;
    QComboBox* m_fileChannels = nullptr;
    QLineEdit* m_title = nullptr;
    QLineEdit* m_artist = nullptr;
    QLineEdit* m_comment = nullptr;
    QLineEdit* m_album = nullptr;
    QPushButton* m_cover = nullptr;
    QPushButton* m_removeCover = nullptr;
    QLabel* m_coverHint = nullptr;
    QByteArray m_coverData;
    QString m_coverPath;
    QCheckBox* m_openAfterRender = nullptr;
    QPushButton* m_openFolder = nullptr;
    QString m_renderedFile;
    QWidget* m_page = nullptr;
    QBoxLayout* m_columns = nullptr;
    QBoxLayout* m_actions = nullptr;
    QBoxLayout* m_headerRow = nullptr;
    QLabel* m_previewTitle = nullptr;
    QLabel* m_previewFormat = nullptr;
    /// Kept so a row can be hidden label and all: hiding only the field leaves
    /// an orphaned caption behind.
    QFormLayout* m_formatForm = nullptr;

    QLabel* m_stemWarning = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QPushButton* m_renderButton = nullptr;

    QGroupBox* m_stemsBox = nullptr;

    bool m_rendering = false;
    bool m_cancelRequested = false;
    bool m_populating = false;
};
