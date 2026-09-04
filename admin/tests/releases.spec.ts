import { expect, test } from "@playwright/test";

test("admin saves a draft, uploads an installer, and publishes it", async ({ page }) => {
  let release: Record<string, unknown> | undefined;
  await page.route("**/api/v1/admin/**", async (route) => {
    const url = new URL(route.request().url());
    if (url.pathname.endsWith("/me")) return route.fulfill({ json: { admin: { id: "a", email: "owner@example.com", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2026-08-30T00:00:00Z" } });
    if (url.pathname.endsWith("/releases") && route.request().method() === "GET") return route.fulfill({ json: { releases: release ? [release] : [] } });
    if (url.pathname.endsWith("/releases") && route.request().method() === "POST") {
      const input = route.request().postDataJSON();
      release = { id: "10000000-0000-4000-8000-000000000001", status: "draft", ...input, artifacts: [], screenshots: [], created_at: "2026-08-29T10:00:00Z", updated_at: "2026-08-29T10:00:00Z", published_at: null };
      return route.fulfill({ status: 201, json: release });
    }
    if (/\/releases\/[^/]+$/.test(url.pathname) && route.request().method() === "PUT") {
      release = { ...release, ...route.request().postDataJSON(), updated_at: "2026-08-29T10:03:00Z" };
      return route.fulfill({ json: release });
    }
    if (url.pathname.endsWith("/publish")) {
      if (!release?.summary_ru || !release?.summary_en || !Array.isArray(release.artifacts) || release.artifacts.length === 0) {
        return route.fulfill({ status: 422, json: { code: "release_not_ready", message: "Заполните обязательные поля.", field_errors: { version: "Укажите версию X.Y.Z.", summary_ru: "Добавьте русское описание.", summary_en: "Add an English summary.", artifacts: "Загрузите хотя бы один установщик." } } });
      }
      release = { ...release, status: "published", published_at: "2026-08-29T10:10:00Z" }; return route.fulfill({ json: release });
    }
    return route.fulfill({ json: release });
  });
  await page.route("**/release-upload/**", async (route) => {
    if (new URL(route.request().url()).pathname.endsWith("/screenshots")) {
      const screenshot = { id: "30000000-0000-4000-8000-000000000001", caption_ru: "Микшер", caption_en: "Mixer", sort_order: 0, width: 1280, height: 720, sha256: "b".repeat(64), url: "/v1/releases/0.1.6/screenshots/30000000-0000-4000-8000-000000000001" };
      release = { ...release, screenshots: [screenshot] };
      return route.fulfill({ status: 201, json: screenshot });
    }
    const artifact = { id: "20000000-0000-4000-8000-000000000001", kind: "windows-exe", platform: "windows", label: "Windows Setup", file_name: "VLT-Setup.exe", bytes: 12, sha256: "a".repeat(64), download_url: "/v1/releases/0.1.2/download/windows-exe", updated_at: "2026-08-29T10:05:00Z" };
    release = { ...release, artifacts: [artifact] };
    await route.fulfill({ json: artifact });
  });

  await page.setViewportSize({ width: 375, height: 800 });
  await page.goto("/releases");
  await page.getByRole("button", { name: "Опубликовать" }).click();
  const errorSummary = page.locator(".release-error-summary");
  await expect(errorSummary).toBeVisible();
  await expect(errorSummary).toBeFocused();
  await page.getByRole("button", { name: "Новый релиз" }).click();
  await page.getByRole("button", { name: "Заполнить 0.1.6" }).click();
  await expect(page.getByLabel("Версия X.Y.Z")).toHaveValue("0.1.6");
  await expect(page.getByLabel("Кратко — русский")).toHaveValue(/Bounce in Place/);

  // Choosing a file on a new release must create its draft automatically.
  await page.getByLabel("Загрузить").first().setInputFiles({ name: "VLT-Setup.exe", mimeType: "application/octet-stream", buffer: Buffer.from("installer") });
  await expect(page.getByText("VLT-Setup.exe · 1 КиБ")).toBeVisible();
  await page.getByLabel("Изображение").setInputFiles({ name: "mixer.png", mimeType: "image/png", buffer: Buffer.from("png") });
  await page.getByLabel("Подпись RU").fill("Микшер");
  await page.getByLabel("Caption EN").fill("Mixer");
  await page.getByRole("button", { name: "Добавить" }).click();
  await expect(page.getByText("Скриншот добавлен.")).toBeVisible();
  await page.getByRole("button", { name: "Опубликовать" }).click();
  await expect(page.getByText("Версия 0.1.6 опубликована.")).toBeVisible();
  await expect(page.getByText("Опубликован", { exact: true })).toBeVisible();
  await expect.poll(() => page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.setViewportSize({ width: 800, height: 375 });
  await expect.poll(() => page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});
