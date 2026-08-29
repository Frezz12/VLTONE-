import { expect, test } from "@playwright/test";

test("release history exposes only available files and warns before DMG", async ({ page }) => {
  await page.context().grantPermissions(["clipboard-read", "clipboard-write"]);
  await page.route("**/api/v1/releases/0.1.2/download/macos-dmg", (route) => route.fulfill({
    body: "installer",
    headers: { "Content-Type": "application/octet-stream", "Content-Disposition": 'attachment; filename="VLT-installer.dmg"' },
  }));
  await page.goto("/ru/releases");
  await expect(page.getByRole("heading", { name: "Обновления" })).toBeVisible();
  await expect(page.getByText("v0.1.2")).toBeVisible();
  await page.getByRole("link", { name: "Открыть версию 0.1.2" }).click();

  await expect(page.getByRole("heading", { name: "v0.1.2" })).toBeVisible();
  await expect(page.getByRole("link", { name: /Windows Setup/ })).toBeVisible();
  await expect(page.getByRole("link", { name: /Linux DEB/ })).toBeVisible();
  await expect(page.getByRole("link", { name: /Linux RPM/ })).toBeVisible();
  await expect(page.getByText("Linux AppImage")).toHaveCount(0);
  await expect(page.getByAltText("Обновлённый микшер")).toBeVisible();

  const macDownload = page.getByRole("button", { name: /macOS DMG/ });
  await macDownload.click();
  const dialog = page.getByRole("dialog", { name: "Перед запуском на macOS" });
  await expect(dialog).toBeVisible();
  await expect(dialog.getByText(/sudo xattr -rd/)).toBeVisible();
  await dialog.getByRole("button", { name: "Копировать" }).click();
  await expect(dialog.getByRole("button", { name: "Скопировано" })).toBeVisible();
  await expect.poll(() => page.evaluate(() => navigator.clipboard.readText())).toBe('sudo xattr -rd com.apple.quarantine "/Applications/VLT Studio Pro.app"');
  await page.keyboard.press("Escape");
  await expect(dialog).not.toBeVisible();
  await expect(macDownload).toBeFocused();

  await macDownload.click();
  const downloadPromise = page.waitForEvent("download");
  await dialog.getByRole("link", { name: "Скачать DMG" }).click();
  await expect((await downloadPromise).suggestedFilename()).toBe("VLT-installer.dmg");

  await page.setViewportSize({ width: 375, height: 800 });
  await page.goto("/en/releases/0.1.2");
  await expect(page.getByText("New mixer and fixes")).toBeVisible();
  await expect.poll(() => page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});
