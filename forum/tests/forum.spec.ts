import { test, expect, type Page } from "@playwright/test";

async function english(page: Page) {
  await page.addInitScript(() =>
    localStorage.setItem("vlt-forum-locale", "en"),
  );
}
test("responsive navigation, theme, keyboard and error states", async ({
  page,
}) => {
  await english(page);
  await page.route("**/api/v1/me", (route) =>
    route.fulfill({ status: 401, json: { code: "authentication_required" } }),
  );
  await page.route("**/api/v1/forum/feed**", (route) =>
    route.fulfill({ json: { articles: [], next_cursor: "" } }),
  );
  await page.route("**/api/v1/forum/sections", (route) =>
    route.fulfill({ json: { sections: [] } }),
  );
  await page.route("**/api/v1/forum/config", (route) =>
    route.fulfill({ json: { max_attachments: 10 } }),
  );
  await page.goto("/");
  await expect(
    page.getByRole("heading", { name: "It’s quiet here" }),
  ).toBeVisible();
  await page.keyboard.press("Tab");
  await expect(page.getByText("Skip to content")).toBeFocused();
  await page.getByRole("button", { name: "Change theme" }).click();
  await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
  await expect(
    page.getByRole("navigation", { name: "Main navigation" }),
  ).toBeVisible();
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= window.innerWidth,
    ),
  ).toBeTruthy();
  await page.route("**/api/v1/forum/sections", (route) =>
    route.fulfill({
      status: 503,
      json: { message: "Temporarily unavailable" },
    }),
  );
  await page.getByRole("link", { name: "Forums", exact: true }).click();
  await expect(page.getByRole("main").getByRole("alert")).toHaveText(
    "Temporarily unavailable",
  );
});

