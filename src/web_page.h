#ifndef DS5_BRIDGE_WEB_PAGE_H
#define DS5_BRIDGE_WEB_PAGE_H

// Config UI served at http://10.55.55.105/ (default subnet; selectable in the UI).
// Single self-contained page; loads from GET /api/config and persists via
// POST /api/config. Settings mirror Config_body (src/config.h); the firmware
// re-validates every field, so the page is a convenience, not the source of
// truth for bounds.
//
// The COMPLETE HTTP response (headers + body) lives in flash so serving the
// ~10 KB page never touches the heap -- make_file()'s malloc of the whole
// response was the single biggest allocation in the firmware, on a heap the
// audio path already pressures. No Content-Length: every response uses
// Connection: close, so the peer reads to EOF.
static const char WEB_PAGE_RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Cache-Control: no-store\r\n"
    "Connection: close\r\n"
    "\r\n"
    R"rawhtml(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DS5-Linux-Bridge</title>
<style>
:root{color-scheme:dark}
body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:740px;margin:2rem auto;padding:0 1rem}
h1{font-size:1.4rem}h1 small{color:#888;font-weight:normal;font-size:.7em}
.field{margin:1.1rem 0}
label.lbl{display:block;margin-bottom:.3rem;font-size:.95rem}
.hint{color:#888;font-size:.8rem;margin-top:.2rem}
select,input[type=number]{background:#222;border:1px solid #444;color:#eee;padding:.4rem;border-radius:4px;width:100%;box-sizing:border-box;font-size:.95rem}
input[type=range]{width:100%}
.chk{display:flex;align-items:center;gap:.5rem}
.chk input{width:auto}
button{background:#2563eb;border:0;color:#fff;padding:.55rem 1.3rem;border-radius:4px;cursor:pointer;font-size:1rem;margin-top:1rem}
button:disabled{background:#333;color:#777;cursor:default}
#status{min-height:1.2em;margin-left:1rem}
.dirty{color:#facc15}
.ok{color:#4ade80}
.err{color:#f87171}
hr{border:0;border-top:1px solid #333;margin:2rem 0}
h2{font-size:1.1rem;margin-bottom:.3rem}
.bond{display:flex;align-items:center;gap:.5rem;flex-wrap:wrap;padding:.5rem 0;border-bottom:1px solid #222}
.bond .nm{flex:1 1 8rem;min-width:0;background:#222;border:1px solid #444;color:#eee;padding:.35rem;border-radius:4px;font-size:.9rem}
.bond .addr{color:#888;font-size:.78rem;font-family:monospace}
.bond .dot{color:#4ade80;font-size:.78rem;white-space:nowrap}
.bond button{margin:0;padding:.35rem .7rem;font-size:.85rem;background:#3a3a3a;flex:none}
.bond button.fg,button.fg{background:#7f1d1d}
.btns{display:flex;gap:.5rem;align-items:center}
#bonds_empty{color:#888;font-size:.9rem}
#statuscard{display:flex;flex-direction:column;gap:.55rem;background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:.7rem 1rem;margin:1rem 0}
.slotrow{display:flex;align-items:center;gap:.55rem;flex-wrap:wrap}
.slotrow .dot{width:.6rem;height:.6rem;border-radius:50%;background:#555;flex:none}
.slotrow .s{font-size:.9rem}
.slotrow .s b{color:#fff}
.slotrow.off .s{color:#777}
.slotrow .lowb{color:#f87171}
.slotrow .warnb{color:#facc15}
.chip{font-size:.72rem;color:#9ca3af;background:#252525;border:1px solid #3a3a3a;border-radius:999px;padding:.1rem .5rem;white-space:nowrap}
.chip.aud{color:#93c5fd;border-color:#1e3a8a}
select.mv{width:auto;font-size:.72rem;padding:.1rem .3rem;background:#252525;border:1px solid #3a3a3a;color:#9ca3af;border-radius:6px}
#led_dbg input[type=color]{width:3rem;height:2.1rem;padding:0;border:1px solid #444;background:#222;border-radius:4px}
#led_dbg select{width:auto}
#led_dbg button{margin-top:0;padding:.4rem .9rem;font-size:.85rem;background:#3a3a3a}
.ledrow{display:flex;align-items:center;gap:.5rem;flex-wrap:wrap;margin-top:.6rem}
.ledrow .ledlbl{color:#888;font-size:.85rem;min-width:6.5rem}
.ledmaprow{display:flex;align-items:center;gap:.3rem;margin:.3rem 0;flex-wrap:wrap}
.ledmaprow .mlbl{color:#888;font-size:.75rem;min-width:3.2rem}
.ledcell{width:1.2rem;height:1.2rem;border-radius:50%;border:1px solid #444;background:#222;cursor:pointer;font-size:.55rem;color:#666;display:inline-flex;align-items:center;justify-content:center;user-select:none}
#layout{display:flex;gap:1rem;align-items:flex-start;margin-top:1rem}
#nav{flex:0 0 8.7rem;display:flex;flex-direction:column;gap:.35rem;position:sticky;top:1rem}
#nav button{background:#1a1a1a;border:1px solid #333;color:#bbb;text-align:left;padding:.55rem .8rem;border-radius:6px;margin:0;font-size:.9rem;cursor:pointer}
#nav button.act{background:#2563eb;border-color:#2563eb;color:#fff}
#content{flex:1;min-width:0;border:1px solid #333;border-radius:8px;background:#151515;padding:.2rem 1rem .6rem}
.pane{display:none}
.pane.act{display:block}
@media (max-width:600px){#layout{flex-direction:column}#nav{position:static;flex-direction:row;flex-wrap:wrap;flex-basis:auto;width:100%}}
.batt{display:inline-flex;align-items:center;gap:.35rem}
.batt .bar{width:34px;height:14px;border:1px solid #888;border-radius:2px;position:relative;padding:1px}
.batt .bar::after{content:"";position:absolute;right:-3px;top:4px;width:2px;height:6px;background:#888}
.batt .fill{height:100%;background:#4ade80;border-radius:1px}
.batt.low .fill{background:#f87171}
footer{margin:2.5rem 0 1rem;padding-top:1rem;border-top:1px solid #333;display:flex;gap:1rem;flex-wrap:wrap;align-items:center;font-size:.85rem;color:#888}
footer a{color:#60a5fa;text-decoration:none}
footer a:hover{text-decoration:underline}
footer .kofi{color:#fff;background:#13c3ff;padding:.3rem .7rem;border-radius:4px}
footer .kofi:hover{text-decoration:none;opacity:.9}
</style></head><body>
<h1>DS5-Linux-Bridge <small id="ver"></small></h1>
<p>Adapter configuration. Changes are saved to the adapter's flash.</p>

<div id="statuscard"><div class="slotrow"><span class="dot"></span><span class="s">Checking…</span></div></div>

<div id="layout">
<nav id="nav">
  <button data-pane="controller" class="act">Controller</button>
  <button data-pane="paired">Paired controllers</button>
  <button data-pane="lights">Lights</button>
  <button data-pane="network">Network</button>
</nav>
<div id="content">

<section class="pane act" id="pane_controller">

<div class="field">
  <label class="lbl">Controller mode</label>
  <select id="controller_mode">
    <option value="2">Auto-detect</option>
    <option value="0">DualSense (DS5)</option>
    <option value="1">DualSense Edge (DSE)</option>
  </select>
  <div class="hint">Takes effect after reconnecting the controller.</div>
</div>

<div class="field">
  <label class="lbl">Polling rate</label>
  <select id="polling_rate_mode">
    <option value="0">250 Hz</option>
    <option value="1">500 Hz</option>
    <option value="2">Real-time (1000 Hz)</option>
  </select>
  <div class="hint">Takes effect after reconnecting the controller.</div>
</div>

<div class="field">
  <label class="lbl">Audio buffer length: <span id="ab_val"></span></label>
  <input type="range" id="audio_buffer_length" min="16" max="128" step="1">
  <div class="hint">Lower = less latency, higher = more stutter resistance (16-128).</div>
</div>

<div class="field">
  <label class="lbl">Inactivity timeout: <span id="it_val"></span> min</label>
  <input type="range" id="inactive_time" min="5" max="60" step="1">
  <div class="hint">Disconnect the controller after this idle time.</div>
</div>

<div class="field chk">
  <input type="checkbox" id="disable_inactive_disconnect">
  <label for="disable_inactive_disconnect">Never auto-disconnect on inactivity</label>
</div>

</section>

<section class="pane" id="pane_lights">

<div class="field">
  <label class="lbl">Slot colors</label>
  <div class="ledrow" id="slot_colors" style="margin-top:.2rem"></div>
  <div class="hint">Lightbar and strip LED color per controller slot (default
  blue, like player 1 on a PS5). Battery warnings on the strip still blink
  yellow/red, and games can still override the lightbar while they run.</div>
</div>

<div class="field">
  <div class="chk">
    <input type="checkbox" id="lightbar_override">
    <label for="lightbar_override">Lightbar override: repaint this color with the slot color</label>
  </div>
  <div style="display:flex;align-items:center;gap:.6rem;margin-top:.3rem">
    <span class="hint" style="margin:0">Color to watch for</span>
    <input type="color" id="lightbar_filter_rgb">
  </div>
  <div class="hint">When the PC writes exactly this color (default black) to a
  controller's lightbar, the dongle shows the slot color instead — this keeps
  seat colors through Steam's brightness-0% black and the blank colors
  rumble-only drivers send. Turn it <b>off</b> to give the OS full control of
  the lightbar (e.g. if Steam Input keeps fighting your colors), including
  turning it black.</div>
</div>

<div class="field">
  <div class="chk">
    <input type="checkbox" id="player_led_lock">
    <label for="player_led_lock">Always show the slot number on the player LEDs</label>
  </div>
  <div class="hint">With 2+ controllers connected, each pad's white player LEDs
  stay pinned to its slot number even if the PC (e.g. a glitchy Steam Input)
  tries to clear or change them. With a single controller the PC stays in
  control as usual.</div>
</div>

<div class="field chk">
  <input type="checkbox" id="disable_pico_led">
  <label for="disable_pico_led">Disable the onboard Pico LED</label>
</div>

<div id="led_strip_cfg" style="display:none">
<div class="field">
  <label class="lbl">LED strip layout</label>
  <div style="display:flex;align-items:center;gap:.6rem">
    <span class="hint" style="margin:0">Number of LEDs</span>
    <input type="number" id="led_count" min="1" max="32" style="width:5rem">
  </div>
  <div class="hint">⚠️ The Pico can safely power at most <b>8</b> LEDs from
  its own USB supply. For more, power the strip from an external 5&nbsp;V
  source — only the data line stays on GP28.</div>
  <div id="led_map" style="margin-top:.6rem"></div>
  <div class="hint">Click the LEDs each slot should light — any shape works
  (line, ring, square…). LEDs are numbered from the first one on the strip.
  Unassigned LEDs stay dark. The <b>Pairing</b> row blinks (in its chosen
  color) while the dongle is searching for a controller.</div>
</div>
</div>

<div id="led_dbg" style="display:none">
<div class="field">
  <label class="lbl">LED debug</label>
  <div class="hint">Drives the WS2812B strip directly (still brightness-capped).
  Everything reverts to the live status display automatically after 60&nbsp;s.</div>
  <div class="ledrow">
    <span class="ledlbl">Simulate slot</span>
    <select id="led_slot"></select>
    <button id="led_sim_low">Low battery</button>
    <button id="led_sim_crit">Critical</button>
    <button id="led_sim_norm">Normal</button>
  </div>
  <div class="ledrow">
    <span class="ledlbl">Whole strip</span>
    <input type="color" id="led_color" value="#0000ff">
    <button id="led_chase">Chase</button>
    <button id="led_off">All off</button>
    <button id="led_normal">All normal</button>
    <span id="lstatus"></span>
  </div>
</div>
</div>

</section>

<section class="pane" id="pane_network">

<div class="field">
  <label class="lbl">Config page address</label>
  <select id="webconfig_subnet">
    <option value="0">10.55.55.105 (default)</option>
    <option value="1">172.31.55.105</option>
    <option value="2">192.168.137.105</option>
    <option value="3">Custom…</option>
  </select>
  <div class="hint">Where this page is served. Change only if it collides with
  your network — or to give each of several adapters on one PC its own address.
  Takes effect after you unplug and replug the adapter — then browse to the new
  address.</div>
  <div id="customip_wrap" style="display:none;margin-top:.5rem">
    <input id="webconfig_custom_ip" type="text" inputmode="decimal"
           placeholder="e.g. 10.20.30.105" pattern="\d{1,3}(\.\d{1,3}){3}">
    <div class="hint">⚠️ <b>Advanced.</b> Must be a <b>private</b> address
    (<code>10.x.x.x</code>, <code>172.16–31.x.x</code>, or
    <code>192.168.x.x</code>), and not a <code>.0</code>/<code>.255</code>. If
    you enter something unreachable the adapter falls back to the default
    address — you won't get locked out, but you may not land where you expected.
    The PC gets a DHCP lease in the same <code>/29</code> block.</div>
  </div>
</div>

</section>

<section class="pane" id="pane_paired">
<div class="hint" style="margin-top:1rem">Controllers the adapter remembers. The
  adapter holds up to <span id="bond_max">4</span>. Once a controller is paired
  the adapter stops looking for new ones (a remembered controller reconnects on
  its own) &mdash; use <b>Pair new controller</b> to add another, or forget one
  to free a slot. The list updates automatically, but if it ever looks stale
  (right after pairing or forgetting), hit <b>Refresh</b>.</div>
<div id="bonds"></div>
<div id="bonds_empty" style="display:none">No paired controllers stored.</div>
<div class="btns">
  <button id="pair">Pair new controller</button>
  <button id="forgetall" class="fg">Forget all</button>
  <button id="bonds_refresh" style="background:#3a3a3a">Refresh</button>
  <span id="bstatus"></span>
</div>
</section>

</div>
</div>

<div>
  <button id="save">Save</button>
  <button id="factoryreset" class="fg">Factory reset</button>
  <button id="flashmode" style="background:#3a3a3a">Flash mode</button>
  <span id="status"></span>
</div>
<div class="hint">Factory reset restores all settings to defaults. Paired
  controllers are kept (use <b>Forget all</b> to remove those). Flash mode
  reboots the adapter into its UF2 bootloader for a firmware update — no need
  to reach the BOOTSEL button.</div>

<script>
const $=id=>document.getElementById(id);
function setStatus(msg,cls){const s=$('status');s.className=cls||'';s.textContent=msg}

function bindRange(id,out){const el=$(id);const fn=()=>$(out).textContent=el.value;el.oninput=()=>{fn();markDirty()};return fn}
const upd=[bindRange('audio_buffer_length','ab_val'),bindRange('inactive_time','it_val')];

function markDirty(){$('save').disabled=false;setStatus('unsaved changes','dirty')}
['controller_mode','polling_rate_mode','disable_inactive_disconnect','disable_pico_led','player_led_lock','lightbar_override','lightbar_filter_rgb','webconfig_subnet']
  .forEach(id=>$(id).onchange=markDirty);
function toggleCustomIp(){$('customip_wrap').style.display=$('webconfig_subnet').value==='3'?'':'none'}
$('webconfig_subnet').addEventListener('change',toggleCustomIp);
$('webconfig_custom_ip').oninput=markDirty;

async function load(){
  try{
    const c=await (await fetch('/api/config')).json();
    $('ver').textContent=c.version;
    $('controller_mode').value=c.controller_mode;
    $('polling_rate_mode').value=c.polling_rate_mode;
    $('audio_buffer_length').value=c.audio_buffer_length;
    $('inactive_time').value=c.inactive_time;
    $('disable_inactive_disconnect').checked=!!c.disable_inactive_disconnect;
    $('disable_pico_led').checked=!!c.disable_pico_led;
    $('player_led_lock').checked=!c.disable_player_led_lock;
    $('lightbar_override').checked=!c.disable_lightbar_override;
    if(c.lightbar_filter_rgb)$('lightbar_filter_rgb').value='#'+c.lightbar_filter_rgb.toLowerCase();
    $('webconfig_subnet').value=c.webconfig_subnet;
    if(c.webconfig_custom_ip&&c.webconfig_custom_ip!=='0.0.0.0')
      $('webconfig_custom_ip').value=c.webconfig_custom_ip;
    toggleCustomIp();
    const sc=$('slot_colors');
    if(sc.children.length===0){
      for(let i=0;i<(c.max_slots||4);i++){
        const w=document.createElement('label');
        w.style.cssText='display:flex;flex-direction:column;align-items:center;gap:.25rem;font-size:.75rem;color:#888';
        const inp=document.createElement('input');inp.type='color';inp.id='slot_rgb'+i;
        inp.onchange=markDirty;
        w.append(inp,document.createTextNode('Slot '+(i+1)));
        sc.appendChild(w);
      }
    }
    (c.slot_rgb||[]).forEach((h,i)=>{const el=$('slot_rgb'+i);if(el)el.value='#'+h.toLowerCase()});
    slotColors=c.slot_rgb||null;
    maxSlots=c.max_slots||4;
    ledMax=c.led_max||32;
    $('led_count').max=ledMax;
    ledCount=c.led_count||8;
    $('led_count').value=ledCount;
    ledMasks=(c.led_masks||['2','8','20','80']).map(h=>parseInt(h,16)>>>0);
    pairingMask=parseInt(c.pairing_mask||'55',16)>>>0;
    if(c.pairing_rgb)pairingRgb='#'+c.pairing_rgb.toLowerCase();
    buildLedMap();
    upd.forEach(f=>f());
    $('save').disabled=true;setStatus('');
  }catch(e){setStatus('load failed','err')}
}

async function save(){
  const parts=[
    'controller_mode='+$('controller_mode').value,
    'polling_rate_mode='+$('polling_rate_mode').value,
    'audio_buffer_length='+$('audio_buffer_length').value,
    'inactive_time='+$('inactive_time').value,
    'disable_inactive_disconnect='+($('disable_inactive_disconnect').checked?1:0),
    'disable_pico_led='+($('disable_pico_led').checked?1:0),
    'disable_player_led_lock='+($('player_led_lock').checked?0:1),
    'disable_lightbar_override='+($('lightbar_override').checked?0:1),
    'lightbar_filter_rgb='+$('lightbar_filter_rgb').value.slice(1),
    'webconfig_subnet='+$('webconfig_subnet').value,
    'webconfig_custom_ip='+encodeURIComponent($('webconfig_custom_ip').value.trim())
  ];
  for(let i=0;i<4;i++){
    const el=$('slot_rgb'+i);
    if(el)parts.push('slot_rgb'+i+'='+el.value.slice(1));
  }
  parts.push('led_count='+ledCount);
  for(let i=0;i<4;i++)parts.push('led_mask'+i+'='+(ledMasks[i]>>>0).toString(16));
  parts.push('pairing_mask='+(pairingMask>>>0).toString(16));
  parts.push('pairing_rgb='+pairingRgb.slice(1));
  const body=parts.join('&');
  setStatus('saving…','dirty');
  try{
    const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    if(r.ok){
      $('save').disabled=true;setStatus('Saved ✓','ok');
      const sc=[];
      for(let i=0;i<4;i++){const el=$('slot_rgb'+i);sc.push(el?el.value.slice(1).toUpperCase():'0000FF')}
      slotColors=sc;
      buildLedMap();
      loadStatus();
    }
    else setStatus('save failed — not written to flash, try again','err');
  }catch(e){setStatus('save failed','err')}
}

$('save').onclick=save;

async function factoryReset(){
  if(!confirm('Reset all settings to defaults? Paired controllers are kept.'))return;
  setStatus('resetting…','dirty');
  try{
    const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'factory_reset=1'});
    if(r.ok){setStatus('Reset ✓ — reloading','ok');setTimeout(()=>location.reload(),600)}
    else setStatus('reset failed — not written to flash, try again','err');
  }catch(e){setStatus('reset failed','err')}
}
$('factoryreset').onclick=factoryReset;

async function flashMode(){
  if(!confirm('Reboot into flash (UF2) mode?\n\nThe adapter stops working and shows up as a USB drive instead — copy the new firmware .uf2 onto it and it restarts as the bridge.\n\nChanged your mind after clicking OK? Just unplug and replug the adapter to boot the current firmware.'))return;
  setStatus('rebooting…','dirty');
  try{
    const r=await fetch('/api/reboot',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action=bootsel'});
    if(r.ok)setStatus('Flash mode ✓ — this page is now offline; look for the USB drive','ok');
    else setStatus('flash mode refused','err');
  }catch(e){setStatus('flash mode request failed','err')}
}
$('flashmode').onclick=flashMode;

// ----- Paired controllers -----
function fmtAddr(h){return h.match(/.{2}/g).join(':')}
function bstatus(msg,cls){const s=$('bstatus');s.className=cls||'';s.textContent=msg}

async function loadBonds(){
  try{
    const d=await (await fetch('/api/bonds')).json();
    // The BT stack may still be starting when the page loads (the bond list
    // reads its flash store); retry until it reports ready.
    if(d.ready===false){
      $('bonds_empty').style.display='none';
      setTimeout(loadBonds,1500);
      return;
    }
    $('bond_max').textContent=d.max;
    const box=$('bonds');box.innerHTML='';
    const bonds=d.bonds||[];
    $('bonds_empty').style.display=bonds.length?'none':'block';
    bonds.forEach(b=>{
      const row=document.createElement('div');row.className='bond';
      const nm=document.createElement('input');
      nm.className='nm';nm.maxLength=15;
      nm.value=b.name||'DualSense'; // default name; rename to tell pads apart
      const meta=document.createElement('span');meta.className='addr';
      meta.textContent=fmtAddr(b.addr);
      row.append(nm,meta);
      if(b.slot>=0){
        const sc=document.createElement('span');sc.className='chip';
        sc.textContent='Slot '+(b.slot+1);
        const col=(slotColors&&slotColors[b.slot])?'#'+slotColors[b.slot]:SLOT_COLORS[b.slot%4];
        sc.style.color=col;sc.style.borderColor=col;
        row.append(sc);
      }
      const ren=document.createElement('button');ren.textContent='Rename';
      ren.onclick=()=>renameBond(b.addr,nm.value);
      const fg=document.createElement('button');fg.className='fg';fg.textContent='Forget';
      fg.onclick=()=>forgetBond(b.addr,nm.value||fmtAddr(b.addr));
      row.append(ren,fg);
      box.appendChild(row);
    });
    bstatus('');
    bondsRetries=0;
  }catch(e){
    // Fetches fail transiently while the dongle is busy (flash writes,
    // connects/disconnects). Retry quietly a few times; past that, the
    // Refresh button is the recovery path — no scary sticky error text.
    bstatus('');
    if(bondsRetries++<3)setTimeout(loadBonds,2000);
  }
}
let bondsRetries=0;

async function postBonds(body,msg){
  bstatus(msg,'dirty');
  try{
    const r=await fetch('/api/bonds',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    if(r.ok){bstatus('Done ✓','ok');loadBonds()}
    else if(r.status===409)bstatus('no free slot — forget a controller first','err');
    else bstatus('failed','err');
  }catch(e){bstatus('failed','err')}
}
function renameBond(addr,name){
  postBonds('action=rename&addr='+addr+'&name='+encodeURIComponent(name),'saving…');
}
function forgetBond(addr,label){
  if(!confirm('Forget "'+label+'"?\nYou will need to re-pair it (Share + PS).'))return;
  postBonds('action=forget&addr='+addr,'forgetting…');
}
$('pair').onclick=()=>{
  const haveFree=lastSlots&&(lastSlots.connected<lastSlots.max);
  const msg=haveFree
    ?'Pair a new controller?\nPut it in pairing mode (hold Share + PS until the light bar flashes).'
    :'Pair a new controller?\nThe controller you are using now will disconnect (it stays remembered and reconnects later). Then put the new controller in pairing mode (hold Share + PS until the light bar flashes).';
  if(!confirm(msg))return;
  postBonds('action=pair','opening pairing…');
};
$('forgetall').onclick=()=>{
  if(!confirm('Forget ALL paired controllers?\nEach will need to be re-paired.'))return;
  postBonds('action=forgetall','forgetting all…');
};
$('bonds_refresh').onclick=()=>{bondsRetries=0;bstatus('refreshing…','dirty');loadBonds()};

// ----- LED strip layout mapper -----
let maxSlots=4;
let ledMax=32;
let ledCount=8;
let ledMasks=[2,8,32,128]; // default: slot k -> pixel 2k+1
let pairingMask=0x55; // default: the spacer pixels blink in pairing mode
let pairingRgb='#ffffff'; // pairing blink color (default white)
function slotCss(i){return (slotColors&&slotColors[i])?'#'+slotColors[i]:SLOT_COLORS[i%4]}
function buildLedMap(){
  const box=$('led_map');box.innerHTML='';
  for(let s=0;s<maxSlots;s++){
    const row=document.createElement('div');row.className='ledmaprow';
    const l=document.createElement('span');l.className='mlbl';l.textContent='Slot '+(s+1);
    row.appendChild(l);
    for(let p=0;p<ledCount;p++){
      const c=document.createElement('span');c.className='ledcell';
      c.textContent=p+1;
      const paint=()=>{
        const on=!!(ledMasks[s]&(1<<p));
        c.style.background=on?slotCss(s):'#222';
        c.style.borderColor=on?'#999':'#444';
        c.style.color=on?'#111':'#666';
      };
      paint();
      c.onclick=()=>{ledMasks[s]^=(1<<p);paint();markDirty()};
      row.appendChild(c);
    }
    box.appendChild(row);
  }
  // Pairing row: pixels that blink (in the chosen color) while the dongle
  // is searching for a controller (explicit pairing window, or nothing
  // bonded yet).
  const prow=document.createElement('div');prow.className='ledmaprow';
  const pl=document.createElement('span');pl.className='mlbl';pl.textContent='Pairing';
  prow.appendChild(pl);
  const cells=[];
  for(let p=0;p<ledCount;p++){
    const c=document.createElement('span');c.className='ledcell';
    c.textContent=p+1;
    const paint=()=>{
      const on=!!(pairingMask&(1<<p));
      c.style.background=on?pairingRgb:'#222';
      c.style.borderColor=on?'#999':'#444';
      c.style.color=on?'#111':'#666';
    };
    paint();
    c.onclick=()=>{pairingMask^=(1<<p);paint();markDirty()};
    prow.appendChild(c);
    cells.push(paint);
  }
  const pc=document.createElement('input');pc.type='color';pc.id='pairing_rgb';
  pc.value=pairingRgb;
  pc.style.cssText='width:2rem;height:1.4rem;padding:0;border:1px solid #444;background:#222;border-radius:4px;margin-left:.3rem';
  pc.onchange=()=>{pairingRgb=pc.value;cells.forEach(f=>f());markDirty()};
  prow.appendChild(pc);
  box.appendChild(prow);
}
$('led_count').onchange=()=>{
  let v=parseInt($('led_count').value,10);
  if(isNaN(v))v=8;
  v=Math.min(ledMax,Math.max(1,v));
  $('led_count').value=v;
  ledCount=v;
  buildLedMap();
  markDirty();
};

// ----- Live status (GET /api/slots) -----
const SLOT_COLORS=['#3b82f6','#ef4444','#22c55e','#ec4899']; // fallback until config loads
let slotColors=null; // configured per-slot colors ("RRGGBB"), set by load()
let lastSlots=null;
function chip(txt,extra){const c=document.createElement('span');c.className='chip'+(extra?' '+extra:'');c.textContent=txt;return c}
function slotRow(d,s){
  const row=document.createElement('div');
  row.className='slotrow'+(s.connected?'':' off');
  const dot=document.createElement('span');dot.className='dot';
  if(s.connected)dot.style.background=d.max>1
    ?(slotColors&&slotColors[s.slot]?'#'+slotColors[s.slot]:SLOT_COLORS[s.slot%4])
    :'#4ade80';
  const txt=document.createElement('span');txt.className='s';
  const pre=d.max>1?('Slot '+(s.slot+1)+': '):'';
  row.append(dot,txt);
  if(!s.connected){
    txt.textContent=pre+(d.max>1?'empty':'No controller connected');
    return row;
  }
  const model=s.model==='DSE'?'DualSense Edge':'DualSense';
  txt.textContent=pre+model+(s.name?' “'+s.name+'”':'');
  if(s.battery_valid){
    const b=document.createElement('span');
    // Same thresholds as the strip LEDs: <=20% critical (red), <=40% low (yellow).
    const warn=s.charging?'':(s.battery_pct<=20?' lowb':(s.battery_pct<=40?' warnb':''));
    b.className='s'+warn;
    b.textContent=s.battery_pct+'%'+(s.charging?' charging':'');
    row.append(b);
  }
  // Feature availability at the current tier: rumble + adaptive triggers
  // always work on every pad; speaker/HD haptics/mic stream only while a
  // single pad is connected (Bluetooth bandwidth).
  row.append(chip('rumble'),chip('triggers'));
  if(d.audio_allowed&&s.slot===d.audio_slot){
    row.append(chip('audio','aud'),chip('HD haptics','aud'),chip('mic','aud'));
  }
  if(d.max>1){
    const mv=document.createElement('select');mv.className='mv';
    const ph=document.createElement('option');ph.value='';ph.textContent='⇄ move';
    mv.appendChild(ph);
    for(let i=0;i<d.max;i++){
      if(i===s.slot)continue;
      const o=document.createElement('option');o.value=i;
      o.textContent='to slot '+(i+1);
      mv.appendChild(o);
    }
    mv.onchange=async()=>{
      if(mv.value==='')return;
      const ok=await postSlots('action=swap&a='+s.slot+'&b='+mv.value);
      if(!ok)alert('move failed (slot busy?)');
      loadStatus();
    };
    row.append(mv);
  }
  return row;
}

async function postSlots(body){
  try{
    const r=await fetch('/api/slots',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    return r.ok;
  }catch(e){return false}
}
async function loadStatus(){
  try{
    const d=await (await fetch('/api/slots')).json();
    // A connect/disconnect can change the bond list (pairing just finished,
    // forget just disconnected a pad) — refresh it on any count change.
    if(lastSlots&&lastSlots.connected!==d.connected)loadBonds();
    lastSlots=d;
    const card=$('statuscard');card.innerHTML='';
    card.className=d.connected>0?'on':'';
    (d.slots||[]).forEach(s=>card.appendChild(slotRow(d,s)));
    if(d.led){
      $('led_dbg').style.display='';
      $('led_strip_cfg').style.display='';
      const sel=$('led_slot');
      if(sel.options.length===0){
        for(let i=0;i<d.max;i++){
          const o=document.createElement('option');o.value=i;
          o.textContent='Slot '+(i+1);
          sel.appendChild(o);
        }
      }
    }
  }catch(e){
    const card=$('statuscard');card.innerHTML='';
    const row=document.createElement('div');row.className='slotrow off';
    const t=document.createElement('span');t.className='s';t.textContent='status unavailable';
    row.append(t);card.appendChild(row);
  }
}

// ----- LED debug (POST /api/led) -----
async function postLed(body){
  const st=$('lstatus');st.className='dirty';st.textContent='…';
  try{
    const r=await fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    st.className=r.ok?'ok':'err';st.textContent=r.ok?'✓':'failed';
  }catch(e){st.className='err';st.textContent='failed'}
}
$('led_sim_low').onclick=()=>postLed('action=sim&slot='+$('led_slot').value+'&level=low');
$('led_sim_crit').onclick=()=>postLed('action=sim&slot='+$('led_slot').value+'&level=critical');
$('led_sim_norm').onclick=()=>postLed('action=sim&slot='+$('led_slot').value+'&level=normal');
$('led_chase').onclick=()=>postLed('action=chase&rgb='+$('led_color').value.slice(1));
$('led_off').onclick=()=>postLed('action=set&rgb=000000&pixel=all');
$('led_normal').onclick=()=>postLed('action=clear');

// Re-fetch the bond list whenever the drawer is opened, so it can never
// stay stale from a fetch that ran before the BT stack was up.
// ----- Category navigation (sidebar) -----
document.querySelectorAll('#nav button').forEach(b=>{
  b.onclick=()=>{
    document.querySelectorAll('#nav button').forEach(x=>x.classList.remove('act'));
    b.classList.add('act');
    document.querySelectorAll('.pane').forEach(p=>p.classList.remove('act'));
    $('pane_'+b.dataset.pane).classList.add('act');
    // The bond list can go stale while hidden; refresh on entry.
    if(b.dataset.pane==='paired')loadBonds();
  };
});

load();
loadBonds();
loadStatus();
setInterval(loadStatus,4000);
</script>
<footer>
  <a href="https://github.com/kungaa/ds5-linux-bridge" target="_blank" rel="noopener">GitHub</a>
  <a class="kofi" href="https://ko-fi.com/mkungaa" target="_blank" rel="noopener">☕ Support on Ko-fi</a>
</footer>
</body></html>
)rawhtml";

#endif // DS5_BRIDGE_WEB_PAGE_H
