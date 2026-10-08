import { expect, test } from "@playwright/test";
import { readFile } from "node:fs/promises";

const session = { admin: { id: "owner", email: "owner@example.com", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2099-01-01T00:00:00Z" };

test("quick navigation supports keyboard user search and the mobile menu", async ({ page }) => {
  let search = "";
  await page.route("**/api/v1/admin/**", route => {
    const url = new URL(route.request().url());
    if (url.pathname.endsWith("/me")) return route.fulfill({ json: session });
    if (url.pathname.endsWith("/users")) { search = url.searchParams.get("q") ?? ""; return route.fulfill({ json: { users: [] } }); }
    return route.fulfill({ json: { crashes: [], bugs: [] } });
  });
  await page.goto("/users");
  await expect(page.getByRole("button", { name: "Искать", exact: true })).toBeEnabled();
  await page.keyboard.press("Control+k");
  const menu = page.getByRole("dialog", { name: "Быстрый переход" });
  await expect(menu).toBeVisible();
  await expect(menu.getByRole("combobox")).toBeFocused();
  await menu.getByRole("combobox").fill("tester@example.com");
  await page.keyboard.press("Enter");
  await expect(page).toHaveURL(/users\?q=tester%40example.com$/);
  await expect.poll(() => search).toBe("tester@example.com");
  await expect(page.getByLabel("Поиск пользователей")).toHaveValue("tester@example.com");
  await page.setViewportSize({ width: 375, height: 812 });
  await page.getByRole("button", { name: "Открыть навигацию" }).click();
  const nav = page.getByRole("dialog", { name: "Навигация админки" });
  await expect(nav).toBeVisible();
  await nav.getByRole("link", { name: "Баги", exact: true }).click();
  await expect(page).toHaveURL(/\/bugs$/);
  await expect(nav).not.toBeVisible();
  await expect(page.getByRole("heading", { name: "Баг-репорты" })).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});

test("diagnostic filtering and CSV retain unsaved notes and escape spreadsheet formulas", async ({ page }) => {
  let bugs = [
    { id: "1", number: 17, title: "=SUM(1;1)", description: "Сбой микшера", status: "new", internal_note: "", created_at: new Date().toISOString() },
    { id: "2", number: 16, title: "Piano Roll", description: "Редактирование нот", status: "fixed", internal_note: "Исправлено", created_at: new Date().toISOString() },
  ];
  await page.route("**/api/v1/admin/**", route => {
    const url = new URL(route.request().url());
    if (url.pathname.endsWith("/me")) return route.fulfill({ json: session });
    if (route.request().method() === "PATCH") { expect(route.request().headers()["x-csrf-token"]).toBe("csrf"); bugs = bugs.map(item => item.id === "1" ? { ...item, ...route.request().postDataJSON() } : item); return route.fulfill({ json: bugs[0] }); }
    return route.fulfill({ json: { bugs, crashes: [] } });
  });
  await page.goto("/bugs?status=open");
  await expect(page.getByLabel("Статус", { exact: true })).toHaveValue("open");
  await expect(page.getByRole("row", { name: /Piano Roll/ })).toHaveCount(0);
  await page.getByLabel("Внутренняя заметка").fill("Проверить запись");
  await page.getByRole("button", { name: "Обновить", exact: true }).click();
  await expect(page.getByLabel("Внутренняя заметка")).toHaveValue("Проверить запись");
  await page.getByLabel("Поиск в списке").fill("микшера");
  const downloaded = page.waitForEvent("download");
  await page.getByRole("button", { name: "Выгрузить CSV" }).click();
  const file = await downloaded;
  const text = await readFile((await file.path())!, "utf8");
  expect(text).toContain("'=SUM(1;1)");
  expect(text).not.toContain("Piano Roll");
  // CSV exports saved records; editing one row never loses another row's work.
  await page.getByRole("button", { name: "Сохранить", exact: true }).click();
  await expect.poll(() => bugs[0].internal_note).toBe("Проверить запись");
});

test("paused polling stops background requests while manual refresh remains available", async ({ page }) => {
  let dashboardCalls = 0;
  await page.clock.install();
  await page.route("**/api/v1/admin/**", route => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/me")) return route.fulfill({ json: session });
    if (path.endsWith("/dashboard")) { dashboardCalls++; return route.fulfill({ json: { users: 12, active_sessions: 3, crashes_24h: 0, open_bugs: 0, ai_tokens_month: 123, generated_at: new Date().toISOString() } }); }
    return route.fulfill({ json: { crashes: [] } });
  });
  await page.goto("/");
  await expect.poll(() => dashboardCalls).toBe(1);
  await page.getByRole("button", { name: "Приостановить автообновление" }).click();
  await page.clock.fastForward(45000);
  expect(dashboardCalls).toBe(1);
  await page.getByRole("button", { name: "Обновить", exact: true }).click();
  await expect.poll(() => dashboardCalls).toBe(2);
  await page.getByRole("button", { name: "Продолжить автообновление" }).click();
  await page.clock.fastForward(15000);
  await expect.poll(() => dashboardCalls).toBe(3);
});

test("unsaved release content can be recovered after a reload without storing credentials", async ({ page }) => {
  await page.route("**/api/v1/admin/**", route => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/me")) return route.fulfill({ json: session });
    if (path.endsWith("/releases")) return route.fulfill({ json: { releases: [] } });
    return route.fulfill({ json: { crashes: [] } });
  });
  await page.goto("/releases");
  await page.locator("#release-version").fill("0.3.2");
  await page.getByLabel("Кратко — русский").fill("Новый проект и улучшенный микшер");
  await expect.poll(() => page.evaluate(() => localStorage.getItem("vlt-admin-release-draft:owner"))).toContain("Новый проект");
  const local = await page.evaluate(() => localStorage.getItem("vlt-admin-release-draft:owner"));
  expect(local).not.toContain("csrf"); expect(local).not.toContain("owner@example.com");
  page.on("dialog", dialog => dialog.accept());
  await page.reload();
  await expect(page.getByText("Есть несохранённый текст версии 0.3.2")).toBeVisible();
  await page.getByRole("button", { name: "Восстановить", exact: true }).click();
  await expect(page.getByLabel("Кратко — русский")).toHaveValue("Новый проект и улучшенный микшер");
  await expect(page.locator("#release-version")).toHaveValue("0.3.2");
});
