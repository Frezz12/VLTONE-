import { expect, test } from "@playwright/test";

for (const locale of ["ru", "en"]) {
  test(`${locale} homepage exposes product metadata and the complete beta path`, async ({ page, request }) => {
    await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
    await page.goto("/");
    await expect(page).toHaveTitle(/VLTone.*(?:Открытая бета|Open beta)/);
    await expect(page.locator("h1")).toContainText(locale === "ru" ? "Дай форму своему звуку." : "Give shape to your sound.");
    await expect(page.locator(".hero-announcement")).toHaveCount(0);
    await expect(page.locator(".hero-topline")).toHaveCount(0);
    await expect(page.locator(".vlt-brand .brand-mark img")).toBeVisible();
    await expect(page.locator(".vlt-brand .brand-mark img")).toHaveCSS("object-fit", "contain");
    await expect(page.locator(".vlt-brand .brand-mark")).toHaveCSS("background-color", "rgba(0, 0, 0, 0)");
    await expect(page.locator('.cta-sound')).toHaveCount(0);
    await expect(page.locator('link[rel="icon"]')).toHaveAttribute("href", "/icon.png");
    await expect(page.locator('meta[name="description"]')).toHaveAttribute("content", /VLTone.*(?:DAW|digital audio workstation)/);
    await expect(page.locator('link[rel="canonical"]')).toHaveAttribute("href", "https://vltstudio.ru");
    await expect(page.locator('meta[property="og:site_name"]')).toHaveAttribute("content", "VLTone");
    await expect(page.locator('link[rel="alternate"][hreflang]')).toHaveCount(0);
    const data = JSON.parse(await page.locator('script[type="application/ld+json"]').innerText());
    expect(data["@graph"].find((item: { "@type": string }) => item["@type"] === "SoftwareApplication")).toMatchObject({ name: "VLTone", applicationCategory: "MultimediaApplication", operatingSystem: "Windows, macOS" });

    const steps = page.locator(".beta-steps li");
    await expect(steps).toHaveCount(3);
    await expect(steps.nth(0).getByRole("link")).toHaveAttribute("href", "/releases");
    await expect(steps.nth(1).getByRole("link")).toHaveAttribute("href", "/register");
    await steps.nth(2).getByRole("link").click();
    await expect(page).toHaveURL(/\/manual#first-launch$/);
    await expect(page.locator("#first-launch")).toBeInViewport();
    await page.goto("/");
    await page.locator(".faq-list summary").first().focus();
    await page.keyboard.press("Enter");
    await expect(page.locator(".faq-list details").first()).toHaveAttribute("open", "");
    await page.keyboard.press("Enter");
    await expect(page.locator(".faq-list details").first()).not.toHaveAttribute("open");
    await page.locator(".hero-copy-panel").getByRole("link").first().click();
    await expect(page).toHaveURL(/\/releases$/);
    await expect(page.locator("h1")).toBeVisible();
    const ogImage = await request.get("/opengraph-image");
    expect(ogImage.status()).toBe(200);
    expect(ogImage.headers()["content-type"]).toContain("image/png");
  });
}

for (const width of [320, 375, 768, 1440]) {
  test(`homepage fits ${width}px with accessible product navigation`, async ({ page }) => {
    await page.setViewportSize({ width, height: 900 });
    await page.goto("/");
    await expect(page.locator(".hero-product .product-shot img")).toBeVisible();
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    await expect(page.locator(".workflow-card")).toHaveCount(3);
    await expect(page.locator(".gallery-switcher button")).toHaveCount(4);
    await expect(page.locator(".vlt-nav")).toBeVisible();
    await expect(page.locator(".vlt-nav a")).toHaveCount(4);
    await expect(page.locator(".locale-link")).toBeVisible();
    await expect(page.getByRole("link", { name: "Аккаунт", exact: true })).toBeVisible();
    const header = await page.evaluate(() => {
      const brand = document.querySelector(".vlt-brand")!.getBoundingClientRect();
      const nav = document.querySelector(".vlt-nav")!.getBoundingClientRect();
      const actions = document.querySelector(".header-navigation")!.getBoundingClientRect();
      return { brandLeft: brand.left, brandRight: brand.right, brandBottom: brand.bottom, navLeft: nav.left, navRight: nav.right, navTop: nav.top, actionsLeft: actions.left, actionsRight: actions.right };
    });
    expect(header.navLeft).toBeLessThan(width / 2);
    expect(header.actionsRight).toBeLessThanOrEqual(width - 16);
    if (width <= 580) {
      expect(header.navTop).toBeGreaterThanOrEqual(header.brandBottom);
      expect(header.navLeft).toBeCloseTo(header.brandLeft, 0);
    } else {
      expect(header.navLeft).toBeGreaterThanOrEqual(header.brandRight);
      expect(header.actionsLeft).toBeGreaterThan(header.navRight);
    }
  });
}

test("homepage respects reduced motion", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/");
  await expect(page.locator(".hero-product")).toHaveCSS("transform", "none");
  await expect(page.locator(".hero-art")).toHaveCSS("animation-name", "none");
  await expect(page.locator(".cta-phrase .phrase-hit").first()).toHaveCSS("animation-name", "none");
  await expect(page.locator(".hero-editorial h1 > span").first()).toHaveCSS("animation-name", "none");
  await expect(page.locator(".hero-motion-footer .signal-bar").first()).toHaveCSS("animation-name", "none");
  await expect(page.locator(".ambient-motion-control")).toBeHidden();
  await expect(page.locator(".hero-screen video")).not.toHaveAttribute("src");
  await expect(page.locator(".hero-screen video")).toHaveAttribute("poster", /showcase-instrumental/);

});

