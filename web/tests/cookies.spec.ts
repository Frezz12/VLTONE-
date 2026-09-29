import { expect, test } from "@playwright/test";

test("public pages work without cookies and a refusal persists", async ({ page }) => {
  const first = await page.goto("/");
  expect(first?.headers()["set-cookie"] ?? "").not.toContain("vlt-locale");
  await expect(page.getByRole("region", { name: "Выбор cookie" })).toBeVisible();
  await page.getByRole("button", { name: "Без cookie" }).click();
  await expect(page.getByRole("region", { name: "Выбор cookie" })).toHaveCount(0);
  expect(await page.context().cookies()).toEqual([]);

  await page.reload();
  await expect(page.getByRole("region", { name: "Выбор cookie" })).toHaveCount(0);
  await page.getByRole("button", { name: "Open in English" }).click();
  await expect(page).toHaveURL(/\?lang=en$/);
  await expect(page.getByRole("heading", { level: 1 })).toContainText("Record, arrange and mix");
  expect(await page.context().cookies()).toEqual([]);
  await page.goto("/privacy");
  await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
});

test("necessary-only choice enables language and can be withdrawn later", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("button", { name: "Только необходимые" }).click();
  await expect(page.getByRole("region", { name: "Выбор cookie" })).toHaveCount(0);
  await page.getByRole("button", { name: "Open in English" }).click();
  await expect(page).toHaveURL(/\/$/);
  await expect(page.getByRole("heading", { level: 1 })).toContainText("Record, arrange and mix");
  expect((await page.context().cookies()).find(cookie => cookie.name === "vlt-locale")?.value).toBe("en");

  await page.context().addCookies([{ name: "vlt_web_session", value: "old-session", httpOnly: true, url: "http://127.0.0.1:3100" }]);
  await page.getByRole("button", { name: "Cookie settings" }).click();
  const reloaded = page.waitForEvent("load");
  await page.getByRole("button", { name: "No cookies" }).click();
  await reloaded;
  await expect(page.getByRole("region", { name: "Выбор cookie" })).toHaveCount(0);
  expect((await page.context().cookies()).filter(cookie => ["vlt-locale", "vlt_web_session"].includes(cookie.name))).toEqual([]);
});

test("sign-in waits until necessary session cookies are allowed", async ({ page }) => {
  let submitted = false;
  await page.route("**/api/v1/web/auth/login", route => {
    submitted = true;
    return route.fulfill({ status: 401, json: { message: "invalid credentials" } });
  });
  await page.goto("/login");
  await page.getByLabel("Почта").fill("visitor@example.test");
  await page.getByLabel("Пароль", { exact: true }).fill("test-password");
  await page.getByRole("button", { name: "Войти", exact: true }).click();
  expect(submitted).toBe(false);
  await expect(page.getByRole("region", { name: "Выбор cookie" })).toContainText("cookie сессии");
  await page.getByRole("button", { name: "Только необходимые" }).click();
  await page.getByRole("button", { name: "Войти", exact: true }).click();
  await expect.poll(() => submitted).toBe(true);
});
