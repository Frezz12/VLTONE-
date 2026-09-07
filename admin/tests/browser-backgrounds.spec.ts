import { expect, test } from "@playwright/test";

const id = "00000000-0000-4000-8000-000000000123";
const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=", "base64");
const initial = { id, title: "Горы " + "длинноеназвание".repeat(10), sort_order: 0, published: true, width: 3840, height: 2160, bytes: 1200000, thumbnail_url: `/v1/admin/browser-backgrounds/${id}/thumbnail`, url: `/v1/admin/browser-backgrounds/${id}/image` };

for (const width of [1440, 375]) test(`background collection upload, hide, edit and delete at ${width}px`, async ({ page }) => {
  await page.setViewportSize({ width, height: 1000 });
  let items: typeof initial[] = [], uploaded = false;
  await page.route("**/api/v1/admin/**", async (route) => {
    const request = route.request(), path = new URL(request.url()).pathname;
    if (path.endsWith("/admin/me")) return route.fulfill({ json: { admin: { id: "owner", email: "owner@example.com", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2099-01-01T00:00:00Z" } });
    if (path.endsWith("/thumbnail") || path.endsWith("/image")) return route.fulfill({ contentType: "image/png", body: png });
    if (request.method() === "PUT") {
      expect(request.headers()["x-csrf-token"]).toBe("csrf");
      items = [{ ...items[0], ...request.postDataJSON() }]; return route.fulfill({ json: items[0] });
    }
    if (request.method() === "DELETE") { expect(request.headers()["x-csrf-token"]).toBe("csrf"); items = []; return route.fulfill({ status: 204 }); }
    return route.fulfill({ json: { backgrounds: items } });
  });
  await page.route("**/release-upload/v1/admin/browser-backgrounds", async (route) => {
    expect(route.request().headers()["x-csrf-token"]).toBe("csrf");
    expect(route.request().headers()["content-type"]).toContain("multipart/form-data");
    expect(route.request().postDataBuffer()!.toString()).toContain("wallpaper.png");
    uploaded = true; items = [initial]; return route.fulfill({ status: 201, json: initial });
  });
  await page.goto("/browser-backgrounds");
  await expect(page.getByText("Фонов пока нет.", { exact: false })).toBeVisible();
  await page.getByLabel("Название фона", { exact: true }).fill("Горы");
  await page.getByLabel("Файл изображения").setInputFiles({ name: "wallpaper.png", mimeType: "image/png", buffer: png });
  await expect(page.getByAltText("Предпросмотр нового фона")).toBeVisible();
  await page.getByRole("button", { name: "Добавить в коллекцию" }).click();
  await expect(page.locator(".background-card")).toHaveCount(1);
  expect(uploaded).toBe(true);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.locator(".background-card").getByLabel("Предлагать в настройках браузера").uncheck();
  await page.getByRole("button", { name: "Сохранить", exact: true }).click();
  await expect(page.getByText("Скрыт", { exact: true })).toBeVisible();
  // Refresh metadata edited by another admin: the controls must match the new row.
  items = [{ ...items[0], title: "Новое название", sort_order: 7 }];
  await page.getByRole("button", { name: "Обновить", exact: true }).click();
  await expect(page.locator(".background-card").getByLabel("Название", { exact: true })).toHaveValue("Новое название");
  await expect(page.getByLabel("Порядок в коллекции")).toHaveValue("7");
  await page.screenshot({ path: `test-results/backgrounds-${width}.png`, fullPage: true });
  page.once("dialog", (dialog) => dialog.accept());
  await page.getByRole("button", { name: "Удалить", exact: true }).click();
  await expect(page.locator(".background-card")).toHaveCount(0);
});
