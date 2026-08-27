import { expect, test } from "@playwright/test";

const user = { id: "00000000-0000-4000-8000-000000000101", email: "tester@example.com", nickname: "Тестировщик", locale: "ru", status: "active", consent_version: "2026-08-23", consent_accepted_at: "2026-08-23T00:00:00Z", created_at: "2026-08-23T00:00:00Z" };
const account = { user, csrf_token: "csrf", subscription: { plan: "demo", display_name: "Demo", all_features: true }, quota: { base_limit: 20_000_000, adjustment: 0, used_tokens: 2_000_000, reserved_tokens: 0, remaining_tokens: 18_000_000, starts_at: "2026-08-01T00:00:00Z", ends_at: "2026-09-01T00:00:00Z" } };
const refreshedQuota = { ...account.quota, used_tokens: 4_000_000, remaining_tokens: 16_000_000 };

test("RU/EN pages and registration-to-account flow", async ({ page }) => {
  await page.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/meta")) return route.fulfill({ json: { consent_version: "2026-08-23" } });
    if (path.endsWith("/web/auth/register")) return route.fulfill({ status: 201, json: account });
    if (path.endsWith("/me/devices")) return route.fulfill({ json: { devices: [] } });
    if (path.endsWith("/me/quota")) return route.fulfill({ json: refreshedQuota });
    if (path.endsWith("/me")) return route.fulfill({ json: account });
    return route.fulfill({ status: 204 });
  });

  await page.goto("/en");
  await expect(page.getByRole("heading", { name: "Music under complete control." })).toBeVisible();
  await page.goto("/ru/register");
  await page.getByLabel("Почта").fill("tester@example.com");
  await page.getByLabel("Никнейм").fill("Тестировщик");
  await page.getByLabel("Пароль", { exact: true }).fill("correct horse battery staple");
  await page.getByLabel("Повторите пароль").fill("correct horse battery staple");
  await page.getByRole("checkbox").check();
  await page.getByRole("button", { name: "Зарегистрироваться" }).click();
  await expect(page).toHaveURL(/\/ru\/account$/);
  await expect(page.getByText("18 000 000")).toBeVisible();
  await page.evaluate(() => window.dispatchEvent(new Event("focus")));
  await expect(page.getByRole("progressbar")).toHaveAttribute("aria-valuenow", "20");
  await expect(page.getByText("Demo", { exact: true })).toBeVisible();
});

test("bug report rejects more than five attachments before upload", async ({ page }) => {
  let uploaded = false;
  await page.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/bug-reports")) { uploaded = true; return route.fulfill({ status: 201, json: { number: 1 } }); }
    if (path.endsWith("/me")) return route.fulfill({ json: account });
    return route.fulfill({ status: 204 });
  });
  await page.goto("/ru/bug-report");
  await page.getByLabel("Краткий заголовок").fill("Ошибка экспорта");
  await page.getByLabel("Описание").fill("Экспорт останавливается после запуска");
  await page.getByLabel(/До 5 изображений/).setInputFiles(Array.from({ length: 6 }, (_, index) => ({
    name: `shot-${index}.png`, mimeType: "image/png", buffer: Buffer.from("not-uploaded"),
  })));
  await page.getByRole("button", { name: "Отправить отчёт" }).click();
  await expect(page.locator(".vlt-error[role=alert]")).toContainText("не более пяти");
  expect(uploaded).toBe(false);
});