test("shared login, publishing files, comments, saved posts and notifications", async ({
  page,
  browser,
}) => {
  test.setTimeout(120_000);
  test.skip(
    !process.env.VLT_FORUM_E2E,
    "Run from the Go PostgreSQL browser harness for real account/API integration",
  );
  await english(page);
  const context = page.context();
  const main = "http://127.0.0.1:3020";
  const login = await context.request.post(main + "/api/v1/web/auth/login", {
    headers: { Origin: main },
    data: {
      email: "Alice@test.local",
      password: "correct horse battery staple",
    },
  });
  expect(login.status()).toBe(200);
  await page.goto("/");
  await page.getByRole("button", { name: "Sign in", exact: true }).click();
  await expect(page.locator(".top-actions .avatar")).toBeVisible();
  expect(new URL(page.url()).hostname).toBe("localhost");
  const cookies = await context.cookies();
  expect(
    cookies.some(
      (c) =>
        c.name === "vlt_web_session" && c.domain === "localhost" && c.httpOnly,
    ),
  ).toBeTruthy();
  expect(
    cookies.some(
      (c) =>
        c.name === "vlt_web_session" && c.domain === "127.0.0.1" && c.httpOnly,
    ),
  ).toBeTruthy();
  const title = `Browser session ${Date.now()}`;
  await page.getByRole("textbox", { name: "Title — optional" }).fill(title);
  await page
    .getByRole("textbox", { name: "Post text" })
    .fill("A real browser publication with **Markdown** and a Mini module.");
  await page
    .locator('input[type="file"]')
    .setInputFiles({
      name: "browser.vltmini",
      mimeType: "application/octet-stream",
      buffer: Buffer.from('{"format":"vltmini","version":1}'),
    });
  await expect(
    page.getByRole("button", { name: "Publish", exact: true }),
  ).toBeEnabled();
  await expect(page.getByText("100%", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Publish", exact: true }).click();
  const post = page
    .locator("article")
    .filter({ has: page.getByRole("heading", { name: title }) });
  await expect(post.getByText("browser.vltmini")).toBeVisible();
  await post.getByRole("button", { name: "Save", exact: true }).click();
  await expect(
    post.getByRole("button", { name: "Saved", exact: true }),
  ).toHaveAttribute("aria-pressed", "true");
  await post.getByRole("link", { name: title }).click();
  await expect(page).toHaveURL(/\/posts\/[0-9a-f-]+$/);
  const articleURL = page.url();
  const bobContext = await browser.newContext();
  const bob = await bobContext.newPage();
  await english(bob);
  await bobContext.request.post(main + "/api/v1/web/auth/login", {
    headers: { Origin: main },
    data: { email: "Bob@test.local", password: "correct horse battery staple" },
  });
  await bob.goto(articleURL);
  await bob.getByRole("button", { name: "Sign in", exact: true }).click();
  await expect(bob.locator(".top-actions .avatar")).toBeVisible();
  await bob
    .getByRole("textbox", { name: "Post text" })
    .fill("Thanks for sharing from the second account!");
  await bob.getByRole("button", { name: "Publish", exact: true }).click();
  await expect(
    bob.getByText("Thanks for sharing from the second account!", {
      exact: true,
    }),
  ).toBeVisible();
  await bob
    .locator("article")
    .first()
    .getByRole("button", { name: /Like/ })
    .click();
  await bob.goto("http://localhost:3022/u/Alice");
  await expect(
    bob.getByRole("heading", { name: "Alice", exact: true }),
  ).toBeVisible();
  if (
    await bob
      .getByRole("button", { name: "Following", exact: true })
      .isVisible()
  )
    await bob.getByRole("button", { name: "Following", exact: true }).click();
  await bob.getByRole("button", { name: "Follow", exact: true }).click();
  await expect(
    bob.getByRole("button", { name: "Following", exact: true }),
  ).toBeVisible();
  await page.goto("/notifications");
  await expect(page.getByRole("main")).toContainText("comment");
  await page.goto("/saved");
  await expect(page.getByRole("heading", { name: title })).toBeVisible();
  await page.screenshot({
    path: `/private/tmp/vlt-forum-${test.info().project.name}.png`,
    fullPage: true,
  });
  await bobContext.close();
});

test("prepared video plays without autoplaying sound", async ({ page }) => {
  await english(page);
  const attachment = {
    id: "12345678-1234-1234-1234-123456789abc", name: "studio.mp4", bytes: 10000,
    sha256: "0".repeat(64), content_type: "video/mp4", kind: "video", resource_type: "",
    minimum_version: "", status: "ready", media_status: "ready", media_error: "",
  };
  await page.route("**/api/v1/me", r => r.fulfill({ status: 401, json: {} }));
  await page.route("**/api/v1/forum/config", r => r.fulfill({ json: { max_attachments: 10 } }));
  await page.route("**/api/v1/forum/sections", r => r.fulfill({ json: { sections: [] } }));
  await page.route("**/api/v1/forum/feed**", r => r.fulfill({ json: { articles: [{
    id: "12345678-1234-1234-1234-123456789abd", kind: "post", author: { id: "author", nickname: "Creator" },
    title: "Studio session", body: "A short video", excerpt: "A short video", created_at: new Date().toISOString(),
    like_count: 0, comment_count: 0, liked: false, saved: false, attachments: [attachment],
  }], next_cursor: "" } }));
  await page.route("**/media/poster.jpg", r => r.fulfill({ status: 404 }));
  await page.route("**/media/video.mp4", r => r.fulfill({ contentType: "video/mp4", path: "../tests/fixtures/video/background.mp4" }));
  await page.route("**/media/video.webm", r => r.fulfill({ contentType: "video/webm", path: "../tests/fixtures/video/forum.webm" }));
  await page.goto("/");
  const player = page.locator("video"); await expect(player).toBeVisible();
  expect(await player.evaluate((v: HTMLVideoElement) => v.autoplay)).toBe(false);
  await player.evaluate(async (v: HTMLVideoElement) => { v.muted = true; await v.play(); });
  await expect.poll(() => player.evaluate((v: HTMLVideoElement) => v.currentTime)).toBeGreaterThan(0);
});
