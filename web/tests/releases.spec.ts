import { expect, test } from "@playwright/test";

test("release history exposes only available files and warns before DMG", async ({ page }) => {
  await page.context().grantPermissions(["clipboard-read", "clipboard-write"]);
  await page.context().addCookies([{ name: "vlt-locale", value: "ru", url: "http://127.0.0.1:3100" }]);
  await page.route("**/api/v1/releases/0.1.2/download/macos-dmg", (route) => route.fulfill({
    body: "installer",
    headers: { "Content-Type": "application/octet-stream", "Content-Disposition": 'attachment; filename="VLT-installer.dmg"' },
  }));
  await page.goto("/releases");
  await expect(page.getByRole("heading", { name: "Скачать VLTone" })).toBeVisible();
  await expect(page.getByText("v0.1.2")).toBeVisible();
  await expect(page.getByRole("link", { name: /Windows Setup/ })).toBeVisible();
  await expect(page.getByRole("link", { name: /Linux DEB/ })).toBeVisible();
  await expect(page.getByRole("link", { name: /Linux RPM/ })).toBeVisible();
  await expect(page.getByText("Linux AppImage")).toHaveCount(0);
  await expect(page.locator(".release-highlight h3")).toHaveText(["Добавлен новый микшер"]);
  await expect(page.locator(".release-note-group h3")).toHaveText(["Исправления"]);

  const macDownload = page.getByRole("button", { name: /macOS DMG/ });
  await macDownload.click();
  const dialog = page.getByRole("dialog", { name: "Перед запуском на macOS" });
  await expect(dialog).toBeVisible();
  const dialogBox = await dialog.boundingBox();
  const viewport = page.viewportSize();
  expect(dialogBox && viewport && Math.abs(dialogBox.x + dialogBox.width / 2 - viewport.width / 2)).toBeLessThan(2);
  expect(dialogBox && viewport && Math.abs(dialogBox.y + dialogBox.height / 2 - viewport.height / 2)).toBeLessThan(2);
  await expect(dialog.getByText(/sudo xattr -rd/)).toBeVisible();
  await dialog.getByRole("button", { name: "Копировать" }).click();
  await expect(dialog.getByRole("button", { name: "Скопировано" })).toBeVisible();
  await expect.poll(() => page.evaluate(() => navigator.clipboard.readText())).toBe('sudo xattr -rd com.apple.quarantine "/Applications/VLTONE.app"');
  await page.keyboard.press("Escape");
  await expect(dialog).not.toBeVisible();
  await expect(macDownload).toBeFocused();

  await macDownload.click();
  const downloadPromise = page.waitForEvent("download");
  await dialog.getByRole("link", { name: "Скачать DMG" }).click();
  await expect((await downloadPromise).suggestedFilename()).toBe("VLT-installer.dmg");

  await page.locator(".release-version-row").first().click();
  await expect(page).toHaveURL(/\/releases\/0\.1\.2$/);
  await expect(page.getByRole("heading", { name: "v0.1.2" })).toBeVisible();
  await expect(page.locator(".release-highlight .product-shot picture").getByAltText("Обновлённый микшер")).toBeVisible();

  await page.setViewportSize({ width: 375, height: 800 });
  await page.context().addCookies([{ name: "vlt-locale", value: "en", url: "http://127.0.0.1:3100" }]);
  await page.goto("/releases/0.1.2");
  await expect(page.getByText("New mixer and fixes")).toBeVisible();
  await expect.poll(() => page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});

for (const locale of ["ru", "en"]) {
  test(`${locale} release highlights preserve source notes and pair each update with its own image`, async ({ page, request }) => {
    await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
    await page.addInitScript(() => localStorage.setItem("vlt-cookie-preference-v1", "none"));
    await page.goto("/releases/0.3.1");
    const cards = page.locator(".release-highlight");
    await expect(cards).toHaveCount(8);
    const source = await (await request.get(`/api/v1/releases/0.3.1?locale=${locale}`)).json();
    const displayed = await page.locator(".release-notes").textContent();
    for (const note of [...source.features, ...source.changes, ...source.fixes]) expect(displayed).toContain(note);
    const tones = await cards.evaluateAll(elements => elements.map(element => getComputedStyle(element).backgroundColor));
    expect(new Set(tones).size).toBe(8);
    const images = cards.locator(".product-shot picture img");
    const imageSources = await images.evaluateAll(elements => elements.map(element => element.getAttribute("src")));
    expect(new Set(imageSources).size).toBe(8);
    const detail = cards.first().locator("details");
    await expect(detail).not.toHaveAttribute("open");
    await detail.locator("summary").focus();
    await page.keyboard.press("Enter");
    await expect(detail).toHaveAttribute("open", "");
    await expect(detail.locator("li")).toHaveCount(6);
    await page.keyboard.press("Enter");
    await expect(detail).not.toHaveAttribute("open");
    const enlarge = cards.first().locator(".product-shot");
    await enlarge.click();
    await expect(page.locator("dialog[open]")).toBeVisible();
    await page.keyboard.press("Escape");
    await expect(page.locator("dialog[open]")).toHaveCount(0);
    await expect(enlarge).toBeFocused();
    await expect(page.locator(".release-gallery")).toHaveCount(0);
  });
}

for (const width of [320, 768, 1440]) {
  test(`release highlight text and media fit ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 900 });
    await page.goto("/releases/0.3.1");
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    for (const card of await page.locator(".release-highlight").all()) {
      const copy = await card.locator(".release-highlight-copy").boundingBox();
      const image = await card.locator(".release-highlight-media").boundingBox();
      if (!copy || !image) throw new Error("Release highlight is missing text or image");
      if (width <= 900) expect(image.y).toBeGreaterThanOrEqual(copy.y + copy.height + 20);
      else expect(Math.min(copy.x + copy.width, image.x + image.width)).toBeLessThanOrEqual(Math.max(copy.x, image.x) - 24);
    }
  });
}

for (const locale of ["ru", "en"]) test(`${locale} authored admin blocks appear on the public release with all remaining notes`, async ({ page, request }) => {
  await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
  await page.goto("/releases/0.3.2");
  const cards = page.locator(".release-highlight");
  await expect(cards).toHaveCount(2);
  await expect(cards.first().locator("h3")).toHaveText(locale === "ru" ? "Мелодия с движением" : "Melody in motion");
  await expect(cards.first()).toHaveAttribute("data-tone", "mint");
  await expect(cards.last()).toHaveAttribute("data-tone", "rose");
  const source = await (await request.get(`/api/v1/releases/0.3.2?locale=${locale}`)).json();
  const displayed = await page.locator(".release-notes").textContent();
  for (const text of [...source.features, ...source.changes, ...source.fixes]) expect(displayed).toContain(text);
  await expect(page.locator(".release-gallery")).toHaveCount(0);
  await cards.first().locator("summary").click();
  await expect(cards.first().locator("details li")).toHaveText(locale === "ru" ? "Подробность из админки" : "Detail from the admin editor");
});
