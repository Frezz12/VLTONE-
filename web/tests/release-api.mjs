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
  return { id: "20000000-0000-4000-8000-000000000001", version: "0.1.2", summary: ru ? "Новый микшер и исправления" : "New mixer and fixes", features: [ru ? "Добавлен новый микшер" : "Added a new mixer"], changes: [], fixes: [ru ? "Исправлен запуск" : "Fixed startup"], artifacts, screenshots: [{ id: "30000000-0000-4000-8000-000000000001", caption: ru ? "Обновлённый микшер" : "Updated mixer", sort_order: 10, width: 1, height: 1, sha256: "e".repeat(64), url: "/v1/releases/0.1.2/screenshots/30000000-0000-4000-8000-000000000001" }], page_url: `http://127.0.0.1:3100/${locale}/releases/0.1.2`, published_at: "2026-08-29T10:00:00Z" };
}

createServer((request, response) => {
  const url = new URL(request.url, "http://127.0.0.1:8099");
  if (url.pathname === "/health") return response.end("ok");
  if (url.pathname.includes("/screenshots/")) { response.writeHead(200, { "Content-Type": "image/png", "Content-Length": png.length }); return response.end(png); }
  if (url.pathname.includes("/download/")) { const body = Buffer.from("installer"); response.writeHead(200, { "Content-Type": "application/octet-stream", "Content-Length": body.length, "Content-Disposition": 'attachment; filename="VLT-installer.bin"' }); return response.end(body); }
  const locale = url.searchParams.get("locale") === "ru" ? "ru" : "en";
  if (url.pathname === "/v1/releases") { response.setHeader("Content-Type", "application/json"); return response.end(JSON.stringify({ releases: [release(locale)] })); }
  if (url.pathname === "/v1/releases/0.1.2") { response.setHeader("Content-Type", "application/json"); return response.end(JSON.stringify(release(locale))); }
  response.writeHead(404, { "Content-Type": "application/json" }); response.end(JSON.stringify({ code: "not_found", message: "Not found" }));
}).listen(8099, "127.0.0.1");
