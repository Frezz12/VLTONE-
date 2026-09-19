'use strict';
(() => {
  const $ = id => document.getElementById(id);
  const notes = ['C','C♯','D','D♯','E','F','F♯','G','G♯','A','A♯','B'];
  const factories = ['Natural','Pop','Tight','Hard'];
  const defaults = [20,0,0,440,0,0,0,1,100,0,4095,0];
  const bounds = [[0,200],[0,100],[0,100],[400,480],[0,11],[0,7],[0,3],[0,1],[0,100],[-24,12],[1,4095],[0,1]];
  let bridge = null, state = {values:[...defaults]}, presetSignature = '', themeSignature = '', menuIndex = -1;
  const editing = new Set(), gestures = new Map(), wheelTimers = new Map();
  const queued = new Map(), pending = new Map(), dirty = new Set();
  const fields = Array.from({length:12},(_,i) => [...document.querySelectorAll(`[data-param="${i}"]`)]);
  const dials = Array.from({length:12},(_,i) => document.querySelector(`[data-dial="${i}"]`));
  const numberTimes = new Map();
  let frame = 0, revision = 0, latestSnapshot = null;
  const text = (el,value) => { if (el.textContent !== value) el.textContent = value; };
  const attribute = (el,name,value) => { if (el.getAttribute(name) !== value) el.setAttribute(name,value); };
  let replaceName = '', deleteArmed = false;
  const clamp = (value, min, max) => Math.max(min, Math.min(max, value));
  const decimals = i => [0,1,2,3,8,9].includes(i) ? 1 : 0;
  const formatted = (i,v) => v.toFixed(decimals(i));
  const fraction = (i,v) => i === 0 ? 1-Math.sqrt(v/200) : v/100;
  const fromFraction = (i,f) => i === 0 ? 200*(1-f)*(1-f) : 100*f;

  function drawValue(i, value, exact = false) {
    const now = performance.now();
    // The marker tracks each presentation frame; numbers remain legible while
    // dragging. Release and all keyboard/numeric edits show the exact value.
    const showNumber = exact || !gestures.has(i) || now-(numberTimes.get(i) || 0) >= 50;
    if (showNumber) {
      numberTimes.set(i,now);
      fields[i].forEach(el => {
        const next = el.tagName === 'SELECT' ? String(value) : formatted(i,value);
        if ((el.tagName === 'SELECT' || (el.tagName === 'INPUT' && document.activeElement !== el)) && el.value !== next)
          el.value = next;
      });
    }
    const dial = dials[i];
    if (dial) {
      const pointer = dial.querySelector('.pointer');
      const transform = `rotate(${-135+270*fraction(i,value)}deg)`;
      if (pointer.style.transform !== transform) pointer.style.transform = transform;
      attribute(dial,'aria-valuenow',formatted(i,value));
      attribute(dial,'aria-valuetext',`${formatted(i,value)} ${i === 0 ? 'milliseconds' : 'percent'}`);
    }
    if (i === 0) document.querySelector('.retune-value').classList.toggle('hard', value <= .05);
  }
  function schedule() {
    if (frame) return;
    frame = requestAnimationFrame(() => {
      frame = 0;
      dirty.forEach(i => drawValue(i,state.values[i])); dirty.clear();
      for (const i of queued.keys()) flush(i);
      // Publish the last numerical value even when the hand stops mid-gesture.
      for (const i of gestures.keys()) {
        if (fields[i].some(el => el.tagName === 'INPUT' && el.value !== formatted(i,state.values[i]))) dirty.add(i);
      }
      if (dirty.size) schedule();
    });
  }
  function acknowledged(i, token) {
    if (pending.get(i) !== token) return;
    pending.delete(i);
    if (latestSnapshot) render(latestSnapshot);
  }
  function flush(i) {
    const update = queued.get(i); if (!update) return;
    queued.delete(i);
    bridge.edit(i,update.value,false);
  }
  function edit(i, value, finished = true) {
    if (!bridge || !Number.isFinite(value)) return;
    value = Number(clamp(value,...bounds[i]).toFixed(decimals(i)));
    if (!finished && value === state.values[i]) return;
    const token = ++revision; pending.set(i,token);
    state.values[i] = value;
    if (finished) {
      queued.delete(i); dirty.delete(i); drawValue(i,value,true);
      bridge.edit(i,value,true,() => acknowledged(i,token));
    } else {
      queued.set(i,{value}); dirty.add(i); schedule();
    }
  }
  function finish(i) {
    editing.delete(i); gestures.delete(i);
    clearTimeout(wheelTimers.get(i)); wheelTimers.delete(i);
    dials[i]?.classList.remove('dragging');
    flush(i); dirty.delete(i); drawValue(i,state.values[i],true);
    const token = pending.get(i);
    bridge?.finish(i,() => acknowledged(i,token));
  }
  function finishAll() { [...editing].forEach(finish); }
  function render(next) {
    latestSnapshot = next;
    const theme = JSON.stringify(next.theme);
    if (next.theme && theme !== themeSignature) {
      themeSignature = theme;
      for (const [key, variable] of [['accent','--accent'],['shellTop','--shell-top'],['shellBottom','--shell-bottom']])
        document.documentElement.style.setProperty(variable, next.theme[key]);
    }
    const values = [...next.values];
    new Set([...editing,...pending.keys()]).forEach(i => { values[i] = state.values[i]; });
    const previous = state.values; state = {...next,values};
    values.forEach((v,i) => { if (v !== previous[i]) { dirty.add(i); schedule(); } });
    attribute($('active'),'aria-pressed',String(!!state.active));
    $('active').title = state.active ? 'Bypass pitch correction' : 'Enable pitch correction';
    if (state.sendToAllLabel) {
      $('send-to-all').title = state.sendToAllLabel;
      attribute($('send-to-all'),'aria-label',state.sendToAllLabel);
    }
    attribute($('formants'),'aria-pressed',String(values[7] > .5));
    if ($('quality').disabled !== !!state.busy) $('quality').disabled = !!state.busy;
    $('quality').title = state.busy ? 'Stop playback and input monitoring to change quality' : 'Processing quality';
    text($('latency'),state.pending ? `${values[11] ? 'HD' : 'Real-Time'} pending` : `${Number((state.latency || 0).toFixed(1))} ms latency`);
    text($('input-note'),state.input || '—'); text($('target-note'),state.target || '—');
    const cents = state.cents || 0; text($('cents'),`${cents > 0 ? '+' : ''}${cents} ct`);
    text($('status'),state.status || (state.active ? 'Click keys to set your scale' : 'Bypassed'));
    document.querySelectorAll('.key').forEach(key => {
      const n = Number(key.dataset.note), allowed = !!(state.mask & (1 << n));
      attribute(key,'aria-pressed',String(allowed));
      key.classList.toggle('current',state.active && state.targetNote === n);
      attribute(key,'aria-label',`${notes[n]}: ${allowed ? 'allowed' : 'excluded'}${state.active && state.targetNote === n ? ', current target' : ''}`);
    });
    const signature = JSON.stringify([state.presets,state.preset,state.modified,state.userPreset]);
    if (signature !== presetSignature) {
      presetSignature = signature; const select = $('preset'); select.replaceChildren();
      const current = new Option(`${state.preset || 'Custom'}${state.modified ? ' *' : ''}`,'current'); current.disabled = true; select.add(current);
      const factoryGroup = document.createElement('optgroup'); factoryGroup.label = 'Factory';
      factories.forEach((name,i) => factoryGroup.append(new Option(name,`factory:${i}`))); select.append(factoryGroup);
      if (state.presets?.length) {
        const group = document.createElement('optgroup'); group.label = 'Your presets';
        state.presets.forEach(name => group.append(new Option(name,`user:${name}`))); select.append(group);
      }
      select.value = 'current'; $('delete-preset').disabled = !state.userPreset;
    }
  }

  notes.forEach((name,i) => $('key').add(new Option(name,String(i))));
  ['Chromatic','Major','Natural minor','Harmonic minor','Melodic minor','Major pentatonic','Minor pentatonic','Custom'].forEach((name,i) => $('scale').add(new Option(name,String(i))));
  const white = [0,2,4,5,7,9,11], black = [1,3,6,8,10], boundaries = [1,2,4,5,6];
  [...white,...black].forEach(n => {
    const key = document.createElement('button'); key.className = `key${black.includes(n) ? ' black' : ''}`;
    key.id = `note-${n}`; key.dataset.note = n; key.title = `Include or exclude ${notes[n]}`;
    const face = document.createElement('span'); face.className = 'key-top';
    const label = document.createElement('span'); label.className = 'label'; label.textContent = notes[n]; face.append(label); key.append(face);
    if (black.includes(n)) key.style.left = `${boundaries[black.indexOf(n)]*100/7}%`;
    key.addEventListener('click',() => bridge?.toggleNote(n)); $('piano').append(key);
  });
  // Natural chromatic tab order even though black keys overlay the white row.
  [...$('piano').children].sort((a,b) => a.dataset.note-b.dataset.note).forEach(key => $('piano').append(key));
  white.forEach((n,i) => { $(`note-${n}`).style.gridColumn = String(i+1); $(`note-${n}`).style.gridRow = '1'; });

  document.querySelectorAll('[data-param]').forEach(el => {
    const i = Number(el.dataset.param);
    if (el.tagName === 'INPUT' || el.tagName === 'SELECT') {
      el.addEventListener('change',() => {
        if (el.value.trim() === '' || !Number.isFinite(Number(el.value))) { el.value = formatted(i,state.values[i]); return; }
        edit(i,Number(el.value)); el.value = formatted(i,state.values[i]);
      });
      if (el.tagName === 'INPUT') el.addEventListener('blur',() => { el.value = formatted(i,state.values[i]); });
      el.addEventListener('keydown',e => {
        if (e.key === 'Enter' && el.tagName === 'INPUT') { el.blur(); e.preventDefault(); }
        if (e.key === 'Escape' && el.tagName === 'INPUT') { el.value = formatted(i,state.values[i]); el.blur(); e.stopPropagation(); }
      });
    }
    if (i !== 10 && i !== 11) el.addEventListener('contextmenu',e => showAutomation(e,i));
  });
  document.querySelectorAll('[data-dial]').forEach(dial => {
    const i = Number(dial.dataset.dial);
    const move = e => {
      const g = gestures.get(i); if (!g) return;
      const delta = (g.y-e.clientY)/(e.shiftKey ? 3200 : 320);
      g.f = clamp(g.f+delta,0,1); g.y = e.clientY; edit(i,fromFraction(i,g.f),false);
    };
    dial.addEventListener('pointerdown',e => {
      if (e.button !== 0 || !bridge) return;
      e.preventDefault(); dial.focus(); closeAutomation();
      editing.add(i); gestures.set(i,{y:e.clientY,f:fraction(i,state.values[i])});
      dial.setPointerCapture(e.pointerId); dial.classList.add('dragging');
    });
    dial.addEventListener('pointermove',move);
    dial.addEventListener('pointerup',e => { move(e); if (editing.has(i)) finish(i); });
    for (const event of ['pointercancel','lostpointercapture']) dial.addEventListener(event,() => { if (editing.has(i)) finish(i); });
    dial.addEventListener('dblclick',() => { finish(i); edit(i,defaults[i]); });
    dial.addEventListener('keydown',e => {
      let v = state.values[i], step = e.shiftKey ? .1 : 1;
      if (e.key === 'Home') v = bounds[i][0];
      else if (e.key === 'End') v = bounds[i][1];
      else if (e.key === 'ArrowUp' || e.key === 'ArrowRight') v += step;
      else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') v -= step;
      else if (e.key === 'PageUp') v += step*10;
      else if (e.key === 'PageDown') v -= step*10;
      else if (e.key === 'Enter') { document.querySelector(`input[data-param="${i}"]`).focus(); return; }
      else if (e.key === 'Delete' || e.key === 'Backspace') v = defaults[i];
      else if (e.key === 'F10' && e.shiftKey) { showAutomation(e,i); return; }
      else return;
      e.preventDefault(); editing.add(i); edit(i,v,false);
    });
    dial.addEventListener('keyup',() => { if (editing.has(i) && !gestures.has(i)) finish(i); });
    dial.addEventListener('blur',() => { if (editing.has(i)) finish(i); });
    dial.addEventListener('wheel',e => {
      if (document.activeElement !== dial) return;
      e.preventDefault(); editing.add(i); const delta = Math.sign(-e.deltaY)*(e.shiftKey ? .1 : 1);
      edit(i,state.values[i]+delta,false); clearTimeout(wheelTimers.get(i)); wheelTimers.set(i,setTimeout(() => finish(i),160));
    },{passive:false});
    dial.addEventListener('contextmenu',e => showAutomation(e,i));
  });
  function showAutomation(e,i) {
    e.preventDefault(); finishAll(); menuIndex = i;
    const menu = $('automation-menu'), rect = e.currentTarget.getBoundingClientRect();
    menu.hidden = false;
    menu.style.left = `${clamp(e.clientX || rect.left,8,innerWidth-menu.offsetWidth-8)}px`;
    menu.style.top = `${clamp(e.clientY || rect.bottom,8,innerHeight-menu.offsetHeight-8)}px`;
    $('automate').focus();
  }
  function closeAutomation() { $('automation-menu').hidden = true; }
  $('automate').addEventListener('click',() => { bridge?.automate(menuIndex); closeAutomation(); });
  document.addEventListener('pointerdown',e => { if (!$('automation-menu').contains(e.target)) closeAutomation(); });
  document.addEventListener('keydown',e => { if (e.key === 'Escape') closeAutomation(); });
  window.addEventListener('blur',finishAll);
  document.addEventListener('visibilitychange',() => { if (document.hidden) finishAll(); });
  $('active').addEventListener('click',() => bridge?.toggleBypass());
  $('send-to-all').addEventListener('click',() => { finishAll(); bridge?.sendToAll(); });
  $('formants').addEventListener('click',() => edit(7,state.values[7] > .5 ? 0 : 1));
  $('preset').addEventListener('change',e => {
    finishAll(); const value = e.target.value;
    if (value.startsWith('factory:')) bridge?.factoryPreset(Number(value.slice(8)));
    if (value.startsWith('user:')) bridge?.loadPreset(value.slice(5));
  });
  $('settings').addEventListener('click',() => {
    finishAll(); $('preset-message').textContent = ''; $('replace-preset').hidden = true;
    $('preset-name').value = state.userPreset ? state.preset : ''; deleteArmed = false;
    $('delete-preset').textContent = 'Delete current preset'; $('settings-dialog').showModal();
  });
  function save(replace) {
    const name = $('preset-name').value.trim();
    bridge?.savePreset(name, replace && name === replaceName, message => {
      $('replace-preset').hidden = message !== 'replace';
      if (message === 'replace') { replaceName = name; $('preset-message').textContent = `“${name}” already exists. Replace it?`; }
      else $('preset-message').textContent = message || 'Preset saved.';
    });
  }
  $('save-preset').addEventListener('click',() => save(false));
  $('replace-preset').addEventListener('click',() => save(true));
  $('preset-name').addEventListener('input',() => { $('replace-preset').hidden = true; });
  $('delete-preset').addEventListener('click',() => {
    if (!deleteArmed) { deleteArmed = true; $('delete-preset').textContent = 'Confirm delete'; return; }
    bridge?.deletePreset(state.preset); deleteArmed = false; $('delete-preset').textContent = 'Delete current preset';
  });
  defaults.forEach((v,i) => drawValue(i,v));
  if (typeof QWebChannel === 'function' && typeof qt === 'object') {
    new QWebChannel(qt.webChannelTransport, channel => {
      bridge = channel.objects.pitch; bridge.snapshot.connect(render); bridge.ready();
      document.documentElement.dataset.connected = 'true';
    });
  } else $('status').textContent = 'Connecting to VLT Pitch…';
})();
