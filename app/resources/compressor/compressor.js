'use strict';
(() => {
  const $ = id => document.getElementById(id);
  let bridge, parameters, values = [], revision = 0, menuIndex = 0, menuSource;
  const pending = new Map(), active = new Set(), controls = new Map(), wheelTimers = new Map();
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const logarithmic = i => i === 0 || i === 3 || i === 4;
  const fraction = (i, v) => { const p = parameters[i]; return logarithmic(i) ? Math.log(v / p.min) / Math.log(p.max / p.min) : (v - p.min) / (p.max - p.min); };
  const plain = (i, f) => { const p = parameters[i]; f = clamp(f, 0, 1); return logarithmic(i) ? p.min * (p.max / p.min) ** f : p.min + (p.max - p.min) * f; };
  const text = (i, v) => v.toFixed(i === 4 || i === 6 ? 0 : 1);
  function paint(i) {
    const p = parameters[i], v = values[i], control = controls.get(i);
    if (control) {
      control.dial.style.setProperty('--angle', `${-135 + 270 * fraction(i, v)}deg`);
      control.dial.setAttribute('aria-valuenow', v);
      control.dial.setAttribute('aria-valuetext', `${text(i, v)} ${p.unit}`);
      if (document.activeElement !== control.input) control.input.value = text(i, v);
    }
    if (i === 0 || i === 1) {
      const handle = $(i === 0 ? 'graph-ratio' : 'graph-threshold');
      handle.setAttribute('transform', i === 0 ? `translate(28 ${200 - 188 * fraction(i, v)})` : `translate(${28 + 188 * fraction(i, v)} 200)`);
      handle.setAttribute('aria-valuenow', v); handle.setAttribute('aria-valuetext', `${text(i, v)} ${p.unit}`);
      handle.setAttribute('aria-valuemin', p.min); handle.setAttribute('aria-valuemax', p.max);
    }
    if (i === 7) $('mode').setAttribute('aria-pressed', v >= .5);
    if (i === 8) { $('autoGain').setAttribute('aria-pressed', v >= .5); $('auto-state').textContent = v >= .5 ? 'ON' : 'OFF'; }
  }
  function edit(i, v, finished = false) {
    if (!bridge || !Number.isFinite(v)) return;
    const p = parameters[i]; values[i] = clamp(v, p.min, p.max); paint(i);
    pending.set(i, ++revision); bridge.edit(i, values[i], finished, revision);
  }
  function finish(i) {
    active.delete(i); clearTimeout(wheelTimers.get(i)); wheelTimers.delete(i);
    if (bridge) bridge.finish(i);
  }
  function showMenu(e, i) {
    e.preventDefault(); finish(i); menuIndex = i; menuSource = e.currentTarget;
    const menu = $('menu'), rect = menuSource.getBoundingClientRect(); menu.hidden = false;
    menu.style.left = `${clamp(e.clientX || rect.left, 5, innerWidth - 195)}px`;
    menu.style.top = `${clamp(e.clientY || rect.bottom, 5, innerHeight - 90)}px`;
    $('reset').focus();
  }
  function closeMenu() { $('menu').hidden = true; if (menuSource) menuSource.focus(); }
  function context(element, i) {
    element.addEventListener('contextmenu', e => showMenu(e, i));
    element.addEventListener('keydown', e => { if (e.key === 'ContextMenu' || (e.shiftKey && e.key === 'F10')) showMenu(e, i); });
  }
  function gestures(element, i, axis = 'vertical', graph = false) {
    let drag;
    element.addEventListener('pointerdown', e => {
      if (e.button !== 0 || !bridge) return;
      e.preventDefault(); element.focus(); element.setPointerCapture(e.pointerId);
      active.add(i); drag = {x: e.clientX, y: e.clientY, f: fraction(i, values[i])};
    });
    element.addEventListener('pointermove', e => {
      if (!drag) return;
      const range = graph ? $('graph').getBoundingClientRect().width * 188 / 236 : 260;
      const delta = axis === 'horizontal' ? e.clientX - drag.x : drag.y - e.clientY;
      drag.f = clamp(drag.f + delta / range * (e.shiftKey ? .1 : 1), 0, 1);
      drag.x = e.clientX; drag.y = e.clientY; edit(i, plain(i, drag.f));
    });
    const end = e => { if (!drag) return; drag = null; if (element.hasPointerCapture(e.pointerId)) element.releasePointerCapture(e.pointerId); finish(i); };
    element.addEventListener('pointerup', end); element.addEventListener('pointercancel', end); element.addEventListener('lostpointercapture', end);
    element.addEventListener('dblclick', e => { e.preventDefault(); edit(i, parameters[i].default, true); });
    element.addEventListener('wheel', e => {
      e.preventDefault(); active.add(i); clearTimeout(wheelTimers.get(i));
      edit(i, plain(i, fraction(i, values[i]) - Math.sign(e.deltaY) * (e.shiftKey ? .001 : .01)));
      wheelTimers.set(i, setTimeout(() => finish(i), 180));
    }, {passive: false});
    element.addEventListener('keydown', e => {
      const direction = ['ArrowUp', 'ArrowRight'].includes(e.key) ? 1 : ['ArrowDown', 'ArrowLeft'].includes(e.key) ? -1 : 0;
      if (!direction && e.key !== 'Home' && e.key !== 'End') return;
      e.preventDefault(); active.add(i);
      const step = (i === 4 || i === 6 ? 1 : .1) * (e.shiftKey ? .1 : 1);
      edit(i, direction ? values[i] + direction * step : e.key === 'Home' ? parameters[i].min : parameters[i].max);
    });
    element.addEventListener('keyup', e => { if (e.key.startsWith('Arrow') || e.key === 'Home' || e.key === 'End') finish(i); });
    element.addEventListener('blur', () => { if (!drag) finish(i); }); context(element, i);
  }
  function build() {
    for (let i = 0; i < 7; ++i) {
      const p = parameters[i], wrapper = document.createElement('div'); wrapper.className = 'control';
      const dial = document.createElement('div'); dial.className = 'dial'; dial.id = p.id; dial.tabIndex = 0;
      dial.setAttribute('role', 'slider'); dial.setAttribute('aria-label', p.name); dial.setAttribute('aria-orientation', 'vertical');
      dial.setAttribute('aria-valuemin', p.min); dial.setAttribute('aria-valuemax', p.max);
      dial.title = `${p.name}: drag vertically. Shift for fine adjustment. Double-click to reset.`;
      const pointer = document.createElement('span'); pointer.className = 'pointer'; dial.append(pointer);
      const label = document.createElement('label'); label.htmlFor = `${p.id}-value`; label.textContent = p.name.toUpperCase();
      const row = document.createElement('div'); row.className = 'value';
      const input = document.createElement('input'); input.id = `${p.id}-value`; input.type = 'number'; input.step = 'any'; input.min = p.min; input.max = p.max; input.setAttribute('aria-label', `${p.name} ${p.unit}`);
      const unit = document.createElement('span'); unit.textContent = p.unit;
      row.append(input, unit); wrapper.append(dial, label, row);
      $(i < 3 ? 'primary' : 'secondary').insertBefore(wrapper, i < 2 ? $('autoGain') : null);
      if (i < 3) { wrapper.style.gridColumn = i === 2 ? '4' : String(i + 1); wrapper.style.gridRow = '1'; }
      controls.set(i, {dial, input}); gestures(dial, i); context(input, i);
      input.addEventListener('change', () => { if (input.value.trim() && Number.isFinite(input.valueAsNumber)) edit(i, input.valueAsNumber, true); input.value = text(i, values[i]); });
      input.addEventListener('keydown', e => {
        if (e.key === 'Enter') input.blur();
        if (e.key === 'Escape') { input.value = text(i, values[i]); input.blur(); }
      });
      input.addEventListener('blur', () => { input.value = text(i, values[i]); });
    }
    gestures($('graph-ratio'), 0, 'vertical', true); gestures($('graph-threshold'), 1, 'horizontal', true);
    for (const [id, i] of [['mode', 7], ['autoGain', 8]]) { $(id).addEventListener('click', () => edit(i, values[i] >= .5 ? 0 : 1, true)); context($(id), i); }
    for (const id of ['gr', 'in', 'out']) {
      const bar = $(`meter-${id}`).querySelector('.segments');
      for (let j = 0; j < 30; ++j) { const segment = document.createElement('i'); segment.className = j < 3 ? 'hot' : j < 7 ? 'warm' : ''; bar.append(segment); }
    }
  }
  function meter(id, db, reduction = false) {
    const root = $(`meter-${id}`), bar = root.querySelector('.segments');
    const amount = reduction ? clamp(db / 30, 0, 1) : clamp((db + 60) / 66, 0, 1);
    [...bar.children].forEach((s, j) => s.classList.toggle('on', reduction ? j < Math.round(amount * 30) : 30 - j <= Math.round(amount * 30)));
    bar.setAttribute('aria-valuenow', clamp(db, reduction ? 0 : -60, reduction ? 30 : 6).toFixed(1));
    bar.setAttribute('aria-valuetext', `${db.toFixed(1)} dB`);
    root.querySelector('output').textContent = db <= -60 ? '−∞' : db.toFixed(1);
  }
  $('reset').onclick = () => { edit(menuIndex, parameters[menuIndex].default, true); closeMenu(); };
  $('automate').onclick = () => { bridge.automate(menuIndex); closeMenu(); };
  document.addEventListener('pointerdown', e => { if (!$('menu').hidden && !$('menu').contains(e.target)) closeMenu(); });
  $('menu').addEventListener('keydown', e => {
    if (e.key === 'Escape') { e.preventDefault(); closeMenu(); }
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); (document.activeElement === $('reset') ? $('automate') : $('reset')).focus(); }
    if (e.key === 'Tab') closeMenu();
  });
  window.addEventListener('blur', () => { for (const i of [...active]) finish(i); });
  if (typeof qt === 'undefined' || typeof QWebChannel === 'undefined') return;
  new QWebChannel(qt.webChannelTransport, channel => {
    bridge = channel.objects.compressor;
    bridge.snapshot.connect(state => {
      if (!parameters) { parameters = state.parameters; values = [...state.values]; build(); }
      state.values.forEach((v, i) => {
        if (!active.has(i) && (pending.get(i) || 0) <= state.revision) { values[i] = v; pending.delete(i); }
        paint(i);
      });
      // Curve samples come from the same C++ function that computes DSP gain.
      // Ignore stale replies while a newer edit is still crossing WebChannel.
      if (state.revision >= revision) {
        $('curve').setAttribute('d', state.curve.map((v, i) => `${i ? 'L' : 'M'}${28 + i * 188 / 120},${200 - (v + 60) * 188 / 60}`).join(' '));
      }
      const input = clamp(state.input, -60, 0);
      $('level-marker').setAttribute('cx', 28 + (input + 60) * 188 / 60);
      $('level-marker').setAttribute('cy', 200 - (state.marker + 60) * 188 / 60);
      $('level-marker').style.opacity = state.input <= -60 ? '0' : '1';
      meter('gr', state.reduction, true); meter('in', state.input); meter('out', state.output);
      document.documentElement.dataset.connected = 'true';
    });
    bridge.ready();
  });
})();
