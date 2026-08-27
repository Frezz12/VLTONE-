import { expect, test } from "@playwright/test";

// Editing the assistant's instructions is the point of the page: an admin opens
// a playbook, changes the text, saves, and the desktop picks it up on its next
// fetch. The API is stubbed — this checks the page, not the backend.

const session = { admin: { id: "owner", email: "owner@example.com", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2026-08-26T20:00:00Z" };

const documents = [
  { id: "main", kind: "main", title: "Main instructions", use_when: "always", tags: [], body: "You are a music producer.", enabled: true, updated_at: "2026-08-25T10:00:00Z", builtin: true },
  { id: "bass", kind: "playbook", title: "Bass", use_when: "writing a bass part", tags: ["bass"], body: "Follow the root note.", enabled: true, updated_at: "2026-08-25T10:00:00Z", builtin: true },
];

test("administrator edits a playbook and saves it", async ({ page }) => {
  const saved: { id: string; body: string }[] = [];
  await page.route("**/api/v1/admin/**", async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    if (path.endsWith("/admin/me")) return route.fulfill({ json: session });
    if (path.endsWith("/ai/prompts") && request.method() === "GET")
      return route.fulfill({ json: { version: "vabc123", prompts: documents } });
    if (path.endsWith("/ai/prompts/bass") && request.method() === "PUT") {
      const body = request.postDataJSON() as { body: string };
      saved.push({ id: "bass", body: body.body });
      documents[1].body = body.body;
      documents[1].builtin = false;
      return route.fulfill({ json: { ...documents[1] } });
    }
    return route.fulfill({ status: 204 });
  });

  await page.goto("/prompts");
  await expect(page.getByRole("heading", { name: "Промпты ассистента" })).toBeVisible();
  await expect(page.getByText("vabc123")).toBeVisible();

  await page.getByRole("button", { name: "Bass" }).click();
  const body = page.locator("#prompt-body");
  await expect(body).toHaveValue("Follow the root note.");
  await body.fill("Follow the root note, an octave down.");
  await page.getByRole("button", { name: "Сохранить" }).click();

  await expect.poll(() => saved.length).toBe(1);
  expect(saved[0]).toEqual({ id: "bass", body: "Follow the root note, an octave down." });
  await expect(page.getByText("изменён")).toBeVisible();
});

test("administrator restores the text the app ships with", async ({ page }) => {
  let reverted = false;
  await page.route("**/api/v1/admin/**", async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    if (path.endsWith("/admin/me")) return route.fulfill({ json: session });
    if (path.endsWith("/ai/prompts") && request.method() === "GET")
      return route.fulfill({ json: { version: "vabc123", prompts: [documents[0], { ...documents[1], body: "Something wrong.", builtin: false }] } });
    if (path.endsWith("/ai/prompts/bass/revert")) {
      reverted = true;
      return route.fulfill({ json: { ...documents[1], body: "Follow the root note.", builtin: true } });
    }
    return route.fulfill({ status: 204 });
  });

  await page.goto("/prompts");
  await page.getByRole("button", { name: "Bass" }).click();
  await page.getByRole("button", { name: "Вернуть встроенный" }).click();
  await expect.poll(() => reverted).toBe(true);
});
