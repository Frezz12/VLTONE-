'use strict';
(() => {
  const $ = id => document.getElementById(id);
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  let bridge, parameters, divisions, values = [], revision = 0, menuIndex = 0, menuSource, timeControl;
  const controls = new Map(), active = new Set(), pending = new Map(), wheelTimers = new Map();
  const descriptions = ['Pure, open repeats', 'Narrow band · soft saturation', 'Broadcast tone · compression', 'Metallic · frequency modulation', 'Warm saturation · gentle drift', 'Soft clipping · rich harmonics', '10-bit texture · 12 kHz'];
  const colours = ['#c3aed8', '#f19c9f', '#9dcfc7', '#cdd783'];
  const logarithmic = i => [1, 10, 11, 13].includes(i);
  const fraction = (i, v) => { const p = parameters[i]; return logarithmic(i) ? Math.log(v / p.min) / Math.log(p.max / p.min) : (v - p.min) / (p.max - p.min); };
  const plain = (i, f) => { const p = parameters[i]; f = clamp(f, 0, 1); const v = logarithmic(i) ? p.min * (p.max / p.min) ** f : p.min + (p.max - p.min) * f; return p.stepped ? Math.round(v) : v; };
  const format = (i, v) => i === 2 ? divisions[Math.round(v)] : v.toFixed([10, 11, 12].includes(i) ? 0 : i === 13 ? 2 : 1);
  const timeIndex = () => values[0] === 2 ? 1 : 2;
  function display(control, i) {
    const p = parameters[i], v = values[i];
    control.dial.style.setProperty('--angle', `${-135 + 270 * fraction(i, v)}deg`);
    for (const [key, value] of Object.entries({'valuemin':p.min, 'valuemax':p.max, 'valuenow':v, 'valuetext':`${format(i, v)} ${p.unit}`})) control.dial.setAttribute(`aria-${key}`, value);
    if (control.input && document.activeElement !== control.input) {
      const text = format(i, v); control.input.value = text;
      control.input.style.width = `${Math.max(2, text.length) + .2}ch`;
    }
    if (control.output) control.output.textContent = `${format(i, v)}${i === 1 ? ' ms' : ''}`;
  }
  function paint(i) {
    if (controls.has(i)) display(controls.get(i), i);
    if ([0, 1, 2].includes(i)) {
      display(timeControl, timeIndex());
      const ms = values[0] === 2;
      $('division').hidden = ms; $('timeMs-value').hidden = !ms;
      $('division').value = Math.round(values[2]);
      if (document.activeElement !== $('timeMs-value')) $('timeMs-value').value = format(1, values[1]);
      ['host', 'local', 'milliseconds'].forEach((id, n) => $(id).setAttribute('aria-pressed', values[0] === n));
      $('bpm-value').disabled = values[0] !== 1;
      $('tap').disabled = values[0] === 0;
      $('tap-hint').textContent = values[0] === 0 ? 'HOST SYNC' : 'TAP TEMPO';
      $('tap').title = values[0] === 0 ? 'Host controls tempo. Select BPM or ms to tap.' : 'Tap a steady rhythm';
      $('tempo-unit').textContent = values[0] === 0 ? 'BPM · HOST' : values[0] === 1 ? 'BPM · LOCAL' : 'BPM';
    }
    if (i === 4) { $('stereo').setAttribute('aria-pressed', values[i] === 0); $('pingPong').setAttribute('aria-pressed', values[i] === 1); }
    if (i === 8) { $('character').value = Math.round(values[i]); $('character-description').textContent = descriptions[Math.round(values[i])]; }
    if (i === 3 && values[0] === 1 && document.activeElement !== $('bpm-value')) $('bpm-value').value = values[3].toFixed(1);
  }
  function edit(i, v, finished = false) {
    if (!bridge || !Number.isFinite(v)) return;
    const p = parameters[i]; v = clamp(v, p.min, p.max); values[i] = p.stepped ? Math.round(v) : v;
    paint(i); pending.set(i, ++revision); bridge.edit(i, values[i], finished, revision);
  }
  function finish(i) { active.delete(i); clearTimeout(wheelTimers.get(i)); wheelTimers.delete(i); if (bridge) bridge.finish(i); }
  function finishAll() { for (const i of [...active]) finish(i); }
  function showMenu(e, i) {
    e.preventDefault(); finish(i); menuIndex = i; menuSource = e.currentTarget;
    const rect = menuSource.getBoundingClientRect(), menu = $('menu'); menu.hidden = false;
    menu.style.left = `${clamp(e.clientX || rect.left, 6, innerWidth - 198)}px`;
    menu.style.top = `${clamp(e.clientY || rect.bottom, 6, innerHeight - 91)}px`; $('reset').focus();
  }
  function closeMenu() { $('menu').hidden = true; if (menuSource) menuSource.focus(); }
  function context(el, getIndex) {
    el.addEventListener('contextmenu', e => showMenu(e, getIndex()));
    el.addEventListener('keydown', e => { if (e.key === 'ContextMenu' || (e.shiftKey && e.key === 'F10')) showMenu(e, getIndex()); });
  }
  function gestures(el, getIndex) {
    let drag;
    el.addEventListener('pointerdown', e => {
      if (e.button !== 0 || !bridge || drag) return;
      e.preventDefault(); el.focus(); el.setPointerCapture(e.pointerId);
      const i = getIndex(); active.add(i); drag = {i, id:e.pointerId, y:e.clientY, f:fraction(i, values[i])};
    });
    el.addEventListener('pointermove', e => {
      if (!drag || drag.id !== e.pointerId) return;
      drag.f = clamp(drag.f + (drag.y - e.clientY) / 260 * (e.shiftKey ? .1 : 1), 0, 1);
      drag.y = e.clientY; edit(drag.i, plain(drag.i, drag.f));
    });
    const end = e => {
      if (!drag || (e.pointerId !== undefined && drag.id !== e.pointerId)) return;
      const {i, id} = drag; drag = null;
      if (el.hasPointerCapture(id)) el.releasePointerCapture(id); finish(i);
    };
    ['pointerup', 'pointercancel', 'lostpointercapture'].forEach(event => el.addEventListener(event, end));
    window.addEventListener('blur', end);
    el.addEventListener('dblclick', e => { e.preventDefault(); const i = getIndex(); edit(i, parameters[i].default, true); });
    el.addEventListener('wheel', e => {
      e.preventDefault(); if (!e.deltaY) return;
      const i = getIndex(), p = parameters[i]; active.add(i); clearTimeout(wheelTimers.get(i));
      edit(i, p.stepped ? values[i] - Math.sign(e.deltaY) : plain(i, fraction(i, values[i]) - Math.sign(e.deltaY) * (e.shiftKey ? .001 : .01)));
      wheelTimers.set(i, setTimeout(() => finish(i), 180));
    }, {passive:false});
    el.addEventListener('keydown', e => {
      const direction = ['ArrowUp','ArrowRight'].includes(e.key) ? 1 : ['ArrowDown','ArrowLeft'].includes(e.key) ? -1 : 0;
      if (!direction && !['Home','End'].includes(e.key)) return;
      e.preventDefault(); const i = getIndex(), p = parameters[i]; active.add(i);
      const step = p.stepped ? 1 : (i === 13 ? .01 : [10, 11].includes(i) ? 10 : 1) * (e.shiftKey ? .1 : 1);
      edit(i, direction ? values[i] + direction * step : e.key === 'Home' ? p.min : p.max);
    });
    el.addEventListener('keyup', e => { if (e.key.startsWith('Arrow') || ['Home','End'].includes(e.key)) finish(getIndex()); });
    el.addEventListener('blur', () => { if (!drag) finish(getIndex()); }); context(el, getIndex);
  }
  function numeric(input, i) {
    const p = parameters[i]; input.min = p.min; input.max = p.max;
    input.addEventListener('change', () => { if (input.value.trim() && Number.isFinite(input.valueAsNumber)) edit(i, input.valueAsNumber, true); input.value = format(i, values[i]); });
    input.addEventListener('keydown', e => {
      if (e.key === 'Enter') input.blur();
      if (e.key === 'Escape') { input.value = format(i, values[i]); input.blur(); }
    });
    input.addEventListener('blur', () => { input.value = format(i, values[i]); }); context(input, () => i);
  }
  function knob(slot, i, size, colour, name) {
    const p = parameters[i], root = document.createElement('div'); root.className = `control ${size}`;
    if (colour) { root.style.setProperty('--face', colour); root.style.setProperty('--shadow', '#677b6c44'); }
    const dial = document.createElement('div'); dial.className = 'dial'; dial.id = name || p.id;
    dial.tabIndex = 0; dial.setAttribute('role', 'slider'); dial.setAttribute('aria-label', name === 'time' ? 'Time' : p.name); dial.setAttribute('aria-orientation', 'vertical');
    dial.title = 'Drag vertically · Shift for fine adjustment · Double-click to reset';
    const face = document.createElement('span'), pointer = document.createElement('span'); face.className = 'face'; pointer.className = 'pointer'; dial.append(face, pointer);
    const label = document.createElement('label'); label.textContent = name === 'time' ? 'TIME' : p.name.toUpperCase();
    const row = document.createElement('div'); row.className = 'value'; const control = {dial};
    if (name === 'time') { control.output = document.createElement('output'); row.append(control.output); label.htmlFor = 'time'; }
    else {
      const input = document.createElement('input'); input.id = `${p.id}-value`; input.type = 'number'; input.step = 'any'; input.setAttribute('aria-label', `${p.name} ${p.unit}`);
      label.htmlFor = input.id; const unit = document.createElement('span'); unit.textContent = p.unit;
      row.append(input, unit); control.input = input; numeric(input, i); controls.set(i, control);
    }
    root.append(dial, label, row); $(slot).append(root); gestures(dial, name === 'time' ? timeIndex : () => i); return control;
  }
  function options(select, labels) { labels.forEach((name, i) => { const option = document.createElement('option'); option.value = i; option.textContent = name; select.append(option); }); }
  function build(state) {
    parameters = state.parameters; divisions = state.divisions; values = [...state.values];
    options($('character'), state.characters); options($('division'), divisions);
    timeControl = knob('time-slot', 2, 'large', null, 'time');
    knob('feedback-slot', 5, 'large'); knob('mix-slot', 6, 'medium', '#94d6c6'); knob('output-slot', 7, 'medium', '#f3a09b');
    knob('amount-slot', 9, 'mini', '#c3aed8'); knob('depth-slot', 12, '', '#9bcfd8'); knob('rate-slot', 13, '', '#d0dd8e');
    knob('low-slot', 10, '', '#b7acd7'); knob('high-slot', 11, '', '#9dd8d1');
    numeric($('timeMs-value'), 1); numeric($('bpm-value'), 3);
    for (const [id, i] of [['character',8],['division',2]]) {
      $(id).addEventListener('change', () => edit(i, Number($(id).value), true)); context($(id), () => i);
    }
    for (const [id, i, v] of [['stereo',4,0],['pingPong',4,1],['host',0,0],['local',0,1],['milliseconds',0,2]]) {
      $(id).addEventListener('click', () => { finishAll(); edit(i, v, true); taps = []; }); context($(id), () => i);
    }
    values.forEach((_, i) => paint(i));
  }
  let taps = [];
  $('tap').addEventListener('click', () => {
    const now = performance.now(); if (taps.length && now - taps[taps.length - 1] > 2200) taps = [];
    if (taps.length && now - taps[taps.length - 1] < 100) return;
    taps.push(now); if (taps.length > 5) taps.shift();
    if (taps.length < 2) return;
    const intervals = taps.slice(1).map((t, i) => t - taps[i]).sort((a,b) => a-b);
    const ms = intervals[Math.floor(intervals.length / 2)];
    if (values[0] === 1) edit(3, 60000 / ms, true); else if (values[0] === 2) edit(1, ms, true);
  });
  $('reset').onclick = () => { edit(menuIndex, parameters[menuIndex].default, true); closeMenu(); };
  $('automate').onclick = () => { bridge.automate(menuIndex); closeMenu(); };
  document.addEventListener('pointerdown', e => { if (!$('menu').hidden && !$('menu').contains(e.target)) closeMenu(); });
  $('menu').addEventListener('keydown', e => {
    if (e.key === 'Escape') { e.preventDefault(); closeMenu(); }
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); (document.activeElement === $('reset') ? $('automate') : $('reset')).focus(); }
    if (e.key === 'Tab') closeMenu();
  });
  window.addEventListener('blur', finishAll);
  const svg = (name, attributes, parent) => { const el = document.createElementNS('http://www.w3.org/2000/svg',name); for (const [k,v] of Object.entries(attributes)) el.setAttribute(k,v); parent.append(el); };
  for (let i = 0; i < 8; ++i) {
    svg('path', {d:'M180 53 Q228 82 210 127 L180 147 L150 127 Q132 82 180 53Z', fill:colours[i % 4], transform:`rotate(${i * 45} 180 180)`, stroke:'#fff1a5', 'stroke-width':1.1}, $('petals'));
    svg('path', {d:'m180 39 5 6-5 6-5-6Z', fill:colours[(i+1)%4], transform:`rotate(${i*45} 180 180)`}, $('satellites'));
    const angle = i * Math.PI / 4;
    svg('circle', {cx:180 + 166*Math.cos(angle), cy:180 + 166*Math.sin(angle), r:i%2 ? 3 : 4, fill:i%2 ? '#fff9cf' : colours[i%4]}, $('satellites'));
  }
  if (typeof qt === 'undefined' || typeof QWebChannel === 'undefined') return;
  new QWebChannel(qt.webChannelTransport, channel => {
    bridge = channel.objects.delay;
    bridge.snapshot.connect(state => {
      if (!parameters) build(state);
      $('unavailable').hidden = state.available;
      state.values.forEach((v, i) => { if (!active.has(i) && (pending.get(i) || 0) <= state.revision) { values[i] = v; pending.delete(i); } paint(i); });
      if (state.revision >= revision) {
        $('time-unit').textContent = `${state.milliseconds.toFixed(1)} ms`;
        $('time-limit').textContent = state.limited ? 'Maximum delay · 8 seconds' : '';
        if (document.activeElement !== $('bpm-value')) $('bpm-value').value = (values[0] === 2 ? 60000 / values[1] : values[0] === 1 ? values[3] : state.bpm).toFixed(1);
      }
      const level = clamp(Math.log10(Math.max(state.wet, .001)) / 3 + 1, 0, 1);
      $('petals').style.transform = `scale(${1 + .025 * level})`; $('petals').style.opacity = .78 + .18 * level;
      $('signal-led').style.opacity = .25 + .75 * level;
      const db = clamp(20 * Math.log10(Math.max(state.wet, .001)), -60, 12);
      $('signal').setAttribute('aria-valuenow', db.toFixed(1)); $('signal').textContent = db <= -60 ? 'READY' : `WET ${db.toFixed(1)} dB`;
      document.documentElement.dataset.connected = 'true';
    });
    bridge.ready();
  });
})();
