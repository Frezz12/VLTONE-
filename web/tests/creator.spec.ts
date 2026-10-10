import { expect, test } from "@playwright/test";
import { readFileSync } from "node:fs";
import path from "node:path";
import { creatorNodes, portTypes } from "../lib/creator/catalog";
import { guides } from "../lib/creator/guides";

test("Creator reference covers the complete native registry and valid cross-links", () => {
  const ids = new Set<string>();
  for (const file of ["MiniNodeRegistry.cpp", "CreatorProgrammingNodes.cpp"]) {
    const source = readFileSync(path.join(process.cwd(), "../plugins/Internal", file), "utf8");
    for (const pattern of [/(?:add|effect)\(\s*"([a-z_0-9]+)"/g, /(?:Math|Entry)?\{\s*"([a-z_0-9]+)",\s*"[^"\n]+",\s*(?:Operation|O)::/g]) {
      for (const match of source.matchAll(pattern)) ids.add(match[1]);
    }
  }
  expect(creatorNodes.map(node => node.id).sort()).toEqual([...ids].sort());
  expect(ids.size).toBe(113);
  expect(portTypes).toHaveLength(8);
  for (const node of creatorNodes) {
    for (const locale of ["ru", "en"] as const) {
      expect(node.summary[locale].length, node.id).toBeGreaterThan(20);
      expect(node.example[locale].length, node.id).toBeGreaterThan(20);
    }
    for (const id of node.related) expect(ids.has(id), `${node.id} → ${id}`).toBe(true);
    for (const port of [...node.inputs, ...node.outputs]) expect(portTypes.some(type => type.id === port.type)).toBe(true);
    for (const parameter of node.parameters) {
      expect(parameter.initial).toBeGreaterThanOrEqual(parameter.minimum);
      expect(parameter.initial).toBeLessThanOrEqual(parameter.maximum);
    }
  }
  for (const guide of guides) for (const section of guide.sections) for (const id of section.links ?? []) expect(ids.has(id)).toBe(true);
});

for (const locale of ["ru", "en"] as const) {
  test(`${locale} Creator navigation, genuine screenshot and documentation`, async ({ page }) => {
    await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
    await page.goto("/");
    await page.getByRole("button", { name: locale === "ru" ? "Только необходимые" : "Necessary only", exact: true }).click();
    await page.locator('.vlt-nav a[href="/creator"]').click();
    await expect(page).toHaveURL(/\/creator$/);
    const link = page.locator('.vlt-nav a[href="/creator"]');
    await expect(link).toHaveAttribute("aria-current", "page");
    await expect(link.locator(".creator-new")).toHaveText("SOON");
    await expect(page.locator(".vlt-nav a")).toHaveCount(4);
    await expect(page.locator("h1")).toContainText(locale === "ru" ? "Создавай эффекты." : "Create effects.");
    await expect(page.locator('.hero-screen img')).toHaveAttribute("src", `/images/creator/editor-${locale}.webp`);
    await expect.poll(() => page.locator('.hero-screen img').evaluate((img: HTMLImageElement) => img.naturalWidth)).toBeGreaterThan(1000);
    await expect(page.locator('link[rel="canonical"]')).toHaveAttribute("href", "https://vltstudio.ru/creator");
    await page.getByRole("link", { name: locale === "ru" ? "Документация Creator" : "Creator documentation", exact: true }).click();
    await expect(page).toHaveURL(/\/creator\/docs$/);
    await expect(link).toHaveAttribute("aria-current", "page");
    await page.locator(".creator-search input").fill("variable");
    await expect(page.locator('.creator-catalog-node[href$="/history"]')).toContainText("Variable / History");
    await page.locator('.creator-catalog-node[href$="/history"]').click();
    await expect(page.locator("h1")).toHaveText("Variable / History");
    await expect(page.locator("#notes")).toContainText("Function");
    await page.locator('.creator-port-columns button').filter({ hasText: "Next" }).click();
    await expect(page.locator('[data-port-row="input-next"]')).toHaveAttribute("data-highlighted", "true");
  });

  test(`${locale} every node and guide has a server-rendered, directly accessible page`, async ({ request }) => {
    test.setTimeout(120_000);
    for (const node of creatorNodes) {
      const response = await request.get(`/creator/docs/nodes/${node.id}?lang=${locale}`);
      expect(response.status(), node.id).toBe(200);
      const html = await response.text();
      expect(html, node.id).toContain(`class="creator-node-id">${node.id}`);
      expect(html, node.id).toContain('id="parameters"');
      expect(html, node.id).toContain('id="example"');
    }
    for (const guide of guides) {
      const response = await request.get(`/creator/docs/guides/${guide.slug}?lang=${locale}`);
      expect(response.status(), guide.slug).toBe(200);
      expect(await response.text()).toContain(guide.title[locale].replaceAll("&", "&amp;"));
    }
  });
}

test("Creator catalog filters ports and categories, reports no matches and restores state from a URL", async ({ page }) => {
  await page.goto("/creator/docs?lang=en&category=Memory&q=buffer#catalog");
  await expect(page.locator(".creator-category-select select")).toHaveValue("Memory");
  await expect(page.locator(".creator-search input")).toHaveValue("buffer");
  await expect(page.locator(".creator-catalog-node")).toHaveCount(2);
  await page.locator(".creator-search input").fill("there-is-no-such-node");
  await expect(page.getByRole("heading", { name: "No matching nodes" })).toBeVisible();
  await page.getByRole("button", { name: "Clear search" }).click();
  await expect(page.locator(".creator-catalog-node")).toHaveCount(24);
  await page.getByRole("button", { name: "Show all 113 nodes" }).click();
  await expect(page.locator(".creator-catalog-node")).toHaveCount(113);
  await page.locator(".creator-category-select select").selectOption("Effects");
  await expect(page.locator(".creator-catalog-node")).toHaveCount(9);
});

test("Creator diagrams support keyboard selection, type switching, pause and reduced motion", async ({ page }) => {
  await page.goto("/creator?lang=en");
  await page.getByRole("button", { name: "No cookies", exact: true }).click();
  const graph = page.locator(".creator-demo").first();
  await graph.scrollIntoViewIfNeeded();
  await expect(graph.locator(".creator-wire-pulse").first()).toHaveCSS("animation-play-state", "running");
  await graph.locator('[data-node="lfo"]').focus();
  await page.keyboard.press("Enter");
  await expect(graph.locator('[data-node="lfo"]')).toHaveAttribute("aria-pressed", "true");
  await expect(graph.locator(".creator-demo-caption")).toContainText("Amount 0.5");
  await expect(graph.locator('[data-wire-active="true"]')).toHaveCount(2);
  await graph.getByRole("button", { name: "Audio", exact: true }).click();
  await expect(graph.locator('[data-wire-type="number"]').first()).toHaveAttribute("opacity", "0.1");
  await page.locator('.creator-type-options button').filter({ hasText: "Function" }).click();
  await expect(page.locator(".creator-port-explanation")).toContainText("same signature");
  await page.getByRole("button", { name: "Pause animation" }).click();
  await expect(page.locator("main")).toHaveAttribute("data-motion-paused", "true");
  await expect(graph.locator(".creator-wire-pulse").first()).toHaveCSS("animation-play-state", "paused");
  await page.emulateMedia({ reducedMotion: "reduce" });
  await expect(graph.locator(".creator-wire-pulse").first()).toHaveCSS("animation-name", "none");
  await expect(page.locator(".ambient-motion-control")).toBeHidden();
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await page.goto("/creator/docs?lang=en");
  await graph.scrollIntoViewIfNeeded();
  await expect(graph.locator(".creator-wire-pulse").first()).toHaveCSS("animation-play-state", "running");
  await expect(page.getByRole("button", { name: "Pause animation" })).toBeInViewport();
  await page.getByRole("button", { name: "Pause animation" }).click();
  await expect(graph.locator(".creator-wire-pulse").first()).toHaveCSS("animation-play-state", "paused");
});

test("Colored chapters alternate, and every wire stays attached to its actual ports", async ({ page }) => {
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto("/creator?lang=en");
  await expect(page.locator(".creator-chapter")).toHaveCount(11);
  await expect(page.locator(".creator-section-wire")).toHaveCount(10);
  const chapters = await page.locator(".creator-chapter").evaluateAll(elements => elements.map(element => ({ x: element.getBoundingClientRect().x, color: getComputedStyle(element).backgroundColor })));
  expect(chapters[0].x).toBeLessThan(chapters[1].x);
  expect(chapters[2].x).toBe(chapters[0].x);
  expect(new Set(chapters.map(chapter => chapter.color)).size).toBe(6);
  for (const width of [1440, 768, 375, 320]) {
    await page.setViewportSize({ width, height: 1000 });
    await expect.poll(async () => page.locator(".creator-section-wires").evaluate(svg => {
      const box = svg.getBoundingClientRect();
      const chapters = [...svg.parentElement!.querySelectorAll(".creator-chapter")];
      return [...svg.querySelectorAll<SVGPathElement>(".creator-section-wire")].every((path, index) => {
        const start = path.getPointAtLength(0), end = path.getPointAtLength(path.getTotalLength());
        return [[chapters[index], "output", start], [chapters[index + 1], "input", end]].every(([section, side, point]) => {
          const port = (section as Element).querySelector(`[data-section-port="${side}"]`)!.getBoundingClientRect();
          const p = point as DOMPoint;
          return Math.abs(p.x + box.x - port.x - port.width / 2) < 3 && Math.abs(p.y + box.y - port.y - port.height / 2) < 3;
        });
      });
    })).toBe(true);
    await expect.poll(async () => page.locator(".creator-wires").evaluate(svg => {
      const box = svg.getBoundingClientRect();
      return [...svg.querySelectorAll<SVGGElement>("g[data-from]")].every(edge => {
        const path = edge.querySelector<SVGPathElement>(".creator-wire-base")!;
        return [[edge.dataset.from, path.getPointAtLength(0)], [edge.dataset.to, path.getPointAtLength(path.getTotalLength())]].every(([id, point]) => {
          const port = svg.parentElement!.querySelector(`[data-demo-port="${id}"]`)!.getBoundingClientRect();
          const p = point as DOMPoint;
          return Math.abs(p.x + box.x - port.x - port.width / 2) < 2 && Math.abs(p.y + box.y - port.y - port.height / 2) < 2;
        });
      });
    })).toBe(true);
  }
});

test("Modulation, memory and workflow examples respond and ambient motion stops off screen", async ({ page }) => {
  await page.goto("/creator?lang=en");
  await page.getByRole("button", { name: "No cookies", exact: true }).click();
  const lab = page.locator(".creator-modulation-lab");
  await lab.scrollIntoViewIfNeeded();
  await lab.getByRole("slider", { name: "Rate" }).press("ArrowRight");
  await lab.getByRole("slider", { name: "Depth" }).press("ArrowRight");
  await expect(lab).toContainText("1.9 Hz");
  await expect(lab).toContainText("36%");
  await expect(lab.locator(".creator-scope-playhead")).toHaveCSS("animation-play-state", "running");
  const memory = page.locator("#memory");
  await memory.getByRole("button", { name: "Next sample" }).click();
  await expect(memory.locator(".creator-memory-cell strong").first()).toHaveText("0.1");
  await memory.getByRole("button", { name: "Reset memory" }).click();
  await expect(memory.locator(".creator-memory-cell strong").first()).toHaveText("0.0");
  await expect(lab.locator(".creator-scope-playhead")).toHaveCSS("animation-play-state", "paused");
  await page.locator("#ai").getByRole("button", { name: "02 Edits" }).click();
  await expect(page.locator("#ai .creator-process-detail")).toContainText("Changes appear in the actual graph");
  await page.locator("#compile").getByRole("button", { name: "03 VLTone" }).click();
  await expect(page.locator("#compile .creator-process-detail")).toContainText(".vltmini → Channel Strip");
  await page.getByRole("button", { name: "Pause animation" }).click();
  await expect(page.locator("#compile .creator-process-meter i")).toHaveCSS("animation-play-state", "paused");
  await page.getByRole("button", { name: "Resume animation" }).click();
  await expect(page.locator("#compile .creator-process-meter i")).toHaveCSS("animation-play-state", "running");
});

for (const width of [320, 375, 768, 1024, 1440]) {
  test(`Creator and documentation fit ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 1000 });
    for (const url of ["/creator", "/creator/docs", "/creator/docs/guides/ports", "/creator/docs/nodes/chorus"]) {
      await page.goto(url);
      expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), url).toBe(true);
      const nav = await page.locator(".vlt-nav").boundingBox();
      expect(nav!.x + nav!.width).toBeLessThanOrEqual(width);
      await expect(page.locator('a.creator-nav-link')).toBeVisible();
    }
  });
}

test("Creator tap targets, enlarged text, anchor placement and browser errors", async ({ browser }) => {
  const context = await browser.newContext({ viewport: { width: 375, height: 900 }, hasTouch: true, isMobile: true });
  const page = await context.newPage();
  const errors: string[] = [];
  page.on("pageerror", error => errors.push(error.message));
  await page.goto("http://127.0.0.1:3100/creator?lang=en");
  await page.locator('[data-node="mix"]').tap();
  await expect(page.locator('[data-node="mix"]')).toHaveAttribute("aria-pressed", "true");
  await page.locator('.creator-type-options button').filter({ hasText: "Buffer" }).tap();
  await expect(page.locator(".creator-port-explanation h3")).toHaveText("Buffer");
  await page.setViewportSize({ width: 320, height: 900 });
  for (const path of ["/creator", "/creator/docs", "/creator/docs/guides/ports", "/creator/docs/nodes/history"]) {
    await page.goto(`http://127.0.0.1:3100${path}?lang=en`);
    await page.evaluate(() => {
      const sizes = [...document.querySelectorAll<HTMLElement>("p,span,a,button,li,label,code,pre,h1,h2,h3,small,dt,dd,th,td,summary")].map(element => [element, parseFloat(getComputedStyle(element).fontSize)] as const);
      sizes.forEach(([element, size]) => element.style.setProperty("font-size", `${size * 2}px`, "important"));
    });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `${path} at 200% text`).toBe(true);
  }
  await page.goto("http://127.0.0.1:3100/creator/docs/guides/ports#feedback");
  await expect.poll(() => page.locator("#feedback").evaluate(el => el.getBoundingClientRect().top >= document.querySelector(".vlt-topbar")!.getBoundingClientRect().bottom)).toBe(true);
  expect(errors).toEqual([]);
  await context.close();
});

test("Creator discovery, downloads and unknown pages", async ({ request }) => {
  const sitemap = await request.get("/sitemap.xml");
  const xml = await sitemap.text();
  for (const path of ["/creator", "/creator/docs", "/creator/docs/nodes/lfo", "/creator/docs/guides/cpp"]) expect(xml).toContain(`https://vltstudio.ru${path}</loc>`);
  expect((await request.get("/creator/docs/nodes/does-not-exist")).status()).toBe(404);
  expect((await request.get("/creator/docs/guides/does-not-exist")).status()).toBe(404);
  expect((await request.get("/creator/downloads/Gentle-Tremolo.vltcreator")).status()).toBe(200);
  expect((await request.get("/creator/downloads/creator-sdk-ru.md")).status()).toBe(200);
});
