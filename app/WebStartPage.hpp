#pragma once

// The built-in page stays offline. Navigation and native actions are handled
// by WebBrowserPanel. The four pinned music sites stay separate from bookmarks.
inline constexpr char kWebStartPageHtml[] = R"HTML(
<!doctype html><html lang="%LANG%"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src file: data:; media-src file:; font-src vlt-font:; style-src 'unsafe-inline'; form-action https://duckduckgo.com; base-uri 'none'">
<title>VLTONE Start</title><style>%FONTS%
:root{color-scheme:dark;--bg:#191a22;--surface:#1d1e27;--hover:#252731;--line:#30323d;
--text:#f5f5f7;--muted:#a6a8b5;--accent:#c94347;--blue:#a0cdef}
*{box-sizing:border-box}html,body{margin:0;min-height:100%;background:var(--bg);color:var(--text);
font-family:Inter,system-ui,sans-serif;font-size:13px}body{min-height:100vh}
::-webkit-scrollbar{width:7px}::-webkit-scrollbar-track{background:var(--bg)}
::-webkit-scrollbar-thumb{background:#393c48;border:2px solid var(--bg);border-radius:8px}
::-webkit-scrollbar-thumb:hover{background:#626777}
button,input{font:inherit}a{color:inherit;text-decoration:none}button,a,label{ -webkit-tap-highlight-color:transparent }
button,a{touch-action:manipulation}button{cursor:pointer}svg{display:block;flex-shrink:0}
.backdrop-media,.backdrop-scrim{position:fixed;inset:0;width:100%;height:100%;pointer-events:none}
.backdrop-media{object-fit:cover}.backdrop-scrim{background:rgba(25,26,34,.82)}
.page{position:relative;width:min(100%,700px);margin:0 auto;padding:36px 28px 32px}
.hero{display:flex;flex-direction:column;align-items:center;text-align:center}
.brand{width:56px;height:56px;object-fit:contain;margin:0 0 24px;filter:drop-shadow(0 7px 12px #0003)}
.benefits{display:flex;align-items:center;justify-content:center;flex-wrap:wrap;gap:10px 22px;
color:var(--muted);font-size:11px;margin-bottom:24px}.benefits span{display:flex;gap:7px;align-items:center}
.benefits img{width:14px;height:14px}
.search{width:min(100%,488px);display:flex;align-items:center;gap:10px;padding:4px 4px 4px 17px;
min-height:46px;background:#171820;border:1px solid var(--line);border-radius:30px;text-align:left}
.search:focus-within{border-color:var(--blue)}.search>svg{width:17px;height:17px}
.search input{width:100%;min-width:0;flex:1;border:0;background:transparent;color:var(--text);outline:none;padding:8px 0}
.search input::placeholder{color:var(--muted);opacity:1}.search button{align-self:stretch;min-width:112px;
padding:9px 20px;border:1px solid #e36c6e;border-radius:24px;background:var(--accent);color:#fff;font-size:12px;font-weight:600}
.search button:hover{background:#d24e52}.search button:active{background:#aa353a}
a:focus-visible,button:focus-visible{outline:2px solid var(--blue);outline-offset:4px}
.shortcuts{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px;margin:40px 0 0}
.shortcut{min-width:0;min-height:82px;padding:12px 8px 10px;display:flex;flex-direction:column;align-items:center;
justify-content:center;gap:10px;background:var(--surface);border:1px solid transparent;border-radius:19px}
.shortcut:hover{background:var(--hover);border-color:var(--line)}.shortcut:active{background:#14151c}
.shortcut .mark{display:grid;place-items:center;flex-shrink:0;width:36px;height:36px;border-radius:50%;
background:var(--mark,#343746);color:#fff;font-size:17px;font-weight:650;box-shadow:inset 0 1px 1px #ffffff26}
.mark img,.mark svg{width:21px;height:21px}.shortcut .name{max-width:100%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:11px}
.footer{display:flex;align-items:center;justify-content:center;gap:8px;color:var(--muted);font-size:10px;margin-top:30px}
.footer svg{width:13px;height:13px}.sr-only{position:absolute;width:1px;height:1px;padding:0;margin:-1px;
overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}
@media(max-width:600px){.page{padding:32px 20px 24px}.brand{width:56px;height:56px;object-fit:contain;margin-bottom:22px}
.benefits{gap:8px 14px;font-size:10px;margin-bottom:22px}.shortcuts{margin-top:32px;gap:6px}
.shortcut{min-height:80px;border-radius:15px;padding:12px 6px;gap:8px}.shortcut .name{font-size:10px}
}
@media(max-width:380px){.page{padding:26px 14px 22px}.benefits{max-width:240px}.search{gap:8px;padding-left:12px}
.search input{font-size:12px}.search button{min-width:70px;padding:8px 12px}.shortcuts{grid-template-columns:repeat(2,minmax(0,1fr));margin-top:26px}
.shortcut{min-height:78px}.shortcut .name{font-size:11px}}
@media(prefers-reduced-motion:reduce){.backdrop-media.motion{display:none}}
</style></head><body>%MEDIA%<main class="page">
<section class="hero" aria-label="VLTONE">
<img class="brand" src="%LOGO%" alt="VLTONE" width="56" height="56">
<div class="benefits">%BENEFITS%</div>
<form class="search" role="search" action="https://duckduckgo.com/" method="get">
<svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="10.5" cy="10.5" r="6.5"/><path d="m16 16 5 5"/></svg>
<label class="sr-only" for="web-query">%SEARCH_LABEL%</label>
<input id="web-query" name="q" type="search" required autocomplete="off" placeholder="%SEARCH_PLACEHOLDER%">
<button type="submit">%SEARCH_BUTTON%</button></form>
</section>
<nav class="shortcuts" aria-label="%SHORTCUTS_LABEL%">%SHORTCUTS%</nav>
<div class="footer"><svg viewBox="0 0 24 24" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M12 3v12m-4-4 4 4 4-4M5 16v4h14v-4"/></svg>%FOOTER%</div>
</main></body></html>)HTML";
