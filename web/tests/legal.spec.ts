import { expect, test } from "@playwright/test";

for (const locale of ["ru", "en"]) {
  for (const documentName of ["terms", "privacy", "consent", "diagnostics-consent"]) {
    test(`${locale} ${documentName} is readable without an account`, async ({ page }) => {
      await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
      await page.setViewportSize({ width: 375, height: 850 });
      await page.goto(`/${documentName}?version=2026-09-26`);
      await expect(page.locator("h1")).toBeVisible();
      await expect(page.locator(".legal-operator")).toContainText("Климов Григорий Владимирович");
      await expect(page.locator(".legal-operator")).toContainText("vltmscw@outlook.com");
      await expect(page.locator(".legal-draft")).toBeVisible();
      await expect(page.locator('meta[name="robots"]')).toHaveAttribute("content", "noindex, follow");
      expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
      expect(await page.locator(".legal-body section[id]").count()).toBeGreaterThan(2);
    });
  }
}

for (const state of ["unavailable", "outdated", "missing", "disabled"]) {
  test(`registration fails closed when registration metadata is ${state}`, async ({ page }) => {
    let submitted = false;
    await page.route("**/api/v1/**", route => {
      if (route.request().url().endsWith("/meta")) {
        if (state === "unavailable") return route.fulfill({ status: 503, json: { message: "unavailable" } });
        if (state === "missing") return route.fulfill({ json: { consent_version: "2026-08-23" } });
        return route.fulfill({ json: { registration_enabled: state !== "disabled", registration_legal: { ready: false, version: state === "outdated" ? "old" : "2026-09-26" } } });
      }
      submitted = true;
      return route.fulfill({ status: 500 });
    });
    await page.goto("/register");
    await expect(page.getByRole("button", { name: "Зарегистрироваться", exact: true })).toBeDisabled();
    await expect(page.getByRole("status")).toContainText("Не удалось проверить условия регистрации");
    expect(submitted).toBe(false);
    for (const route of ["terms", "consent"]) {
      const link = page.locator(`.auth-legal a[href="/${route}?version=2026-09-26"]`);
      await expect(link).toHaveAttribute("target", "_blank");
    }
  });
}

test("registration works with an incomplete legal profile but still requires separate decisions", async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("vlt-cookie-preference-v1", "necessary"));
  let submitted = false;
  await page.route("**/api/v1/**", route => {
    if (route.request().url().endsWith("/meta")) return route.fulfill({ json: { registration_enabled: true, registration_captcha: { provider: "turnstile", required: false, configured: false, site_key: "" }, registration_legal: { ready: false, version: "2026-09-26" } } });
    submitted = true;
    expect(route.request().postDataJSON()).toMatchObject({ consent_accepted: true, terms_accepted: true, diagnostics_accepted: false });
    return route.fulfill({ status: 422, json: { message: "test request" } });
  });
  await page.goto("/register");
  await expect(page.getByRole("button", {name:"Зарегистрироваться",exact:true})).toBeEnabled();
  await page.getByLabel("Почта").fill("fixture@example.test");
  await page.getByLabel("Никнейм").fill("Fixture");
  await page.getByLabel("Пароль", {exact: true}).fill("local-test-password");
  await page.getByLabel("Повторите пароль").fill("local-test-password");
  await page.getByRole("button", {name:"Зарегистрироваться",exact:true}).click();
  expect(submitted).toBe(false);
  await page.getByRole("checkbox", {name:/Принимаю пользовательское/}).check();
  await page.getByRole("button", {name:"Зарегистрироваться",exact:true}).click();
  expect(submitted).toBe(false);
  await page.getByRole("checkbox", {name:/Даю отдельное согласие/}).check();
  await page.getByRole("button", {name:"Зарегистрироваться",exact:true}).click();
  expect(submitted).toBe(true);
  await expect(page.getByRole("checkbox",{name:/Необязательно/})).toHaveCount(0);
});
