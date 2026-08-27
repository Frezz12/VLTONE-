import { expect, test } from "@playwright/test";

const session = { admin: { id: "owner", email: "owner@example.com", nickname: "Owner" }, csrf_token: "csrf", expires_at: "2026-08-26T20:00:00Z" };

test("administrator adds a managed model connection", async ({ page }) => {
  const models: Record<string, unknown>[] = [];
  let submitted: Record<string, unknown> | undefined;
  await page.route("**/api/v1/admin/**", async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    if (path.endsWith("/admin/me")) return route.fulfill({ json: session });
    if (path.endsWith("/ai/models") && request.method() === "GET")
      return route.fulfill({ json: { models } });
    if (path.endsWith("/ai/models") && request.method() === "POST") {
      submitted = request.postDataJSON() as Record<string, unknown>;
      const stored = {
        id: "11111111-1111-4111-8111-111111111111", ...submitted,
        api_key: undefined, has_api_key: true,
      };
      models.push(stored);
      return route.fulfill({ status: 201, json: stored });
    }
    return route.fulfill({ status: 404 });
  });

  await page.goto("/models");
  await expect(page.getByRole("heading", { name: "Модели AI" })).toBeVisible();
  await page.getByLabel("Название в программе").fill("VLT Assistant");
  await page.getByLabel("ID модели у провайдера").fill("gpt-4.1");
  await page.getByRole("textbox", { name: "API-ключ", exact: true }).fill("secret-provider-key");
  await page.getByRole("button", { name: "Сохранить" }).click();

  await expect.poll(() => submitted?.display_name).toBe("VLT Assistant");
  expect(submitted).toMatchObject({
    provider: "openai", model: "gpt-4.1",
    endpoint_url: "https://api.openai.com/v1",
    api_key: "secret-provider-key", enabled: true,
  });
  await expect(page.getByText("Модель сохранена и доступна программе.")).toBeVisible();
  await expect(page.getByRole("button", { name: /VLT Assistant/ })).toBeVisible();
});

test("model settings stay usable on a narrow screen", async ({ page }) => {
  await page.setViewportSize({ width: 375, height: 812 });
  await page.route("**/api/v1/admin/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/admin/me")) return route.fulfill({ json: session });
    if (path.endsWith("/ai/models")) return route.fulfill({ json: { models: [] } });
    return route.fulfill({ status: 404 });
  });
  await page.goto("/models");
  await expect(page.getByRole("heading", { name: "Модели AI" })).toBeVisible();
  await expect(page.getByLabel("Название в программе")).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
});
