import { expect, test } from "@playwright/test";

test("visits show today, identities, empty periods and recover from an error", async ({ page }) => {
  let fail = false;
  await page.route("**/api/v1/admin/**", async route => {
    const url = new URL(route.request().url());
    if (url.pathname.endsWith("/me")) return route.fulfill({ json: { admin: { id: "admin", is_owner: false, permissions: ["analytics.read"], nickname: "Owner" } } });
    if (url.pathname.endsWith("/analytics")) {
      if (fail) return route.fulfill({ status: 500, json: { message: "Unavailable" } });
      const empty = url.searchParams.get("days") === "30";
      return route.fulfill({ json: { today: { views: 12, visitors: 5, accounts: 1 }, daily: [{ day: "2026-10-18", views: empty ? 0 : 12, visitors: 5, accounts: 1 }], pages: empty ? [] : [{ path: "/", views: 12 }], visits: empty ? [] : [{ id: "visit", visited_at: "2026-10-18T10:00:00Z", visitor_key: "abcdef12", user_id: "user", nickname: "Тестовый пользователь", path: "/", referrer_host: "search.example", browser: "Firefox", os: "Windows" }], has_more: false, generated_at: "2026-10-18T10:00:00Z" } });
    }
    return route.fulfill({ json: {} });
  });
  await page.goto("/analytics");
  await expect(page.getByRole("heading", { name: "Посещения сайта" })).toBeVisible();
  await expect(page.getByText("Тестовый пользователь", { exact: true })).toBeVisible();
  await expect(page.getByRole("link", { name: "Тестовый пользователь" })).toHaveCount(0);
  await expect(page.getByText("18.10, 13:00:00", { exact: true })).toBeVisible();
  await expect(page.getByRole("button", { name: "Далее", exact: true })).toBeDisabled();
  await page.screenshot({ path: "../artifacts/site-visits-20261018/admin-desktop.png", fullPage: true });
  await page.setViewportSize({ width: 390, height: 844 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: "../artifacts/site-visits-20261018/admin-mobile.png", fullPage: true });
  await page.getByLabel("Период").selectOption("30");
  await expect(page.getByText("Посещений пока нет.", { exact: true })).toBeVisible();
  fail = true;
  await page.getByRole("button", { name: "Обновить", exact: true }).click();
  await expect(page.getByRole("alert").filter({ hasText: "Не удалось загрузить посещения" })).toBeVisible();
  fail = false;
  await page.getByRole("button", { name: "Обновить", exact: true }).click();
  await expect(page.getByText("Посещений пока нет.", { exact: true })).toBeVisible();
  await page.setViewportSize({ width: 390, height: 844 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});
