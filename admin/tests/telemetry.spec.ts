import { expect, test } from "@playwright/test";

const userID = "00000000-0000-4000-8000-000000000101";
const session = { id: "session-one", app_version: "0.1.7", build_id: "build-" + "a".repeat(180), started_at: "2026-09-07T09:00:00Z", hardware: { cpu_model: "Apple M4", ram_bytes: 16000000000 } };
const slot = { id: "slot", slot: 2, name: "Compressor-" + "x".repeat(180), vendor: "Test Vendor", version: "2.1", format: "VST3", bypassed: true, mix: .75, channel_mode: "stereo", sidechain_track_id: "track-1" };
const device = { name: "USB Interface " + "device".repeat(50), manufacturer: "Test", host_api: "Core Audio", input_channels: 2, output_channels: 2, is_alive: true, is_asio: false };
const sample = { id: "sample-one", event_id: "event-one", session_id: session.id, device_id: "device-one", recorded_at: "2026-09-07T09:05:00Z", process_cpu: 12, system_cpu: 20, dsp_load: 8, dsp_peak: 15, xruns: 3, resident_bytes: 256000000, sample_rate: 48000, buffer_frames: 256, track_count: 80, clip_count: 90, plugin_count: 160, playback_state: "playing", recording: false, foreground: true };
const snapshot = { schema_version: 1, window_ms: 300000, measurement_count: 300, truncated: false, tempo: 120, time_signature_numerator: 4, time_signature_denominator: 4, position_seconds: 10, loop_enabled: false, loop_start_seconds: 0, loop_end_seconds: 20, master_volume: 1, master_pan: 0, master_inserts: [slot], tracks: Array.from({ length: 80 }, (_, i) => ({ id: `track-${i}`, name: i === 0 ? "Бас " + "long".repeat(60) : `Дорожка ${i}`, kind: "Instrument", parent_id: "", output_bus_id: "", volume: 1, pan: 0, muted: false, soloed: false, armed: true, monitor: false, mono: false, frozen: false, input_enabled: true, input_channel: 0, input_channel_count: 2, clip_count: 3, instrument: { ...slot, name: "Synth", bypassed: false }, inserts: [slot], sampler_fx: [], sampler_fx_active: false, sends: [{ destination_track_id: "track-1", level: .5, enabled: true, pre_fader: true }] })), audio: { input: device, output: device, running: true, input_enabled: true, input_channels: [0, 1], output_channels: [0, 1], input_underflow: 0, input_overflow: 1, output_underflow: 2, output_overflow: 0, gated_blocks: 4, workers: 4, realtime_workers: 4, workgroup_workers: 4 } };

for (const width of [1440, 375]) test(`telemetry history, long reports and polling at ${width}px`, async ({ page }) => {
  await page.setViewportSize({ width, height: 1000 });
  await page.clock.install();
  let detailRequests = 0, fresh = false, failed = false, burst = false;
  const arrivals = Array.from({ length: 25 }, (_, i) => ({ ...sample, id: `burst-${i}`, event_id: `burst-${i}`, recorded_at: new Date(Date.parse("2026-09-07T12:00:00Z") - i * 300000).toISOString() }));
  await page.route("**/api/v1/admin/**", async (route) => {
    const url = new URL(route.request().url()), path = url.pathname;
    if (path.endsWith("/admin/me")) return route.fulfill({ json: { admin: { id: "owner", email: "owner@example.com", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2099-01-01T00:00:00Z" } });
    if (path.endsWith(`/users/${userID}`)) return route.fulfill({ json: { user: { id: userID, nickname: "Тестировщик", email: "tester@example.com", status: "active", created_at: session.started_at }, devices: [], quota: { base_limit: 20000000, adjustment: 0, used_tokens: 0, remaining_tokens: 20000000 }, subscription: { plan: { display_name: "Demo" } }, counts: { launches: 1, crashes: 1, bugs: 0 } } });
    if (path.endsWith("/ledger")) return route.fulfill({ json: { entries: [] } });
    if (path.endsWith("/telemetry") && burst) return route.fulfill({ json: { sessions: [session], samples: url.searchParams.get("before") === "burst-cursor" ? [...arrivals.slice(20), sample] : arrivals.slice(0, 20), next_cursor: url.searchParams.has("before") ? "" : "burst-cursor" } });
    if (path.endsWith("/telemetry")) return route.fulfill({ json: { sessions: [session], samples: url.searchParams.has("before") ? [{ ...sample, id: "old", event_id: "old", recorded_at: "2026-09-07T09:00:00Z" }] : fresh ? [{ ...sample, id: "new", event_id: "new", recorded_at: "2026-09-07T09:10:00Z" }, sample] : [sample], next_cursor: url.searchParams.has("before") ? "" : "older-cursor" } });
    if (path.endsWith("/telemetry/event-one")) { detailRequests++; if (!failed) { failed = true; return route.fulfill({ status: 503, json: { message: "Временная ошибка" } }); } return route.fulfill({ json: { payload: { ...sample, snapshot }, session, recorded_at: sample.recorded_at, device_id: sample.device_id, event_id: sample.event_id } }); }
    if (path.endsWith("/telemetry/old")) return route.fulfill({ json: { payload: { ...sample, plugins: [{ name: "Legacy synth", count: 1 }] }, session, recorded_at: sample.recorded_at, device_id: sample.device_id } });
    return route.fulfill({ status: 204 });
  });
  await page.goto(`/users/${userID}`);
  await expect(page.getByRole("heading", { name: "История состояния программы" })).toBeVisible();
  await expect(page.locator(".telemetry-sample")).toHaveCount(1);
  expect(detailRequests).toBe(0);
  await page.locator(".telemetry-sample-toggle").click();
  await page.getByRole("button", { name: "Повторить", exact: true }).click();
  await expect(page.getByRole("heading", { name: "Аудиоустройства и движок" })).toBeVisible();
  await page.getByRole("textbox", { name: "Найти дорожку или плагин" }).fill("Бас");
  await expect(page.locator(".telemetry-tracks > details")).toHaveCount(1);
  await page.locator(".telemetry-tracks summary").click();
  await expect(page.locator(".telemetry-track-body").getByText("Bypass", { exact: true })).toBeVisible();
  await expect(page.locator(".telemetry-track-body").getByText("Sidechain: Дорожка 1").first()).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  if (width > 1000) expect((await page.locator(".detail-grid > aside").boundingBox())!.height).toBeLessThan(1200);
  await page.screenshot({ path: `test-results/telemetry-${width}.png`, fullPage: true });
  fresh = true;
  await page.clock.fastForward(31000);
  await expect(page.locator(".telemetry-sample")).toHaveCount(2);
  await expect(page.getByRole("textbox", { name: "Найти дорожку или плагин" })).toHaveValue("Бас");
  expect(detailRequests).toBe(2);
  await page.getByRole("button", { name: "Загрузить более ранние" }).click();
  await expect(page.locator(".telemetry-sample")).toHaveCount(3);
  await page.locator(".telemetry-sample-toggle").last().click();
  await expect(page.getByText("Старая версия отчёта:", { exact: false })).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  burst = true;
  await page.clock.fastForward(31000);
  await expect(page.locator(".telemetry-sample")).toHaveCount(28);
  await expect(page.getByRole("textbox", { name: "Найти дорожку или плагин" })).toHaveValue("Бас");
});
