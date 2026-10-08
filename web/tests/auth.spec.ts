import { expect, test } from "@playwright/test";

const meta = { registration_enabled: true, registration_legal: { version: "2026-09-26", ready: false }, registration_captcha: { provider: "turnstile", required: true, configured: true, site_key: "public-test-key" } };
// Only this intercepted provider script is simulated. Product code always uses
// the real widget and the backend always verifies tokens with Siteverify.
const widgetScript = `window.__captchaOptions=[];window.turnstile={ready:fn=>fn(),render:(element,options)=>{const id='widget-'+window.__captchaOptions.length;window.__captchaOptions.push(options);const button=document.createElement('button');button.type='button';button.textContent='Пройти тестовую проверку';button.onclick=()=>options.callback('test-token-'+id);element.appendChild(button);window.__captchaElement=element;return id;},remove:()=>{window.__captchaElement?.replaceChildren();}};`;
test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("vlt-cookie-preference-v1", "necessary"));
});

for (const locale of ["ru", "en"]) for (const width of [320, 1440]) test(`${locale} auth screen fits ${width}px and waves can pause`, async ({ page }) => {
  await page.setViewportSize({ width, height: 1000 });
  await page.goto(`/login?lang=${locale}`);
  await expect(page.getByRole("heading", { name: locale === "ru" ? "Вход в аккаунт" : "Sign in" })).toBeVisible();
  await expect(page.locator("header.vlt-topbar")).not.toBeVisible();
  await expect(page.locator(".site-footer")).not.toBeVisible();
  await expect(page.locator(".auth-story")).toBeVisible();
  await expect(page.locator(".auth-spectrum span").first()).toHaveCSS("animation-play-state", "running");
  const pause = page.getByRole("button", { name: locale === "ru" ? "Остановить анимацию" : "Pause animation" });
  await pause.click();
  await expect(page.locator(".auth-spectrum span").first()).toHaveCSS("animation-play-state", "paused");
  await page.getByRole("button", { name: locale === "ru" ? "Продолжить анимацию" : "Resume animation" }).click();
  await expect(page.locator(".auth-spectrum span").first()).toHaveCSS("animation-play-state", "running");
  await page.emulateMedia({ reducedMotion: "reduce" });
  await expect(page.locator(".auth-spectrum span").first()).toHaveCSS("animation-name", "none");
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});

test("registration requires a fresh CAPTCHA after expiry and a rejected submit", async ({ page }) => {
  let requests = 0;
  await page.route("**/turnstile/v0/api.js*", route => route.fulfill({ contentType: "application/javascript", body: widgetScript }));
  await page.route("**/api/v1/**", route => {
    if (route.request().url().endsWith("/meta")) return route.fulfill({ json: meta });
    if (route.request().url().endsWith("/web/auth/register")) {
      requests++;
      expect(route.request().postDataJSON()).toMatchObject({ captcha_token: expect.stringMatching(/^test-token-widget-/), consent_accepted: true, terms_accepted: true });
      expect(route.request().postDataJSON()).not.toHaveProperty("cf-turnstile-response");
      return route.fulfill({ status: 422, json: { code: "captcha_invalid", message: "token already used" } });
    }
    return route.fulfill({ status: 204 });
  });
  await page.setViewportSize({ width: 320, height: 900 });
  await page.goto("/register?lang=ru");
  const submit = page.getByRole("button", { name: "Зарегистрироваться", exact: true });
  await expect(submit).toBeDisabled();
  await page.getByLabel("Почта").fill("fixture@example.test");
  await page.getByLabel("Никнейм").fill("Fixture");
  await page.getByLabel("Пароль", { exact: true }).fill("test-password");
  await page.getByLabel("Повторите пароль").fill("test-password");
  await page.getByRole("checkbox", { name: /Принимаю пользовательское/ }).check();
  await page.getByRole("checkbox", { name: /Даю отдельное/ }).check();
  await page.getByRole("button", { name: "Пройти тестовую проверку" }).click();
  await expect(submit).toBeEnabled();
  await page.evaluate("window.__captchaOptions.at(-1)['expired-callback']()");
  await expect(submit).toBeDisabled();
  expect(requests).toBe(0);
  await page.getByRole("button", { name: "Повторить проверку", exact: true }).click();
  await page.getByRole("button", { name: "Пройти тестовую проверку" }).click();
  await submit.click();
  await expect(page.locator(".vlt-error[role='alert']")).toContainText("Проверка истекла");
  await expect(submit).toBeDisabled();
  await expect.poll(() => requests).toBe(1);
  await expect(page.getByLabel("Никнейм")).toHaveValue("Fixture");
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});

test("missing CAPTCHA setup disables signup and a failed script can retry without losing fields", async ({ page }) => {
  let configured = false, scripts = 0;
  await page.route("**/api/v1/meta", route => route.fulfill({ json: { ...meta, registration_captcha: { ...meta.registration_captcha, configured } } }));
  await page.route("**/turnstile/v0/api.js*", route => { scripts++; return scripts === 1 ? route.abort() : route.fulfill({ contentType: "application/javascript", body: widgetScript }); });
  await page.goto("/register?lang=ru");
  const submit = page.getByRole("button", { name: "Зарегистрироваться", exact: true });
  await expect(submit).toBeDisabled();
  await expect(page.getByRole("status")).toContainText("Проверка безопасности временно недоступна");
  await page.getByLabel("Почта").fill("retained@example.test");
  configured = true;
  await page.getByRole("button", { name: "Повторить проверку" }).click();
  await expect(page.getByRole("status")).toContainText("Не удалось загрузить");
  await page.getByRole("button", { name: "Повторить проверку" }).click();
  await page.getByRole("button", { name: "Пройти тестовую проверку" }).click();
  await expect(submit).toBeEnabled();
  await expect(page.getByLabel("Почта")).toHaveValue("retained@example.test");
});
