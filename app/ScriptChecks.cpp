#include "Internal/CompressorInstance.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "LuaCreatorWindow.hpp"
#include "RackParameterBinding.hpp"
#include "ScriptManagerWindow.hpp"
#include "ScriptRun.hpp"
#include "ScriptService.hpp"
#include "ShortcutManager.hpp"
#include "collaboration/SharedMutationSink.hpp"
#include "scripting/ScriptProject.hpp"
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFocusFrame>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>

namespace ui {
namespace {
class ScriptCloudFixture final : public daw::collab::SharedMutationSink {
  public:
    using Result = daw::collab::SharedMutationResult;
    int mutations = 0;
    bool handlesCloudBinding() override { return true; }
    Result submit(daw::collab::SharedMutationRequest) override {
        ++mutations;
        return Result::Blocked;
    }
    Result setTimeSignature(int, int) override { return Result::Blocked; }
    Result setProjectKey(int, std::string_view) override { return Result::Blocked; }
    Result setAiInstructions(std::string_view) override { return Result::Blocked; }
    Result renameTrack(std::string_view, std::string_view) override { return Result::Blocked; }
    Result setTrackMuted(std::string_view, bool) override { return Result::Blocked; }
    Result setTracksMuted(std::span<const std::string>, bool) override { return Result::Blocked; }
    Result clearAllMutes(std::span<const std::string>) override { return Result::Blocked; }
};
} // namespace
bool ScriptService::runCheck(const QString &screenshots) {
    if (qEnvironmentVariableIsSet("VLT_SCRIPT_AUDIO_CHECK"))
        return runAudioCheck();
    QTemporaryDir temp;
    if (!temp.isValid())
        return false;
    qputenv("VLT_SCRIPT_LIBRARY", temp.filePath("Scripts").toUtf8());
    auto expect = [](bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    auto wait = [](auto predicate, int timeout = 6000) {
        QElapsedTimer clock;
        clock.start();
        while (!predicate() && clock.elapsed() < timeout) {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        return predicate();
    };
    QElapsedTimer elapsed;
    elapsed.start();
    const auto phase = [&](const char *name) {
        std::fprintf(stderr, "[script-check] %s: %lld ms\n", name,
                     static_cast<long long>(elapsed.elapsed()));
        std::fflush(stderr);
    };
    try {
        daw::EngineController first, second;
        expect(first.initialize(48000, 256, false).isOk(), "Headless engine could not initialize");
        second.initializeDocumentOnly();
        phase("engine initialized");
        first.newProject();
        second.newProject();
        const auto track = first.addTrack(daw::TrackKind::Midi, "Script fixture");
        const auto clip = first.addMidiClip(track, 0, 2), other = first.addMidiClip(track, 2, 2);
        daw::NoteModel note;
        note.id = "note-one";
        note.pitch = 60;
        first.setClipNotes(track, clip, {note}, "Fixture");
        first.setClipNotes(track, other, {note}, "Fixture");
        QString selected = "one";
        QMainWindow window;
        ShortcutManager shortcuts(&window);
        Bindings bindings;
        bindings.current = [&] {
            return selected == "one" ? Project{"one", "Fixture one", &first}
                                     : Project{"two", "Fixture two", &second};
        };
        bindings.resolve = [&](const QString &id) {
            return id == "one"   ? Project{id, "Fixture one", &first}
                   : id == "two" ? Project{id, "Fixture two", &second}
                                 : Project{};
        };
        bindings.context = [&](const QString &) {
            return Json{
                {"target", {{"track", track}, {"clip", clip}}},
                {"selection", {{"clips", Json::array({{{"track", track}, {"clip", clip}}})}}}};
        };
        bindings.edited = [](const QString &) {};
        bindings.command = [&](const QString &id, const QString &project) {
            return project == selected && shortcuts.invoke(id);
        };
        int workspaceCalls = 0;
        bindings.workspace = [&](const QString &project, const Json &request) -> Json {
            ++workspaceCalls;
            return {{"project", project.toStdString()}, {"method", request.at("method")}};
        };
        ScriptService scripts(&window, &shortcuts, bindings);
        scripts.setupMenu();
        ScriptPackage package{
            {},
            ScriptLibrary::manifestForLua("Transpose selection", "test.transpose"),
            {{"main.lua", R"lua(
local context=vlt.context.get()
local project=vlt.project.get()
for _,track in ipairs(project.tracks) do
  if track.id==context.target.track then
    for _,clip in ipairs(track.clips) do
      if clip.id==context.target.clip then
        for _,note in ipairs(clip.notes) do note.pitch=note.pitch+1 end
        return vlt.project.apply{revision=project.revision,label="Lua transpose",
          operations={{op="notes.set",track=track.id,clip=clip.id,notes=clip.notes}}}
      end
    end
  end
end
)lua"}}};
        auto automatic = package;
        automatic.manifest["id"] = "test.automatic-project-read";
        automatic.manifest["permissions"] = {"project.read", "files.read"};
        expect(scripts.approvePermissions(automatic, false),
               "Project read permissions were not automatic");
        auto automaticPrefs = scripts.settings(automatic.id());
        expect(automaticPrefs["permissions"] == automatic.manifest["permissions"],
               "Automatic read grants missed a declared capability");
        for (const auto *extra : {"project.write", "files.write", "network", "process"}) {
            automatic.manifest["permissions"].push_back(extra);
            expect(!scripts.approvePermissions(automatic, false),
                   "Non-read permission was granted automatically");
            automatic.manifest["permissions"].erase(automatic.manifest["permissions"].end() - 1);
        }
        automaticPrefs["permissions"] = Json::array();
        automaticPrefs["automaticProjectRead"] = false;
        scripts.saveSettings(automatic.id(), automaticPrefs);
        expect(!scripts.approvePermissions(automatic, false),
               "Revoked read access was silently restored");
        scripts.saveSettings(package.id(),
                             {{"permissions", package.manifest["permissions"]}, {"enabled", true}});
        const auto pitch = [&](const std::string &id) {
            const auto p = daw::scripting::projectSnapshot(first);
            for (const auto &t : p["tracks"])
                for (const auto &c : t["clips"])
                    if (c["id"] == id)
                        return c["notes"][0]["pitch"].get<int>();
            return -1;
        };
        QString error;
        expect(ScriptLibrary::validate(package, error), qPrintable(error));
        auto run =
            scripts.runPackage(package, "run", {{"target", {{"track", track}, {"clip", other}}}});
        expect(wait([&] { return scripts.status(run).value("complete", false); }),
               "Lua command timed out");
        if (!scripts.status(run).value("error", "").empty())
            std::fprintf(stderr, "%s\n", scripts.status(run).dump(2).c_str());
        expect(scripts.status(run).value("error", "").empty(), "Lua command failed");
        expect(pitch(other) == 61 && pitch(clip) == 60,
               "Explicit target was replaced by global selection");
        expect(first.undoLabel() == "Lua transpose", "Batch did not create named Undo");
        first.undo();
        expect(pitch(other) == 60, "Undo failed");
        first.redo();
        expect(pitch(other) == 61, "Redo failed");
        first.undo();
        const auto revision = first.projectRevision();
        const auto label = first.undoLabel();
        const auto redo = first.redoLabel();
        bool batchRejected = false;
        try {
            daw::scripting::applyEdits(
                first,
                {{"revision", revision},
                 {"label", "Failure"},
                 {"operations",
                  Json::array({{{"op", "track.rename"}, {"track", track}, {"name", "Partial"}},
                               {{"op", "invalid"}}})}});
        } catch (const std::exception &) {
            batchRejected = true;
        }
        expect(batchRejected, "Invalid batch succeeded");
        bool invalidPitch = false;
        try {
            daw::scripting::applyEdits(
                first,
                {{"revision", first.projectRevision()},
                 {"label", "Bad note"},
                 {"operations", Json::array({{{"op", "notes.set"},
                                              {"track", track},
                                              {"clip", clip},
                                              {"notes", Json::array({{{"id", "bad"},
                                                                      {"pitch", 60.5},
                                                                      {"velocity", 100},
                                                                      {"start", 0},
                                                                      {"length", 1}}})}}})}});
        } catch (const std::exception &) {
            invalidPitch = true;
        }
        expect(invalidPitch && pitch(clip) == 60, "Fractional MIDI pitch was silently truncated");
        expect(first.project().findTrack(track)->name == "Script fixture" &&
                   first.undoLabel() == label && first.redoLabel() == redo,
               "Rollback changed document or existing history");
        const auto secondTrack = second.addTrack(daw::TrackKind::Midi, "Snapshot fixture");
        const auto largeClip = second.addMidiClip(secondTrack, 0, 100);
        std::vector<daw::NoteModel> manyNotes(10000, note);
        for (size_t i = 0; i < manyNotes.size(); ++i)
            manyNotes[i].id = "note-" + std::to_string(i);
        second.setClipNotes(secondTrack, largeClip, manyNotes, "Large fixture");
        bool bounded = false;
        try {
            daw::scripting::projectSnapshot(second, {{"eventLimit", 10000}});
        } catch (const std::exception &) {
            bounded = true;
        }
        expect(bounded, "Oversized project snapshot was not bounded");
        const auto page =
            daw::scripting::projectSnapshot(second, {{"eventOffset", 9999}, {"eventLimit", 1}});
        expect(page["tracks"][0]["clips"][0]["notes"].size() == 1 &&
                   page["tracks"][0]["clips"][0]["noteCount"] == 10000,
               "Snapshot paging lost notes");
        daw::AutomationTarget automationTarget;
        automationTarget.kind = daw::AutomationTargetKind::TrackVolume;
        automationTarget.channelId = secondTrack;
        const auto [automationTrack, automationClip] = second.ensureAutomation(automationTarget);
        const Json automationPoints = Json::array(
            {{{"id", "hold"}, {"beat", 0}, {"value", 0.7}, {"shape", "hold"}, {"eventOrder", 42}},
             {{"id", "curve"}, {"beat", 2}, {"value", 0.4}, {"shape", "scurve"}, {"curve", 0.5}}});
        daw::scripting::applyEdits(second,
                                   {{"revision", second.projectRevision()},
                                    {"label", "Shape test"},
                                    {"operations", Json::array({{{"op", "automation.set"},
                                                                 {"track", automationTrack},
                                                                 {"clip", automationClip},
                                                                 {"points", automationPoints}}})}});
        const auto automationSnapshot = [&] {
            return daw::scripting::projectSnapshot(
                second, {{"track", automationTrack},
                         {"clip", automationClip}})["tracks"][0]["clips"][0]["points"];
        };
        const auto roundTrip = automationSnapshot();
        expect(roundTrip[0]["shape"] == "hold" && roundTrip[0]["eventOrder"] == 42 &&
                   roundTrip[1]["shape"] == "scurve",
               "Automation shapes or order were lost");
        second.undo();
        second.redo();
        expect(automationSnapshot() == roundTrip, "Automation shape Undo/Redo failed");
        second.newProject();
        phase("runtime and transaction checks");
        LuaCreatorWindow creator(&scripts, &window);
        auto blank = package;
        blank.sources["main.lua"] = "-- AI starts from an empty source\n";
        creator.openPackage(blank);
        LuaAiWorkspace workspace(creator);
        daw::ai::ToolContext ai;
        ai.mode = daw::ai::InteractionMode::Do;
        const auto exampleForAi = workspace.execute(
            {"example", "lua_example", {{"name", "interactive-workbench"}}, false}, ai, 1);
        expect(exampleForAi && exampleForAi->ok &&
                   exampleForAi->value["sources"]["main.lua"].get<std::string>().find(
                       "vlt.workspace.open") != std::string::npos,
               "Creator AI cannot read the bundled UI example");
        daw::ai::ToolCall edit{"edit",
                               "lua_edit",
                               {{"revision", creator.revision()},
                                {"path", "main.lua"},
                                {"source", package.sources["main.lua"]}},
                               false};
        expect(workspace.execute(edit, ai, 1)->ok, "AI source edit failed");
        auto *creatorHistory = creator.findChild<QUndoStack *>();
        expect(creatorHistory && creatorHistory->canUndo(), "AI edit is not in Creator history");
        creatorHistory->undo();
        expect(creator.package().sources == blank.sources, "Undo did not restore Creator source");
        creatorHistory->redo();
        expect(
            workspace.execute({"check", "lua_check", Json::object(), false}, ai, 1)->value["valid"],
            "AI validation failed");
        expect(workspace.execute({"install", "lua_install", Json::object(), false}, ai, 1)->ok,
               "AI installation failed");
        phase("Creator and AI checks");
        auto brokenUpdate = package;
        brokenUpdate.sources["main.lua"] = "local = broken";
        expect(!ScriptLibrary::install(brokenUpdate, error), "Invalid update was installed");
        ScriptPackage retained;
        expect(ScriptLibrary::readDirectory(QDir(ScriptLibrary::directory()).filePath(package.id()),
                                            retained, error) &&
                   retained.sources == package.sources,
               "Failed update replaced the working package");
        auto draft = creator.document();
        draft["manifestSource"] = "{ unfinished";
        QFile draftFile(temp.filePath("draft.vltcreator"));
        expect(draftFile.open(QIODevice::WriteOnly), "Could not create draft fixture");
        draftFile.write(QByteArray::fromStdString(draft.dump()));
        draftFile.close();
        creatorHistory->setClean();
        expect(creator.openProject(draftFile.fileName(), error) && !creator.check(error),
               "Incomplete Creator source must open for repair but cannot install");
        expect(creator.openPackage(package), "Creator could not return to valid project");
        auto multipleCommands = package;
        multipleCommands.sources = {{"first.lua", "return 10"}, {"second.lua", "return 20"}};
        multipleCommands.resources = {{"resources/preserved.bin", "AQID"}};
        multipleCommands.manifest["commands"] =
            Json::array({{{"id", "first"}, {"label", "First"}, {"entry", "first.lua"}},
                         {{"id", "second"}, {"label", "Second"}, {"entry", "second.lua"}}});
        expect(creator.openPackage(multipleCommands), "Creator did not open multiple entry points");
        auto *sourceEditor = creator.findChild<QPlainTextEdit *>("LuaCreator.Source");
        auto *sourceTree = creator.findChild<QTreeWidget *>();
        expect(sourceEditor && sourceEditor->toPlainText() == "return 10",
               "Non-main entry did not open");
        sourceEditor->ensurePolished();
        expect(sourceEditor->fontMetrics().horizontalAdvance("iiii") ==
                   sourceEditor->fontMetrics().horizontalAdvance("WWWW"),
               "Source font is not monospace");
        auto *sourceItem = sourceTree->currentItem();
        sourceEditor->insertPlainText("-- edited\n");
        expect(sourceTree->currentItem() == sourceItem, "Typing rebuilt the file tree");
        creatorHistory->undo();
        expect(creator.package().sources == multipleCommands.sources &&
                   creator.package().resources == multipleCommands.resources,
               "Source Undo changed another file or resource");
        auto *commandPicker = creator.findChild<QComboBox *>("LuaCreator.Command");
        commandPicker->setCurrentIndex(1);
        creator.execute(true);
        const auto secondCommandRun = scripts.m_runs.back()->id;
        expect(wait([&] { return scripts.status(secondCommandRun).value("complete", false); }) &&
                   scripts.status(secondCommandRun)["result"] == 20,
               "Creator ignored the selected command");
        creatorHistory->setClean();
        creator.openPackage(package);
        const auto command = "script." + package.id() + "/run";
        expect(shortcuts.command(command) != nullptr, "Installed command missing");
        shortcuts.setShortcut(command, QKeySequence("Ctrl+Alt+9"));
        scripts.reload();
        expect(shortcuts.shortcut(command) == QKeySequence("Ctrl+Alt+9"), "Reload lost shortcut");
        // The menu command picker and shortcuts share the installed catalogue.
        auto *chooseAction = window.findChild<QAction *>("ScriptCommandPicker.Open");
        expect(chooseAction, "Choose command is missing from the Scripts menu");
        chooseAction->trigger();
        QPointer<QDialog> picker = scripts.m_commandPicker;
        expect(picker && !picker->isModal(), "Command picker blocks the workspace");
        scripts.chooseCommand();
        expect(scripts.m_commandPicker == picker, "Repeated command picker created another window");
        auto *commandSearch = picker->findChild<QLineEdit *>("ScriptCommandPicker.Search");
        auto *commandList = picker->findChild<QListWidget *>("ScriptCommandPicker.Commands");
        auto *runButton = picker->findChild<QPushButton *>("ScriptCommandPicker.Run");
        expect(commandSearch && commandList && runButton, "Command picker controls are missing");
        commandSearch->setText("no-such-command-983274");
        expect(commandList->count() == 0 && !runButton->isEnabled(),
               "Empty command search can run");
        commandSearch->setText(QString::fromStdString(package.manifest["commands"][0]["label"]));
        expect(commandList->count() == 1 &&
                   commandList->currentItem()->data(Qt::UserRole) == command,
               "Command picker cannot discover an installed command");
        auto disabledPrefs = scripts.settings(package.id());
        disabledPrefs["enabled"] = false;
        scripts.saveSettings(package.id(), disabledPrefs);
        expect(commandList->count() == 0, "Command picker offers a disabled extension");
        disabledPrefs["enabled"] = true;
        scripts.saveSettings(package.id(), disabledPrefs);
        expect(commandList->count() == 1 && runButton->isEnabled(),
               "Command picker did not refresh");
        if (!screenshots.isEmpty()) {
            QDir().mkpath(screenshots);
            QApplication::processEvents();
            picker->grab().save(QDir(screenshots).filePath("command-picker.png"));
        }
        const auto runsBeforePicker = scripts.m_runs.size();
        QMetaObject::invokeMethod(commandSearch, "returnPressed", Qt::DirectConnection);
        expect(wait([&] { return pitch(clip) == 61 && !picker; }) &&
                   scripts.m_runs.size() == runsBeforePicker + 1,
               "Command picker did not invoke exactly one command");
        first.undo();
        expect(pitch(clip) == 60, "Command picker result not undoable");
        QSettings().setValue(
            "scripts/toolbar/buttons",
            QString::fromStdString(
                Json::array({{{"command", command.toStdString()}, {"label", "Transpose"}}})
                    .dump()));
        const auto customImage = temp.filePath("button-photo.png");
        QImage photo(80, 60, QImage::Format_ARGB32);
        photo.fill(QColor(32, 140, 180));
        expect(photo.save(customImage), "Cannot create toolbar image fixture");
        QString toolbarError;
        bool toolbarChecked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("ScriptToolbar.Configure");
            if (!dialog) {
                toolbarError = "Toolbar dialog not found";
                toolbarChecked = true;
                return;
            }
            try {
                auto *caption = dialog->findChild<QLineEdit *>("ScriptToolbar.Label");
                auto *args = dialog->findChild<QPlainTextEdit *>("ScriptToolbar.Arguments");
                auto *buttons = dialog->findChild<QDialogButtonBox *>();
                expect(caption && args && buttons, "Toolbar fields missing");
                auto *appearance = dialog->findChild<QComboBox *>("ScriptToolbar.Appearance");
                auto *image = dialog->findChild<QLineEdit *>("ScriptToolbar.Image");
                auto *preview = dialog->findChild<QToolButton *>("ScriptToolbar.Preview");
                expect(appearance && image && preview, "Toolbar appearance controls missing");
                appearance->setCurrentIndex(appearance->findData("image"));
                image->setText(customImage);
                expect(preview->toolButtonStyle() == Qt::ToolButtonIconOnly &&
                           !preview->icon().isNull(),
                       "Image-only preview is missing");
                caption->setText("Saved directly");
                args->setPlainText("{ unfinished");
                buttons->button(QDialogButtonBox::Save)->click();
                expect(dialog->isVisible() && args->toPlainText() == "{ unfinished",
                       "Invalid arguments were lost or accepted");
                args->setPlainText("{\"steps\":2}");
                if (!screenshots.isEmpty()) {
                    QDir().mkpath(screenshots);
                    dialog->findChild<QTabWidget *>()->setCurrentIndex(0);
                    QApplication::processEvents();
                    dialog->grab().save(QDir(screenshots).filePath("toolbar-appearance.png"));
                }
                buttons->button(QDialogButtonBox::Save)->click();
            } catch (const std::exception &error) {
                toolbarError = QString::fromUtf8(error.what());
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->deleteLater();
            }
            toolbarChecked = true;
        });
        scripts.configureToolbar();
        expect(!scripts.m_toolbarEditor->isModal(), "Toolbar editor blocks the workspace");
        expect(wait([&] { return toolbarChecked; }), "Toolbar editor check timed out");
        expect(toolbarError.isEmpty(), qPrintable(toolbarError));
        const auto savedButtons =
            Json::parse(QSettings().value("scripts/toolbar/buttons").toString().toStdString());
        expect(savedButtons[0]["label"] == "Saved directly" &&
                   savedButtons[0]["arguments"]["steps"] == 2,
               "Toolbar Save did not commit the current fields");
        expect(savedButtons[0]["appearance"] == "image" &&
                   savedButtons[0]["icon"] != customImage.toStdString(),
               "Toolbar image was not copied into persistent storage");
        QFile::remove(customImage);
        scripts.rebuildToolbar();
        auto *customButton = qobject_cast<QToolButton *>(
            scripts.toolbar()->widgetForAction(scripts.toolbar()->actions().front()));
        expect(customButton && customButton->toolButtonStyle() == Qt::ToolButtonIconOnly &&
                   !customButton->icon().isNull() &&
                   scripts.toolbar()->minimumSizeHint().height() >= 44,
               "Toolbar lost its image/style or remained too thin");
        auto appearanceEntries = savedButtons;
        for (const auto &appearance : {"text", "both", "image"}) {
            appearanceEntries[0]["appearance"] = appearance;
            QSettings().setValue("scripts/toolbar/buttons",
                                 QString::fromStdString(appearanceEntries.dump()));
            scripts.rebuildToolbar();
            customButton = qobject_cast<QToolButton *>(
                scripts.toolbar()->widgetForAction(scripts.toolbar()->actions().front()));
            const auto expected = std::string_view(appearance) == "text" ? Qt::ToolButtonTextOnly
                                  : std::string_view(appearance) == "image"
                                      ? Qt::ToolButtonIconOnly
                                      : Qt::ToolButtonTextBesideIcon;
            expect(customButton && customButton->toolButtonStyle() == expected,
                   "Button appearance did not restore");
        }
        expect(scripts.toolbar()
                       ->findChildren<QAction *>(QString(), Qt::FindDirectChildrenOnly)
                       .size() <= 3,
               "Rebuilding the toolbar leaked actions");
        scripts.toolbar()->actions().front()->trigger();
        expect(wait([&] { return pitch(clip) == 61; }), "Installed toolbar action failed");
        first.undo();
        expect(pitch(clip) == 60, "Toolbar result not undoable");
        QMenu contextMenu;
        scripts.appendMenu(&contextMenu, &first,
                           {{"type", "clip"}, {"target", {{"track", track}, {"clip", other}}}});
        expect(!contextMenu.actions().empty(), "Context action missing");
        contextMenu.actions().front()->menu()->actions().front()->trigger();
        expect(wait([&] { return pitch(other) == 61; }), "Context menu invocation failed");
        first.undo();
        expect(shortcuts.invoke(command), "Registered command dispatch failed");
        expect(wait([&] { return pitch(clip) == 61; }), "Shared command handler failed");
        first.undo();
        // Exercise the actual key path, including a Russian layout and protected
        // text/MIDI input.
        auto *focusButton = new QPushButton("Keyboard fixture", &window);
        window.setCentralWidget(focusButton);
        window.show();
        window.activateWindow();
        focusButton->setFocus();
        QApplication::processEvents();
        shortcuts.setShortcut(command, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A));
        QKeyEvent russian(QEvent::KeyPress, 0x01000424, Qt::ControlModifier | Qt::AltModifier,
                          QString::fromUtf8("ф"));
        QApplication::sendEvent(focusButton, &russian);
        expect(wait([&] { return pitch(clip) == 61; }),
               "Russian physical shortcut did not invoke script");
        first.undo();
        shortcuts.setShortcut(command, QKeySequence(Qt::Key_A));
        shortcuts.setKeySuppressor([](int key) { return key == Qt::Key_A; });
        expect(shortcuts.command(command)->action->shortcuts().isEmpty(),
               "Computer MIDI key was stolen by dynamic command");
        scripts.reload();
        expect(shortcuts.command(command)->action->shortcuts().isEmpty(),
               "Reload bypassed MIDI key suppression");
        shortcuts.setKeySuppressor({});
        auto *textField = new QLineEdit(&window);
        textField->show();
        textField->setFocus();
        QApplication::processEvents();
        QApplication::processEvents();
        expect(shortcuts.command(command)->action->shortcuts().isEmpty(),
               "Text entry did not suppress bare script shortcut");
        delete textField;
        focusButton->setFocus();
        QApplication::processEvents();
        shortcuts.setShortcut(command, QKeySequence("Ctrl+Alt+9"));
        for (const auto edge : {Qt::TopToolBarArea, Qt::BottomToolBarArea, Qt::LeftToolBarArea,
                                Qt::RightToolBarArea}) {
            window.addToolBar(edge, scripts.toolbar());
            expect(window.toolBarArea(scripts.toolbar()) == edge, "Toolbar docking failed");
        }
        auto windowPackage = package;
        windowPackage.manifest["id"] = "test.window";
        windowPackage.sources["main.lua"] = R"lua(
local p=vlt.project.get()
vlt.ui.window{id="main",title="Script fixture",children={
 {type="label",id="label",text=p.name},
 {type="button",id="button",text="Read pinned project",onClick=function()
    vlt.ui.set{id="label",text=vlt.project.get().name}
 end},
 {type="canvas",id="canvas",drawing={{type="line",x=10,y=10,x2=150,y2=80}}}
}}
)lua";
        scripts.saveSettings(windowPackage.id(),
                             {{"permissions", windowPackage.manifest["permissions"]}});
        run = scripts.runPackage(windowPackage, "run");
        expect(wait([&] {
                   for (const auto &r : scripts.m_runs)
                       if (r->id == run)
                           return !r->windows.isEmpty() && r->idle;
                   return false;
               }),
               "Native window failed");
        expect(scripts.runPackage(windowPackage, "run") == run,
               "Repeated tool run duplicated a window");
        for (const auto &r : scripts.m_runs)
            if (r->id == run) {
                auto *button = r->windows.constBegin().value()->findChild<QPushButton *>();
                button->click();
                button->click();
                button->click();
                expect(r->pending.size() == 3, "Distinct button clicks were coalesced");
            }
        const auto previewWindow = scripts.runPackage(windowPackage, "run", Json::object(), true);
        expect(!previewWindow.isEmpty() && previewWindow != run,
               "Preview reused a live project window");
        expect(wait([&] {
                   for (const auto &r : scripts.m_runs)
                       if (r->id == previewWindow)
                           return r->idle && !r->windows.isEmpty();
                   return false;
               }),
               "Preview window failed");
        scripts.stop(previewWindow);
        selected = "two";
        for (const auto &r : scripts.m_runs)
            if (r->id == run)
                expect(scripts.controller(r.get()) == &first,
                       "Running tool changed project on tab switch");
        scripts.showManager();
        phase("manager opened");
        auto *keyEditor = scripts.m_manager->findChild<QKeySequenceEdit *>();
        expect(keyEditor && keyEditor->isEnabled(), "Manager did not select the installed command");
        const auto assignedKey = keyEditor->keySequence();
        keyEditor->setKeySequence(QKeySequence("Ctrl+Alt+G"));
        auto quick = package;
        quick.sources["main.lua"] = "return true";
        const auto quickRun = scripts.runPackage(quick, "run");
        expect(wait([&] { return scripts.status(quickRun).value("complete", false); }),
               "Quick command did not finish");
        expect(keyEditor->keySequence() == QKeySequence("Ctrl+Alt+G"),
               "Run log updates interrupted shortcut editing");
        keyEditor->setKeySequence(assignedKey);
        if (!screenshots.isEmpty()) {
            QDir().mkpath(screenshots);
            window.resize(900, 500);
            window.show();
            creator.show();
            QApplication::processEvents();
            creator.grab().save(QDir(screenshots).filePath("lua-creator.png"));
            for (const auto &r : scripts.m_runs)
                if (r->id == run)
                    for (auto widget : r->windows)
                        if (widget) {
                            widget->activateWindow();
                            widget->findChild<QPushButton *>()->setFocus();
                            QApplication::processEvents();
                            widget->grab().save(QDir(screenshots).filePath("lua-window.png"));
                        }
            scripts.showManager();
            QApplication::processEvents();
            scripts.m_manager->grab().save(QDir(screenshots).filePath("script-manager.png"));
        }
        scripts.m_manager->hide();
        phase("UI screenshots");
        scripts.closeProject("one");
        expect(scripts.status(run).value("complete", false), "Closing project did not stop window");
        selected = "one";
        const auto fx =
            first.addInsert(track, daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
        expect(!fx.empty(), "Test device could not be loaded");
        const Json address = {{"track", track},
                              {"device", fx},
                              {"parameter", "output.gain"},
                              {"identity", daw::scripting::parameterIdentity(first, track, fx)}};
        const auto allParameters =
            daw::scripting::parameters(first, {{"track", track}, {"device", fx}});
        const auto parameterPage = daw::scripting::parameters(
            first, {{"track", track}, {"device", fx}, {"offset", 1}, {"limit", 1}});
        expect(allParameters.size() > 1 && parameterPage.size() == 1 &&
                   parameterPage[0] == allParameters[1],
               "Parameter paging selected the wrong item");
        const auto filteredParameter = daw::scripting::parameters(first, address);
        expect(filteredParameter.size() == 1 && filteredParameter[0]["id"] == "output.gain",
               "Binding parameter filter failed");
        auto panel = package;
        panel.manifest["id"] = "test.parameters";
        panel.sources["main.lua"] = "local bind=vlt.context.get().parameter; "
                                    "vlt.ui.window{title='Parameters',children={{"
                                    "type='knob',id='gain',bind=bind}}}";
        scripts.saveSettings(panel.id(), {{"permissions", panel.manifest["permissions"]}});
        const auto parameterPreview =
            scripts.runPackage(panel, "run", {{"parameter", address}}, true);
        expect(wait([&] {
                   for (const auto &r : scripts.m_runs)
                       if (r->id == parameterPreview)
                           return r->idle && !r->bindings.isEmpty();
                   return false;
               }),
               "Preview did not expose copied device parameters");
        const auto realValue = first.insertParameter(track, fx, "output.gain");
        for (const auto &r : scripts.m_runs)
            if (r->id == parameterPreview) {
                auto *b = r->bindings.front().data();
                b->write("output.gain", -11);
                b->finish("output.gain");
                expect(b->value("output.gain") == -11 &&
                           first.insertParameter(track, fx, "output.gain") == realValue,
                       "Preview binding touched the live device");
            }
        scripts.stop(parameterPreview);
        phase("preview parameter checks");
        auto boundControls = panel;
        boundControls.sources["main.lua"] = R"lua(
local bind=vlt.context.get().parameter
local toggle={track=bind.track,device=bind.device,identity=bind.identity,parameter="band.0.enabled"}
-- Find the first boolean band parameter without depending on a device's naming scheme.
for _,parameter in ipairs(vlt.parameters.list{track=bind.track,device=bind.device}) do
    if parameter.id:match("%.enabled$") then toggle.parameter=parameter.id;break end
end
vlt.ui.window{title="Bound values",children={
 {type="slider",id="bound-slider",bind=bind,onChange=function(e) vlt.log{text="slider gesture"} end},
 {type="toggle",id="bound-toggle",bind=toggle,style={checked={background="$accent",color="$onAccent"}},
  onChange=function(e) assert(type(e.value)=="boolean"); vlt.log{text="toggle gesture"} end},
 {type="label",id="bound-result",text=toggle.parameter}
}}
for i=1,12 do
 assert(not pcall(vlt.ui.window,{id="invalid-binding",title="Invalid",children={
  {type="knob",bind=bind},{type="invalid"}}}))
end
local previous=vlt.ui.get{id="bound-toggle"}.value
assert(type(previous)=="boolean")
vlt.ui.set{id="bound-slider",value=-7}
assert(vlt.ui.get{id="bound-slider"}.value==-7)
vlt.ui.set{id="bound-toggle",value=not previous}
assert(vlt.ui.get{id="bound-toggle"}.value==not previous)
)lua";
        const auto controlDepth = first.undoDepth();
        const auto controlValue = first.insertParameter(track, fx, "output.gain");
        const auto controlsRun = scripts.runPackage(boundControls, "run", {{"parameter", address}});
        std::shared_ptr<ScriptRun> controlsInstance;
        expect(wait([&] {
                   for (const auto &r : scripts.m_runs)
                       if (r->id == controlsRun && (r->idle || r->complete)) {
                           controlsInstance = r;
                           return true;
                       }
                   return false;
               }),
               "Bound controls did not settle");
        if (!controlsInstance->error.isEmpty())
            std::fprintf(stderr, "Bound controls: %s\n", qPrintable(controlsInstance->error));
        expect(!controlsInstance->complete && controlsInstance->bindings.size() == 2 &&
                   first.undoDepth() == controlDepth + 2,
               "Bound values failed or failed tree retained bindings");
        first.undo();
        first.undo();
        expect(first.insertParameter(track, fx, "output.gain") == controlValue,
               "Bound control update did not undo");
        expect(controlsInstance->log.isEmpty(), "ui.set emitted bound-control onChange");
        for (auto binding : controlsInstance->bindings)
            if (binding)
                binding->refresh();
        auto *toggleButton =
            qobject_cast<QAbstractButton *>(controlsInstance->widgets.value("bound-toggle").data());
        toggleButton->click();
        expect(wait([&] { return controlsInstance->log.contains("toggle gesture"); }),
               "Bound toggle gesture did not call Lua");
        first.undo();
        auto *sliderBinding = controlsInstance->bindings.front().data();
        sliderBinding->write("output.gain", -8);
        sliderBinding->finish("output.gain");
        expect(wait([&] { return controlsInstance->log.contains("slider gesture"); }),
               "Bound slider gesture did not call Lua");
        first.undo();
        scripts.stop(controlsRun);
        const auto parameterRun = scripts.runPackage(panel, "run", {{"parameter", address}});
        expect(wait([&] {
                   for (const auto &r : scripts.m_runs)
                       if (r->id == parameterRun)
                           return r->idle && !r->bindings.isEmpty();
                   return false;
               }),
               "Parameter binding did not open");
        std::shared_ptr<ScriptRun> parameterInstance;
        for (const auto &r : scripts.m_runs)
            if (r->id == parameterRun)
                parameterInstance = r;
        auto *binding = parameterInstance->bindings.front().data();
        const auto before = first.insertParameter(track, fx, "output.gain");
        const auto depth = first.undoDepth();
        binding->write("output.gain", -6);
        binding->write("output.gain", -9);
        binding->finish("output.gain");
        expect(first.undoDepth() == depth + 1 && binding->value("output.gain") == -9,
               "Parameter gesture must create exactly one Undo");
        first.undo();
        binding->refresh();
        expect(binding->value("output.gain") == before,
               "Parameter Undo did not synchronize the control");
        first.setInsertParameter(track, fx, "output.gain", -3);
        binding->refresh();
        expect(binding->value("output.gain") == -3, "Native parameter edit did not reach binding");
        daw::scripting::applyEdits(first,
                                   {{"revision", first.projectRevision()},
                                    {"label", "Device transaction"},
                                    {"operations", Json::array({{{"op", "device.bypass"},
                                                                 {"track", track},
                                                                 {"device", fx},
                                                                 {"identity", address["identity"]},
                                                                 {"bypassed", true}}})}});
        first.undo();
        expect(first.hasInsert(track, fx) && first.insertParameter(track, fx, "output.gain") == -3,
               "Device state was lost during batch Undo");
        first.redo();
        first.undo();
        const auto deviceHistory = first.undoDepth();
        try {
            daw::scripting::applyEdits(
                first,
                {{"revision", first.projectRevision()},
                 {"label", "Rollback device"},
                 {"operations",
                  Json::array({{{"op", "device.remove"},
                                {"track", track},
                                {"device", fx},
                                {"identity", daw::scripting::parameterIdentity(first, track, fx)}},
                               {{"op", "invalid"}, {"track", track}}})}});
        } catch (...) {
        }
        expect(first.hasInsert(track, fx) &&
                   first.insertParameter(track, fx, "output.gain") == -3 &&
                   first.undoDepth() == deviceHistory,
               "Device rollback lost live state or history");
        // Reopen against the restored device incarnation; the original binding is
        // intentionally stale.
        scripts.stop(parameterRun);
        const auto currentAddress =
            Json{{"track", track},
                 {"device", fx},
                 {"parameter", "output.gain"},
                 {"identity", daw::scripting::parameterIdentity(first, track, fx)}};
        const auto rebound = scripts.runPackage(panel, "run", {{"parameter", currentAddress}});
        expect(wait([&] {
                   for (const auto &r : scripts.m_runs)
                       if (r->id == rebound)
                           return r->idle && !r->bindings.isEmpty();
                   return false;
               }),
               "Restored parameter panel did not open");
        for (const auto &r : scripts.m_runs)
            if (r->id == rebound)
                parameterInstance = r;
        binding = parameterInstance->bindings.front().data();
        ScriptCloudFixture cloud;
        first.attachSharedMutationSink(cloud);
        auto detachCloud = qScopeGuard([&] { first.detachSharedMutationSink(cloud); });
        auto guarded = package;
        guarded.manifest["id"] = "test.cloud";
        guarded.sources["main.lua"] = R"lua(
local p=vlt.project.get()
local edit=pcall(function() vlt.project.apply{revision=p.revision,label="Forbidden",operations={{op="track.add",name="Forbidden"}}} end)
local transport=pcall(function() vlt.transport.set{playing=true} end)
local command=pcall(function() vlt.commands.run{id="anything"} end)
local parameter=pcall(function() local a=vlt.context.get().parameter;a.value=-12;vlt.parameters.set(a) end)
return p.readOnly and not edit and not transport and not command and not parameter
)lua";
        scripts.saveSettings(guarded.id(), {{"permissions", guarded.manifest["permissions"]}});
        run = scripts.runPackage(guarded, "run", {{"parameter", currentAddress}}, false, "do");
        const bool cloudDone = wait([&] { return scripts.status(run).value("complete", false); });
        if (!cloudDone || scripts.status(run)["result"] != true)
            std::fprintf(stderr, "Cloud result: %s\n", scripts.status(run).dump(2).c_str());
        expect(cloudDone && scripts.status(run)["result"] == true, "Cloud API or AI guard failed");
        binding->write("output.gain", -12);
        binding->finish("output.gain");
        expect(first.insertParameter(track, fx, "output.gain") == -3 && cloud.mutations == 0,
               "Cloud binding bypassed host guard");
        first.detachSharedMutationSink(cloud);
        detachCloud.dismiss();
        expect(first.replaceInsert(
                   track, fx, daw::plugins::compressor::CompressorInstance::staticDescriptor()),
               "Replacement fixture failed");
        const auto replacementValue = first.insertParameter(track, fx, "output.gain");
        binding->write("output.gain", -18);
        binding->finish("output.gain");
        expect(first.insertParameter(track, fx, "output.gain") == replacementValue &&
                   !binding->available(),
               "Stale binding reached a replacement device");
        scripts.stop(rebound);
        auto malformed = package;
        malformed.manifest["id"] = "test.invalid-ui";
        malformed.sources["main.lua"] = "local ok=pcall(function() "
                                        "vlt.ui.window{title='Bad',children={{type='canvas',"
                                        "drawing={{type='line',x='bad'}}}}} end); return not ok";
        scripts.saveSettings(malformed.id(), {{"permissions", malformed.manifest["permissions"]}});
        run = scripts.runPackage(malformed, "run");
        expect(wait([&] { return scripts.status(run).value("complete", false); }) &&
                   scripts.status(run)["result"] == true,
               "Malformed drawing reached paintEvent");
        auto workbench = package;
        workbench.manifest["id"] = "test.workbench";
        workbench.manifest["permissions"] = {"project.read", "files.read"};
        QByteArray iconBytes;
        QBuffer iconBuffer(&iconBytes);
        iconBuffer.open(QIODevice::WriteOnly);
        expect(photo.save(&iconBuffer, "PNG"), "Cannot create widget image resource");
        workbench.resources["resources/icon.png"] = iconBytes.toBase64().toStdString();
        const auto forbiddenImage = temp.filePath("not-granted.png");
        expect(photo.save(forbiddenImage), "Cannot create non-granted image fixture");
        workbench.sources["main.lua"] = R"lua(
local replacement = {
 {type="tabs",id="pages",children={
   {type="scroll",title="Controls",children={{type="grid",columns=2,children={
     {type="field",id="name",label="Name",value="one"},
     {type="textEdit",id="notes",value="hello"},
     {type="choice",id="choice",items={"one","two"}},
     {type="table",id="table",columns={"Name","Value"},rows={{"a","1"}},editable=true,
       onChange=function(e) vlt.ui.set{id="status",text=e.value.text} end},
     {type="image",id="photo",path="resources/icon.png",width=64,height=48}
   }}}},
   {type="canvas",title="Drawing",id="paint",height=140,
     onPointer=function(e) vlt.ui.set{id="status",text=e.value.phase} end,
     onKey=function(e) vlt.ui.set{id="status",text=e.value.key} end,
     onWheel=function(e) vlt.ui.set{id="status",text="wheel"} end}
 }},
 {type="label",id="status",text="ready"},
 {type="button",id="navigate",text="Navigate",onClick=function()
    local opened=pcall(function() vlt.workspace.open{view="mixer"} end)
    local selected=pcall(function() vlt.workspace.select{tracks=vlt.array({})} end)
    vlt.ui.set{id="status",text=(not opened and not selected) and "inactive" or "wrong project"}
 end},
 {type="button",id="verify",text="Verify state",onClick=function()
    assert(vlt.ui.get{id="name"}.value=="two")
    assert(vlt.ui.get{id="choice"}.value==2)
    assert(vlt.ui.get{id="table"}.rows[1][2]=="2")
    vlt.ui.set{id="status",text="verified"}
 end}
}
vlt.ui.window{id="bench",title="Workbench",dock="right",children={{type="label",id="old",text="retained"}}}
local bad=pcall(function() vlt.ui.replace{id="bench",children={{type="label",id="old"},{type="invalid"}}} end)
assert(not bad and vlt.ui.get{id="old"}.text=="retained")
assert(not pcall(function() vlt.ui.window{id="badImage",title="Denied",children={
    {type="image",path=vlt.context.get().forbiddenImage}}} end))
assert(not pcall(function() vlt.ui.replace{id="bench",children={{type="label",id="bench"}}} end))
vlt.ui.replace{id="bench",children=replacement}
assert(not pcall(function() vlt.ui.get{id="old"} end))
vlt.ui.set{id="name",value="two"}
vlt.ui.set{id="choice",items={"first","second","third"},value=2}
vlt.ui.set{id="table",rows={{"b","2"}}}
assert(not pcall(function() vlt.ui.set{id="table",rows={{"bad","row","extra"}}} end))
assert(vlt.ui.get{id="table"}.rows[1][1]=="b")
assert(vlt.workspace.get().project=="one")
vlt.ui.set{id="bench",visible=false}
vlt.ui.focus{id="name"}
assert(vlt.ui.get{id="name"}.visible)
)lua";
        scripts.saveSettings(workbench.id(), {{"permissions", workbench.manifest["permissions"]}});
        run = scripts.runPackage(workbench, "run",
                                 {{"forbiddenImage", forbiddenImage.toStdString()}});
        std::shared_ptr<ScriptRun> workbenchRun;
        for (const auto &r : scripts.m_runs)
            if (r->id == run)
                workbenchRun = r;
        expect(workbenchRun && wait([&] { return workbenchRun->complete || workbenchRun->idle; }),
               "Workbench did not settle");
        if (workbenchRun->complete)
            std::fprintf(stderr, "Workbench: %s\n", scripts.status(run).dump().c_str());
        expect(!workbenchRun->complete && workspaceCalls > 0, "Workbench Lua assertions failed");
        auto *dock = qobject_cast<QDockWidget *>(workbenchRun->windows.value("bench").data());
        expect(dock && window.dockWidgetArea(dock) == Qt::RightDockWidgetArea,
               "Custom dock did not attach");
        auto *statusLabel = qobject_cast<QLabel *>(workbenchRun->widgets.value("status").data());
        qobject_cast<QPushButton *>(workbenchRun->widgets.value("verify").data())->click();
        expect(wait([&] { return statusLabel->text() == "verified"; }),
               "Replacement lost callbacks or widget values");
        auto *table = qobject_cast<QTableWidget *>(workbenchRun->widgets.value("table").data());
        table->item(0, 1)->setText("edited");
        expect(wait([&] { return statusLabel->text() == "edited"; }),
               "Editable table event failed");
        auto *pages = qobject_cast<QTabWidget *>(workbenchRun->widgets.value("pages").data());
        pages->setCurrentIndex(1);
        auto *canvas = workbenchRun->widgets.value("paint").data();
        QKeyEvent key(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
        QApplication::sendEvent(canvas, &key);
        expect(wait([&] { return statusLabel->text() == "Left"; }), "Canvas key callback failed");
        QMouseEvent pointer(QEvent::MouseButtonPress, QPointF(12, 16), QPointF(12, 16),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &pointer);
        expect(wait([&] { return statusLabel->text() == "down"; }),
               "Canvas pointer callback failed");
        QWheelEvent wheel(QPointF(12, 16), QPointF(12, 16), QPoint(), QPoint(0, 120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &wheel);
        expect(wheel.isAccepted() && wait([&] { return statusLabel->text() == "wheel"; }),
               "Canvas wheel callback failed");
        QKeyEvent shortcutOverride(QEvent::ShortcutOverride, Qt::Key_C, Qt::NoModifier);
        QApplication::sendEvent(canvas, &shortcutOverride);
        expect(shortcutOverride.isAccepted(), "Canvas did not protect typing input");
        for (const auto edge : {Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea,
                                Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea}) {
            window.addDockWidget(edge, dock);
            expect(window.dockWidgetArea(dock) == edge, "Custom panel docking failed");
        }
        dock->setFloating(true);
        expect(dock->isFloating(), "Custom panel cannot float");
        if (!screenshots.isEmpty()) {
            pages->setCurrentIndex(0);
            dock->resize(620, 520);
            QApplication::processEvents();
            dock->grab().save(QDir(screenshots).filePath("custom-workbench.png"));
        }
        // Navigation is local UI only, but must never target a different tab or
        // escape preview.
        const int callsBefore = workspaceCalls;
        selected = "two";
        qobject_cast<QPushButton *>(workbenchRun->widgets.value("navigate").data())->click();
        expect(wait([&] { return statusLabel->text() == "inactive"; }),
               "Workspace navigation escaped the pinned tab");
        expect(workspaceCalls == callsBefore, "Inactive tab reached native workspace");
        selected = "one";
        scripts.stop(run);
        auto navigation = package;
        navigation.manifest["id"] = "test.navigation";
        navigation.sources["main.lua"] = R"lua(
local state=vlt.workspace.get()
return state.preview and not pcall(function() vlt.workspace.open{view="mixer"} end)
    and not pcall(function() vlt.workspace.select{tracks=vlt.array({})} end)
)lua";
        run = scripts.runPackage(navigation, "run", Json::object(), true);
        expect(wait([&] { return scripts.status(run).value("complete", false); }) &&
                   scripts.status(run)["result"] == true,
               "Preview reached the real workspace");
        expect(workspaceCalls == callsBefore, "Preview called native workspace");
        phase("custom widgets and workspace");
        auto styledPackage = workbench;
        styledPackage.manifest["id"] = "test.custom-style";
        styledPackage.sources["main.lua"] = R"lua(
local styles={card={background="$surface",radius=12,padding=18},
              heading={color="$accent",fontSize=24,fontWeight=700}}
vlt.ui.window{id="plain",title="App theme",children={{type="label",text="Unchanged"}}}
vlt.ui.window{id="styled",title="Custom theme",theme={background="#111923",surface="#243244",
 text="#f4f8fc",accent="#4ee0b5"},styles=styles,style={padding=24},children={
 {type="column",id="card",style="card",children={
   {type="label",id="heading",text="Keep",style="heading"},
   {type="field",id="input",value="kept",width=110},
   {type="choice",id="options",items={"a","b"},value=2},
   {type="button",id="style-button",text="Test callback",style={background="$accent",color="$onAccent",
      hover={background="#8cecd1"}},onClick=function() vlt.ui.set{id="heading",text="Clicked"} end}
 }}
}}
local function rejected(change) assert(not pcall(vlt.ui.set,change)) end
rejected{id="heading",text="Bad",style={radius=-1}}
rejected{id="heading",text="Bad",style={background="url(file:///test)"}}
rejected{id="heading",text="Bad",style={fontFamily="x'; color:red; '"}}
rejected{id="heading",text="Bad",style={unsupported=true}}
rejected{id="heading",text="Bad",style={focus={accent="red"}}}
rejected{id="heading",text="Bad",value=1}
rejected{id="input",text="Bad",value=false}
rejected{id="input",value="Bad",width=1e15}
rejected{id="options",items={"changed"},value=2}
rejected{id="styled",theme={background="transparent"}}
assert(vlt.ui.get{id="heading"}.text=="Keep")
assert(vlt.ui.get{id="input"}.value=="kept")
assert(vlt.ui.get{id="options"}.value==2)
vlt.ui.set{id="input",width=210,height=36}
vlt.ui.set{id="input",style={color="#ffcc55",fontSize=16}}
vlt.ui.set{id="input",style={}}
assert(vlt.ui.get{id="input"}.value=="kept")
assert(next(vlt.ui.get{id="input"}.style)==nil)
assert(not pcall(vlt.ui.replace,{id="styled",children={{type="label",style="missing"}}}))
assert(vlt.ui.get{id="heading"}.text=="Keep")
vlt.ui.window{id="inherit",title="Inherited",styles=styles,theme={accent="#ab57f7"},
 children={{type="label",id="old-inherited",style="heading"}}}
vlt.ui.replace{id="inherit",children={{type="label",id="inherited",text="New",style="heading"}}}
assert(vlt.ui.get{id="inherited"}.style.fontSize==24)
)lua";
        run = scripts.runPackage(styledPackage, "run", Json::object(), true);
        std::shared_ptr<ScriptRun> styledRun;
        expect(wait([&] {
                   for (const auto &r : scripts.m_runs)
                       if (r->id == run && (r->idle || r->complete)) {
                           styledRun = r;
                           return true;
                       }
                   return false;
               }),
               "Style fixture did not settle");
        if (!styledRun->error.isEmpty())
            std::fprintf(stderr, "Style fixture: %s\n", qPrintable(styledRun->error));
        expect(!styledRun->complete && styledRun->error.isEmpty(), "Style Lua assertions failed");
        auto *styledWindow = styledRun->windows.value("styled").data();
        auto *input = qobject_cast<QLineEdit *>(styledRun->widgets.value("input").data());
        auto *heading = qobject_cast<QLabel *>(styledRun->widgets.value("heading").data());
        auto *card = styledRun->widgets.value("card").data();
        auto *options = qobject_cast<QComboBox *>(styledRun->widgets.value("options").data());
        if (input)
            std::fprintf(stderr, "[script-check] styled field dimensions: min=%dx%d max=%dx%d\n",
                         input->minimumWidth(), input->minimumHeight(), input->maximumWidth(),
                         input->maximumHeight());
        expect(input && input->minimumWidth() == 210 && input->maximumWidth() == 210 &&
                   input->minimumHeight() == 36,
               "ui.set did not update fixed dimensions");
        expect(options->count() == 2 && options->itemText(0) == "a",
               "Invalid update cleared items");
        expect(card->layout()->contentsMargins().left() == 18, "Styled card lost padding");
        expect(heading->palette().windowText().color() == QColor("#4ee0b5"),
               "Named theme token did not reach label");
        const auto plainPalette = styledRun->windows.value("plain")->palette();
        const auto hostPalette = window.palette();
        scripts.uiCall(styledRun.get(), "ui.set",
                       {{"id", "styled"},
                        {"theme",
                         {{"accent", "#ff9944"},
                          {"background", "#faf9f5"},
                          {"text", "#1c2634"},
                          {"surface", "#ffffff"}}}});
        expect(styledRun->widgets.value("input") == input && input->text() == "kept" &&
                   heading->palette().windowText().color() == QColor("#ff9944"),
               "Theme update lost controls or failed to refresh tokens");
        expect(styledRun->windows.value("plain")->palette() == plainPalette &&
                   window.palette() == hostPalette,
               "Window theme leaked into sibling window or DAW");
        scripts.uiCall(styledRun.get(), "ui.set",
                       {{"id", "styled"}, {"theme", {{"accent", "#ffff00"}}}});
        const auto buttonInk =
            styledRun->widgets.value("style-button")->palette().buttonText().color();
        expect(buttonInk.red() < 64 && buttonInk.green() < 64 && buttonInk.blue() < 64,
               "Automatic onAccent text is unreadable on yellow");
        scripts.uiCall(styledRun.get(), "ui.set", {{"id", "card"}, {"style", Json::object()}});
        expect(card->layout()->contentsMargins().left() == 0,
               "Reset style retained custom padding");
        qobject_cast<QPushButton *>(styledRun->widgets.value("style-button").data())->click();
        expect(wait([&] { return heading->text() == "Clicked"; }), "Style update lost callback");
        if (!screenshots.isEmpty())
            styledWindow->grab().save(QDir(screenshots).filePath("style-regression.png"));
        scripts.uiCall(styledRun.get(), "ui.replace",
                       {{"id", "inherit"},
                        {"children", Json::array({{{"type", "field"}, {"id", "focus-old"}}})}});
        scripts.uiCall(styledRun.get(), "ui.focus", {{"id", "focus-old"}});
        QApplication::processEvents();
        scripts.uiCall(styledRun.get(), "ui.replace",
                       {{"id", "inherit"},
                        {"children", Json::array({{{"type", "field"}, {"id", "focus-new"}}})}});
        scripts.uiCall(styledRun.get(), "ui.focus", {{"id", "focus-new"}});
        QApplication::processEvents();
        auto *replacedFocus = styledRun->windows.value("inherit")->findChild<QFocusFrame *>();
        expect(replacedFocus && replacedFocus->widget() == styledRun->widgets.value("focus-new") &&
                   replacedFocus->isVisible(),
               "Replacing a focused tree destroyed its keyboard focus observer");
        scripts.stop(run);
        phase("custom styles and atomic UI updates");
        // Preview fixtures exercise the same PCM and asynchronous publication paths
        // as installed tools.
        auto audio = package;
        audio.manifest["id"] = "test.audio";
        audio.sources["main.lua"] = R"lua(
local input=vlt.ui.chooseFile{mode="open"}
local output=vlt.ui.chooseFile{mode="save"}
return vlt.audio.process{path=input,output=output,operation="gain",gain=0.5}
)lua";
        run = scripts.runPackage(audio, "run", Json::object(), true);
        expect(wait([&] { return scripts.status(run).value("complete", false); }, 12000),
               "Offline job blocked or did not finish");
        const auto jobResult = scripts.status(run)["jobs"];
        expect(jobResult.size() == 1 && jobResult[0]["error"] == "" &&
                   QFile::exists(QString::fromStdString(jobResult[0]["result"]["path"])),
               "Offline job did not atomically publish");
        phase("offline gain job");
        auto analysis = audio;
        analysis.sources["main.lua"] = R"lua(
