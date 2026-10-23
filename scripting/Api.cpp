#include "Api.hpp"
#include <regex>

namespace vlt::script {
const Json &apiCatalog() {
    static const Json catalog = [] {
        Json methods = Json::array();
        const auto add = [&](const char *name, const char *permission, bool edit,
                             const char *description, Json required = Json::object()) {
            methods.push_back({{"name", name},
                               {"permission", permission},
                               {"edit", edit},
                               {"description", description},
                               {"required", required}});
        };
        add("context.get", "", false, "Invocation target, selection, range and pinned project.");
        add("project.get", "project.read", false,
            "Project snapshot; positions and lengths in quarter-note beats.");
        add("project.files", "project.read", false,
            "List linked project media with automatic read access; paged files, "
            "total and "
            "revision.");
        add("devices.list", "project.read", false,
            "Installed device catalog; use format and uid in project.apply device "
            "operations.");
        add("time.convert", "project.read", false,
            "Convert beats, seconds or frames explicitly; frames require "
            "sampleRate.",
            {{"value", "number"}, {"from", "string"}, {"to", "string"}});
        add("project.apply", "project.write", true,
            "Atomically apply operations at expected revision; one Undo.",
            {{"revision", "integer"}, {"operations", "array"}, {"label", "string"}});
        add("project.sliceAudio", "project.write", true,
            "Slice a completed audio.renderClip snapshot in one Undo. Cuts are absolute timeline "
            "beats; sorts and skips duplicates/segments of 20 ms or less. Rejects stale snapshots. "
            "Returns cuts, skipped and created clip IDs. Only the captured clip is edited.",
            {{"snapshot", "string"}, {"cuts", "array"}, {"label", "string"}});
        add("parameters.list", "project.read", false,
            "Host-exposed parameters with stable identity, plain units and "
            "choices.",
            {{"track", "string"}, {"device", "string"}});
        add("parameters.get", "project.read", false, "Read a parameter using its device identity.",
            {{"track", "string"},
             {"device", "string"},
             {"parameter", "string"},
             {"identity", "string"}});
        add("parameters.set", "project.write", true,
            "Set a plain parameter value as one Undo action.",
            {{"track", "string"},
             {"device", "string"},
             {"parameter", "string"},
             {"identity", "string"},
             {"value", "number"}});
        add("commands.list", "project.read", false, "Discover callable commands.");
        add("commands.run", "project.write", true,
            "Invoke a command in the pinned project; host enforces assistant and "
            "collaboration "
            "policy.",
            {{"id", "string"}});
        add("workspace.get", "project.read", false,
            "Read VLTONE panels and selection in the pinned project.");
        add("workspace.open", "project.read", false,
            "Show/hide a native panel or open a validated clip/device editor; "
            "requires active "
            "pinned project.",
            {{"view", "string"}});
        add("workspace.select", "project.read", false,
            "Replace local track/clip selection after validating all targets; does "
            "not edit the "
            "document.");
        add("ui.get", "", false, "Read a script widget's value, visibility, size or table rows.",
            {{"id", "string"}});
        add("ui.focus", "", false, "Focus a script widget and raise its window.",
            {{"id", "string"}});
        add("ui.replace", "", false,
            "Atomically replace window contents; invalid trees keep the existing "
            "controls.",
            {{"id", "string"}, {"children", "array"}});
        add("transport.get", "project.read", false, "Transport state in beats.");
        add("transport.set", "project.write", true,
            "Play, stop, seek, set tempo or loop in beats.");
        add("ui.window", "", false, "Create a modeless native window from a widget tree.",
            {{"title", "string"}, {"children", "array"}});
        add("ui.set", "", false, "Update a widget by its local id.", {{"id", "string"}});
        add("ui.close", "", false, "Close a window by its local id.", {{"id", "string"}});
        add("ui.chooseFile", "", false, "Choose a file or folder and grant this instance access.");
        add("events.on", "", false,
            "Subscribe to coalesced project, selection, transport, parameter or "
            "job events.",
            {{"event", "string"}, {"callback", "object"}});
        add("events.off", "", false, "Remove a subscription.", {{"id", "string"}});
        add("timers.start", "", false, "Start a bounded timer (minimum 30 ms).",
            {{"milliseconds", "integer"}, {"callback", "object"}});
        add("timers.stop", "", false, "Stop a timer.", {{"id", "string"}});
        add("storage.get", "", false, "Read extension-local persistent settings.");
        add("storage.set", "", false, "Write extension-local persistent settings.",
            {{"value", "object"}});
        add("files.list", "files.read", false, "List files in a granted folder.",
            {{"path", "string"}});
        add("files.read", "files.read", false, "Read bounded UTF-8 text in a granted resource.",
            {{"path", "string"}});
        add("files.write", "files.write", false,
            "Atomically write UTF-8 text to a granted destination.",
            {{"path", "string"}, {"text", "string"}});
        add("audio.open", "files.read", false,
            "Open an audio resource; returns frames, channels and sample rate.",
            {{"path", "string"}});
        add("audio.read", "files.read", false,
            "Read at most 65536 frames as an interleaved float32 buffer.",
            {{"id", "string"}, {"frame", "integer"}, {"frames", "integer"}});
        add("audio.create", "files.write", false,
            "Create a staged WAV output; original files are preserved.",
            {{"path", "string"}, {"sampleRate", "integer"}, {"channels", "integer"}});
        add("audio.write", "files.write", false, "Append a PCM buffer to a staged output.",
            {{"id", "string"}});
        add("audio.close", "", false,
            "Close input, or publish a completed staged output when publish=true.",
            {{"id", "string"}});
        add("audio.process", "files.read", false,
            "Start cancellable offline analyze, trim, split, copy, reverse, gain "
            "or fade job.",
            {{"path", "string"}, {"operation", "string"}});
        add("audio.render", "project.read", false,
            "Render an immutable project snapshot into a separate output.", {{"path", "string"}});
        add("audio.renderClip", "project.read", false,
            "Start a cancellable job rendering the audible clip before live FX into run-local PCM. "
            "Includes warp, stretch, reverse, looping, trims and comps. Job result: path, snapshot, "
            "revision, start/length in beats, secondsPerBeat, minimumSliceSeconds. PCM frame 0 is "
            "the clip start; no source offset conversion. Read with audio.open/read.",
            {{"track", "string"}, {"clip", "string"}, {"revision", "integer"}});
        add("jobs.get", "", false, "Read job progress/result.", {{"id", "string"}});
        add("jobs.cancel", "", false, "Cancel job without publishing incomplete output.",
            {{"id", "string"}});
        add("network.request", "network", false,
            "Perform a bounded HTTPS request through the host.", {{"url", "string"}});
        add("process.start", "process", false,
            "Launch an explicitly granted executable without a shell.",
            {{"executable", "string"}, {"arguments", "array"}});
        add("log", "", false, "Append a message to this instance's bounded log.",
            {{"text", "string"}});
        const Json optional = {
            {"project.get",
             {{"track", "string"},
              {"clip", "string"},
              {"includeEvents", "boolean"},
              {"trackOffset", "integer"},
              {"trackLimit", "integer"},
              {"clipOffset", "integer"},
              {"clipLimit", "integer"},
              {"eventOffset", "integer"},
              {"eventLimit", "integer"}}},
            {"project.files", {{"offset", "integer"}, {"limit", "integer"}}},
            {"devices.list", {{"query", "string"}, {"offset", "integer"}, {"limit", "integer"}}},
            {"parameters.list",
             {{"parameter", "string"}, {"offset", "integer"}, {"limit", "integer"}}},
            {"time.convert", {{"sampleRate", "number"}}},
            {"parameters.set", {{"label", "string"}}},
            {"commands.list", {{"query", "string"}}},
            {"transport.set",
             {{"tempo", "number"},
              {"beat", "number"},
              {"loop", "boolean"},
              {"loopStart", "number"},
              {"loopEnd", "number"},
              {"playing", "boolean"}}},
            {"workspace.open",
             {{"visible", "boolean"},
              {"track", "string"},
              {"clip", "string"},
              {"device", "string"},
              {"identity", "string"}}},
            {"workspace.select", {{"tracks", "array"}, {"clips", "array"}}},
            {"ui.window",
             {{"id", "string"},
              {"width", "integer"},
              {"height", "integer"},
              {"dock", "string"},
              {"style", "any"},
              {"styles", "object"},
              {"theme", "object"}}},
            {"ui.replace", {{"style", "any"}, {"styles", "object"}, {"theme", "object"}}},
            {"ui.set",
             {{"value", "any"},
              {"style", "any"},
              {"theme", "object"},
              {"text", "string"},
              {"title", "string"},
              {"tooltip", "string"},
              {"width", "integer"},
              {"height", "integer"},
              {"items", "array"},
              {"rows", "array"},
              {"columns", "array"},
              {"enabled", "boolean"},
              {"visible", "boolean"},
              {"drawing", "array"}}},
            {"ui.chooseFile", {{"mode", "string"}, {"filter", "string"}, {"write", "boolean"}}},
            {"events.on",
             {{"track", "string"},
              {"device", "string"},
              {"parameter", "string"},
              {"identity", "string"},
              {"milliseconds", "integer"}}},
            {"audio.close", {{"publish", "boolean"}}},
            {"audio.process",
             {{"output", "string"},
              {"gain", "number"},
              {"thresholdDb", "number"},
              {"startFrame", "integer"},
              {"endFrame", "integer"},
              {"inFrames", "integer"},
              {"outFrames", "integer"},
              {"preRollMs", "number"},
              {"postRollMs", "number"},
              {"minimumSoundMs", "number"},
              {"minimumSilenceMs", "number"}}},
            {"audio.render", {{"start", "number"}, {"end", "number"}}}};
        for (auto &method : methods)
            method["optional"] = optional.value(method["name"].get<std::string>(), Json::object());
        const Json russian = {
            {"context.get", "Контекст запуска: объект под курсором, выделение, "
                            "диапазон и закреплённый проект."},
            {"project.get", "Снимок проекта с revision. Время — в четвертных долях. Для больших "
                            "проектов используйте фильтры и страницы."},
            {"project.apply", "Применить пакет операций к ожидаемой revision с одной записью Undo. "
                              "При конфликте получите новый снимок."},
            {"devices.list", "Каталог установленных устройств; format и uid используются для "
                             "установки через project.apply."},
            {"time.convert", "Явное преобразование beats, seconds и frames; для "
                             "кадров укажите sampleRate."},
            {"parameters.list", "Параметры устройства: значения, единицы, "
                                "диапазоны, варианты и identity."},
            {"parameters.get", "Прочитать параметр с проверкой identity устройства."},
            {"parameters.set", "Изменить значение параметра в его единицах с одной записью Undo."},
            {"commands.list", "Найти доступные команды приложения."},
            {"commands.run", "Вызвать команду в закреплённом проекте. Разрешения и "
                             "режим AI проверяет хост."},
            {"workspace.get", "Панели VLTONE и выделение закреплённого проекта."},
            {"workspace.open", "Открыть панель, редактор клипа или устройства по стабильному id. "
                               "Требуется активная вкладка проекта."},
            {"workspace.select", "Изменить локальное выделение дорожек и клипов "
                                 "после проверки всех объектов."},
            {"ui.get", "Прочитать состояние собственного виджета: значение, "
                       "видимость, размер и "
                       "строки таблицы."},
            {"ui.focus", "Поднять окно и передать фокус собственному виджету."},
            {"ui.replace", "Заменить содержимое окна целиком. Ошибка сохраняет "
                           "прежние контролы."},
            {"transport.get", "Состояние транспорта и позиция в долях."},
            {"transport.set", "Воспроизведение, позиция, темп и цикл."},
            {"ui.window", "Создать немодальное окно из дерева штатных контролов."},
            {"ui.set", "Обновить текст, значение, доступность или рисунок контрола по id."},
            {"ui.close", "Закрыть окно по id."},
            {"ui.chooseFile", "Выбрать файл или папку и выдать текущему экземпляру доступ."},
            {"events.on", "Подписаться на объединяемые события project, selection, transport, "
                          "parameter или job."},
            {"events.off", "Удалить подписку по id."},
            {"timers.start", "Запустить таймер: интервал не менее 30 мс, callback — функция Lua."},
            {"timers.stop", "Остановить таймер по id."},
            {"storage.get", "Прочитать локальные настройки расширения."},
            {"storage.set", "Сохранить локальный объект настроек расширения."},
            {"files.list", "Список файлов и подпапок в разрешённой папке."},
            {"files.read", "Прочитать текст UTF-8, до 512 КиБ."},
            {"files.write", "Атомарно сохранить текст UTF-8 в разрешённый файл."},
            {"audio.open", "Открыть аудиофайл: возвращает id, frames, channels и sampleRate."},
            {"audio.read", "Прочитать блок до 65 536 кадров; возвращает метаданные "
                           "и PCM-буфер float32."},
            {"project.files", "Файлы привязанного проекта: files, total, revision; offset/limit. "
                              "Источники доступны для чтения без выбора папок."},
            {"audio.create", "Создать временный WAV для нового выходного файла. "
                             "Исходники сохраняются."},
            {"audio.write", "Добавить PCM-буфер вторым аргументом в открытый выходной поток."},
            {"audio.close", "Закрыть поток; publish=true публикует полностью "
                            "записанный выходной файл."},
            {"audio.process", "Запустить отменяемое задание analyze, trim, split, "
                              "copy, reverse, gain или fade."},
            {"audio.render", "Отрендерить снимок проекта в отдельный файл. Возвращает id задания."},
            {"audio.renderClip", "Создать снимок звучания клипа до живых эффектов: Warp, растяжение, "
                                 "реверс, циклы, обрезка и компинг. Возвращает id задания. В результате: "
                                 "path, snapshot, revision, start/length в долях, secondsPerBeat и "
                                 "minimumSliceSeconds. Кадр 0 соответствует началу клипа."},
            {"project.sliceAudio", "Разрезать снимок audio.renderClip одним Undo. cuts — абсолютные "
                                    "доли таймлайна. Точки сортируются; повторы и фрагменты до 20 мс "
                                    "пропускаются. Устаревший снимок отклоняется. Результат: cuts, "
                                    "skipped, created (новые ID фрагментов). Меняется только выбранный клип."},
            {"jobs.get", "Прогресс, результат и ошибка задания."},
            {"jobs.cancel", "Отменить задание без публикации незавершённого результата."},
            {"network.request", "Выполнить ограниченный HTTPS GET через хост с "
                                "отдельным разрешением."},
            {"process.start", "Запустить явно разрешённую программу без оболочки, "
                              "с правами пользователя ОС."},
            {"log", "Добавить сообщение в ограниченный журнал экземпляра."}};
        for (auto &method : methods)
            method["descriptionRu"] = russian.at(method["name"].get<std::string>());
        Json operations = {
            {"track.add", {{"name", "string"}}},
            {"track.remove", {{"track", "string"}}},
            {"track.rename", {{"track", "string"}, {"name", "string"}}},
            {"track.mix", {{"track", "string"}}},
            {"track.move", {{"track", "string"}, {"index", "integer"}}},
            {"send.add", {{"track", "string"}, {"destination", "string"}}},
            {"send.remove", {{"track", "string"}, {"send", "string"}}},
            {"send.set", {{"track", "string"}, {"send", "string"}}},
            {"clip.addMidi", {{"track", "string"}, {"start", "number"}, {"length", "number"}}},
            {"clip.importAudio", {{"track", "string"}, {"path", "string"}, {"start", "number"}}},
            {"clip.remove", {{"track", "string"}, {"clip", "string"}}},
            {"clip.rename", {{"track", "string"}, {"clip", "string"}, {"name", "string"}}},
            {"clip.move", {{"track", "string"}, {"clip", "string"}, {"start", "number"}}},
            {"clip.trim",
             {{"track", "string"},
              {"clip", "string"},
              {"start", "number"},
              {"offset", "number"},
              {"length", "number"}}},
            {"clip.gain", {{"track", "string"}, {"clip", "string"}, {"gain", "number"}}},
            {"clip.fade",
             {{"track", "string"}, {"clip", "string"}, {"in", "number"}, {"out", "number"}}},
            {"clip.mute", {{"track", "string"}, {"clip", "string"}, {"muted", "boolean"}}},
            {"clip.copy", {{"track", "string"}, {"clip", "string"}, {"start", "number"}}},
            {"clip.split", {{"track", "string"}, {"clip", "string"}, {"at", "number"}}},
            {"notes.set", {{"track", "string"}, {"clip", "string"}, {"notes", "array"}}},
            {"notes.update", {{"track", "string"}, {"clip", "string"}, {"notes", "array"}}},
            {"automation.create", {{"track", "string"}, {"target", "object"}}},
            {"automation.set", {{"track", "string"}, {"clip", "string"}, {"points", "array"}}},
            {"device.add", {{"track", "string"}, {"format", "string"}, {"uid", "string"}}},
            {"device.instrument", {{"track", "string"}, {"format", "string"}, {"uid", "string"}}},
            {"device.replace",
             {{"track", "string"},
              {"device", "string"},
              {"identity", "string"},
              {"format", "string"},
              {"uid", "string"}}},
            {"device.remove", {{"track", "string"}, {"device", "string"}, {"identity", "string"}}},
            {"device.bypass",
             {{"track", "string"},
              {"device", "string"},
              {"identity", "string"},
              {"bypassed", "boolean"}}},
            {"device.move",
             {{"track", "string"},
              {"device", "string"},
              {"identity", "string"},
              {"index", "integer"}}}};
        const Json operationOptions = {
            {"track.add", {{"kind", "string"}}},
            {"track.mix",
             {{"volume", "number"},
              {"pan", "number"},
              {"muted", "boolean"},
              {"solo", "boolean"},
              {"output", "string"}}},
            {"track.move", {{"parent", "string"}}},
            {"send.set", {{"level", "number"}, {"enabled", "boolean"}, {"preFader", "boolean"}}},
            {"device.add", {{"index", "integer"}}},
            {"notes.update", {{"remove", "array"}}}};
        Json widgets = {
            {"column", "children: array of widgets"},
            {"row", "children: array of widgets"},
            {"group", "text: title, children: array of widgets"},
            {"grid", "columns: 1..32, children in row-major order"},
            {"scroll", "children: vertically scrollable widget tree"},
            {"splitter", "orientation: horizontal/vertical, children: resizable panes"},
            {"separator", "orientation: horizontal/vertical"},
            {"spacer", "expanding empty space; width/height for fixed spacing"},
            {"textEdit", "value:string, text:placeholder, readOnly, onChange"},
            {"table", "columns:string[], rows:string[][], editable, onClick{row,column}, "
                      "onChange{row,column,text}"},
            {"image", "path: package-relative or granted file, width/height; 8 MiB "
                      "/ 64 MP limit"},
            {"tabs", "children: widget specs with title"},
            {"button", "text, onClick=function(event)"},
            {"menu", "text, items: array of {text,onClick}"},
            {"field", "value:string, text:placeholder, onChange=function(event)"},
            {"toggle", "text, value:boolean, onChange=function(event)"},
            {"list", "items:string[], onChange with one-based value"},
            {"choice", "items:string[], onChange with one-based value"},
            {"knob", "min,max,value,unit,onChange; or "
                     "bind={track,device,parameter,identity}"},
            {"slider", "min,max,value,onChange; or bind"},
            {"number", "label,min,max,value,unit,step,decimals,onChange; or bind"},
            {"progress", "value: 0..1"},
            {"canvas", "drawing: line/rect/ellipse/text shapes; onPointer, onWheel, onKey, "
                       "onResize callbacks"},
            {"waveform", "drawing: finite peak array 0..1"},
            {"label", "text:string, wrap:boolean (default true), align:left/center/right"}};
        return Json{
            {"sdk", 1},
            {"lua", "5.5.1"},
            {"methods", methods},
            {"operations", operations},
            {"operationOptions", operationOptions},
            {"widgets", widgets},
            {"widgetCommon",
             {{"id", "Unique within one running extension; required for "
                     "ui.get/set/focus"},
              {"layout", "width/height fix size; minWidth/minHeight set a minimum; stretch "
                         "grows in row/column and window; margin/spacing are logical pixels"},
              {"style", "Named style or local style object; see uiStyle. Changes "
                        "never affect other windows."},
              {"accessibility", "label, tooltip, enabled, visible; keyboard focus "
                                "uses the VLTONE theme"},
              {"events", "Lua functions receive {id,value}. ui.set never emits onChange. "
                         "Indices are one-based; zero means no selection."}}},
            {"uiStyle",
             {{"theme", "Window-only opaque colors: "
                        "background,surface,field,text,muted,accent,onAccent,"
                        "border. Omitted colors follow the app theme."},
              {"tokens", Json::array({"$background", "$surface", "$field", "$text", "$muted",
                                      "$accent", "$onAccent", "$border"})},
              {"properties",
               {{"background", "Qt color or theme token"},
                {"color", "Text color or token"},
                {"accent", "Control accent color or token"},
                {"borderColor", "Color or token"},
                {"borderWidth", "Integer 0..12"},
                {"radius", "Integer 0..96"},
                {"padding", "Integer 0..96; layout inset for containers"},
                {"fontSize", "Integer 10..96 logical px"},
                {"fontWeight", "100..900 in steps of 100"},
                {"fontFamily", "Font family name; OS fallback if unavailable"}}},
              {"states", "hover,pressed,checked,disabled,focus: objects with "
                         "background,color,borderColor"},
              {"reuse", "window.styles defines up to 64 flat styles. widget.style='card' or "
                        "{use='card',radius=12}. window.style styles its content."},
              {"updates", "ui.set style replaces all local overrides; {} resets. theme "
                          "replaces window overrides; {} follows app. ui.replace inherits "
                          "theme/styles/root style unless supplied."},
              {"limits", "Styles accept validated values, never raw CSS, selectors "
                         "or URLs. Colors/fonts inherit; background/border/padding "
                         "apply only to the styled widget. Knob geometry stays "
                         "native; use canvas for a completely custom dial."}}},
            {"canvasEvents",
             {{"onPointer", "value={phase:down/move/up/"
                            "doubleClick,x,y,nx,ny,width,height,button,buttons,modifiers}; "
                            "moves coalesce within one gesture"},
              {"onWheel", "value={x,y,dx,dy,modifiers}; angle delta, normally 120 per "
                          "notch; only consumed with a handler"},
              {"onKey", "Key presses: value={key,text,code,repeat,modifiers}; focused "
                        "canvas owns typing/MIDI keys"},
              {"onResize", "value={width,height}; logical pixels, coalesced"}}},
            {"workspaceViews",
             Json::array({"arrangement", "mixer", "inspector", "browser", "master", "assistant",
                          "notebook", "trackOverview", "context", "pianoRoll", "pattern", "sample",
                          "automation", "plugin"})},
            {"clipFields",
             {{"source", "Absolute path to the original audio file. Read with audio.open; "
                         "requires project.read and files.read."},
              {"start", "Timeline start in quarter-note beats."},
              {"length", "Timeline length in quarter-note beats."},
              {"offset", "Source offset expressed in quarter-note beats at current tempo."},
              {"reversed", "True for reversed source playback."},
              {"looped", "True for looped source playback."},
              {"stretched", "True when warp, stretch or pitch changes source timing."},
              {"composite", "True for takes/comps or an edited audio document; source "
                            "alone does not describe the rendered clip."}}},
            {"operationSemantics",
             {{"clip.split", "at is an absolute timeline beat. The left segment "
                             "keeps the original clip "
                             "id; created contains the new right segment id. For "
                             "multiple cuts of the "
                             "same original clip, submit cuts right-to-left in one "
                             "project.apply. Every cut must leave more than 20 ms on both sides. "
                             "For transient slicing of arbitrary audio use audio.renderClip and "
                             "project.sliceAudio."}}},
            {"units",
             {{"musicalTime", "quarter-note beats"},
              {"fileTime", "integer frames with explicit sampleRate"},
              {"parameters", "native units"}}},
            {"context", "project, generation, type, target, selection, range, notes (note "
                        "IDs), arguments, preview, resourceDirectory, temporaryDirectory"},
            {"limits",
             {{"luaMemoryMiB", 64},
              {"callbackMilliseconds", 2000},
              {"pcmBlockFrames", 65536},
              {"pcmBlockBytes", 1048576}}},
            {"projectQueries",
             "Defaults: 128 tracks, 256 clips per track, 4096 notes/points per "
             "clip. Count "
             "and offset fields describe each page. Filter by track/clip IDs; use "
             "includeEvents=false for an overview. Compare revision between pages. "
             "notes.update upserts full note records by id, removes listed IDs and "
             "retains "
             "notes on other pages; notes.set replaces the entire clip."},
            {"tables", "Use vlt.array({}) for empty arrays. Nonempty sequential tables and "
                       "arrays returned by the host retain array encoding."}};
    }();
    return catalog;
}
const Json *apiMethod(std::string_view name) {
    for (const auto &method : apiCatalog()["methods"])
        if (method["name"].get_ref<const std::string &>() == name)
            return &method;
    return nullptr;
}
std::string validateArguments(const Json &method, const Json &arguments) {
    if (!arguments.is_object())
        return "Arguments must be an object";
    auto fields = method["required"];
    const auto optional = method.value("optional", Json::object());
    for (const auto &[key, type] : optional.items())
        if (arguments.contains(key))
            fields[key] = type;
    for (const auto &[key, type] : fields.items()) {
        if (!arguments.contains(key))
            return "Missing argument: " + key;
        const auto &value = arguments[key];
        if ((type == "string" && !value.is_string()) || (type == "number" && !value.is_number()) ||
            (type == "integer" && !value.is_number_integer()) ||
            (type == "boolean" && !value.is_boolean()) || (type == "array" && !value.is_array()) ||
            (type == "object" && !value.is_object()))
            return "Invalid argument: " + key;
    }
    if (method.value("name", "") == "project.apply") {
        if (arguments["operations"].size() > 4096)
            return "Batch exceeds 4096 operations";
        for (const auto &operation : arguments["operations"]) {
            if (!operation.is_object() || !operation.contains("op") || !operation["op"].is_string())
                return "Operation requires an op string";
            const auto name = operation["op"].get<std::string>();
            const auto &schemas = apiCatalog()["operations"];
            if (!schemas.contains(name))
                return "Unknown edit operation: " + name;
            const auto error = validateArguments(
                {{"required", schemas[name]},
                 {"optional", apiCatalog()["operationOptions"].value(name, Json::object())}},
                operation);
            if (!error.empty())
                return name + ": " + error;
        }
    }
    return {};
}
bool safeRelativePath(std::string_view path) {
    if (path.empty() || path.size() > 240 || path.front() == '/' || path.back() == '/' ||
        path.front() == '\\' || path.find(':') != path.npos || path.find('\\') != path.npos ||
        path.find('\0') != path.npos)
        return false;
    for (std::size_t start = 0; start < path.size();) {
        auto end = path.find('/', start);
        if (end == path.npos)
            end = path.size();
        const auto part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            return false;
        std::string stem(part.substr(0, part.find('.')));
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](unsigned char c) { return char(std::toupper(c)); });
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
            (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
             stem[3] >= '1' && stem[3] <= '9'))
            return false;
        for (unsigned char c : part)
            if (c < 32 || std::string_view("<>\"|?*").find(char(c)) != std::string_view::npos)
                return false;
        start = end + 1;
    }
    return true;
}
std::string validateManifest(const Json &m) {
    try {
        if (!m.is_object() || m.value("format", "") != "vltscript")
            return "Expected vltscript manifest";
        if (m.dump().size() > 128 * 1024)
            return "Manifest exceeds 128 KiB";
        static const std::regex id("[a-z0-9][a-z0-9._-]{2,95}");
        if (!std::regex_match(m.at("id").get<std::string>(), id) ||
            !safeRelativePath(m.at("id").get<std::string>()))
            return "Invalid extension id";
        if (m.at("name").get<std::string>().empty() || m.at("version").get<std::string>().empty())
            return "Name and version required";
        if (m.contains("description") && !m["description"].is_string())
            return "Description must be a string";
        if (!m.contains("sdk") || !m["sdk"].is_number_integer() || m["sdk"] != 1)
            return "Unsupported SDK (requires SDK 1)";
        if (m.contains("permissions") && !m["permissions"].is_array())
            return "Permissions must be an array";
        if (!m.at("commands").is_array() || m["commands"].empty() || m["commands"].size() > 128)
            return "Invalid commands";
        std::vector<std::string> ids;
        for (const auto &command : m["commands"]) {
            const auto commandId = command.at("id").get<std::string>();
            if (!std::regex_match(commandId, id) ||
                std::find(ids.begin(), ids.end(), commandId) != ids.end())
                return "Invalid or duplicate command id";
            ids.push_back(commandId);
            if (command.contains("contexts") && !command["contexts"].is_array())
                return "Contexts must be an array";
            if (command.contains("background") && !command["background"].is_boolean())
                return "Background must be boolean";
            if (command.contains("description") && !command["description"].is_string())
                return "Command description must be a string";
            if (!safeRelativePath(command.at("entry").get<std::string>()) ||
                command.at("label").get<std::string>().empty())
                return "Invalid command entry or label";
            const std::vector<std::string> contexts{
                "global",     "clip",  "track",  "arrangement", "notes",
                "automation", "mixer", "device", "parameter",   "file"};
            for (const auto &context : command.value("contexts", Json::array()))
                if (!context.is_string() || std::find(contexts.begin(), contexts.end(),
                                                      context.get<std::string>()) == contexts.end())
                    return "Unknown context";
        }
        for (const auto &p : m.value("permissions", Json::array())) {
            if (!p.is_string())
                return "Invalid permission";
            const std::vector<std::string> permissions{
                "project.read", "project.write", "files.read", "files.write", "network", "process"};
            if (std::find(permissions.begin(), permissions.end(), p.get<std::string>()) ==
                permissions.end())
                return "Unknown permission";
        }
    } catch (const std::exception &e) {
        return std::string("Invalid manifest: ") + e.what();
    }
    return {};
}
} // namespace vlt::script
