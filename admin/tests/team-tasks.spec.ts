import { expect, test, type Page } from "@playwright/test";
import { mkdir } from "node:fs/promises";
const owner = { id: "owner", email: "owner@example.com", nickname: "Николай", is_owner: true, permissions: [], status: "active" };
const member = { id: "member", user_id: "user", email: "designer@example.com", nickname: "Александра", is_owner: false, permissions: ["tasks.read", "tasks.write"], status: "active" };
const initialTasks = [
  { id: "1", number: 12, title: "Быстрый поиск по библиотеке", description: "Находить сэмплы по тегам и инструментам прямо во время работы.", status: "idea", priority: "normal", author_id: "owner", assignee_id: null, due_date: null, version: 1, created_at: "2026-10-09T10:00:00Z", updated_at: "2026-10-09T10:00:00Z" },
  { id: "2", number: 11, title: "Набор стартовых шаблонов", description: "Подготовить проекты для первых треков: электроника, хип-хоп и запись вокала.", status: "planned", priority: "high", author_id: "member", assignee_id: "member", due_date: "2026-10-20T00:00:00Z", version: 1, created_at: "2026-10-08T10:00:00Z", updated_at: "2026-10-09T10:00:00Z" },
  { id: "3", number: 10, title: "Упростить настройку аудио", description: "Проверить первый запуск и подсказки при выборе устройства.", status: "in_progress", priority: "urgent", author_id: "owner", assignee_id: "member", due_date: "2026-10-14T00:00:00Z", version: 1, created_at: "2026-10-07T10:00:00Z", updated_at: "2026-10-09T10:00:00Z" },
  { id: "4", number: 9, title: "Новая страница релиза", description: "Тексты и изображения готовы. Нужна финальная проверка.", status: "review", priority: "normal", author_id: "member", assignee_id: "owner", due_date: null, version: 1, created_at: "2026-10-06T10:00:00Z", updated_at: "2026-10-09T10:00:00Z" },
  { id: "5", number: 8, title: "Собрать обратную связь", description: "Первые отзывы участников бета-теста собраны и разобраны.", status: "done", priority: "low", author_id: "owner", assignee_id: "member", due_date: null, version: 1, created_at: "2026-10-05T10:00:00Z", updated_at: "2026-10-09T10:00:00Z" },
];
async function setup(page: Page, readOnly = false) {
  let tasks = structuredClone(initialTasks);
  const comments: Array<{ id: string; author_id: string; body: string; created_at: string }> = [];
  const attachments: Array<{ id: string; name: string; size: number; author_id: string }> = [];
  await page.route("**/api/v1/admin/**", async route => {
    const request = route.request(), path = new URL(request.url()).pathname;
    if (path.endsWith("/me")) return route.fulfill({ json: { admin: readOnly ? { ...member, permissions: ["tasks.read"] } : owner, csrf_token: "csrf", expires_at: "2099-01-01T00:00:00Z" } });
    if (path.endsWith("/tasks/members")) return route.fulfill({ json: { members: [owner, member].map(item => ({ ...item, active: true })) } });
    if (path.endsWith("/team")) return route.fulfill({ json: { members: [owner, member], permissions: [] } });
    if (path.endsWith("/comments")) { expect(request.headers()["x-csrf-token"]).toBe("csrf"); const item = { id: "c1", author_id: owner.id, body: request.postDataJSON().body, created_at: "2026-10-09T12:00:00Z" }; comments.push(item); return route.fulfill({ status: 201, json: item }); }
    if (path.endsWith("/tasks") && request.method() === "POST") { expect(request.headers()["x-csrf-token"]).toBe("csrf"); const task = { ...initialTasks[0], ...request.postDataJSON(), id: "new", number: 13, author_id: owner.id, version: 1 }; tasks.unshift(task); return route.fulfill({ status: 201, json: task }); }
    if (path.endsWith("/tasks")) return route.fulfill({ json: { tasks, next_cursor: 0 } });
    if (/\/tasks\/[\w-]+$/.test(path)) {
      const id = path.split("/").pop(); let task = tasks.find(item => item.id === id)!;
      if (request.method() === "PUT") { expect(request.headers()["x-csrf-token"]).toBe("csrf"); const input = request.postDataJSON(); expect(input.version).toBe(task.version); task = { ...task, ...input, version: task.version + 1 }; tasks = tasks.map(item => item.id === id ? task : item); return route.fulfill({ json: task }); }
      return route.fulfill({ json: { task, comments, attachments } });
    }
    return route.fulfill({ json: { crashes: [] } });
  });
  await page.route("**/release-upload/v1/admin/tasks/*/attachments", route => { expect(route.request().headers()["x-csrf-token"]).toBe("csrf"); const file = { id: "f1", name: "idea.txt", size: 5, author_id: owner.id }; attachments.push(file); return route.fulfill({ status: 201, json: file }); });
  return { tasks: () => tasks };
}

