'use strict';
const $ = id => document.getElementById(id);
const names = ['Chorus','Doubler','Flanger','Phaser','Doubler Pro'];
const subtitles = ['LAYERED VOICES','HUMAN DOUBLES','COMB MOTION','PHASE ROTATION','VOCAL DIMENSION'];
const colors = ['#9779e7','#d45a9b','#3cbb97','#e3a448','#d45a9b'];
const offsets = [0,5,9,14];
const clamp = (v,a,b) => Math.max(a,Math.min(b,v));
let bridge, state, signature = '', selectedBand = 1, pending = new Set(), dragging = false, contextIndex = 0, contextModule = 0;
const controls = new Map();
const suffix = p => p.unit === '%' ? '%' : p.unit === 'Hz' ? 'Hz' : p.unit === 'cents' ? 'ct' : p.unit;
const scale = p => p.unit === '%' ? 100 : 1;
const normalized = (p,v) => p.unit === 'Hz' && p.min > 0 ? Math.log(v/p.min)/Math.log(p.max/p.min) : (v-p.min)/(p.max-p.min);
const plain = (p,n) => p.unit === 'Hz' && p.min > 0 ? p.min*Math.pow(p.max/p.min,n) : p.min+n*(p.max-p.min);
function art(module) {
  const id = `glass-${module}`, c = colors[module];
  const cube = '<path class="glass" d="M40 42 80 25 120 42 120 88 80 106 40 88Z"/><path class="wire" d="M40 42 80 60 120 42M80 25V60 106M40 88 80 71 120 88"/>';
  let shape;
  if(module===0) shape = `${cube}<g class="ripple"><path fill="url(#${id})" d="M41 75Q60 57 80 79T119 70V88L80 105 41 88Z"/><path class="wire" d="M41 70Q59 52 80 73T119 65M41 79Q59 61 80 82T119 74"/></g>`;
  else if(module===1||module===4) shape = `<path class="glass" d="M30 48V89C30 104 82 104 82 89V48M78 36V78C78 94 130 94 130 78V36"/><path fill="url(#${id})" d="M31 72Q55 64 81 72V90C81 103 31 103 31 90ZM79 59Q104 52 129 59V79C129 93 79 93 79 79Z"/><ellipse class="glass" cx="56" cy="48" rx="26" ry="9"/><ellipse class="glass" cx="104" cy="36" rx="26" ry="9"/><path class="wire ripple" d="M32 85Q57 99 80 85M80 74Q105 86 128 74"/>`;
  else if(module===2) shape = `${cube}<path fill="url(#${id})" d="M41 75 80 58 119 75V88L80 105 41 88Z"/><g class="ripple wire"><path d="M49 85 88 68M59 90 98 73M69 95 108 78M41 75 80 92 119 75"/></g>`;
  else shape = `${cube}<path fill="url(#${id})" d="M41 82 80 67 119 82V88L80 105 41 88Z"/><g class="ripple wire"><ellipse cx="80" cy="72" rx="27" ry="11" transform="rotate(-30 80 72)"/><ellipse cx="80" cy="72" rx="27" ry="11" transform="rotate(30 80 72)"/><ellipse cx="80" cy="72" rx="11" ry="27"/></g>`;
  return `<svg viewBox="0 0 160 132" aria-hidden="true"><defs><linearGradient id="${id}" x1="0" y1="0" x2="0" y2="1"><stop stop-color="${c}" stop-opacity=".08"/><stop offset="1" stop-color="${c}" stop-opacity=".94"/></linearGradient></defs><ellipse class="shadow" cx="80" cy="119" rx="37" ry="5"/><g class="motion">${shape}</g></svg>`;
}
const powerIcon = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3v9M6.3 6.3a8 8 0 1 0 11.4 0"/></svg>';
function setValue(index,value,finished=false) {
  if(!state || !state.params[index]) return;
  const p=state.params[index]; value=clamp(value,p.min,p.max); if(!Number.isFinite(value)) return;
  pending.add(index); state.values[index]=value; updateControl(index,value);
  bridge.edit(index,value,finished,()=>{if(finished){pending.delete(index);}});
}
function finish() { bridge.finish(()=>pending.clear()); }
function updateControl(index,value) {
  const c=controls.get(index); if(!c)return;
  const p=state.params[index], n=clamp(normalized(p,value),0,1), text=String(Number((value*scale(p)).toFixed(p.unit==='Hz'?3:p.unit==='cents'?1:0)));
  c.dial.style.setProperty('--angle',`${-135+270*n}deg`);
  c.dial.setAttribute('aria-valuenow',value);c.dial.setAttribute('aria-valuetext',`${text} ${suffix(p)}`);
  if(document.activeElement!==c.input)c.input.value=text;
}
function makeControl(index,position) {
  const p=state.params[index], label=p.name.split(' ').slice(state.rack?1:0).join(' ');
  const div=document.createElement('div');div.className=`control ${position}`;
  const dial=document.createElement('div');dial.className='dial';dial.tabIndex=0;dial.role='slider';dial.id=`param-${index}`;
  dial.setAttribute('aria-label',p.name);dial.setAttribute('aria-valuemin',p.min);dial.setAttribute('aria-valuemax',p.max);dial.setAttribute('aria-orientation','vertical');
  dial.title='Drag up/down · Shift for precision · double-click to reset · right-click for automation';
  dial.innerHTML='<span class="pointer"></span>';
  const caption=document.createElement('label');caption.className='control-label';caption.textContent=label;caption.htmlFor=`value-${index}`;
  const readout=document.createElement('div');readout.className='readout';
  const input=document.createElement('input');input.type='number';input.id=`value-${index}`;input.min=p.min*scale(p);input.max=p.max*scale(p);input.step=p.unit==='Hz'?.01:p.unit==='cents'?.1:1;input.setAttribute('aria-label',`${p.name} value`);
  const unit=document.createElement('span');unit.textContent=suffix(p);readout.append(input,unit);div.append(dial,caption,readout);controls.set(index,{dial,input});
  let gesture=null;
  dial.addEventListener('pointerdown',e=>{if(e.button!==0||gesture)return;e.preventDefault();dial.focus();dial.setPointerCapture(e.pointerId);pending.add(index);gesture={y:e.clientY,n:normalized(p,state.values[index]),pointer:e.pointerId};});
  dial.addEventListener('pointermove',e=>{if(!gesture||gesture.pointer!==e.pointerId)return;const delta=(gesture.y-e.clientY)/(e.shiftKey?1600:240);gesture.y=e.clientY;gesture.n=clamp(gesture.n+delta,0,1);setValue(index,plain(p,gesture.n));});
  const end=()=>{if(gesture){gesture=null;finish();}};
  dial.addEventListener('pointerup',end);dial.addEventListener('pointercancel',end);dial.addEventListener('lostpointercapture',end);
  dial.addEventListener('dblclick',()=>setValue(index,p.default,true));
  dial.addEventListener('keydown',e=>{let n=normalized(p,state.values[index]),step=e.shiftKey?.001:.01;
    if(['ArrowUp','ArrowRight'].includes(e.key))n+=step;else if(['ArrowDown','ArrowLeft'].includes(e.key))n-=step;else if(e.key==='Home')n=0;else if(e.key==='End')n=1;else return;
    e.preventDefault();setValue(index,plain(p,clamp(n,0,1)),true);
  });
  dial.addEventListener('contextmenu',e=>{e.preventDefault();contextIndex=index;$('move-left').hidden=$('move-right').hidden=true;$('automate').hidden=$('reset').hidden=false;const menu=$('context-menu');menu.hidden=false;menu.style.left=`${Math.min(e.clientX,innerWidth-195)}px`;menu.style.top=`${Math.min(e.clientY,innerHeight-90)}px`;$('automate').focus();});
  input.addEventListener('change',()=>{if(Number.isFinite(input.valueAsNumber))setValue(index,input.valueAsNumber/scale(p),true);else updateControl(index,state.values[index]);});
  return div;
}
function build() {
  controls.clear();$('cards').replaceChildren();document.body.classList.toggle('single',!state.rack);$('equalizer').hidden=!state.rack;
  for(const m of state.order){
    const card=document.createElement('section');card.className='card';card.dataset.module=m;card.style.setProperty('--color',colors[m]);card.setAttribute('aria-label',names[m]);
    const head=document.createElement('div');head.className='card-head';
    const pos=document.createElement('span');pos.className='position';
    const grab=document.createElement('button');grab.className='grab';grab.setAttribute('aria-label',`Move ${names[m]}`);grab.title='Drag left/right to reorder · Alt + arrow keys';grab.innerHTML='<svg viewBox="0 0 10 14"><path d="M2 2h1M7 2h1M2 7h1M7 7h1M2 12h1M7 12h1"/></svg><span>STEREO</span>';
    const power=document.createElement('button');power.className='power';power.setAttribute('aria-label',`Enable ${names[m]}`);power.innerHTML=powerIcon;power.addEventListener('click',()=>state.rack?setValue(offsets[m],state.values[offsets[m]]?0:1,true):bridge.toggleBypass());
    head.append(pos,grab,power);card.append(head);
    const picture=document.createElement('div');picture.className='art';picture.innerHTML=art(m);card.append(picture);
    const title=document.createElement('h2');title.textContent=names[m].toUpperCase();card.append(title);
    const desc=document.createElement('div');desc.className='descriptor';desc.textContent=subtitles[m];card.append(desc);
    const row=document.createElement('div');row.className='controls';
    const count=state.rack?(m===1?3:4):state.params.length,base=state.rack?offsets[m]+1:0;
    row.append(makeControl(base,'main'),makeControl(base+1,'left'),makeControl(base+2,'right'));
    for(let i=3;i<count;++i)row.append(makeControl(base+i,'extra'));card.append(row);$('cards').append(card);
    if(state.rack)wireReorder(card,grab,m);else{grab.tabIndex=-1;grab.querySelector('svg').remove();}
  }
}
function wireReorder(card,grab,module) {
  let drag=null, skipClick=false;
  const menu=e=>{e.preventDefault();contextModule=module;$('automate').hidden=$('reset').hidden=true;$('move-left').hidden=$('move-right').hidden=false;
    $('move-left').disabled=state.order.indexOf(module)===0;$('move-right').disabled=state.order.indexOf(module)===3;
    const r=grab.getBoundingClientRect();$('context-menu').style.left=`${Math.min(r.left,innerWidth-195)}px`;$('context-menu').style.top=`${r.bottom}px`;$('context-menu').hidden=false;
  };
  grab.addEventListener('click',e=>{if(skipClick){skipClick=false;return;}menu(e);});grab.addEventListener('contextmenu',menu);
  grab.addEventListener('pointerdown',e=>{if(e.button!==0||drag)return;grab.setPointerCapture(e.pointerId);drag={x:e.clientX,from:state.order.indexOf(module),to:state.order.indexOf(module),id:e.pointerId};grab.focus();e.preventDefault();});
  grab.addEventListener('pointermove',e=>{if(!drag||drag.id!==e.pointerId)return;const dx=e.clientX-drag.x;if(Math.abs(dx)<5&&!dragging)return;dragging=true;card.classList.add('dragging');card.style.transform=`translateX(${dx}px)`;
    const rect=$('cards').getBoundingClientRect();drag.to=clamp(Math.floor((e.clientX-rect.left)/(rect.width/4)),0,3);
    [...$('cards').children].forEach((el,i)=>el.classList.toggle('drop-target',i===drag.to&&i!==drag.from));
  });
  const end=cancel=>{if(!drag)return;const {from,to}=drag;skipClick=dragging;drag=null;dragging=false;card.classList.remove('dragging');card.style.transform='';for(const el of $('cards').children)el.classList.remove('drop-target');
    if(!cancel&&from!==to){bridge.reorder(from,to);$('announcement').textContent=`${names[module]} moved to position ${to+1}`;}
  };
  grab.addEventListener('pointerup',()=>end(false));grab.addEventListener('pointercancel',()=>end(true));grab.addEventListener('lostpointercapture',()=>end(true));
  grab.addEventListener('keydown',e=>{if(e.altKey&&['ArrowLeft','ArrowRight'].includes(e.key)){e.preventDefault();const from=state.order.indexOf(module);bridge.reorder(from,clamp(from+(e.key==='ArrowLeft'?-1:1),0,3));}});
}
function render(next) {
  if(!next.available){document.body.classList.add('unavailable');document.body.classList.remove('playing');return;}
  if(state)for(const i of pending)next.values[i]=state.values[i];state=next;
  document.body.classList.remove('unavailable');document.body.classList.toggle('playing',!!state.playing);document.body.classList.toggle('reduced',!!state.reduced);
  document.documentElement.style.setProperty('--accent',state.accent);$('title').textContent=state.title.toUpperCase();$('active').setAttribute('aria-pressed',state.active);$('live-label').textContent=state.playing?'PLAYING':state.active?'READY':'BYPASSED';
  const key=state.rack?'rack':state.title;if(signature!==key){signature=key;build();}
  if(!dragging && [...$('cards').children].some((el,i)=>Number(el.dataset.module)!==state.order[i])) state.order.forEach(m=>{const card=document.querySelector(`[data-module="${m}"]`);$('cards').append(card);});
  for(const [index] of controls)updateControl(index,state.values[index]);
  [...$('cards').children].forEach((card,i)=>{const m=Number(card.dataset.module),on=state.active&&(!state.rack||state.values[offsets[m]]>=.5);card.classList.toggle('disabled',!on);card.querySelector('.power').setAttribute('aria-pressed',state.rack?state.values[offsets[m]]>=.5:state.active);card.querySelector('.position').textContent=`0${i+1}`;});
  $('chain-label').textContent=state.order.map(m=>names[m].toUpperCase()).join('  →  ')+(state.rack?'  →  EQ':'  /  STEREO PROCESSOR');
  renderPresets();if(state.rack)renderEq();
}
function renderPresets(){
  const key=JSON.stringify([state.factories,state.presets.map(p=>p.name)]),select=$('preset');
  if(select.dataset.key!==key){select.dataset.key=key;select.replaceChildren(new Option('Custom','current'));state.factories.forEach((p,i)=>select.add(new Option(p,`factory:${i}`)));state.presets.forEach(p=>select.add(new Option(p.name,`user:${p.name}`)));}
  select.options[0].textContent=state.preset+(state.modified?' *':'');select.value='current';
}
const bandColors=['#adb9c2','#ce8b9d','#d3b269','#9aca9f','#9ab0e8','#c3a3e8'];
const svgNS='http://www.w3.org/2000/svg';
const xFreq=f=>20+Math.log(clamp(f,20,20000)/20)/Math.log(1000)*880;
const yGain=g=>90-clamp(g,-18,18)/18*72;
function svgEl(tag,attrs){const el=document.createElementNS(svgNS,tag);for(const [k,v]of Object.entries(attrs))el.setAttribute(k,v);return el;}
function initEq(){
  for(const hz of [20,50,100,200,500,1000,2000,5000,10000,20000]){const x=xFreq(hz);$('eq-grid').append(svgEl('line',{x1:x,x2:x,y1:16,y2:168}));const t=svgEl('text',{x,y:190,'text-anchor':'middle'});t.textContent=hz>=1000?`${hz/1000}k`:hz;$('eq-grid').append(t);}
  for(const db of [-18,-12,-6,0,6,12,18]){const y=yGain(db);$('eq-grid').append(svgEl('line',{x1:20,x2:900,y1:y,y2:y}));}
  for(let b=0;b<6;++b){const g=svgEl('g',{class:'eq-handle',tabindex:'0',role:'slider','aria-label':$('eq-band').options[b].text,'aria-valuemin':20,'aria-valuemax':20000,'data-band':b});g.style.setProperty('--band',bandColors[b]);g.append(svgEl('circle',{r:15,class:'hit'}),svgEl('circle',{r:7,class:'dot'}));$('eq-handles').append(g);wireBand(g,b);}
}
function renderEq(){
  $('eq-power').setAttribute('aria-pressed',state.values[20]>=.5);
  if(state.response){const d=state.response.map((v,i)=>`${i?'L':'M'}${20+i/179*880},${yGain(v)}`).join(' ');$('eq-curve').setAttribute('d',d);$('eq-area').setAttribute('d',`${d}L900 169L20 169Z`);$('mini-curve').setAttribute('d',state.response.map((v,i)=>`${i?'L':'M'}${i},${15-clamp(v,-18,18)*.6}`).join(' '));}
  for(let b=0;b<6;++b){const base=21+b*4,g=document.querySelector(`[data-band="${b}"]`),v=state.values;g.setAttribute('transform',`translate(${xFreq(v[base+1])} ${yGain(b===0||b===5?0:v[base+2])})`);g.classList.toggle('off',!v[base]);g.classList.toggle('selected',b===selectedBand);g.setAttribute('aria-valuenow',v[base+1]);g.setAttribute('aria-valuetext',`${Math.round(v[base+1])} Hz, ${v[base+2].toFixed(1)} dB`);}
  const base=21+selectedBand*4,v=state.values;$('eq-band').value=String(selectedBand);$('band-power').setAttribute('aria-pressed',!!v[base]);$('band-power').textContent=v[base]?'On':'Off';
  for(const [id,f]of [['eq-frequency',1],['eq-gain',2],['eq-q',3]]){const input=$(id);if(document.activeElement!==input)input.value=v[base+f].toFixed(f===1?0:1);input.min=state.params[base+f].min;input.max=state.params[base+f].max;}
  $('eq-gain').disabled=selectedBand===0||selectedBand===5;
}
function wireBand(el,band){
  const base=21+band*4;let drag=null;
  el.addEventListener('pointerdown',e=>{if(e.button!==0||drag)return;selectedBand=band;el.focus();el.setPointerCapture(e.pointerId);const r=$('eq-graph').getBoundingClientRect();drag={x:e.clientX,y:e.clientY,freq:state.values[base+1],gain:state.values[base+2],width:r.width,height:r.height,id:e.pointerId};[base,base+1,base+2].forEach(i=>pending.add(i));renderEq();e.preventDefault();});
  el.addEventListener('pointermove',e=>{if(!drag||drag.id!==e.pointerId)return;const dx=(e.clientX-drag.x)*920/drag.width,dy=(e.clientY-drag.y)*200/drag.height;
    if(Math.abs(dx)+Math.abs(dy)<1)return;setValue(base,1);setValue(base+1,drag.freq*Math.pow(1000,dx/880));if(band>0&&band<5)setValue(base+2,drag.gain-dy*18/72);renderEq();});
  const end=()=>{if(drag){drag=null;finish();}};el.addEventListener('pointerup',end);el.addEventListener('pointercancel',end);el.addEventListener('lostpointercapture',end);
  el.addEventListener('keydown',e=>{const step=e.shiftKey?.1:1;if(e.key==='ArrowLeft'||e.key==='ArrowRight'){setValue(base+1,state.values[base+1]*Math.pow(2,(e.key==='ArrowLeft'?-step:step)/12));}else if((e.key==='ArrowUp'||e.key==='ArrowDown')&&band>0&&band<5){setValue(base+2,state.values[base+2]+(e.key==='ArrowUp'?step:-step));}else if(e.key==='Delete'||e.key==='Backspace'){setValue(base,0,true);e.preventDefault();renderEq();return;}else return;e.preventDefault();setValue(base,1);finish();renderEq();});
  el.addEventListener('wheel',e=>{e.preventDefault();selectedBand=band;setValue(base+3,state.values[base+3]*Math.pow(1.08,-Math.sign(e.deltaY)),true);renderEq();},{passive:false});
  el.addEventListener('dblclick',()=>{setValue(base+2,0,true);renderEq();});
}
$('eq-band').options.length=0;['Low Cut','Low','Low Mid','High Mid','High','High Cut'].forEach((s,i)=>$('eq-band').add(new Option(s,String(i))));initEq();
$('eq-toggle').addEventListener('click',()=>{const expanded=$('eq-content').hidden;$('eq-content').hidden=!expanded;$('eq-toggle').setAttribute('aria-expanded',expanded);bridge.setEqExpanded(expanded);setTimeout(()=>{if(expanded)$('equalizer').scrollIntoView({block:'nearest'});else scrollTo(0,0);},150);});
$('eq-power').addEventListener('click',()=>setValue(20,state.values[20]?0:1,true));
$('eq-band').addEventListener('change',()=>{selectedBand=Number($('eq-band').value);renderEq();});
$('band-power').addEventListener('click',()=>{const i=21+selectedBand*4;setValue(i,state.values[i]?0:1,true);renderEq();});
for(const [id,f]of [['eq-frequency',1],['eq-gain',2],['eq-q',3]])$(id).addEventListener('change',()=>{if(Number.isFinite($(id).valueAsNumber)){setValue(21+selectedBand*4,1);setValue(21+selectedBand*4+f,$(id).valueAsNumber);finish();renderEq();}});
$('active').addEventListener('click',()=>bridge.toggleBypass());
$('preset').addEventListener('change',()=>{const v=$('preset').value;if(v.startsWith('factory:'))bridge.factoryPreset(Number(v.slice(8)));else if(v.startsWith('user:'))bridge.loadPreset(v.slice(5));});
$('library').addEventListener('click',()=>{$('preset-name').value=state.presets.some(p=>p.name===state.preset)?state.preset:'';$('preset-status').textContent='';$('preset-dialog').showModal();});
function status(text){$('preset-status').textContent=text||'Saved.';}
$('save-preset').onclick=()=>bridge.savePreset($('preset-name').value,false,status);
$('replace-preset').onclick=()=>bridge.savePreset($('preset-name').value,true,status);
$('rename-preset').onclick=()=>bridge.renamePreset(state.preset,$('preset-name').value,status);
$('delete-preset').onclick=()=>{const name=$('preset-name').value;if($('delete-preset').dataset.confirm!==name){$('delete-preset').dataset.confirm=name;$('preset-status').textContent='Click Delete again to remove this preset.';return;}bridge.deletePreset(name);delete $('delete-preset').dataset.confirm;$('preset-status').textContent='Deleted.';};
for(const [id,delta] of [['move-left',-1],['move-right',1]])$(id).onclick=()=>{const from=state.order.indexOf(contextModule);bridge.reorder(from,clamp(from+delta,0,3));$('context-menu').hidden=true;};
$('automate').onclick=()=>{bridge.automate(contextIndex);$('context-menu').hidden=true;};
$('reset').onclick=()=>{setValue(contextIndex,state.params[contextIndex].default,true);$('context-menu').hidden=true;};
document.addEventListener('pointerdown',e=>{if(!$('context-menu').contains(e.target))$('context-menu').hidden=true;});
document.addEventListener('keydown',e=>{if(e.key==='Escape')$('context-menu').hidden=true;});
window.addEventListener('blur',()=>{if(bridge)finish();});
document.addEventListener('visibilitychange',()=>{if(document.hidden){document.body.classList.remove('playing');if(bridge)finish();}});
new QWebChannel(qt.webChannelTransport,channel=>{bridge=channel.objects.modulation;bridge.snapshot.connect(render);bridge.ready();document.documentElement.dataset.connected='true';});
