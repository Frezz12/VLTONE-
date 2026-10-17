import { expect, test } from "@playwright/test";

test("Boosty stats show sources, handle errors, refresh, and fit mobile", async ({ page }) => {
  let clicks = 12;
  let failed = false;
  await page.route("**/api/v1/admin/**", route => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/me")) return route.fulfill({ json: { admin: { is_owner: true, permissions: [], id: "owner", email: "owner@example.com", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2030-01-01T00:00:00Z" } });
    if (path.endsWith("/support-stats")) return failed ? route.fulfill({ status: 503 }) : route.fulfill({ json: { clicks: [
      { source: "home", clicks, last_clicked_at: "2026-10-17T12:00:00Z" },
      { source: "footer", clicks: 3, last_clicked_at: null },
    ] } });
    return route.fulfill({ json: { users: 4, active_sessions: 1, crashes_24h: 0, open_bugs: 0, ai_tokens_month: 0, generated_at: "2026-10-17T12:00:00Z" } });
  });
  await page.goto("/");
  const stats = page.getByRole("region", { name: "Переходы на Boosty" });
  await expect(stats.getByText("Всего переходов:")).toContainText("15");
  await expect(stats.getByRole("row").filter({ hasText: "Блок на главной" }).getByRole("cell").first()).toHaveText("12");
  await expect(stats.getByRole("row").filter({ hasText: "Футер сайта" })).toContainText("Пока нет");
  failed = true;
  await stats.getByRole("button").click();
  await expect(stats.getByRole("alert")).toBeVisible();
  await expect(stats.getByText("Всего переходов:")).toContainText("15");
  failed = false; clicks = 13;
  await stats.getByRole("button").click();
  await expect(stats.getByRole("alert")).toHaveCount(0);
  await expect(stats.getByText("Всего переходов:")).toContainText("16");
  await page.setViewportSize({ width: 375, height: 812 });
  await stats.scrollIntoViewIfNeeded();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: "../artifacts/site-refresh-20261017/admin-support-mobile.png" });
  await page.setViewportSize({ width: 1440, height: 1000 });
  await stats.scrollIntoViewIfNeeded();
  await page.screenshot({ path: "../artifacts/site-refresh-20261017/admin-support-desktop.png" });
});
