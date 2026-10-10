import { expect, test } from "@playwright/test";

test("usage totals, launch history, search and pagination on desktop and mobile", async ({ page }) => {
  const user = { user_id: "10000000-0000-4000-8000-000000000001", nickname: "Тестировщик", sessions: 12, total_seconds: 9000, last_started_at: "2026-09-19T10:00:00Z" };
  await page.route("**/api/v1/admin/**", async (route) => {
    const url = new URL(route.request().url());
    if (url.pathname.endsWith("/me")) return route.fulfill({ json: { admin: { is_owner: true, permissions: [], id: "owner", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2099-01-01T00:00:00Z" } });
    if (url.pathname.endsWith("/dashboard")) {
      const searched = Boolean(url.searchParams.get("usage_q"));
      const pageIndex = Number(url.searchParams.get("usage_page"));
      return route.fulfill({ json: {
        users: 21, active_sessions: 1, crashes_24h: 0, open_bugs: 2, ai_tokens_month: 4200, generated_at: "2026-09-19T12:00:00Z", activity: [], ai_daily: [], online_users: [],
        usage: { summary: { total_seconds: 90000, total_sessions: 48, launches_24h: 7, users_24h: 3 }, user_count: searched ? 0 : 21, page: pageIndex, page_size: 20,
          users: searched ? [] : [{ ...user, nickname: pageIndex ? "Второй пользователь" : user.nickname }],
          recent_sessions: [
            { id: "live", ...user, app_version: "0.2.4", started_at: "2026-09-19T10:00:00Z", last_seen_at: "2026-09-19T11:59:00Z", seconds: 7140 },
            { id: "ended", ...user, app_version: "0.2.3", started_at: "2026-09-18T10:00:00Z", last_seen_at: "2026-09-18T11:00:00Z", ended_at: "2026-09-18T11:00:00Z", seconds: 3600 },
            { id: "stale", ...user, app_version: "0.2.3", started_at: "2026-09-17T10:00:00Z", last_seen_at: "2026-09-17T10:30:00Z", seconds: 1800 },
          ],
        },
      } });
    }
    return route.fulfill({ json: { crashes: [] } });
  });
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto("/");
  const usage = page.getByRole("region", { name: "Время в программе" });
  await expect(usage.getByText("2 ч 30 мин", { exact: true })).toBeVisible();
  await expect(page.getByText("В программе", { exact: true })).toBeVisible();
  await expect(page.getByText("Завершён", { exact: true })).toBeVisible();
  await expect(page.getByText("Нет свежих отчётов", { exact: true })).toBeVisible();
  await expect(page.locator(".admin-side .admin-nav-group")).toHaveCount(3);
  await expect(page.locator(".admin-side .admin-nav a.active")).toHaveAttribute("aria-current", "page");
  await page.screenshot({ path: "test-results/dashboard-usage-desktop.png", fullPage: true });
  await usage.getByRole("button", { name: "Далее" }).click();
  await expect(usage.getByRole("link", { name: "Второй пользователь" })).toBeVisible();
  await expect(usage.getByRole("button", { name: "Далее" })).toBeDisabled();
  await page.getByLabel("Пользователь", { exact: true }).fill("Никого");
  await usage.getByRole("button", { name: "Найти", exact: true }).click();
  await expect(usage.getByText("Пользователи не найдены.")).toBeVisible();
  await expect(usage.getByText(/страница 1 из 1/)).toBeVisible();
  await page.setViewportSize({ width: 375, height: 812 });
  await page.getByLabel("Пользователь", { exact: true }).fill("");
  await usage.getByRole("button", { name: "Найти", exact: true }).click();
  await expect(usage.getByText("2 ч 30 мин", { exact: true })).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.evaluate(() => window.scrollTo({ top: 0, behavior: "instant" }));
  await expect.poll(() => page.evaluate(() => window.scrollY)).toBe(0);
  await page.screenshot({ path: "test-results/dashboard-usage-mobile.png", fullPage: true });
});
