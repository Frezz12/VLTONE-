import { createServer } from "node:http";

const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9ZQmcAAAAASUVORK5CYII=", "base64");
const artifacts = [
  { id: "10000000-0000-4000-8000-000000000001", kind: "windows-exe", platform: "windows", label: "Windows Setup", file_name: "VLT-Setup.exe", bytes: 104857600, sha256: "a".repeat(64), download_url: "/v1/releases/0.1.2/download/windows-exe", updated_at: "2026-08-29T10:00:00Z" },
  { id: "10000000-0000-4000-8000-000000000002", kind: "macos-dmg", platform: "macos", label: "macOS DMG", file_name: "VLT.dmg", bytes: 125829120, sha256: "b".repeat(64), download_url: "/v1/releases/0.1.2/download/macos-dmg", updated_at: "2026-08-29T10:00:00Z" },
  { id: "10000000-0000-4000-8000-000000000003", kind: "linux-deb", platform: "linux", label: "Linux DEB", file_name: "vlt.deb", bytes: 94371840, sha256: "c".repeat(64), download_url: "/v1/releases/0.1.2/download/linux-deb", updated_at: "2026-08-29T10:00:00Z" },
  { id: "10000000-0000-4000-8000-000000000004", kind: "linux-rpm", platform: "linux", label: "Linux RPM", file_name: "vlt.rpm", bytes: 94371840, sha256: "d".repeat(64), download_url: "/v1/releases/0.1.2/download/linux-rpm", updated_at: "2026-08-29T10:00:00Z" },
];
function release(locale) {
  const ru = locale === "ru";
  return { id: "20000000-0000-4000-8000-000000000001", version: "0.1.2", summary: ru ? "Новый микшер и исправления" : "New mixer and fixes", features: [ru ? "Добавлен новый микшер" : "Added a new mixer"], changes: [], fixes: [ru ? "Исправлен запуск" : "Fixed startup"], artifacts, screenshots: [{ id: "30000000-0000-4000-8000-000000000001", caption: ru ? "Обновлённый микшер" : "Updated mixer", sort_order: 10, width: 1, height: 1, sha256: "e".repeat(64), url: "/v1/releases/0.1.2/screenshots/30000000-0000-4000-8000-000000000001" }], page_url: "http://127.0.0.1:3100/releases/0.1.2", published_at: "2026-08-29T10:00:00Z" };
}

