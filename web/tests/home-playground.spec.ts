import { expect, test } from "@playwright/test";

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("vlt-cookie-preference-v1", "none"));
});

for (const locale of ["ru", "en"]) {
  test(`${locale} home explains the rack, Creator, automation and mixing`, async ({ page, request }) => {
    const ru = locale === "ru";
    await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
    await page.goto("/");
    await page.locator(".gallery-switcher").getByRole("button", { name: ru ? "Рэк" : "Rack", exact: true }).click();
    await expect(page.locator(".hero-product img").first()).toHaveAttribute("src", new RegExp(`showcase-rack-${locale}\\.webp`));
    expect((await request.get(`/images/studio/showcase-rack-${locale}.webp`)).ok()).toBe(true);
    await expect(page.locator('.workflow-visual [data-shot="showcase-rack"]')).toBeVisible();
    await expect(page.locator('[data-shot="showcase-eq"]')).toHaveCount(0);

    const creator = page.locator(".playground-creator");
    await creator.locator('[data-node="lfo"]').click();
    await expect(creator.locator(".creator-demo-caption")).toContainText(ru ? "управляющий поток" : "control stream");
    await creator.getByRole("button", { name: ru ? "Модуляция" : "Modulation", exact: true }).click();
    await expect(creator.locator('[data-wire-type="audio"]').first()).toHaveAttribute("opacity", "0.1");
    await expect(creator.getByRole("link")).toHaveAttribute("href", "/creator");

    const automation = page.locator(".automation-demo");
    const originalCurve = await automation.locator(".automation-line").getAttribute("points");
    await automation.getByRole("button", { name: ru ? "Пульс" : "Pulse", exact: true }).click();
    await expect(automation.locator(".automation-line")).not.toHaveAttribute("points", originalCurve!);
    const depth = automation.getByRole("slider");
    await depth.focus();
    await page.keyboard.press("Home");
    await expect(depth).toHaveValue("0");
    await expect(automation.locator("output")).toHaveText("0%");
    const flat = await automation.locator(".automation-line").getAttribute("points");
    expect(new Set(flat!.split(" ").map(point => point.split(",")[1])).size).toBe(1);
    await page.keyboard.press("End");
    await expect(depth).toHaveValue("100");

    const mixer = page.locator(".mixer-demo");
    const drums = mixer.getByRole("slider", { name: ru ? "Ударные" : "Drums", exact: true });
    await drums.focus();
    await page.keyboard.press("Home");
    await expect(drums).toHaveValue("0");
    await expect(mixer.locator("output").first()).toHaveText("−∞ dB");
    await page.keyboard.press("End");
    await expect(mixer.locator("output").first()).toHaveText("0 dB");
    const mute = mixer.getByRole("button", { name: ru ? "Заглушить: Ударные" : "Mute: Drums", exact: true });
    await mute.click();
    await expect(mute).toHaveAttribute("aria-pressed", "true");
    await expect(mixer.locator("output").first()).toHaveText(ru ? "Выкл." : "Muted");
    await expect(mixer.locator(".home-mixer-meter i").first()).toHaveCSS("transform", "matrix(1, 0, 0, 0, 0, 0)");
    await mixer.getByRole("button", { name: ru ? "Сбросить микшер" : "Reset mixer" }).click();
    await expect(drums).toHaveValue("78");
    await expect(mute).toHaveAttribute("aria-pressed", "false");
  });
}

for (const width of [320, 375, 768, 1440]) {
  test(`interactive home cards fit and work at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 1000 });
    await page.goto("/");
    const creator = page.locator(".playground-creator");
    await creator.scrollIntoViewIfNeeded();
    await creator.locator('[data-node="mix"]').click();
    await expect(creator.locator('[data-node="mix"]')).toHaveAttribute("aria-pressed", "true");
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    const cards = await page.locator(".playground-card").evaluateAll(elements => elements.map(element => ({ width: element.clientWidth, content: element.scrollWidth })));
    for (const card of cards) expect(card.content).toBeLessThanOrEqual(card.width);
    const fader = page.locator(".home-mixer-strip input").first();
    await fader.scrollIntoViewIfNeeded();
    await fader.click({ trial: true });
    const box = await fader.boundingBox();
    if (!box) throw new Error("Missing mixer fader");
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await page.mouse.down();
    await page.mouse.move(box.x + box.width / 2, box.y - 8, { steps: 5 });
    await page.mouse.up();
    expect(Number(await fader.inputValue())).toBeGreaterThan(95);
    await page.emulateMedia({ reducedMotion: "reduce" });
    await expect(page.locator(".automation-cursor")).toHaveCSS("animation-name", "none");
    await expect(page.locator(".home-mixer-meter b").first()).toHaveCSS("animation-name", "none");
    if (width === 375 || width === 1440) {
      const screenshotOptions = { animations: "disabled" as const, style: ".vlt-topbar, .skip-link { visibility: hidden; }" };
      await page.locator(".home-playground").screenshot({ ...screenshotOptions, path: `../artifacts/home-playground-${width}.png` });
      await page.locator('.workflow-card[data-track="2"]').screenshot({ ...screenshotOptions, path: `../artifacts/home-rack-${width}.png` });
    }
  });
}

test("playground motion follows the page pause control", async ({ page }) => {
  await page.goto("/");
  await page.locator(".automation-demo").scrollIntoViewIfNeeded();
  await expect(page.locator(".automation-cursor")).toHaveCSS("animation-play-state", "running");
  await page.getByRole("button", { name: "Приостановить анимацию" }).click();
  await page.locator(".automation-demo").scrollIntoViewIfNeeded();
  await expect(page.locator(".automation-cursor")).toHaveCSS("animation-play-state", "paused");
});
