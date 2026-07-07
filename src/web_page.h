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
body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:560px;margin:2rem auto;padding:0 1rem}
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
.chip{font-size:.72rem;color:#9ca3af;background:#252525;border:1px solid #3a3a3a;border-radius:999px;padding:.1rem .5rem;white-space:nowrap}
.chip.aud{color:#93c5fd;border-color:#1e3a8a}
select.mv{width:auto;font-size:.72rem;padding:.1rem .3rem;background:#252525;border:1px solid #3a3a3a;color:#9ca3af;border-radius:6px}
#led_dbg input[type=color]{width:3rem;height:2.1rem;padding:0;border:1px solid #444;background:#222;border-radius:4px}
#led_dbg select{width:auto}
#led_dbg button{margin-top:0}
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

<div class="field chk">
  <input type="checkbox" id="disable_pico_led">
  <label for="disable_pico_led">Disable the onboard Pico LED</label>
</div>

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

<div>
  <button id="save">Save</button>
  <button id="factoryreset" class="fg">Factory reset</button>
  <span id="status"></span>
</div>
<div class="hint">Factory reset restores all settings above to defaults. Paired
  controllers are kept (use <b>Forget all</b> below to remove those).</div>

<hr>

<h2>Paired controllers</h2>
<div class="hint">Controllers the adapter remembers. The adapter holds up to
  <span id="bond_max">4</span>. Once a controller is paired the adapter stops
  looking for new ones (a remembered controller reconnects on its own) &mdash;
  use <b>Pair new controller</b> to add another, or forget one to free a slot.</div>
<div id="bonds"></div>
<div id="bonds_empty" style="display:none">No paired controllers stored.</div>
<div class="btns">
  <button id="pair">Pair new controller</button>
  <button id="forgetall" class="fg">Forget all</button>
  <span id="bstatus"></span>
</div>

<div id="led_dbg" style="display:none">
<hr>
<h2>LED debug</h2>
<div class="hint">Drives the WS2812B chain directly (still brightness-capped).
Reverts to normal status display automatically after 60&nbsp;s.</div>
<div class="btns" style="flex-wrap:wrap;margin-top:.6rem">
  <input type="color" id="led_color" value="#00ff00">
  <select id="led_pixel"><option value="all">all pixels</option></select>
  <button id="led_set">Set</button>
  <button id="led_chase">Chase</button>
  <button id="led_lowy">🟡 Low batt</button>
  <button id="led_lowr">🔴 Critical</button>
  <button id="led_off">All off</button>
  <button id="led_normal">Normal</button>
  <span id="lstatus"></span>
</div>
</div>

<script>
const $=id=>document.getElementById(id);
function setStatus(msg,cls){const s=$('status');s.className=cls||'';s.textContent=msg}

function bindRange(id,out){const el=$(id);const fn=()=>$(out).textContent=el.value;el.oninput=()=>{fn();markDirty()};return fn}
const upd=[bindRange('audio_buffer_length','ab_val'),bindRange('inactive_time','it_val')];

function markDirty(){$('save').disabled=false;setStatus('unsaved changes','dirty')}
['controller_mode','polling_rate_mode','disable_inactive_disconnect','disable_pico_led','webconfig_subnet']
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
    $('webconfig_subnet').value=c.webconfig_subnet;
    if(c.webconfig_custom_ip&&c.webconfig_custom_ip!=='0.0.0.0')
      $('webconfig_custom_ip').value=c.webconfig_custom_ip;
    toggleCustomIp();
    upd.forEach(f=>f());
    $('save').disabled=true;setStatus('');
  }catch(e){setStatus('load failed','err')}
}

async function save(){
  const body=[
    'controller_mode='+$('controller_mode').value,
    'polling_rate_mode='+$('polling_rate_mode').value,
    'audio_buffer_length='+$('audio_buffer_length').value,
    'inactive_time='+$('inactive_time').value,
    'disable_inactive_disconnect='+($('disable_inactive_disconnect').checked?1:0),
    'disable_pico_led='+($('disable_pico_led').checked?1:0),
    'webconfig_subnet='+$('webconfig_subnet').value,
    'webconfig_custom_ip='+encodeURIComponent($('webconfig_custom_ip').value.trim())
  ].join('&');
  setStatus('saving…','dirty');
  try{
    const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    if(r.ok){$('save').disabled=true;setStatus('Saved ✓','ok')}
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

// ----- Paired controllers -----
function fmtAddr(h){return h.match(/.{2}/g).join(':')}
function bstatus(msg,cls){const s=$('bstatus');s.className=cls||'';s.textContent=msg}

async function loadBonds(){
  try{
    const d=await (await fetch('/api/bonds')).json();
    $('bond_max').textContent=d.max;
    const box=$('bonds');box.innerHTML='';
    const bonds=d.bonds||[];
    $('bonds_empty').style.display=bonds.length?'none':'block';
    bonds.forEach(b=>{
      const connected=d.connected&&d.connected===b.addr;
      const row=document.createElement('div');row.className='bond';
      const nm=document.createElement('input');
      nm.className='nm';nm.maxLength=15;nm.value=b.name;
      nm.placeholder=connected?'(connected)':'unnamed';
      const meta=document.createElement('span');meta.className='addr';
      meta.textContent=fmtAddr(b.addr);
      const dot=document.createElement('span');dot.className='dot';
      dot.textContent=connected?'● connected':'';
      const ren=document.createElement('button');ren.textContent='Rename';
      ren.onclick=()=>renameBond(b.addr,nm.value);
      const fg=document.createElement('button');fg.className='fg';fg.textContent='Forget';
      fg.onclick=()=>forgetBond(b.addr,nm.value||fmtAddr(b.addr));
      row.append(nm,meta,dot,ren,fg);
      box.appendChild(row);
    });
    bstatus('');
  }catch(e){bstatus('load failed','err')}
}

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

// ----- Live status (GET /api/slots) -----
const SLOT_COLORS=['#3b82f6','#ef4444','#22c55e','#ec4899'];
let lastSlots=null;
function chip(txt,extra){const c=document.createElement('span');c.className='chip'+(extra?' '+extra:'');c.textContent=txt;return c}
function slotRow(d,s){
  const row=document.createElement('div');
  row.className='slotrow'+(s.connected?'':' off');
  const dot=document.createElement('span');dot.className='dot';
  if(s.connected)dot.style.background=d.max>1?SLOT_COLORS[s.slot%4]:'#4ade80';
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
    b.className='s'+((s.battery_pct<=20&&!s.charging)?' lowb':'');
    b.textContent='🔋'+s.battery_pct+'%'+(s.charging?' ⚡':'');
    row.append(b);
  }
  // Feature availability at the current tier: rumble + adaptive triggers
  // always work on every pad; speaker/HD haptics/mic stream only while a
  // single pad is connected (Bluetooth bandwidth).
  row.append(chip('💥 rumble'),chip('🎯 triggers'));
  if(d.audio_allowed&&s.slot===d.audio_slot){
    row.append(chip('🔊 audio','aud'),chip('📳 HD haptics','aud'),chip('🎤 mic','aud'));
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
    lastSlots=d;
    const card=$('statuscard');card.innerHTML='';
    card.className=d.connected>0?'on':'';
    (d.slots||[]).forEach(s=>card.appendChild(slotRow(d,s)));
    if(d.led){
      $('led_dbg').style.display='';
      const sel=$('led_pixel');
      if(sel.options.length===1){
        for(let i=0;i<d.max*2;i++){
          const o=document.createElement('option');o.value=i;
          o.textContent='pixel '+(i+1);
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
$('led_set').onclick=()=>postLed('action=set&rgb='+$('led_color').value.slice(1)+'&pixel='+$('led_pixel').value);
$('led_chase').onclick=()=>postLed('action=chase&rgb='+$('led_color').value.slice(1));
$('led_lowy').onclick=()=>postLed('action=lowbatt&level=yellow&pixel='+$('led_pixel').value);
$('led_lowr').onclick=()=>postLed('action=lowbatt&level=red&pixel='+$('led_pixel').value);
$('led_off').onclick=()=>postLed('action=set&rgb=000000&pixel=all');
$('led_normal').onclick=()=>postLed('action=clear');

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
