import { expect, test } from "@playwright/test";

test("actual page opens are counted without query tokens or analytics storage", async ({ page }) => {
  const visits: { id: string; path: string; referrer: string }[] = [];
  await page.route("**/api/v1/site-visits", async route => { visits.push(route.request().postDataJSON()); await route.fulfill({ status: 204 }); });
  await page.goto("/login?token=do-not-store");
  await expect.poll(() => visits.length).toBe(1);
  expect(visits[0].path).toBe("/login");
  expect(JSON.stringify(visits)).not.toContain("do-not-store");
  await page.reload();
  await expect.poll(() => visits.length).toBe(2);
  expect(visits[0].id).not.toBe(visits[1].id);
  // Next Link navigation records one more view without reloading the layout.
  await page.getByRole("button", { name: "Только необходимые", exact: true }).click();
  await page.locator('a[href="/register"]:visible').first().click();
  await expect.poll(() => visits.length).toBe(3);
  expect(visits[2].path).toBe("/register");
  expect((await page.context().cookies()).filter(cookie => /visit|analytic/i.test(cookie.name))).toEqual([]);
});

test("do not track suppresses visits", async ({ page }) => {
  await page.addInitScript(() => Object.defineProperty(navigator, "doNotTrack", { value: "1" }));
  let requests = 0;
  await page.route("**/api/v1/site-visits", async route => { requests++; await route.fulfill({ status: 204 }); });
  await page.goto("/login");
  await expect(page.getByRole("button", { name: /только необходимые|necessary only/i })).toBeVisible();
  expect(requests).toBe(0);
});
