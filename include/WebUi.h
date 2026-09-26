#pragma once
#include <Arduino.h>

static const char WEB_UI[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TTLock Bridge</title><style>
:root{font-family:system-ui,sans-serif;color-scheme:dark;background:#101317;color:#e8edf2}body{max-width:980px;margin:0 auto;padding:24px}.card{background:#181d23;border:1px solid #2a313a;border-radius:14px;padding:18px;margin:16px 0}input,button,textarea{font:inherit;border-radius:8px;border:1px solid #3a444f;background:#0f1317;color:#fff;padding:10px}input,textarea{width:100%;box-sizing:border-box}button{cursor:pointer;background:#2b6fd8;border:0;font-weight:600}button.secondary{background:#303943}button.danger{background:#a43b3b}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}label{display:block;font-size:.9rem;color:#b6c0ca;margin-bottom:5px}.muted{color:#94a0ac}.ok{color:#6fdc8c}.bad{color:#ff7777}pre{white-space:pre-wrap;word-break:break-all;background:#0d1115;padding:12px;border-radius:8px}.hidden{display:none}h1,h2{margin-top:0}
</style></head><body>
<h1>UniFi Access → TTLock Bridge</h1><div id="status" class="muted">Loading…</div>
<div id="login" class="card hidden"><h2>Login</h2><input id="loginPassword" type="password" placeholder="Admin password"><br><br><button onclick="login()">Login</button></div>
<div id="setup" class="card hidden"><h2>First setup</h2><p>Set the local web password. A random 256-bit site key and webhook token will be generated.</p><input id="setupPassword" type="password" placeholder="New admin password (min 8 chars)"><br><br><button onclick="setupDevice()">Create configuration</button></div>
<div id="app" class="hidden">
<div class="card"><h2>Bridge secrets</h2><p class="muted">Keep the site key somewhere safe. Replacement ESPs must use the same key to decrypt contexts already stored in UniFi Access.</p><div class="grid"><div><label>Site encryption/decryption key (256-bit hex)</label><input id="siteKey"></div><div><label>Webhook Bearer token</label><input id="webhookToken" readonly></div></div><br><div class="row"><button onclick="saveSiteKey()">Save site key</button><button class="secondary" onclick="generateSiteKey()">Generate new site key</button><button class="secondary" onclick="regenToken()">Regenerate webhook token</button></div></div>
<div class="card"><h2>Add / initialize lock</h2>
<p class="muted">For a new lock: factory-reset it, wake/touch the keypad, scan, select it, then press <b>Initialize New Lock</b>. The lock is normally discoverable for only about 10–15 seconds after wake-up.</p>
<div class="row"><button onclick="scanLocks()">Scan nearby TTLocks</button><button class="secondary" onclick="initNewLock()">Initialize New Lock</button><span id="scanStatus" class="muted"></span></div><div id="scanResults"></div><br>
<div class="grid"><div><label>Lock ID</label><input id="lockId" value="front-door"></div><div><label>Name</label><input id="lockName" value="Front Door"></div><div><label>MAC</label><input id="lockMac" placeholder="AA:BB:CC:DD:EE:FF"></div><div><label>AES key (16 bytes hex)</label><input id="aesKey" placeholder="001122...eeff"></div><div><label>Unlock key (digits)</label><input id="unlockKey"></div><div><label>Admin passcode (optional)</label><input id="adminPasscode"></div><div><label>Protocol type</label><input id="protocolType" type="number" value="5"></div><div><label>Protocol version</label><input id="protocolVersion" type="number" value="3"></div><div><label>Scene</label><input id="scene" type="number" value="2"></div><div><label>Group ID</label><input id="groupId" type="number" value="1"></div><div><label>Organisation ID</label><input id="orgId" type="number" value="1"></div></div><br>
<div class="row"><button onclick="testLock()">Test unlock</button><button onclick="makeBundle()">Generate UniFi context</button></div><p id="lockResult"></p><pre id="bundle">The generated encrypted UniFi custom context will appear here.</pre><button class="secondary" onclick="copyBundle()">Copy context JSON</button></div>
<div class="card"><h2>UniFi webhook</h2><p>Use <code>GET</code> or <code>POST http://&lt;esp-ip&gt;/api/unlock</code> with Bearer authentication. Add the three generated custom headers <code>X-TTLock-Nonce</code>, <code>X-TTLock-Data</code>, and <code>X-TTLock-Tag</code>. No request body is required. The ESP decrypts the headers in RAM, queues the command, finds the configured lock, unlocks, and disconnects.</p></div>
</div>
<script>
const $=id=>document.getElementById(id);async function api(path,opt={}){opt.headers={...(opt.headers||{}),'Content-Type':'application/json'};let r=await fetch(path,opt);let t=await r.text();let j;try{j=JSON.parse(t)}catch{j={message:t}}if(!r.ok)throw new Error(j.error||j.message||r.statusText);return j}
async function boot(){try{let s=await api('/api/status');$('status').textContent=`IP ${s.ip} · ${s.configured?'configured':'not configured'}`;if(!s.configured){$('setup').classList.remove('hidden');return}try{await api('/api/secrets');$('app').classList.remove('hidden');loadSecrets()}catch{$('login').classList.remove('hidden')}}catch(e){$('status').textContent=e.message}}
async function setupDevice(){try{let j=await api('/api/setup',{method:'POST',body:JSON.stringify({password:$('setupPassword').value})});alert('Setup complete. Save the site key shown next.');location.reload()}catch(e){alert(e.message)}}
async function login(){try{await api('/api/login',{method:'POST',body:JSON.stringify({password:$('loginPassword').value})});location.reload()}catch(e){alert(e.message)}}
async function loadSecrets(){let s=await api('/api/secrets');$('siteKey').value=s.site_key;$('webhookToken').value=s.webhook_token}
async function saveSiteKey(){try{await api('/api/settings/site-key',{method:'POST',body:JSON.stringify({site_key:$('siteKey').value})});alert('Saved')}catch(e){alert(e.message)}}
async function generateSiteKey(){if(!confirm('This changes the key. Existing UniFi contexts will stop decrypting unless regenerated. Continue?'))return;let s=await api('/api/settings/generate-site-key',{method:'POST',body:'{}'});$('siteKey').value=s.site_key}
async function regenToken(){let s=await api('/api/settings/regenerate-token',{method:'POST',body:'{}'});$('webhookToken').value=s.webhook_token}
async function scanLocks(){try{$('scanStatus').textContent='Scanning…';let s=await api('/api/ble/scan');$('scanStatus').textContent=`${s.devices.length} candidate(s)`;window.lastBleScan=s.devices;$('scanResults').innerHTML=s.devices.map((d,i)=>{let proto=d.protocol_detected?` · ${d.protocol_name}`:' · protocol unknown';return `<button class="secondary" style="margin:6px" onclick="pick(${i})">${d.name||'TTLock'} · ${d.mac} · ${d.rssi} dBm${proto}</button>`}).join('')}catch(e){$('scanStatus').textContent=e.message}}
function pick(index){let d=(window.lastBleScan||[])[index];if(!d)return;$('lockMac').value=d.mac;$('lockName').value=d.name||'TTLock';if(d.protocol_detected){$('protocolType').value=d.protocol_type;$('protocolVersion').value=d.protocol_version;$('lockResult').className=d.factory_provisioning_supported?'ok':'muted';$('lockResult').textContent=`Detected ${d.protocol_name}. ${d.factory_provisioning_supported?'Local factory provisioning supported.':'Recognized, but factory provisioning is not implemented for this protocol.'}`}}
function lockObj(){return{id:$('lockId').value,name:$('lockName').value,mac:$('lockMac').value,aes_key:$('aesKey').value,unlock_key:$('unlockKey').value,admin_passcode:$('adminPasscode').value,protocol_type:+$('protocolType').value,protocol_version:+$('protocolVersion').value,scene:+$('scene').value,group_id:+$('groupId').value,org_id:+$('orgId').value}}
function initObj(){let o=lockObj();delete o.aes_key;delete o.unlock_key;delete o.admin_passcode;return o}
async function initNewLock(){
  if(!$('lockMac').value){alert('Scan and select a factory-reset lock first.');return}
  if(!confirm('Initialize this factory-reset TTLock locally? If provisioning fails partway through you may need to factory-reset the lock before retrying.'))return;
  try{
    $('lockResult').className='muted';$('lockResult').textContent='Initializing lock… keep it awake and close to the ESP.';
    let j=await api('/api/locks/init',{method:'POST',body:JSON.stringify(initObj())});
    $('aesKey').value=j.lock_data.aes_key||'';
    $('unlockKey').value=j.lock_data.unlock_key||'';
    $('protocolType').value=j.lock_data.protocol_type;
    $('protocolVersion').value=j.lock_data.protocol_version;
    $('scene').value=j.lock_data.scene;
    $('groupId').value=j.lock_data.group_id;
    $('orgId').value=j.lock_data.org_id;
    $('bundle').textContent=JSON.stringify(j.bundle,null,2);
    $('lockResult').className='ok';
    $('lockResult').textContent=`${j.message} Generated admin secret: ${j.lock_data.admin_ps}`;
  }catch(e){$('lockResult').className='bad';$('lockResult').textContent=e.message}
}
async function testLock(){try{$('lockResult').textContent='Connecting…';let j=await api('/api/locks/test',{method:'POST',body:JSON.stringify(lockObj())});$('lockResult').className='ok';$('lockResult').textContent=j.message}catch(e){$('lockResult').className='bad';$('lockResult').textContent=e.message}}
async function makeBundle(){try{let j=await api('/api/locks/bundle',{method:'POST',body:JSON.stringify(lockObj())});let t=j.ttlock||{};$('bundle').textContent=`Authorization: Bearer ${$('webhookToken').value}\nX-TTLock-Nonce: ${t.nonce||''}\nX-TTLock-Data: ${t.data||''}\nX-TTLock-Tag: ${t.tag||''}\n\nMethod: GET or POST\nURL: http://<esp-ip>/api/unlock\nBody: none` }catch(e){alert(e.message)}}
function copyBundle(){navigator.clipboard.writeText($('bundle').textContent)}boot();
</script></body></html>
)HTML";
