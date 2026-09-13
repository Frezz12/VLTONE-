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
  await expect(page.locator(".release-note-group h3")).toHaveText(["Новые функции", "Исправления"]);

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
  await expect(page.getByAltText("Обновлённый микшер")).toBeVisible();

  await page.setViewportSize({ width: 375, height: 800 });
  await page.context().addCookies([{ name: "vlt-locale", value: "en", url: "http://127.0.0.1:3100" }]);
  await page.goto("/releases/0.1.2");
  await expect(page.getByText("New mixer and fixes")).toBeVisible();
  await expect.poll(() => page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});
