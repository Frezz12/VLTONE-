import { expect, test } from "@playwright/test";

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("vlt-cookie-preference-v1", "necessary"));
});

const user = { id: "00000000-0000-4000-8000-000000000101", email: "tester@example.com", nickname: "Тестировщик", locale: "ru", status: "active", consent_version: "2026-08-23", consent_accepted_at: "2026-08-23T00:00:00Z", created_at: "2026-08-23T00:00:00Z" };
const account = { user, csrf_token: "csrf", subscription: { plan: "demo", display_name: "Demo", all_features: true }, quota: { base_limit: 20_000_000, adjustment: 0, used_tokens: 2_000_000, reserved_tokens: 0, remaining_tokens: 18_000_000, starts_at: "2026-08-01T00:00:00Z", ends_at: "2026-09-01T00:00:00Z" } };
const refreshedQuota = { ...account.quota, used_tokens: 4_000_000, remaining_tokens: 16_000_000 };

test("account can withdraw diagnostics without closing the account", async ({ page }) => {
  let withdrawn = false;
  const optedIn = { ...user, diagnostics_consent_version: "2026-09-26", diagnostics_accepted_at: "2026-09-26T00:00:00Z" };
  await page.route("**/api/v1/**", route => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/me/diagnostics-consent")) {
      expect(route.request().method()).toBe("PUT");
      expect(route.request().postDataJSON()).toMatchObject({ accepted: false });
      expect(route.request().headers()["x-csrf-token"]).toBe("csrf");
      withdrawn = true;
      return route.fulfill({ json: { enabled: false } });
    }
    if (path.endsWith("/me/devices")) return route.fulfill({ json: { devices: [] } });
    if (path.endsWith("/me/quota")) return route.fulfill({ json: refreshedQuota });
    if (path.endsWith("/me")) return route.fulfill({ json: { ...account, user: optedIn } });
    return route.fulfill({ status: 204 });
  });
  await page.goto("/account");
  await page.getByRole("button", { name: "Отозвать согласие и отключить" }).click();
  await expect(page.locator(".privacy-account")).toContainText("Выключена");
  await expect(page.getByRole("status")).toHaveText("Выбор сохранён.");
  await expect(page).toHaveURL(/\/account$/);
  expect(withdrawn).toBe(true);
});

test("RU/EN pages and registration-to-account flow", async ({ page }) => {
  await page.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/meta")) return route.fulfill({ json: { registration_enabled: true, registration_captcha: { provider: "turnstile", required: false, configured: false, site_key: "" }, registration_legal: { version: "2026-09-26", ready: false } } });
    if (path.endsWith("/web/auth/register")) {
      expect(route.request().postDataJSON()).toMatchObject({ consent_accepted: true, consent_version: "2026-09-26", terms_accepted: true, terms_version: "2026-09-26", diagnostics_accepted: false });
      return route.fulfill({ status: 201, json: account });
    }
    if (path.endsWith("/me/devices")) return route.fulfill({ json: { devices: [] } });
    if (path.endsWith("/me/quota")) return route.fulfill({ json: refreshedQuota });
    if (path.endsWith("/me")) return route.fulfill({ json: account });
    return route.fulfill({ status: 204 });
  });

  await page.goto("/en");
  await expect(page.getByRole("heading", { name: /Give shape to your sound\./ })).toBeVisible();
  await page.goto("/ru/register");
  await expect(page).toHaveURL(/\/register\?lang=ru$/);
  await page.getByLabel("Почта").fill("tester@example.com");
  await page.getByLabel("Никнейм").fill("Тестировщик");
  const password = page.getByLabel("Пароль", { exact: true });
  const confirmation = page.getByLabel("Повторите пароль");
  await expect(password).toHaveAttribute("minlength", "8");
  await password.fill("passw0rd");
  await confirmation.fill("passw0rd");
  const visibilityButtons = page.getByRole("button", { name: "Показать пароль" });
  await visibilityButtons.first().click();
  await expect(password).toHaveAttribute("type", "text");
  await expect(confirmation).toHaveAttribute("type", "password");
  await page.getByRole("button", { name: "Скрыть пароль" }).click();
  await expect(password).toHaveAttribute("type", "password");
  await expect(page.getByRole("checkbox")).toHaveCount(2);
  for (const checkbox of await page.getByRole("checkbox").all()) await expect(checkbox).not.toBeChecked();
  await page.getByRole("checkbox", { name: /Принимаю пользовательское/ }).check();
  await page.getByRole("checkbox", { name: /Даю отдельное/ }).check();
  await page.getByRole("button", { name: "Зарегистрироваться" }).click();
  await expect(page).toHaveURL(/\/account$/);
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
  await page.context().addCookies([{ name: "vlt-locale", value: "ru", url: "http://127.0.0.1:3100" }]);
  await page.goto("/bug-report");
  await expect(page.getByLabel("Шаги воспроизведения")).toHaveCount(0);
  await expect(page.getByLabel("Ожидаемое поведение")).toHaveCount(0);
  await expect(page.getByLabel("Фактическое поведение")).toHaveCount(0);
  await page.getByLabel("Краткий заголовок").fill("Ошибка экспорта");
  await page.getByLabel("Описание").fill("Экспорт останавливается после запуска");
  await page.getByLabel(/До 5 изображений/).setInputFiles(Array.from({ length: 6 }, (_, index) => ({
    name: `shot-${index}.png`, mimeType: "image/png", buffer: Buffer.from("not-uploaded"),
  })));
  await page.getByRole("button", { name: "Отправить отчёт" }).click();
  await expect(page.locator(".vlt-error[role=alert]")).toContainText("не более пяти");
  expect(uploaded).toBe(false);
});
