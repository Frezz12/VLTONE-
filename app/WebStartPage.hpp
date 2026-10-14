#pragma once

// Offline, script-free start page. All chrome colours come from the active Qt theme.
inline constexpr char kWebStartPageHtml[] = R"HTML(
<!doctype html><html lang="%LANG%"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src file: data:; media-src file:; font-src vlt-font:; style-src 'unsafe-inline'; form-action https://duckduckgo.com; base-uri 'none'">
<title>VLTONE Start</title><style>%FONTS%
:root{%PALETTE%}
*{box-sizing:border-box}html,body{margin:0;min-height:100%;background:var(--bg);color:var(--text);font-family:Inter,system-ui,sans-serif;font-size:13px;line-height:1.5}
body{min-height:100vh}button,input{font:inherit}a{color:inherit;text-decoration:none}button,a{touch-action:manipulation}button{cursor:pointer}svg{flex-shrink:0}
::-webkit-scrollbar{width:8px}::-webkit-scrollbar-track{background:var(--bg)}::-webkit-scrollbar-thumb{background:var(--line);border:2px solid var(--bg);border-radius:8px}
a:focus-visible,button:focus-visible{outline:2px solid var(--accent);outline-offset:3px}
.backdrop-media,.backdrop-scrim{position:fixed;inset:0;width:100%;height:100%;pointer-events:none}.backdrop-media{object-fit:cover}.backdrop-scrim{background:var(--scrim)}
.page{position:relative;max-width:780px;margin:0 auto;padding:32px clamp(16px,5vw,42px) 28px}
.brandline{display:flex;gap:12px;align-items:center;margin-bottom:36px}.brand{width:36px;height:36px;object-fit:contain}
.brandname{font-size:13px;font-weight:650;letter-spacing:.12em}.edition{color:var(--muted);font-size:11px}.status{margin-left:auto;padding:3px 8px;border:1px solid var(--line);border-radius:6px;color:var(--muted);font-size:10px;letter-spacing:.05em}
h1{font-size:clamp(24px,4.5vw,34px);line-height:1.18;font-weight:600;letter-spacing:-.035em;margin:0 0 10px}.subtitle{color:var(--muted);margin:0 0 24px;max-width:470px;font-size:12px}
.search{display:flex;align-items:center;gap:12px;width:100%;min-height:48px;padding:5px 5px 5px 14px;border:1px solid var(--line);border-radius:8px;background:var(--well);box-shadow:inset 0 1px 2px #0000000d}
.search:focus-within{border-color:var(--accent);outline:1px solid var(--accent)}.search>svg{width:18px;height:18px;color:var(--muted)}
.search input{min-width:0;width:100%;flex:1;outline:none;border:0;background:transparent;color:var(--text);padding:6px 0}.search input::placeholder{color:var(--muted);opacity:1}
.search button{min-height:36px;padding:7px 16px;border:1px solid transparent;border-radius:6px;background:var(--accent);color:var(--accent-ink);font-size:12px;font-weight:600}.search button:hover{filter:brightness(1.08)}.search button:active{filter:brightness(.94)}
.section-head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:28px 0 10px}.section-head h2{margin:0;font-size:12px;font-weight:600}.section-head span{color:var(--muted);font-size:10px}
.shortcuts{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px}.shortcut{display:flex;flex-direction:column;align-items:flex-start;gap:16px;min-width:0;padding:14px 12px 12px;background:linear-gradient(var(--top),var(--surface));border:1px solid var(--line);border-radius:8px;box-shadow:inset 0 1px var(--edge)}
.shortcut:hover,.saved-link:hover{border-color:var(--accent);background:var(--hover)}.shortcut:active,.saved-link:active{background:var(--well)}
.mark{display:grid;place-items:center;width:30px;height:30px;border:1px solid var(--line);border-radius:7px;background:var(--well)}.mark img,.mark svg{width:18px;height:18px}
.shortcut .name{max-width:100%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:11px;font-weight:500}
.saved-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:6px}.saved-link{min-width:0;display:flex;gap:10px;align-items:center;padding:10px 12px;border:1px solid var(--line);border-radius:8px;background:var(--surface)}
.saved-link>span:first-child{display:grid;place-items:center;width:26px;height:26px;flex-shrink:0;background:var(--well);border-radius:6px;color:var(--accent);font-weight:600}.saved-copy{min-width:0;display:block}.saved-copy strong,.saved-copy small{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.saved-copy strong{font-size:12px;font-weight:500}.saved-copy small{color:var(--muted);font-size:10px}
.footer{display:flex;align-items:flex-start;gap:10px;margin-top:28px;padding:14px;border:1px solid var(--line);border-radius:8px;color:var(--muted);font-size:11px;background:var(--surface)}.footer svg{width:18px;height:18px;color:var(--accent)}.footer strong{display:block;color:var(--text);font-weight:500;margin-bottom:3px}
.keys{display:flex;flex-wrap:wrap;gap:8px 16px;margin-top:16px;color:var(--muted);font-size:10px}.keys span{display:flex;align-items:center;gap:6px}kbd{font:inherit;color:var(--text);border:1px solid var(--line);border-radius:4px;padding:1px 5px;background:var(--well)}
.sr-only{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}
@media(max-width:420px){.page{padding-top:22px}.brandline{margin-bottom:28px}.shortcuts{grid-template-columns:repeat(2,minmax(0,1fr))}.shortcut{gap:9px;padding:12px;min-height:88px}.saved-grid{grid-template-columns:1fr}.status{display:none}.search{gap:8px;padding-left:10px}.search button{padding:7px 12px}.section-head span{display:none}}
@media(prefers-reduced-motion:reduce){.backdrop-media.motion{display:none}}
</style></head><body>%MEDIA%<main class="page">
<header class="brandline"><img class="brand" src="%LOGO%" alt="" width="36" height="36"><div><div class="brandname">VLTONE</div><div class="edition">%EDITION%</div></div><span class="status">%SPACE%</span></header>
<section aria-label="%SEARCH_LABEL%"><h1>%HEADING%</h1><p class="subtitle">%SUBTITLE%</p>
<form class="search" role="search" action="https://duckduckgo.com/" method="get">
<svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="10.5" cy="10.5" r="6.5"/><path d="m16 16 5 5"/></svg>
<label class="sr-only" for="web-query">%SEARCH_LABEL%</label><input id="web-query" name="q" type="search" required autocomplete="off" placeholder="%SEARCH_PLACEHOLDER%"><button type="submit">%SEARCH_BUTTON%</button></form></section>
<div class="section-head"><h2>%SHORTCUTS_LABEL%</h2><span>%MUSIC_LABEL%</span></div><nav class="shortcuts" aria-label="%SHORTCUTS_LABEL%">%SHORTCUTS%</nav>
%SAVED%%RECENT%
<div class="footer"><svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M12 3v12m-4-4 4 4 4-4M5 16v4h14v-4"/></svg><div><strong>%IMPORT_TITLE%</strong>%FOOTER%</div></div>
<div class="keys"><span><kbd>Ctrl+L</kbd>%KEY_ADDRESS%</span><span><kbd>Ctrl+T</kbd>%KEY_TAB%</span><span><kbd>Ctrl+J</kbd>%KEY_DOWNLOADS%</span></div>
</main></body></html>)HTML";
