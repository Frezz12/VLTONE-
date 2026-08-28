import { expect, test } from "@playwright/test";

test("manual navigation, search, deep links, and locale switch", async ({ page }) => {
  await page.goto("/ru/manual");
  await expect(page.getByRole("heading", { name: "Инструкция VLT Studio Pro" })).toBeVisible();
  await expect(page.getByRole("link", { name: "Инструкция" })).toHaveAttribute("href", "/ru/manual");

  await page.getByRole("link", { name: "Open in English" }).click();
  await expect(page).toHaveURL(/\/en\/manual$/);
  await expect(page.getByRole("heading", { name: "VLT Studio Pro Manual" })).toBeVisible();

  const firstTab = page.getByRole("tab", { name: "Getting started" });
  await firstTab.focus();
  await firstTab.press("ArrowRight");
  await expect(page.getByRole("tab", { name: "Project and arrangement" })).toHaveAttribute("aria-selected", "true");
  await expect(page.getByRole("heading", { name: "Create and save projects" })).toBeVisible();

  await page.goto("/en/manual#piano-roll");
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

test("manual screenshot opens in a native dialog and closes with Escape", async ({ page }) => {
  await page.goto("/en/manual");
  await expect(page.getByRole("tablist", { name: "Manual categories" })).toBeVisible();
  await expect(page.getByAltText("VLT Studio Pro startup window scanning plugins")).toBeVisible();
  await expect(page.getByText("The startup window reports the current loading stage and plugin scan progress.", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: /Enlarge: VLT Studio Pro startup/ }).click();
  const dialog = page.getByRole("dialog");
  await expect(dialog).toBeVisible();
  await expect(dialog.getByAltText("VLT Studio Pro startup window scanning plugins")).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(dialog).not.toBeVisible();
});

for (const viewport of [
  { width: 375, height: 760 },
  { width: 768, height: 900 },
  { width: 1024, height: 900 },
  { width: 1440, height: 1000 },
]) {
  test(`manual has no page-level horizontal scroll at ${viewport.width}px`, async ({ page }) => {
    await page.setViewportSize(viewport);
    await page.goto("/ru/manual");
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    expect(overflow).toBeLessThanOrEqual(1);
  });
}
