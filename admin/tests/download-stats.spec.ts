import { expect, test } from "@playwright/test";

test("download stats group versions, refresh without losing edits, and fit mobile", async ({ page }) => {
 let clicks = 12;
 await page.route("**/api/v1/admin/**", async route => {
  const path = new URL(route.request().url()).pathname;
  if (path.endsWith("/me")) return route.fulfill({ json: { admin: { is_owner: true, permissions: [], id: "owner", email: "owner@example.com", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2030-01-01T00:00:00Z" } });
  if (path.endsWith("/download-stats")) return route.fulfill({ json: { downloads: [
   { release_id: "a", version: "0.5.0", kind: "windows-exe", clicks, last_clicked_at: null },
   { release_id: "a", version: "0.5.0", kind: "macos-pkg", clicks: 3, last_clicked_at: null },
   { release_id: "b", version: "0.4.0", kind: "linux-deb", clicks: 0, last_clicked_at: null },
  ] } });
  return route.fulfill({ json: { releases: [] } });
 });
 await page.goto("/releases");
 const stats = page.getByRole("region", { name: "Статистика скачиваний" });
 await expect(stats.getByText("Всего нажатий:")).toContainText("15");
 await expect(stats.getByRole("row").filter({ hasText: "0.5.0" }).getByRole("cell").first()).toHaveText("15");
 await expect(stats.getByText("Windows EXE")).toBeVisible();
 await page.locator("#release-version").fill("1.0.0");
 clicks = 13;
 await stats.getByRole("button", { name: "Обновить статистику" }).click();
 await expect(stats.getByText("Всего нажатий:")).toContainText("16");
 await expect(page.locator("#release-version")).toHaveValue("1.0.0");
 await page.setViewportSize({ width: 375, height: 800 });
 await expect.poll(() => page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
 await page.screenshot({ path: "../artifacts/download-stats-20261011/admin-mobile.png", fullPage: true });
 await page.setViewportSize({ width: 1440, height: 1000 });
 await page.screenshot({ path: "../artifacts/download-stats-20261011/admin-desktop.png", fullPage: true });
});
