#ifndef DS5_BRIDGE_WEB_PORTAL_H
#define DS5_BRIDGE_WEB_PORTAL_H

// Onboarding captive-portal page, served at / while the WiFi-WOL build is in AP
// mode (no stored WLAN, or BOOTSEL-forced). Lets the user pick a nearby network
// (GET /api/wifi_scan) or type one manually, then save credentials
// (POST /api/wifi_provision) -- after which the dongle reboots into STA mode.
// Self-contained, no external assets (the AP has no internet). Kept small: it is
// streamed from flash exactly like the config page (see web_api.cpp).
static const char PORTAL_PAGE[] = R"rawhtml(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DS5 WiFi Setup</title>
<style>
:root{color-scheme:dark}
body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:480px;margin:1.5rem auto;padding:0 1rem}
h1{font-size:1.3rem}h1 small{color:#888;font-weight:normal;font-size:.65em;display:block;margin-top:.2rem}
.field{margin:1rem 0}
label{display:block;margin-bottom:.3rem;font-size:.95rem}
select,input{background:#222;border:1px solid #444;color:#eee;padding:.5rem;border-radius:4px;width:100%;box-sizing:border-box;font-size:1rem}
button{background:#2563eb;border:0;color:#fff;padding:.6rem 1.3rem;border-radius:4px;cursor:pointer;font-size:1rem;width:100%;margin-top:.5rem}
button:disabled{background:#333;color:#777}
.row{display:flex;gap:.5rem;align-items:center}
.row button{width:auto;margin:0;padding:.5rem .8rem;background:#333}
.hint{color:#888;font-size:.8rem;margin-top:.3rem}
#msg{min-height:1.3em;margin-top:.6rem}
.ok{color:#4ade80}.err{color:#f87171}.busy{color:#facc15}
.lock{color:#888;font-size:.85em}
</style></head><body>
<h1>DS5 Dongle WiFi Setup<small>Pick your home network so the dongle can reach the config page and wake your PC.</small></h1>

<div class="field">
  <label for="net">Network</label>
  <div class="row">
    <select id="net"><option value="">Scanning&hellip;</option></select>
    <button type="button" id="rescan">&#x21bb;</button>
  </div>
  <div class="hint">Not listed? Choose &ldquo;Other&hellip;&rdquo; to type it in (e.g. a hidden network).</div>
</div>

<div class="field" id="ssidManualWrap" style="display:none">
  <label for="ssidManual">Network name (SSID)</label>
  <input id="ssidManual" autocomplete="off" autocapitalize="off" spellcheck="false" maxlength="32">
</div>

<div class="field" id="pwWrap">
  <label for="pw">Password</label>
  <div class="row">
    <input id="pw" type="password" autocomplete="off" maxlength="63">
    <button type="button" id="eye" title="Show/hide password">&#x1F441;</button>
  </div>
  <div class="hint">Leave blank for an open network.</div>
</div>

<button id="save">Save &amp; Connect</button>
<div id="msg"></div>

<script>
var sel=document.getElementById('net'),manual=document.getElementById('ssidManual'),
    manualWrap=document.getElementById('ssidManualWrap'),pwWrap=document.getElementById('pwWrap'),
    pw=document.getElementById('pw'),msg=document.getElementById('msg'),
    save=document.getElementById('save'),rescan=document.getElementById('rescan');

function setMsg(t,c){msg.textContent=t;msg.className=c||'';}

function onSel(){
  var v=sel.value;
  manualWrap.style.display=(v==='__other__')?'block':'none';
  // Hide password for an open (unsecured) network unless typing manually.
  var opt=sel.options[sel.selectedIndex];
  var open=opt&&opt.dataset.secure==='0'&&v!=='__other__'&&v!=='';
  pwWrap.style.display=open?'none':'block';
  if(open) pw.value='';
}
sel.addEventListener('change',onSel);

// Rebuild the dropdown ONLY when the set of SSIDs actually changed, so a refresh
// never destroys the option the user is currently picking. We track the last
// rendered signature; if it matches, we leave the <select> untouched.
var lastSig='', polls=0, pollTimer=null;
function render(nets){
  var sig=nets.map(function(n){return n.ssid;}).join('\n');
  if(sig===lastSig) return;          // no change -> don't disturb the selection
  lastSig=sig;
  var cur=sel.value;
  sel.innerHTML='';
  nets.forEach(function(n){
    var o=document.createElement('option');
    o.value=n.ssid;o.dataset.secure=n.secure;
    o.textContent=n.ssid+(n.secure?' \u{1F512}':'')+'  ('+n.rssi+' dBm)';
    sel.appendChild(o);
  });
  var oth=document.createElement('option');oth.value='__other__';oth.textContent='Other… (type manually)';
  sel.appendChild(oth);
  // Restore the prior selection if it still exists, else leave the first entry.
  if(cur){for(var i=0;i<sel.options.length;i++){if(sel.options[i].value===cur){sel.selectedIndex=i;break;}}}
  onSel();
}
function poll(){
  fetch('/api/wifi_scan').then(function(r){return r.json();}).then(function(d){
    render(d.nets||[]);
    polls++;
    // Keep polling only while a scan is genuinely running, and cap the total so
    // we can't loop forever. Once done, stop -- the rescan button restarts it.
    if(d.scanning && polls<12){pollTimer=setTimeout(poll,1500);setMsg('Scanning…','busy');}
    else{setMsg((d.nets&&d.nets.length)?'':'No networks found — type yours manually, or tap ↻.','');}
  }).catch(function(){
    setMsg('Scan failed; you can still type a network manually.','err');
    render([]);sel.value='__other__';onSel();
  });
}
function startScan(){
  if(pollTimer){clearTimeout(pollTimer);pollTimer=null;}
  polls=0;setMsg('Scanning…','busy');
  fetch('/api/wifi_scan').then(poll).catch(poll);
}
rescan.addEventListener('click',startScan);

var eye=document.getElementById('eye');
eye.addEventListener('click',function(){
  pw.type=(pw.type==='password')?'text':'password';
});

function byteLen(s){
  return encodeURIComponent(s).replace(/%[0-9A-F]{2}/g,'x').length;
}

save.addEventListener('click',function(){
  var ssid=(sel.value==='__other__')?manual.value.trim():sel.value;
  if(!ssid){setMsg('Choose or type a network first.','err');return;}
  if(byteLen(ssid)>32){setMsg('Network name is too long.','err');return;}
  var pskLen=byteLen(pw.value);
  if(pskLen>63){setMsg('Password is too long.','err');return;}
  if(pskLen>0&&pskLen<8){setMsg('Password must be at least 8 characters, or blank for open WiFi.','err');return;}
  save.disabled=true;setMsg('Saving…','busy');
  var body='ssid='+encodeURIComponent(ssid)+'&psk='+encodeURIComponent(pw.value);
  fetch('/api/wifi_provision',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
    .then(function(r){return r.json();}).then(function(d){
      if(d.ok){setMsg('Saved! The dongle is restarting and joining “'+ssid+'”. You can close this page.','ok');}
      else{setMsg('Could not save — check the network name.','err');save.disabled=false;}
    }).catch(function(){
      // The reboot can drop the connection before the response arrives; treat as success.
      setMsg('Saved. The dongle is restarting — reconnect to your home WiFi.','ok');
    });
});

startScan();
</script>
</body></html>)rawhtml";

#endif // DS5_BRIDGE_WEB_PORTAL_H
