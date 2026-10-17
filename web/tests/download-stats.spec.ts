import { expect, test } from "@playwright/test";

test("only actual download clicks are counted, including DMG confirmation", async ({ page }) => {
 const clicks: string[] = [];
 await page.route("**/api/v1/releases/*/download/*/click", route => { clicks.push(new URL(route.request().url()).pathname); return route.fulfill({ status: 204 }); });
 await page.route(/\/api\/v1\/releases\/[^/]+\/download\/[^/]+$/, route => route.fulfill({ body: "installer", headers: { "Content-Type": "application/octet-stream", "Content-Disposition": 'attachment; filename="installer.bin"' } }));
 await page.goto("/releases/0.1.2");
 expect(clicks).toHaveLength(0);
 await page.getByRole("button", { name: /macOS DMG/ }).click();
 await page.getByRole("button", { name: "Отмена", exact: true }).click();
 expect(clicks).toHaveLength(0);
 for (const label of [/Windows Setup/, /macOS PKG/]) {
  const download = page.waitForEvent("download");
  await page.getByRole("link", { name: label }).click(); await download;
 }
 await page.getByRole("button", { name: /macOS DMG/ }).click();
 const download = page.waitForEvent("download");
 await page.getByRole("link", { name: "Скачать DMG", exact: true }).click(); await download;
 await expect.poll(() => clicks.length).toBe(3);
 expect(clicks.map(path => path.split("/").at(-2))).toEqual(["windows-exe", "macos-pkg", "macos-dmg"]);
});