test("owner grants website-account access with dependent read permissions", async ({ page }) => {
  await setup(page);
  let saved: unknown;
  await page.route("**/api/v1/admin/team", route => { if (route.request().method() === "PUT") { saved = route.request().postDataJSON(); expect(route.request().headers()["x-csrf-token"]).toBe("csrf"); return route.fulfill({ json: { id: "member" } }); } return route.fulfill({ json: { members: [owner, member] } }); });
  await page.goto("/team");
  await page.getByLabel("Email аккаунта сайта").fill("new@example.com");
  await page.getByLabel("Релизы: изменение", { exact: true }).check();
  await expect(page.getByLabel("Релизы: просмотр", { exact: true })).toBeChecked();
  await page.getByRole("button", { name: "Предоставить доступ", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("Доступ сохранён");
  expect(saved).toEqual({ email: "new@example.com", enabled: true, permissions: ["tasks.read", "tasks.write", "releases.write", "releases.read"] });
  await page.getByRole("button", { name: /Александра/ }).click();
  await page.getByLabel("Разрешить вход в админку").uncheck();
  await page.getByRole("button", { name: "Сохранить доступ", exact: true }).click();
  expect(saved).toMatchObject({ email: member.email, enabled: false });
});

test("task creation, assignment, comments, files, keyboard movement and mobile layout", async ({ page }) => {
  const state = await setup(page);
  await page.goto("/tasks");
  await page.getByRole("button", { name: "Новая задача", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "Создать задачу" });
  await dialog.getByLabel("Название", { exact: true }).fill("Запись MIDI");
  await dialog.getByLabel("Описание", { exact: true }).fill("Проверить запись с клавиатуры");
  await dialog.getByRole("combobox", { name: "Исполнитель", exact: true }).selectOption("member");
  await dialog.getByRole("button", { name: "Создать задачу", exact: true }).click();
  const card = page.getByRole("dialog", { name: "Карточка задачи" });
  await expect(card).toBeVisible();
  await card.getByLabel("Комментарий", { exact: true }).fill("Беру в работу");
  await card.getByRole("button", { name: "Отправить комментарий" }).click();
  await expect(card.getByText("Беру в работу", { exact: true })).toBeVisible();
  await card.getByLabel("Прикрепить файлы").setInputFiles({ name: "idea.txt", mimeType: "text/plain", buffer: Buffer.from("hello") });
  await expect(card.getByRole("link", { name: /idea.txt/ })).toBeVisible();
  await card.getByRole("button", { name: "Закрыть задачу", exact: true }).click();
  await page.getByLabel("Этап задачи №13", { exact: true }).selectOption("in_progress");
  await expect.poll(() => state.tasks().find(task => task.id === "new")?.status).toBe("in_progress");
  await page.setViewportSize({ width: 390, height: 844 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await mkdir("../artifacts/admin-team-tasks", { recursive: true });
  await page.screenshot({ path: "../artifacts/admin-team-tasks/mobile.png", fullPage: true });
  await page.getByRole("button", { name: /Запись MIDI/ }).click();
  await page.getByLabel("Название", { exact: true }).fill("Несохранённая правка");
  await page.keyboard.press("Escape");
  await expect(page.getByText("Есть несохранённые изменения. Закрыть карточку и потерять их?")).toBeVisible();
  await page.getByRole("button", { name: "Продолжить редактирование" }).click();
  await expect(page.getByLabel("Название", { exact: true })).toHaveValue("Несохранённая правка");
});

test("board-only reader cannot see privileged navigation or edit controls", async ({ page }) => {
  await setup(page, true);
  await page.goto("/tasks");
  await expect(page.getByRole("heading", { name: "Доска задач", exact: true })).toBeVisible();
  await expect(page.getByRole("link", { name: "Команда", exact: true })).toHaveCount(0);
  await expect(page.getByRole("link", { name: "Пользователи", exact: true })).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Новая задача", exact: true })).toHaveCount(0);
  await page.getByRole("button", { name: /Быстрый поиск по библиотеке/ }).click();
  await expect(page.getByLabel("Название", { exact: true })).toBeDisabled();
  await expect(page.getByRole("button", { name: "Сохранить изменения", exact: true })).toHaveCount(0);
  await page.keyboard.press("Escape");
  await page.goto("/team");
  await expect(page.getByRole("heading", { name: "Нет доступа к разделу" })).toBeVisible();
});

test("board and team visual review", async ({ page }) => {
  await setup(page);
  await page.setViewportSize({ width: 1600, height: 1050 });
  await page.goto("/tasks");
  await expect(page.getByRole("button", { name: /Быстрый поиск по библиотеке/ })).toBeVisible();
  await mkdir("../artifacts/admin-team-tasks", { recursive: true });
  await page.screenshot({ path: "../artifacts/admin-team-tasks/board.png", fullPage: true });
  await page.getByRole("button", { name: /Быстрый поиск по библиотеке/ }).click();
  await expect(page.getByRole("dialog")).toBeVisible();
  await page.screenshot({ path: "../artifacts/admin-team-tasks/task.png", fullPage: true });
  await page.keyboard.press("Escape");
  await page.goto("/team");
  await expect(page.getByRole("button", { name: /Александра/ })).toBeVisible();
  await page.screenshot({ path: "../artifacts/admin-team-tasks/team.png", fullPage: true });
});

test("website-account login opens the first permitted admin section", async ({ page }) => {
  await setup(page, true);
  await page.route("**/api/v1/admin/auth/login", route => {
    expect(route.request().postDataJSON()).toEqual({ email: member.email, password: "website password" });
    return route.fulfill({ json: { admin: { ...member, permissions: ["tasks.read"] }, csrf_token: "csrf", expires_at: "2099-01-01T00:00:00Z" } });
  });
  await page.goto("/login");
  await page.getByLabel("Email", { exact: true }).fill(member.email);
  await page.getByLabel("Пароль", { exact: true }).fill("website password");
  await page.getByRole("button", { name: "Войти", exact: true }).click();
  await expect(page).toHaveURL(/\/tasks$/);
  await expect(page.getByRole("heading", { name: "Доска задач", exact: true })).toBeVisible();
});

test("dragging changes a stage and a rejected concurrent edit retains the draft", async ({ page }) => {
  const state = await setup(page);
  await page.goto("/tasks");
  await page.locator(".task-card").filter({ hasText: "Быстрый поиск по библиотеке" }).dragTo(page.locator('[data-stage="in_progress"]'));
  await expect.poll(() => state.tasks()[0].status).toBe("in_progress");
  await page.getByRole("button", { name: /Быстрый поиск по библиотеке/ }).click();
  await page.getByLabel("Название", { exact: true }).fill("Мои изменения");
  await page.route("**/api/v1/admin/tasks/1", route => route.fulfill({ status: 409, json: { code: "task_conflict", message: "Задача уже изменена. Обновите её перед сохранением." } }));
  await page.getByRole("button", { name: "Сохранить изменения", exact: true }).click();
  await expect(page.getByRole("dialog").getByRole("alert")).toContainText("Задача уже изменена");
  await expect(page.getByLabel("Название", { exact: true })).toHaveValue("Мои изменения");
});