local input=vlt.audio.open{path=vlt.ui.chooseFile{mode="open"}}
local info,pcm=vlt.audio.read{id=input.id,frame=4800,frames=2048}
local maximum=0
for i=1,#pcm do maximum=math.max(maximum,math.abs(pcm[i])) end
vlt.audio.close{id=input.id}
return maximum>0.2 and maximum<0.3 and info.frames==2048
)lua";
        run = scripts.runPackage(analysis, "run", Json::object(), true);
        expect(wait([&] { return scripts.status(run).value("complete", false); }) &&
                   scripts.status(run)["result"] == true,
               "Binary PCM round trip failed");
        phase("PCM round trip");
        auto eventsAndFade = audio;
        eventsAndFade.sources["main.lua"] = R"lua(
local input=vlt.audio.open{path=vlt.ui.chooseFile{mode="open"}}
local meta,pcm=vlt.audio.read{id=input.id,frame=0,frames=32}
vlt.audio.close{id=input.id}
for i=1,#pcm do pcm[i]=0.25 end
local output=vlt.ui.chooseFile{mode="save"}
local source=output..".source.wav"
local file=vlt.audio.create{path=source,channels=meta.channels,sampleRate=meta.sampleRate}
vlt.audio.write({id=file.id},pcm)
vlt.audio.close{id=file.id,publish=true}
assert(not pcall(function() vlt.audio.render{path=output,start=0} end))
assert(not pcall(function() vlt.time.convert{value=1e300,from="seconds",to="frames",sampleRate=48000} end))
local first,second
first=vlt.events.on{event="job",callback=function(e)
  assert(e.error=="")
  local result=vlt.audio.open{path=e.result.path}
  local info,data=vlt.audio.read{id=result.id,frame=0,frames=32}
  assert(data[1]==0.25 and data[#data]==0.25 and info.frames==32)
  vlt.audio.close{id=result.id}
  print("first subscriber")
  vlt.events.off{id=first}
end}
second=vlt.events.on{event="job",callback=function(e)
  print("second subscriber")
  vlt.events.off{id=second}
end}
vlt.audio.process{path=source,output=output,operation="fade",inFrames=0,outFrames=0}
)lua";
        run = scripts.runPackage(eventsAndFade, "run", Json::object(), true);
        expect(wait([&] { return scripts.status(run).value("complete", false); }, 12000),
               "Job subscribers did not finish");
        const auto callbackStatus = scripts.status(run);
        const auto &callbackLog = callbackStatus["log"];
        expect(callbackStatus.value("error", "").empty() &&
                   std::find(callbackLog.begin(), callbackLog.end(), "first subscriber") !=
                       callbackLog.end() &&
                   std::find(callbackLog.begin(), callbackLog.end(), "second subscriber") !=
                       callbackLog.end(),
               "Job subscriber, zero fade or numeric range regression");
        phase("job subscribers and fade");
        auto cancellation = audio;
        cancellation.sources["main.lua"] = "vlt.ui.window{title='Cancellation "
                                           "fixture',children={{type='label',text='Processing'}}}";
        run = scripts.runPackage(cancellation, "run", Json::object(), true);
        std::shared_ptr<ScriptRun> cancellationRun;
        expect(wait([&] {
                   for (const auto &r : scripts.m_runs)
                       if (r->id == run && r->idle) {
                           cancellationRun = r;
                           return true;
                       }
                   return false;
               }),
               "Cancellation fixture did not open");
        const auto inputPath = cancellationRun->temporary.filePath("input/preview.wav"),
                   outputPath = cancellationRun->temporary.filePath("output/cancelled.wav");
        const auto fileHash = [](const QString &path) {
            QFile file(path);
            file.open(QIODevice::ReadOnly);
            return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
        };
        const auto originalHash = fileHash(inputPath);
        const auto cancelledJob = scripts
                                      .ioCall(cancellationRun.get(), "audio.process",
                                              {{"path", inputPath.toStdString()},
                                               {"output", outputPath.toStdString()},
                                               {"operation", "reverse"}},
                                              {})
                                      .json;
        scripts.ioCall(cancellationRun.get(), "jobs.cancel", cancelledJob, {});
        expect(wait([&] { return cancellationRun->jobs.front()->complete; }),
               "Cancelled job did not finish");
        expect(!QFile::exists(outputPath) && !cancellationRun->jobs.front()->error.isEmpty() &&
                   fileHash(inputPath) == originalHash,
               "Cancellation published audio or changed original");
        scripts.stop(run);
        // Reproduce the user's actual slicer: source access without selecting a
        // file, timer-driven PCM analysis and a complete batch of cuts in one Undo.
        const auto slicePath = temp.filePath("slicer-input.wav");
        SF_INFO sliceInfo{};
        sliceInfo.channels = 1;
        sliceInfo.samplerate = 48000;
        sliceInfo.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
#ifdef Q_OS_WIN
        auto *sliceFile = sf_wchar_open(reinterpret_cast<const wchar_t *>(slicePath.utf16()),
                                        SFM_WRITE, &sliceInfo);
#else
        auto *sliceFile = sf_open(slicePath.toUtf8().constData(), SFM_WRITE, &sliceInfo);
#endif
        expect(sliceFile, "Cannot create slicer audio fixture");
        std::vector<float> slicePcm(96000, 0.f);
        for (int onset : {12000, 36000, 60000, 84000})
            for (int i = 0; i < 2400; ++i)
                slicePcm[onset + i] = .75f * float(std::sin(i * .15)) * (1.f - float(i) / 2400);
        const auto written = sf_writef_float(sliceFile, slicePcm.data(), slicePcm.size());
        sf_close(sliceFile);
        expect(written == 96000, "Slicer fixture write failed");
        const auto sliceTrack = first.addTrack(daw::TrackKind::Audio, "Transient slicer fixture");
        const auto sliceClip = first.importAudio(slicePath.toStdString(), sliceTrack, 1);
        expect(!sliceClip.empty(), "Cannot import slicer fixture");
        first.setClipTrim(sliceTrack, sliceClip, 1, .1, 1.7);
        const auto originalSliceHash = fileHash(slicePath);
        QFile unrelated(temp.filePath("unrelated.txt"));
        expect(unrelated.open(QIODevice::WriteOnly), "Cannot create unrelated-file fixture");
        unrelated.write("not part of the project");
        unrelated.close();
        ScriptPackage slicer;
        expect(ScriptLibrary::readDirectory(QCoreApplication::applicationDirPath() +
                                                "/ScriptSDK/examples/transient-slicer",
                                            slicer, error),
               qPrintable(error));
        scripts.saveSettings(slicer.id(), {{"permissions", slicer.manifest["permissions"]}});
        const Json sliceContext{{"project", "one"},
                                {"type", "clip"},
                                {"target", {{"track", sliceTrack}, {"clip", sliceClip}}}};
        auto access = slicer;
        access.manifest["id"] = "test.project-file-access";
        access.manifest["permissions"].push_back("files.write");
        scripts.saveSettings(access.id(), {{"permissions", access.manifest["permissions"]}});
        access.sources["main.lua"] = R"lua(
local c=vlt.context.get()
local p=vlt.project.get{track=c.target.track,clip=c.target.clip,includeEvents=false}
local source=p.tracks[1].clips[1].source
local files=vlt.project.files{offset=0,limit=1}
assert(files.total>=1 and #files.files==1 and files.revision==p.revision)
local folder=source:match('^(.*)[/\\][^/\\]+$')
local visible=vlt.files.list{path=folder}
local found=false
for _,file in ipairs(visible) do
    if file.path==source then found=true end
    assert(file.name~='unrelated.txt')
end
assert(found)
assert(not pcall(vlt.files.read,{path=folder..'/unrelated.txt'}))
local input=vlt.audio.open{path=source}
assert(not pcall(vlt.audio.read,{id=input.id,frame=0.5,frames=128}))
local info,pcm=vlt.audio.read{id=input.id,frame=0,frames=128}
vlt.audio.close{id=input.id}
local temp=c.temporaryDirectory..'/scratch.wav'
local output=vlt.audio.create{path=temp,channels=info.channels,sampleRate=info.sampleRate}
vlt.audio.write({id=output.id},pcm)
vlt.audio.close{id=output.id,publish=true}
assert(not pcall(vlt.files.write,{path=source,text='do not overwrite project audio'}))
local imported=vlt.project.apply{revision=p.revision,label='Import script audio',operations={
    {op='clip.importAudio',track=c.target.track,path=temp,start=8}}}
local generated=vlt.project.get{track=c.target.track,clip=imported.created[1],includeEvents=false}
vlt.ui.window{title='Project file access',children={{type='button',id='read-again',text='Read',onClick=function()
    local ok=pcall(vlt.audio.open,{path=source})
    vlt.log{text=ok and 'allowed' or 'denied'}
end}}}
return generated.tracks[1].clips[1].source
)lua";
        QElapsedTimer accessClock;
        accessClock.start();
        run = scripts.runPackage(access, "run", sliceContext);
        std::shared_ptr<ScriptRun> accessInstance;
        const bool accessReady = wait([&] {
            for (const auto &r : scripts.m_runs)
                if (r->id == run && (r->idle || r->complete)) {
                    accessInstance = r;
                    return true;
                }
            return false;
        });
        std::fprintf(stderr, "[script-check] project resource startup: %lld ms\n",
                     static_cast<long long>(accessClock.elapsed()));
        if (!accessReady || !accessInstance->error.isEmpty())
            std::fprintf(stderr, "[script-check] resource failure: %s\n",
                         scripts.status(run).dump().c_str());
        expect(accessReady && accessInstance->error.isEmpty(),
               "Automatic project resource access failed");
        const auto durableAudio = QString::fromStdString(accessInstance->result.get<std::string>());
        expect(accessInstance->temporary.remove() && QFile::exists(durableAudio),
               "Imported script audio disappeared with its temporary folder");
        expect(!scripts.permittedPath(accessInstance.get(), unrelated.fileName(), false) &&
                   !scripts.permittedPath(accessInstance.get(), unrelated.fileName(), true),
               "A removed temporary folder widened file access");
        const auto slicerRun = scripts.runPackage(slicer, "run", sliceContext);
        std::shared_ptr<ScriptRun> slicerInstance;
        expect(wait([&] {
                   for (const auto &r : scripts.m_runs)
                       if (r->id == slicerRun && (r->idle || r->complete)) {
                           slicerInstance = r;
                           return true;
                       }
                   return false;
               }) &&
                   slicerInstance->error.isEmpty(),
               "Slicer did not open");
        auto *sliceWindow = slicerInstance->windows.constBegin().value().data();
        auto *sliceButton = sliceWindow->findChild<QPushButton *>("run");
        auto *sliceStatus = sliceWindow->findChild<QLabel *>("status");
        expect(sliceButton && sliceStatus, "Slicer controls missing");
        bool visibleLabel = false;
        for (auto *label : sliceWindow->findChildren<QLabel *>())
            visibleLabel |= label->text() == QString::fromUtf8("Чувствительность");
        expect(visibleLabel, "Script field label is not visible");
        sliceButton->click();
        expect(wait(
                   [&] {
                       return slicerInstance->complete ||
                              sliceStatus->text().startsWith(QString::fromUtf8("Готово:")) ||
                              sliceStatus->text().startsWith(QString::fromUtf8("Ошибка:"));
                   },
                   12000),
               "Slicer analysis did not complete");
        if (!slicerInstance->log.isEmpty())
            std::fprintf(stderr, "Slicer: %s\n",
                         slicerInstance->log.join('\n').toUtf8().constData());
        expect(!slicerInstance->complete &&
                   sliceStatus->text().startsWith(QString::fromUtf8("Готово:")),
               sliceStatus->text().toUtf8().constData());
        const auto sliced = daw::scripting::projectSnapshot(first, {{"track", sliceTrack}});
        expect(sliced["tracks"][0]["clips"].size() == 6, "Slicer did not cut all four transients");
        expect(fileHash(slicePath) == originalSliceHash, "Slicer changed original audio");
        if (!screenshots.isEmpty()) {
            sliceWindow->grab().save(QDir(screenshots).filePath("transient-slicer.png"));
            scripts.toolbar()->show();
            scripts.toolbar()->grab().save(QDir(screenshots).filePath("script-toolbar.png"));
        }
        first.undo();
        expect(first.project().findTrack(sliceTrack)->clips.size() == 2 &&
                   first.project().findTrack(sliceTrack)->clips.front().id == sliceClip,
               "Slicer batch did not undo in one action");
        first.redo();
        expect(first.project().findTrack(sliceTrack)->clips.size() == 6, "Slicer redo lost slices");
        scripts.stop(slicerRun);
        first.removeTrack(sliceTrack);
        auto *readAgain = qobject_cast<QPushButton *>(accessInstance->widgets["read-again"].data());
        expect(readAgain, "Resource-access fixture button missing");
        readAgain->click();
        expect(wait([&] { return accessInstance->log.contains("denied"); }),
               "Removed project media remained implicitly granted");
        scripts.stop(run);
        phase("real transient slicer and project files");
        // Run the distributed examples against copied project/audio data, including
        // their UI callbacks.
        for (const auto &name :
             {"midi-transpose", "sample-folder", "pcm-analysis", "device-console",
              "background-events", "canvas-window", "interactive-workbench", "styled-workbench"}) {
            ScriptPackage example;
            expect(ScriptLibrary::readDirectory(QCoreApplication::applicationDirPath() +
                                                    "/ScriptSDK/examples/" + name,
                                                example, error),
                   qPrintable(error));
            const auto exampleRun = scripts.runPackage(
                example, QString::fromStdString(example.manifest["commands"][0]["id"]),
                Json::object(), true);
            std::shared_ptr<ScriptRun> instance;
            expect(wait([&] {
                       for (const auto &r : scripts.m_runs)
                           if (r->id == exampleRun && (r->idle || r->complete)) {
                               instance = r;
                               return true;
                           }
                       return false;
                   }),
                   "SDK example did not start");
            if (!instance->error.isEmpty())
                std::fprintf(stderr, "Example %s: %s\n", name, qPrintable(instance->error));
            expect(instance->error.isEmpty(), "SDK example failed at startup");
            if (std::string_view(name) == "sample-folder" ||
                std::string_view(name) == "pcm-analysis") {
                auto *tool = instance->windows.constBegin().value().data();
                tool->findChildren<QPushButton *>().front()->click();
                expect(wait(
                           [&] {
                               if (instance->complete)
                                   return true;
                               auto *bar = tool->findChild<QProgressBar *>();
                               return bar && bar->value() == 1000;
                           },
                           6000),
                       "SDK audio example did not complete");
                expect(!instance->complete && instance->error.isEmpty(),
                       "SDK audio callback failed");
            }
            if (std::string_view(name) == "styled-workbench") {
                auto *tool = instance->windows.value("design").data();
                auto *nameField = qobject_cast<QLineEdit *>(instance->widgets.value("name").data());
                auto *paletteButton =
                    qobject_cast<QPushButton *>(instance->widgets.value("palette").data());
                expect(tool && nameField && paletteButton, "Styled example controls missing");
                nameField->setText("Preserved preset");
                tool->activateWindow();
                paletteButton->setFocus(Qt::TabFocusReason);
                QApplication::processEvents();
                const auto verifyFocus = [&] {
                    auto *frame = tool->findChild<QFocusFrame *>();
                    expect(frame && frame->widget() == paletteButton && frame->isVisible() &&
                               frame->mask().isEmpty(),
                           "Custom focus frame disappeared or retained the native clip mask");
                    const auto image = tool->grab().toImage();
                    const auto at =
                        paletteButton->mapTo(tool, QPoint(paletteButton->width() / 2, 1));
                    const auto ink = image.pixelColor(qRound(at.x() * image.devicePixelRatio()),
                                                      qRound(at.y() * image.devicePixelRatio()));
                    expect(ink.green() > ink.red() + 15, "Focus outline is clipped by its row");
                };
                verifyFocus();
                if (!screenshots.isEmpty())
                    tool->grab().save(QDir(screenshots).filePath("styled-workbench-dark.png"));
                paletteButton->click();
                const bool switched = wait([&] {
                    return instance->complete ||
                           tool->palette().window().color() == QColor("#f3f1eb");
                });
                if (!switched || instance->complete)
                    std::fprintf(stderr, "[script-check] palette callback: %s\n",
                                 scripts.status(exampleRun).dump().c_str());
                expect(switched && !instance->complete, "Styled example could not switch palettes");
                expect(nameField->text() == "Preserved preset", "Palette switch lost input");
                verifyFocus();
                const auto sliderImage = instance->widgets.value("amount")->grab().toImage();
                const auto fillColor =
                    sliderImage.pixelColor(sliderImage.width() / 4, sliderImage.height() / 2);
                expect(fillColor.green() > fillColor.red() + 15,
                       "Custom slider retained the app's accent");
                auto *pad = instance->widgets.value("shape").data();
                QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
                QApplication::sendEvent(pad, &right);
                auto *status = qobject_cast<QLabel *>(instance->widgets.value("status").data());
                expect(wait([&] { return status->text().endsWith("65%"); }),
                       "Styled canvas lost its keyboard handler");
                if (!screenshots.isEmpty())
                    tool->grab().save(QDir(screenshots).filePath("styled-workbench-light.png"));
            }
            scripts.stop(exampleRun);
        }
        const auto stale = first.projectRevision();
        first.renameTrack(track, "New name");
        phase("offline jobs and examples");
        bool staleRejected = false;
        try {
            daw::scripting::applyEdits(
                first,
                {{"revision", stale},
                 {"label", "Stale"},
                 {"operations",
                  Json::array({{{"op", "track.rename"}, {"track", track}, {"name", "Old"}}})}});
        } catch (...) {
            staleRejected = true;
        }
        expect(staleRejected && first.project().findTrack(track)->name == "New name",
               "Stale snapshot was applied");
        selected = "one";
        auto denied = package;
        denied.manifest["id"] = "test.denied";
        denied.sources["main.lua"] = R"lua(
local file=pcall(function() vlt.files.read{path='C:/Windows/win.ini'} end)
local network=pcall(function() vlt.network.request{url='https://example.invalid'} end)
local process=pcall(function() vlt.process.start{executable='C:/Windows/System32/cmd.exe',arguments=vlt.array({})} end)
return not file and not network and not process
)lua";
        scripts.saveSettings(denied.id(), {{"permissions", denied.manifest["permissions"]}});
        run = scripts.runPackage(denied, "run");
        expect(wait([&] { return scripts.status(run).value("complete", false); }),
               "Permission check timed out");
        expect(scripts.status(run)["result"] == true, "Ungranted file was readable");
        const auto chosenFolder = temp.filePath("chosen");
        expect(QDir().mkpath(chosenFolder), "Cannot create folder fixture");
        QFile chosenFile(QDir(chosenFolder).filePath("sample.txt"));
        expect(chosenFile.open(QIODevice::WriteOnly), "Cannot create resource fixture");
        chosenFile.write("chosen resource");
        chosenFile.close();
        denied.sources["main.lua"] = "local c=vlt.context.get();local "
                                     "f=vlt.files.list{path=c.target.path};return #f==1 and "
                                     "vlt.files.read{path=f[1].path}=='chosen resource'";
        run = scripts.runPackage(
            denied, "run", {{"type", "file"}, {"target", {{"path", chosenFolder.toStdString()}}}});
        expect(wait([&] { return scripts.status(run).value("complete", false); }) &&
                   scripts.status(run)["result"] == true,
               "Browser folder context did not grant its selected resources");
        run = scripts.runPackage(
            denied, "run",
            {{"type", "file"}, {"target", {{"path", temp.filePath("deleted.wav").toStdString()}}}});
        expect(scripts.status(run).value("complete", false) &&
                   !scripts.status(run).value("error", "").empty(),
               "Removed browser target did not fail safely");
        denied.sources["main.lua"] = "while true do end";
        run = scripts.runPackage(denied, "run");
        wait([&] {
            for (const auto &r : scripts.m_runs)
                if (r->id == run)
                    return r->started;
            return false;
        });
        QElapsedTimer stop;
        stop.start();
        scripts.stop(run);
        expect(wait([&] { return scripts.status(run).value("complete", false); }, 1000) &&
                   stop.elapsed() < 1000,
               "Stop exceeded 1 second");
        denied.sources["main.lua"] = "while true do pcall(function() while true do end end) end";
        run = scripts.runPackage(denied, "run");
        expect(wait([&] { return scripts.status(run).value("complete", false); }, 4500) &&
                   !scripts.status(run).value("error", "").empty(),
               "Swallowed timeout bypassed host watchdog");
        denied.sources["main.lua"] = "vlt.events.on{event='project',callback=function() end}";
        run = scripts.runPackage(denied, "run");
        expect(wait([&] {
                   for (const auto &r : scripts.m_runs)
                       if (r->id == run)
                           return r->idle;
                   return false;
               }),
               "Background fixture did not start");
        const auto crashed = scripts.runPackage(denied, "run");
        expect(wait([&] {
                   for (const auto &r : scripts.m_runs)
                       if (r->id == crashed && r->idle) {
                           r->autostart = true;
                           r->process->kill();
                           return true;
                       }
                   return false;
               }),
               "Crash fixture did not start");
        expect(wait([&] { return scripts.status(crashed).value("complete", false); }) &&
                   scripts.settings(denied.id()).value("autostartSuspended", false) &&
                   !scripts.status(run).value("complete", true),
               "Worker crash affected another instance or did not pause autostart");
        first.newProject();
        expect(wait([&] { return scripts.status(run).value("complete", false); }),
               "Project replacement left old background instance alive");
        // Persist and restore a floating toolbar through the same setup path as an
        // application restart.
        window.findChild<QAction *>("ScriptToolbar.Float")->trigger();
        expect(scripts.toolbar()->isFloating(), "Toolbar did not detach to a floating window");
        QElapsedTimer settingsWait;
        settingsWait.start();
        wait([&] { return settingsWait.elapsed() > 600; }, 800);
        {
            QMainWindow restored;
            ShortcutManager restoredShortcuts(&restored);
            ScriptService restoredScripts(&restored, &restoredShortcuts, bindings);
            restoredScripts.setupMenu();
            expect(restoredScripts.toolbar()->isFloating() &&
                       !restoredScripts.toolbar()->isHidden() &&
                       restoredScripts.toolbar()->actions().size() == 1,
                   "Floating toolbar configuration did not restore");
        }
        scripts.stopAll();
        phase("finished");
        std::fprintf(stderr, "[script-check] PASS: runtime, target, rollback, device Undo/Redo, "
                             "Creator "
                             "AI/install/drafts, all launch surfaces, Russian/text/MIDI keys, "
                             "dock/float "
                             "restore, pinned windows, preview/native bindings, cloud/AI guards, "
                             "stale "
                             "identities/revisions, custom styles/layout/atomic updates, offline "
                             "jobs/PCM/cancel, SDK examples, "
                             "permissions, watchdog, worker crash/autostart, stop\n");
        return true;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[script-check] FAIL: %s\n", error.what());
        return false;
    }
}
} // namespace ui