function release031(locale) {
  const ru = locale === "ru";
  const features = [
    ["Слайд-ноты меняют высоту без повторной атаки.", "Slide notes change pitch without retriggering."],
    ["Рисуемая кривая высоты поддерживает несколько узлов.", "Draw pitch curves with several nodes."],
    ["Формы слайдов можно копировать и прослушивать.", "Copy and audition slide shapes."],
    ["Режим аккорда сохраняет расстояния между голосами.", "Chord mode preserves voice spacing."],
    ["Подсветка показывает связь слайда с нотой.", "Highlights show how a slide links to its note."],
    ["Sampler соединяет ноты в режиме Slide / Legato.", "Sampler connects notes with Slide / Legato."],
    ["Передача слайдов поддерживает MPE и Pitch Bend.", "Send slides with MPE and Pitch Bend."],
    ["Замена сэмпла из Piano Roll сохраняет настройки.", "Replace a sample from Piano Roll and retain settings."],
    ["Генератор аккордов работает в пустом клипе.", "The chord generator works in an empty clip."],
    ["Автоматизация Master управляет громкостью и плагинами.", "Master automation controls volume and plugins."],
    ["Общий Bounce собирает несколько дорожек в один аудиоклип.", "Bounce several tracks into one audio clip."],
    ["Компактные дорожки сохраняют обзор содержимого папок.", "Compact tracks retain a folder contents overview."],
  ].map(values => values[ru ? 0 : 1]);
  const changes = [
    ["Piano Roll получил обновлённые панели и масштабирование.", "Piano Roll has updated panels and zoom controls."],
    ["Пустая MIDI-дорожка создаёт клип без открытия редактора.", "An empty MIDI track creates a clip without opening the editor."],
    ["Во время воспроизведения можно свободно прокручивать партию.", "Scroll freely during playback."],
    ["Окно рендера объединяет формат, стемы и метаданные.", "The render window combines format, stems and metadata."],
    ["AI-чат получил новое поле ввода и действия с подсказками.", "AI chat has an updated input and actions with tooltips."],
    ["Обрезка MIDI-клипа сохраняет полный источник нот.", "MIDI trimming retains the complete note source."],
    ["Формат проекта 11 сохраняет слайды отдельно от нот.", "Project format 11 stores slides separately from notes."],
    ["Экспорт в аудио учитывает слайды, а MIDI-экспорт сообщает об ограничении.", "Audio export includes slides; MIDI export reports the limitation."],
  ].map(values => values[ru ? 0 : 1]);
  return {
    ...release(locale), id: "release-031", version: "0.3.1", features, changes,
    summary: ru ? "Слайд-ноты, аккорды, Master и обновлённый экспорт" : "Slide notes, chords, Master and updated export",
    screenshots: ["Piano Roll", "Slide curve", "Chord generator", "Render", "AI chat", "Sampler", "Mixer", "Arrangement"].map((caption, index) => ({ id: `shot-${index}`, caption, sort_order: (index + 1) * 10, width: 800, height: 500, sha256: "e".repeat(64), url: `/v1/releases/0.3.1/screenshots/shot-${index}` })),
  };
}

function release032(locale) {
  const source = release031(locale);
  return { ...source, version: "0.3.2", screenshots: source.screenshots.slice(0, 2), highlights: [
    { id: "custom-melody", tone: "mint", title_ru: "Мелодия с движением", title_en: "Melody in motion", lead_ru: "Текст из редактора админки.", lead_en: "Text from the admin editor.", label_ru: "Мелодия", label_en: "Melody", details_ru: ["Подробность из админки"], details_en: ["Detail from the admin editor"], screenshot_id: "shot-0" },
    { id: "custom-slide", tone: "rose", title_ru: "Новый способ работы", title_en: "A new way to work", lead_ru: "Второй авторский блок.", lead_en: "A second authored block.", label_ru: "Редактирование", label_en: "Editing", details_ru: [], details_en: [], screenshot_id: "shot-1" },
  ] };
}

createServer((request, response) => {
  const url = new URL(request.url, "http://127.0.0.1:8099");
  if (url.pathname === "/health") return response.end("ok");
  if (url.pathname.includes("/screenshots/")) { response.writeHead(200, { "Content-Type": "image/png", "Content-Length": png.length }); return response.end(png); }
  if (url.pathname.includes("/download/")) { const body = Buffer.from("installer"); response.writeHead(200, { "Content-Type": "application/octet-stream", "Content-Length": body.length, "Content-Disposition": 'attachment; filename="VLT-installer.bin"' }); return response.end(body); }
  const locale = url.searchParams.get("locale") === "ru" ? "ru" : "en";
  if (url.pathname === "/v1/releases") { response.setHeader("Content-Type", "application/json"); return response.end(JSON.stringify({ releases: [release(locale)] })); }
  if (url.pathname === "/v1/releases/0.1.2") { response.setHeader("Content-Type", "application/json"); return response.end(JSON.stringify(release(locale))); }
  if (url.pathname === "/v1/releases/0.3.1") { response.setHeader("Content-Type", "application/json"); return response.end(JSON.stringify(release031(locale))); }
  if (url.pathname === "/v1/releases/0.3.2") { response.setHeader("Content-Type", "application/json"); return response.end(JSON.stringify(release032(locale))); }
  response.writeHead(404, { "Content-Type": "application/json" }); response.end(JSON.stringify({ code: "not_found", message: "Not found" }));
}).listen(8099, "127.0.0.1");
