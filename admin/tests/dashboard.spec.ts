import { expect, test } from "@playwright/test";

test("administrator signs in and sees operational totals", async ({ page }) => {
  await page.route("**/api/v1/admin/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/auth/login")) return route.fulfill({ json: { ok: true } });
    if (path.endsWith("/me")) return route.fulfill({ json: { admin: { id: "1", email: "owner@example.com", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2026-08-23T20:00:00Z" } });
    if (path.endsWith("/dashboard")) return route.fulfill({ json: { users: 42, active_sessions: 7, crashes_24h: 1, open_bugs: 3, ai_tokens_month: 1_250_000, generated_at: "2026-08-23T12:00:00Z", activity: [{ bucket: "2026-08-23T11:00:00Z", sessions: 4, crashes: 1 }], ai_daily: [{ bucket: "2026-08-23T00:00:00Z", tokens: 1_250_000 }] } });
    return route.fulfill({ status: 204 });
  });
  await page.goto("/login");
  await page.getByLabel("Email").fill("owner@example.com");
  await page.getByLabel("Пароль").fill("correct horse battery staple");
  await page.getByRole("button", { name: "Войти" }).click();
  await expect(page).toHaveURL(/\/$/);
  await expect(page.getByRole("heading", { name: "Оперативный обзор" })).toBeVisible();
  await expect(page.getByText("42", { exact: true })).toBeVisible();
  await expect(page.getByText("1 250 000", { exact: true })).toBeVisible();
  await expect(page.getByRole("region", { name: "График запусков и крашей за 24 часа" })).toBeVisible();
});

test("administrator searches a user and performs protected account actions", async ({ page }) => {
  await page.setViewportSize({ width: 375, height: 812 });
  const id = "00000000-0000-4000-8000-000000000101";
  const user = { id, email: "tester@example.com", nickname: "Тестировщик", locale: "ru", status: "active", collaboration_enabled: false, consent_version: "2026-08-23", created_at: "2026-08-23T00:00:00Z" };
  const quota = { base_limit: 20_000_000, adjustment: 0, used_tokens: 100, reserved_tokens: 0, remaining_tokens: 19_999_900, starts_at: "2026-08-01T00:00:00Z", ends_at: "2026-09-01T00:00:00Z" };
  const actions: string[] = [];
  const accessWrites: Array<{ enabled: boolean; csrf?: string }> = [];
  let collaborationEnabled = false;
  let releaseFirstAccess!: () => void;
  const firstAccessGate = new Promise<void>((resolve) => { releaseFirstAccess = resolve; });
  let search = "";
  await page.route("**/api/v1/admin/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const path = url.pathname;
    if (path.endsWith("/admin/me")) return route.fulfill({ json: { admin: { id: "owner", email: "owner@example.com", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2026-08-23T20:00:00Z" } });
    if (path.endsWith(`/users/${id}/telemetry`)) return route.fulfill({ json: { sessions: [], samples: [] } });
    if (path.endsWith(`/users/${id}/ledger`)) return route.fulfill({ json: { entries: [] } });
    if (path.endsWith(`/users/${id}`) && request.method() === "GET") return route.fulfill({ json: { user: { ...user, collaboration_enabled: collaborationEnabled }, devices: [], quota, subscription: { plan: { display_name: "Demo" } }, counts: { launches: 1, crashes: 0, bugs: 0 } } });
    if (path.endsWith(`/users/${id}/collaboration-access`) && request.method() === "PUT") {
      const body = request.postDataJSON() as { enabled: boolean };
      accessWrites.push({ enabled: body.enabled, csrf: request.headers()["x-csrf-token"] });
      if (accessWrites.length === 1) await firstAccessGate;
      if (accessWrites.length === 2) return route.fulfill({ status: 503, json: { code: "access_update_failed", message: "Онлайн-доступ не изменён." } });
      collaborationEnabled = body.enabled;
      return route.fulfill({ json: { collaboration_enabled: collaborationEnabled } });
    }
    if (path.endsWith(`/users/${id}`) && request.method() === "DELETE") { actions.push("delete"); return route.fulfill({ status: 204 }); }
    if (path.includes(`/users/${id}/`) && request.method() === "POST") { actions.push(path.split(`/users/${id}`)[1]); return route.fulfill({ status: path.endsWith("/tokens/add") || path.endsWith("/tokens/reset") ? 200 : 204, json: quota }); }
    if (path.endsWith("/admin/users")) { search = url.searchParams.get("q") ?? search; return route.fulfill({ json: { users: [{ ...user, collaboration_enabled: collaborationEnabled }] } }); }
    return route.fulfill({ status: 204 });
  });

  await page.goto("/users");
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  await page.getByLabel("Поиск пользователей").fill("tester@example.com");
  await page.getByRole("button", { name: "Искать" }).click();
  await expect.poll(() => search).toBe("tester@example.com");
  const userRow = page.getByRole("row", { name: /Тестировщик/ });
  const registryAccess = userRow.getByRole("switch", { name: "Онлайн-доступ для Тестировщик" });
  await expect(registryAccess).toHaveAttribute("aria-checked", "false");
  await registryAccess.click();
  await expect(registryAccess).toHaveAttribute("aria-checked", "true");
  await expect(registryAccess).toHaveAttribute("aria-busy", "true");
  await expect(registryAccess).toBeDisabled();
  await expect(userRow.getByText("Сохраняем…")).toBeVisible();
  const switchBox = await registryAccess.boundingBox();
  expect(switchBox?.width).toBeGreaterThanOrEqual(44);
  expect(switchBox?.height).toBeGreaterThanOrEqual(44);
  await expect.poll(() => accessWrites).toEqual([{ enabled: true, csrf: "csrf" }]);
  releaseFirstAccess();
  await expect(registryAccess).toBeEnabled();
  await expect(registryAccess).toHaveAttribute("aria-busy", "false");
  await expect(page.getByRole("status")).toHaveText("Тестировщик: онлайн-доступ включён.");
  await page.getByRole("link", { name: "Открыть" }).click();
  await expect(page.getByRole("heading", { name: "Тестировщик" })).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  await expect(page.getByText("Выключение отключит активные онлайн-сессии пользователя.", { exact: false })).toBeVisible();
  const detailAccess = page.getByRole("switch", { name: "Онлайн-доступ для Тестировщик" });
  await expect(detailAccess).toHaveAttribute("aria-checked", "true");
  await detailAccess.click();
  await expect(page.locator(".collaboration-access-setting").getByRole("alert")).toHaveText("Онлайн-доступ не изменён.");
  await expect(detailAccess).toHaveAttribute("aria-checked", "true");
  await expect(detailAccess).toBeEnabled();
  await detailAccess.click();
  await expect(detailAccess).toHaveAttribute("aria-checked", "false");
  await expect(page.locator(".collaboration-access-setting").getByRole("status")).toHaveText("Онлайн-доступ выключен.");
  expect(accessWrites.at(-1)).toEqual({ enabled: false, csrf: "csrf" });

  await page.getByRole("button", { name: "Приостановить" }).click();
  await expect.poll(() => actions).toContain("/suspend");
  await page.getByPlaceholder("Добавить токены").fill("5000");
  await page.getByRole("button", { name: "Добавить токены" }).click();
  await expect.poll(() => actions).toContain("/tokens/add");
  await page.getByPlaceholder("Пароль администратора").first().fill("correct horse battery staple");
  await page.getByRole("button", { name: "Сбросить расход" }).click();
  await expect.poll(() => actions).toContain("/tokens/reset");

  await page.getByPlaceholder("Введите Тестировщик").fill("Тестировщик");
  await page.getByPlaceholder("Пароль администратора").last().fill("correct horse battery staple");
  await page.getByRole("button", { name: "Удалить навсегда" }).click();
  await expect.poll(() => actions).toContain("delete");
  await expect(page).toHaveURL(/\/users$/);
});

test("administrator downloads readable crash logs", async ({ page }) => {
  const crashID = "00000000-0000-4000-8000-000000000301";
  await page.route("**/api/v1/admin/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/admin/me")) return route.fulfill({ json: { admin: { id: "owner", email: "owner@example.com", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2026-08-23T20:00:00Z" } });
    if (path.endsWith(`/crashes/${crashID}/artifact`)) return route.fulfill({ body: "[crash marker]\nexception=access_violation\n", headers: { "Content-Type": "text/plain; charset=utf-8", "Content-Disposition": `attachment; filename="vlt-crash-${crashID}.log"` } });
    if (path.endsWith("/admin/crashes")) return route.fulfill({ json: { crashes: [{ id: crashID, user_id: "user", device_id: "device", build_id: "build-1", app_version: "0.0.1", platform: "windows", reason: "access_violation", artifact_bytes: 4096, occurred_at: "2026-08-23T12:00:00Z" }] } });
    return route.fulfill({ status: 204 });
  });
  await page.goto("/crashes");
  const [download] = await Promise.all([
    page.waitForEvent("download"),
    page.getByRole("link", { name: "Скачать логи" }).click(),
  ]);
  expect(download.suggestedFilename()).toBe(`vlt-crash-${crashID}.log`);
});
