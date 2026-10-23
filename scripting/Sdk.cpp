#include "Sdk.hpp"
#include "Api.hpp"
#include <fstream>
#include <set>
#include <stdexcept>

namespace vlt::script {
void writeSdk(const std::filesystem::path &destination) {
    std::filesystem::create_directories(destination);
    const auto save = [&](const char *name, const std::string &value) {
        std::ofstream file(destination / name, std::ios::binary);
        file << value;
        if (!file)
            throw std::runtime_error("Cannot write SDK documentation");
    };
    save("api.json", apiCatalog().dump(2) + "\n");
    std::string annotations = "---@meta VLTONE SDK 1\n-- Generated from the runtime registry. Do "
                              "not execute this file.\n";
    annotations += "---@class VltPcmBuffer\n---@field [integer] number "
                   "Interleaved float32 sample, "
                   "one-based\n";
    annotations += R"lua(
---@class VltUiEvent
---@field id string Widget id
---@field value any One-based index, scalar value or structured canvas/table event
---@alias VltUiCallback fun(event: VltUiEvent)
---@class VltBinding
---@field track string
---@field device string
---@field parameter string
---@field identity string From parameters.list; invalidated on replacement
---@class VltTheme
---@field background? string
---@field surface? string
---@field field? string
---@field text? string
---@field muted? string
---@field accent? string
---@field onAccent? string
---@field border? string
---@class VltStyleState
---@field background? string Qt color or $theme token
---@field color? string
---@field borderColor? string
---@class VltStyle: VltStyleState
---@field use? string Named style with local overrides
---@field accent? string
---@field borderWidth? integer 0..12
---@field radius? integer 0..96
---@field padding? integer 0..96
---@field fontSize? integer 10..96 logical pixels
---@field fontWeight? integer 100..900, step 100
---@field fontFamily? string
---@field hover? VltStyleState
---@field pressed? VltStyleState
---@field checked? VltStyleState
---@field disabled? VltStyleState
---@field focus? VltStyleState
---@class VltWidget
---@field type string See api.json widgets
---@field id? string Unique in this instance
---@field children? VltWidget[]
---@field text? string Caption or placeholder
---@field title? string Tab title
---@field label? string Accessible name and control label
---@field tooltip? string
---@field style? string|VltStyle
---@field wrap? boolean Label text wrapping
---@field align? 'left'|'center'|'right' Label alignment
---@field value? any
---@field min? number
---@field max? number
---@field step? number
---@field decimals? integer
---@field unit? string
---@field items? string[]|table[]
---@field columns? integer|string[] Grid column count or table headings
---@field rows? string[][]
---@field drawing? table[]|number[] Canvas shapes or waveform peaks
---@field path? string Package-relative or granted image
---@field bind? VltBinding
---@field width? integer Fixed logical pixels
---@field height? integer Fixed logical pixels
---@field minWidth? integer
---@field minHeight? integer
---@field stretch? integer
---@field spacing? integer
---@field margin? integer
---@field orientation? 'horizontal'|'vertical'
---@field enabled? boolean
---@field visible? boolean
---@field readOnly? boolean
---@field editable? boolean
---@field onClick? VltUiCallback
---@field onChange? VltUiCallback
---@field onPointer? VltUiCallback
---@field onWheel? VltUiCallback
---@field onKey? VltUiCallback
---@field onResize? VltUiCallback
---@class VltWindow
---@field id? string Defaults to main
---@field title string
---@field children VltWidget[]
---@field theme? VltTheme Overrides for this window only
---@field styles? table<string,VltStyle> Reusable flat style definitions
---@field style? string|VltStyle Window content style
---@field dock? 'float'|'left'|'right'|'top'|'bottom' Preview always floats
---@field width? integer
---@field height? integer
)lua";
    annotations += "vlt = {}\n---@generic T\n---@param values T[]\n---@return T[]\nfunction "
                   "vlt.array(values) end\n";
    std::set<std::string> groups;
    std::string html = "<!doctype html><meta charset=\"utf-8\"><title>VLTONE SDK 1 API</title>"
                       "<style>body{font:16px/1.55 system-ui;max-width:1000px;margin:40px "
                       "auto;padding:0 "
                       "24px}code{background:#eee;color:#222;padding:2px "
                       "5px}article{border-top:1px solid "
                       "#aaa;padding:12px 0}a{color:#447acc}</style>"
                       "<h1>VLTONE SDK 1</h1><p><a href=\"SDK_RU.html\">Руководство</a> · <a "
                       "href=\"SDK_EN.html\">Guide</a> · <a href=\"api.json\">JSON</a></p>"
                       "<p>Lua 5.5.1. Musical time: quarter-note beats. File time: frames with "
                       "explicit sample "
                       "rate. Parameter values: native units.</p>";
    std::string htmlRu = "<!doctype html><html lang=\"ru\"><meta charset=\"utf-8\"><title>VLTONE "
                         "SDK 1 — "
                         "API</title><style>body{font:16px/1.6 "
                         "system-ui;max-width:1000px;margin:40px "
                         "auto;padding:0 24px}article{border-top:1px solid #aaa;padding:12px "
                         "0}pre{overflow:auto;background:#eee;color:#222;padding:12px}a{color:#"
                         "447acc}</"
                         "style><h1>Справочник VLTONE SDK 1</h1><p><a "
                         "href=\"SDK_RU.html\">Руководство</a> · <a "
                         "href=\"API.html\">English API</a></p>";
    for (const auto &method : apiCatalog()["methods"]) {
        const auto name = method["name"].get<std::string>(),
                   description = method["description"].get<std::string>();
        const auto dot = name.find('.');
        if (dot != name.npos && groups.insert(name.substr(0, dot)).second)
            annotations += "vlt." + name.substr(0, dot) + " = {}\n";
        annotations += "---" + description + "\n";
        std::string type = "{";
        auto fields = method["required"];
        fields.update(method["optional"]);
        for (const auto &[key, kind] : fields.items()) {
            if (type.size() > 1)
                type += ", ";
            type += key + (method["required"].contains(key) ? ": " : "?: ") +
                    (kind == "array"    ? std::string("table[]")
                     : kind == "object" ? std::string("table")
                                        : kind.get<std::string>());
        }
        type += "}";
        if (name == "ui.window")
            type = "VltWindow";
        if (name == "ui.replace")
            type = "{id: string, children: VltWidget[], style?: string|VltStyle, "
                   "styles?: table<string,VltStyle>, theme?: VltTheme}";
        if (name == "project.sliceAudio")
            type = "{snapshot: string, cuts: number[], label: string}";
        annotations += "---@param args" + std::string(method["required"].empty() ? "? " : " ") +
                       (fields.empty() ? std::string("table") : type) + "\n";
        if (name == "audio.read")
            annotations += "---@return table metadata\n---@return VltPcmBuffer buffer\n";
        else
            annotations += "---@return any\n";
        if (name == "audio.write")
            annotations += "---@param buffer VltPcmBuffer\n";
        annotations += "function vlt." + name + "(args" +
                       (name == "audio.write" ? ", buffer" : "") + ") end\n";
        html += "<article id=\"" + name + "\"><h2><code>vlt." + name + "(args)</code></h2><p>" +
                description + "</p><p>Permission: <code>" +
                method["permission"].get<std::string>() +
                "</code></p><p>Required arguments:</p><ul>";
        for (const auto &[key, kind] : method["required"].items())
            html += "<li><code>" + key + "</code>: " + kind.get<std::string>() + "</li>";
        html += "</ul><p>Optional arguments:</p><ul>";
        for (const auto &[key, kind] : method["optional"].items())
            html += "<li><code>" + key + "</code>: " + kind.get<std::string>() + "</li>";
        html += "</ul></article>";
        htmlRu += "<article id=\"" + name + "\"><h2><code>vlt." + name + "(args)</code></h2><p>" +
                  method["descriptionRu"].get<std::string>() + "</p><p>Разрешение: <code>" +
                  method["permission"].get<std::string>() +
                  "</code></p><p>Обязательные поля:</p><ul>";
        for (const auto &[key, kind] : method["required"].items())
            htmlRu += "<li><code>" + key + "</code>: " + kind.get<std::string>() + "</li>";
        htmlRu += "</ul><p>Необязательные поля:</p><ul>";
        for (const auto &[key, kind] : method["optional"].items())
            htmlRu += "<li><code>" + key + "</code>: " + kind.get<std::string>() + "</li>";
        htmlRu += "</ul></article>";
    }
    html += "<h2>Project queries</h2><p>" + apiCatalog()["projectQueries"].get<std::string>() +
            "</p><h2>Edit operations</h2>";
    htmlRu += "<h2>Постраничное чтение</h2><p>По умолчанию: 128 дорожек, 256 "
              "клипов на дорожку, "
              "4096 нот и точек на клип. Фильтры track и clip выбирают объект по "
              "id. Поля "
              "trackCount, clipCount, noteCount, pointCount содержат полное "
              "количество. "
              "Используйте соответствующие Offset/Limit и сравнивайте revision "
              "между страницами. "
              "notes.update обновляет ноты по id и сохраняет остальные; "
              "notes.set заменяет все "
              "ноты клипа.</p><h2>Операции редактирования</h2>";
    for (const auto &[name, required] : apiCatalog()["operations"].items()) {
        const auto optional = apiCatalog()["operationOptions"].value(name, Json::object());
        html += "<article><h3>" + name + "</h3><p>Required:</p><pre>" + required.dump(2) +
                "</pre><p>Optional:</p><pre>" + optional.dump(2) + "</pre></article>";
        htmlRu += "<article><h3>" + name + "</h3><p>Обязательные поля:</p><pre>" +
                  required.dump(2) + "</pre><p>Необязательные поля:</p><pre>" + optional.dump(2) +
                  "</pre></article>";
    }
    html += "<h2>Clip fields</h2><pre>" + apiCatalog()["clipFields"].dump(2) + "</pre>";
    html += "<h2>Split semantics</h2><p>" +
            apiCatalog()["operationSemantics"]["clip.split"].get<std::string>() + "</p>";
    htmlRu += "<h2>Источник аудиоклипа</h2><p>source — абсолютный путь к "
              "исходному файлу. "
              "start, length, offset выражены в долях. reversed, looped, "
              "stretched и composite "
              "указывают на обработку или составной источник. project.files "
              "возвращает файлы "
              "проекта с автоматическим доступом на чтение. clip.split сохраняет "
              "исходный id "
              "у левой части; created содержит id правой. Для нескольких "
              "разрезов одного клипа "
              "передавайте позиции at справа налево в одном project.apply.</p>";
    html += "<h2>Widgets</h2><pre>" + apiCatalog()["widgets"].dump(2) + "</pre>";
    htmlRu += "<h2>Контролы</h2><p>Описание свойств и примеры приведены в <a "
              "href=\"SDK_RU.html\">руководстве</a>.</p><pre>" +
              apiCatalog()["widgets"].dump(2) + "</pre>";
    const std::string search = R"html(<style>
:root{color-scheme:light dark}body{background:Canvas;color:CanvasText}pre,code{background:color-mix(in srgb,CanvasText 8%,Canvas);color:CanvasText}pre{white-space:pre-wrap;overflow-wrap:anywhere}input{font:inherit;width:100%;box-sizing:border-box;padding:12px;margin:12px 0;position:sticky;top:8px;border:1px solid #888;border-radius:8px;background:Canvas;color:CanvasText}article[hidden]{display:none}a{color:LinkText}a:focus-visible,input:focus-visible{outline:3px solid Highlight;outline-offset:3px}
</style><script>const search=document.createElement('input');search.type='search';search.placeholder=document.documentElement.lang==='ru'?'Поиск по API и операциям':'Search API and operations';search.setAttribute('aria-label',search.placeholder);document.querySelector('h1').after(search);search.addEventListener('input',()=>{const words=search.value.toLocaleLowerCase().split(/\s+/).filter(Boolean);for(const article of document.querySelectorAll('article'))article.hidden=!words.every(w=>article.textContent.toLocaleLowerCase().includes(w));});</script>)html";
    html += "<h2>Widget conventions and events</h2><pre>" + apiCatalog()["widgetCommon"].dump(2) +
            "</pre><pre>" + apiCatalog()["canvasEvents"].dump(2) + "</pre>";
    htmlRu += "<h2>Свойства виджетов и события</h2><pre>" + apiCatalog()["widgetCommon"].dump(2) +
              "</pre><pre>" + apiCatalog()["canvasEvents"].dump(2) + "</pre>";
    html += "<h2>Custom window design</h2><p><a href=\"STYLE_EN.html\">Styling "
            "guide and example</a></p><pre>" +
            apiCatalog()["uiStyle"].dump(2) + "</pre>";
    htmlRu += "<h2>Свои стили окон</h2><p><a href=\"STYLE_RU.html\">Руководство "
              "и пример</a></p><pre>" +
              apiCatalog()["uiStyle"].dump(2) + "</pre>";
    html += search;
    htmlRu += search;
    save("vlt.lua", annotations);
    save("API.html", html);
    save("API_RU.html", htmlRu);
}
} // namespace vlt::script