test("search discovery files expose clean canonical routes", async ({ request }) => {
  const robots = await request.get("/robots.txt");
  expect(await robots.text()).toContain("Sitemap: https://vltstudio.ru/sitemap.xml");
  const sitemap = await request.get("/sitemap.xml");
  const xml = await sitemap.text();
  for (const path of ["", "/capabilities", "/manual", "/releases"]) expect(xml).toContain(`<loc>https://vltstudio.ru${path}</loc>`);
  expect(xml).not.toMatch(/vltstudio\.ru\/(ru|en)(\/|<)/);
  expect(xml).not.toContain("/account");
});

for (const locale of ["ru", "en"]) {
  test(`${locale} capabilities page describes every current area and links to the manual`, async ({ page }) => {
    await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
    await page.goto("/capabilities");
    await expect(page).toHaveTitle(/Возможности VLTone|VLTone capabilities/);
    await expect(page.locator(".capabilities-beta")).toContainText(locale === "ru" ? "Открытая бета" : "Open beta");
    await expect(page.locator(".capability-detail")).toHaveCount(6);
    for (const id of ["recording", "midi", "mixing", "plugins", "ai", "recovery"]) {
      const section = page.locator(`#${id}`);
      await expect(section).toBeVisible();
      await section.locator("summary").click();
      await expect(section.locator("li")).toHaveCount(3);
      await expect(section.locator(".text-link")).toHaveAttribute("href", /\/manual#/);
    }
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  });
}

for (const width of [320, 768, 1024, 1440]) {
  test(`capabilities layout keeps text and screenshots separated at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 900 });
    await page.goto("/capabilities");
    const layout = await page.evaluate(() => {
      const hero = document.querySelector(".capabilities-hero")!.getBoundingClientRect();
      const heading = document.querySelector(".capabilities-hero h1")!;
      const sections = Array.from(document.querySelectorAll(".capability-primary"), section => {
        const copy = section.querySelector(".capability-copy")!.getBoundingClientRect();
        const image = section.querySelector(".capability-visual")!.getBoundingClientRect();
        return { textRight: copy.right, textBottom: copy.bottom, imageLeft: image.left, imageRight: image.right, imageTop: image.top };
      });
      return { left: hero.left, right: hero.right, headingSize: parseFloat(getComputedStyle(heading).fontSize), sections };
    });
    expect(layout.left).toBeGreaterThanOrEqual(16);
    expect(layout.right).toBeLessThanOrEqual(width - 16);
    expect(layout.headingSize).toBeGreaterThanOrEqual(38);
    for (const section of layout.sections) {
      expect(section.imageRight).toBeLessThanOrEqual(width - 16);
      if (width > 900) expect(section.imageLeft - section.textRight).toBeGreaterThanOrEqual(24);
      else expect(section.imageTop - section.textBottom).toBeGreaterThanOrEqual(20);
    }

    await page.locator('.capabilities-toc a[href="#mixing"]').click();
    await expect(page.locator("#mixing-title")).toBeInViewport();
    await expect.poll(() => page.evaluate(() => {
      const heading = document.querySelector("#mixing-title")!.getBoundingClientRect();
      const header = document.querySelector(".vlt-topbar")!.getBoundingClientRect();
      return heading.top >= header.bottom && heading.bottom <= innerHeight;
    })).toBe(true);
    const shot = page.locator("#mixing .product-shot");
    await shot.click();
    await expect(page.locator("dialog[open]")).toBeVisible();
    await page.keyboard.press("Escape");
    await expect(page.locator("dialog[open]")).toHaveCount(0);
    await expect(shot).toBeFocused();
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  });
}

test("language preference persists without changing the URL", async ({ page }) => {
  await page.goto("/en/manual");
  await expect(page).toHaveURL(/\/manual\?lang=en$/);
  await page.getByRole("button", { name: "Necessary only" }).click();
  await expect(page).toHaveURL(/\/manual$/);
  await expect(page.getByRole("heading", { name: "VLTone Manual" })).toBeVisible();
  // Consent reloads the page after removing ?lang; wait for the new header
  // to hydrate before sending its first locale-switch click.
  await page.waitForLoadState("networkidle");
  await page.getByRole("button", { name: "Открыть на русском" }).click();
  await expect(page).toHaveURL(/\/manual$/);
  await expect(page.getByRole("heading", { name: "Инструкция VLTone" })).toBeVisible();
  await page.reload();
  await expect(page.getByRole("heading", { name: "Инструкция VLTone" })).toBeVisible();
});


test("gallery switches on demand and enlargement restores keyboard focus", async ({ page }) => {
  await page.goto("/");
  const piano = page.locator(".gallery-switcher").getByRole("button", {name:"Piano Roll",exact:true});
  await piano.focus(); await page.keyboard.press("Enter");
  await expect(piano).toHaveAttribute("aria-pressed","true");
  await expect(page.locator(".hero-product .product-shot img")).toHaveAttribute("src",/showcase-midi-ru.webp/);
  expect(await page.locator(".hero-product picture").evaluate(element => element.getAnimations().length)).toBe(0);
  const enlarge = page.locator(".hero-product .product-shot");
  await enlarge.focus(); await page.keyboard.press("Enter");
  await expect(page.locator("dialog[open]")).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(page.locator("dialog[open]")).toHaveCount(0);
  await expect(enlarge).toBeFocused();
});

test("gallery playhead drags between views and responds immediately to keyboard navigation", async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("vlt-cookie-preference-v1", "none"));
  await page.goto("/");
  const playhead = page.getByRole("slider", { name: "Переключить экран программы" });
  await playhead.scrollIntoViewIfNeeded();
  const bounds = await playhead.boundingBox();
  if (!bounds) throw new Error("Gallery playhead is missing");
  await page.mouse.move(bounds.x + 8, bounds.y + bounds.height / 2);
  await page.mouse.down();
  await page.mouse.move(bounds.x + bounds.width - 8, bounds.y + bounds.height / 2, { steps: 16 });
  await page.mouse.up();
  await expect(playhead).toHaveValue("3");
  await expect(page.locator(".hero-product .product-shot img")).toHaveAttribute("src", /showcase-eq-ru.webp/);
  await expect(page.locator('.gallery-channel[aria-pressed="true"]')).toHaveCount(1);
  await playhead.focus();
  await page.keyboard.press("ArrowLeft");
  await expect(playhead).toHaveValue("2");
  await expect(page.locator(".hero-product .product-shot img")).toHaveAttribute("src", /showcase-mix-ru.webp/);
  expect(await page.locator(".hero-product picture").evaluate(element => element.getAnimations().length)).toBe(0);
  await page.keyboard.press("Home");
  await expect(playhead).toHaveValue("0");
  await expect(page.locator('.gallery-channel[aria-pressed="true"]')).toHaveAttribute("data-view", "0");
});

test("homepage keeps its product image and download path without JavaScript", async ({ browser }) => {
  const context = await browser.newContext({ javaScriptEnabled: false });
  const page = await context.newPage();
  try {
    await page.goto("http://127.0.0.1:3100/");
    await expect(page.locator("h1")).toContainText("Дай форму своему звуку.");
    await expect(page.locator(".hero-art video")).toBeVisible();
    await expect(page.locator(".hero-copy-panel .vlt-button")).toHaveAttribute("href", "/releases");
    await expect(page.locator(".workflow-copy")).toHaveCount(3);
    await expect(page.locator(".hero-motion-footer .signal-bar").first()).toHaveCSS("animation-play-state", "paused");
    await expect(page.locator(".ambient-motion-control")).toBeHidden();
    await expect(page.locator(".hero-screen video")).not.toHaveAttribute("src");
    await expect(page.locator(".hero-screen video")).toHaveAttribute("poster", /showcase-instrumental/);

  } finally {
    await context.close();
  }
});

test("ambient motion runs without scrolling and can pause and resume", async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("vlt-cookie-preference-v1", "none"));
  await page.goto("/");
  const wave = page.locator(".hero-motion-footer .signal-bar").first();
  await expect(wave).toHaveCSS("animation-play-state", "running");
  const firstPosition = await wave.evaluate(element => getComputedStyle(element).transform);
  await expect.poll(() => wave.evaluate(element => getComputedStyle(element).transform)).not.toBe(firstPosition);
  expect(await page.evaluate(() => scrollY)).toBe(0);

  await page.getByRole("button", { name: "Приостановить анимацию" }).click();
  await expect(wave).toHaveCSS("animation-play-state", "paused");
  // Allow the compositor to deliver the pause before checking the frozen frame.
  await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
  const pausedPosition = await wave.evaluate(element => getComputedStyle(element).transform);
  await page.waitForTimeout(180);
  expect(await wave.evaluate(element => getComputedStyle(element).transform)).toBe(pausedPosition);
  await page.getByRole("button", { name: "Продолжить анимацию" }).click();
  await expect(wave).toHaveCSS("animation-play-state", "running");

  const feature = page.locator('.workflow-card[data-track="1"]');
  await feature.scrollIntoViewIfNeeded();
  await expect(wave).toHaveCSS("animation-play-state", "paused");
  await expect(feature.locator(".loop-motion").first()).toHaveCSS("animation-play-state", "running");

  const outro = page.locator(".studio-cta");
  await outro.scrollIntoViewIfNeeded();
  await expect(outro.locator(".phrase-cursor")).toHaveCSS("animation-play-state", "running");
  const outroPosition = await outro.locator(".phrase-cursor").evaluate(element => getComputedStyle(element).transform);
  await expect.poll(() => outro.locator(".phrase-cursor").evaluate(element => getComputedStyle(element).transform)).not.toBe(outroPosition);
  await page.emulateMedia({ reducedMotion: "reduce" });
  await expect(feature.locator(".loop-motion").first()).toHaveCSS("animation-name", "none");
  await expect(outro.locator(".phrase-cursor")).toHaveCSS("animation-name", "none");
});

for (const locale of ["ru", "en"]) {
  test(`hero video plays, loops and follows the motion toggle in ${locale}`, async ({ page }) => {
    await page.addInitScript(() => localStorage.setItem("vlt-cookie-preference-v1", "none"));
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
    await page.goto("/");
    const video = page.locator(".hero-screen video");
    await expect(video).toHaveAttribute("src", new RegExp(`/videos/studio-playback-${locale}\\.mp4\\?v=`));
    await expect.poll(() => video.evaluate((v: HTMLVideoElement) => v.currentTime)).toBeGreaterThan(0);
    expect(await video.evaluate((v: HTMLVideoElement) => v.muted && v.loop && v.playsInline)).toBe(true);
    await page.locator(".ambient-motion-control").click();
    await expect.poll(() => video.evaluate((v: HTMLVideoElement) => v.paused)).toBe(true);
    await page.locator(".ambient-motion-control").click();
    await expect.poll(() => video.evaluate((v: HTMLVideoElement) => v.paused)).toBe(false);
    await video.evaluate((v: HTMLVideoElement) => { v.currentTime = v.duration - 0.15; });
    await expect.poll(() => video.evaluate((v: HTMLVideoElement) => v.currentTime)).toBeLessThan(2);
    await page.locator("footer").scrollIntoViewIfNeeded();
    await expect.poll(() => video.evaluate((v: HTMLVideoElement) => v.paused)).toBe(true);
  });
}
