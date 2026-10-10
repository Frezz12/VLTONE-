import { expect, test } from "@playwright/test";

for (const [locale, chapter, title] of [
  ["ru", "slide-notes", "Слайд-ноты и рисуемая высота"],
  ["en", "slide-routing", "Slides in plugins: MIDI, MPE and range"],
  ["ru", "chord-generator", "Генератор аккордов"],
  ["en", "warp-audio", "Warp: audio timing with markers"],
]) {
  test(`manual opens new chapter ${chapter} in ${locale}`, async ({ page }) => {
    await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
    await page.goto(`/manual#${chapter}`);
    await expect(page.getByRole("heading", { name: title, exact: true })).toBeInViewport();
    await expect(page.locator(`#${chapter} .manual-section ol li`)).not.toHaveCount(0);
    await expect(page.locator(".manual-content img")).toHaveCount(0);
    await expect(page.locator(".manual-version")).toContainText("2026");
  });
}

test("manual navigation, search, deep links, and locale switch", async ({ page }) => {
  await page.context().addCookies([{ name: "vlt-locale", value: "ru", url: "http://127.0.0.1:3100" }]);
  await page.goto("/manual");
  await expect(page.getByRole("heading", { name: "Инструкция VLTone" })).toBeVisible();
  await expect(page.getByRole("link", { name: "Инструкция" })).toHaveAttribute("href", "/manual");
  await page.getByRole("button", { name: "Только необходимые" }).click();

  await page.getByRole("button", { name: "Open in English" }).click();
  await expect(page).toHaveURL(/\/manual$/);
  await expect(page.getByRole("heading", { name: "VLTone Manual" })).toBeVisible();
  await page.waitForLoadState("networkidle");

  const firstTab = page.getByRole("tab", { name: "Getting started" });
  await firstTab.focus();
  await firstTab.press("ArrowRight");
  await expect(page.getByRole("tab", { name: "Project and arrangement" })).toHaveAttribute("aria-selected", "true");
  await expect(page.getByRole("heading", { name: "Create and save projects" })).toBeVisible();

  await page.goto("/manual#piano-roll");
  await expect(page.getByRole("tab", { name: "MIDI and instruments" })).toHaveAttribute("aria-selected", "true");
  await expect(page.getByRole("heading", { name: "Piano roll" })).toBeVisible();

  const search = page.getByRole("searchbox", { name: "Search the manual" });
  await search.fill("blacklist");
  await expect(page.getByRole("heading", { name: "Plugin Manager" })).toBeVisible();
  await search.fill("no-such-vlt-topic");
  await expect(page.getByRole("status")).toContainText("No results");
  await page.getByRole("button", { name: "Clear search" }).click();
  await expect(search).toHaveValue("");
});

for (const locale of ["ru", "en"]) {
  test(`manual is text only and documents numeric editing in ${locale}`, async ({ page }) => {
    await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
    const images: string[] = [];
    page.on("request", request => { if (request.url().includes("/manual/") && /\.png/.test(request.url())) images.push(request.url()); });
    await page.goto("/manual#numeric-fields");
    await expect(page.locator("#numeric-fields")).toBeInViewport();
    await expect(page.locator("#numeric-fields")).toContainText("Shift");
    await expect(page.locator("#numeric-fields")).toContainText("Enter");
    await page.getByRole("searchbox").fill(" ");
    for (const tab of await page.getByRole("tab").all()) {
      await tab.click();
      await expect(page.locator(".manual-content img, .manual-gallery, .manual-lightbox")).toHaveCount(0);
      await expect(page.locator(".manual-chapter").first()).toBeVisible();
    }
    expect(images).toEqual([]);
  });
}

test("manual sidebar opens a chapter from another category", async ({ page }) => {
  await page.context().addCookies([{ name: "vlt-locale", value: "en", url: "http://127.0.0.1:3100" }]);
  await page.goto("/manual");
  const sidebar = page.getByRole("complementary", { name: "On this page" });
  await sidebar.getByRole("link", { name: "Piano roll" }).click();
  await expect(page).toHaveURL(/\/manual#piano-roll$/);
  await expect(page.getByRole("tab", { name: "MIDI and instruments" })).toHaveAttribute("aria-selected", "true");
  await expect(page.getByRole("heading", { name: "Piano roll" })).toBeInViewport();
});

for (const viewport of [
  { width: 375, height: 760 },
  { width: 768, height: 900 },
  { width: 1024, height: 900 },
  { width: 1440, height: 1000 },
]) {
  test(`manual has no page-level horizontal scroll at ${viewport.width}px`, async ({ page }) => {
    await page.setViewportSize(viewport);
    await page.context().addCookies([{ name: "vlt-locale", value: "ru", url: "http://127.0.0.1:3100" }]);
    await page.goto("/manual");
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    expect(overflow).toBeLessThanOrEqual(1);
  });
}

for (const [locale, query, title] of [
  ["ru", "заморозка", "Заморозка дорожки"],
  ["en", "unfreeze", "Freeze and unfreeze tracks"],
  ["ru", "Fill rhythm", "Pattern tracks"],
  ["en", "Ctrl+Shift+U", "Automation"],
]) {
  test(`updated manual documents ${query} in ${locale}`, async ({ page }) => {
    await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
    await page.goto("/manual");
    await page.getByRole("searchbox").fill(query);
    await expect(page.getByRole("heading", { name: title, exact: true })).toBeVisible();
  });
}
