import { expect, test } from "@playwright/test";

test("support links count deliberate activations by location and still open if tracking fails", async ({ page, context }) => {
  const clicks: string[] = [];
  await page.addInitScript(() => localStorage.setItem("vlt-cookie-preference-v1", "none"));
  await context.route("https://boosty.to/**", route => route.fulfill({ body: "Boosty destination" }));
  await page.route("**/api/v1/support/boosty/click/*", route => {
    expect(route.request().method()).toBe("POST");
    clicks.push(new URL(route.request().url()).pathname.split("/").at(-1)!);
    return route.fulfill({ status: 503 });
  });
  await page.goto("/");
  const home = page.locator(".studio-support a");
  await expect(home).toHaveAttribute("href", "https://boosty.to/vltone");
  await expect(page.locator("main > section").last()).toHaveClass("studio-support");
  await home.scrollIntoViewIfNeeded();
  await home.hover();
  await home.focus();
  expect(clicks).toEqual([]);
  const popup = page.waitForEvent("popup");
  await home.press("Enter");
  const destination = await popup;
  await expect(destination).toHaveURL("https://boosty.to/vltone");
  await destination.close();
  await expect.poll(() => clicks).toEqual(["home"]);
  const footer = page.locator(".footer-support");
  await footer.dispatchEvent("auxclick", { button: 1 });
  await expect.poll(() => clicks).toEqual(["home", "footer"]);
  await footer.dispatchEvent("auxclick", { button: 2 });
  expect(clicks).toEqual(["home", "footer"]);
  await page.setViewportSize({ width: 375, height: 812 });
  await home.scrollIntoViewIfNeeded();
  await expect(home).toBeInViewport();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: "../artifacts/site-refresh-20261017/support-mobile.png" });
  await page.setViewportSize({ width: 1440, height: 1000 });
  await home.scrollIntoViewIfNeeded();
  await page.screenshot({ path: "../artifacts/site-refresh-20261017/support-desktop.png" });
});

test("Boosty remains a native link without JavaScript", async ({ browser }) => {
  const context = await browser.newContext({ javaScriptEnabled: false });
  const page = await context.newPage();
  await page.goto("http://127.0.0.1:3100/");
  await expect(page.locator('a[href="https://boosty.to/vltone"]')).toHaveCount(2);
  await expect(page.locator(".studio-support a")).toBeVisible();
  await context.close();
});

test("capabilities track the visible section and retain immediate reduced-motion disclosure", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/capabilities");
  await page.locator("#midi").scrollIntoViewIfNeeded();
  await expect(page.locator('.capabilities-toc a[href="#midi"]')).toHaveAttribute("aria-current", "location");
  const summary = page.locator("#midi summary");
  await summary.focus();
  await summary.press("Enter");
  await expect(page.locator("#midi details")).toHaveAttribute("open", "");
  expect(await page.locator("#midi details ul").evaluate(element => element.getAnimations().length)).toBe(0);
  await page.locator('#midi .product-shot').focus();
  await page.locator('#midi .product-shot').press("Enter");
  await expect(page.locator("#midi dialog")).toBeVisible();
  expect(await page.locator("#midi dialog").evaluate(element => element.getAnimations().length)).toBe(0);
  await page.keyboard.press("Escape");
  await expect(page.locator("#midi dialog")).not.toBeVisible();
});
