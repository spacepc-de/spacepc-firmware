#pragma once

const char spacePCPortalPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>{{PROJECT_NAME}} setup</title>
  <style>
    :root{color-scheme:light dark;font-family:system-ui,sans-serif;background:#0b1020;color:#f3f6fa}
    *{box-sizing:border-box}body{margin:0;padding:0 24px 32px}.wrap{max-width:820px;margin:auto}
    .brandbar{height:64px;display:flex;align-items:center;border-bottom:1px solid #263247;margin-bottom:32px}
    .brand{color:#f3f6fa;text-decoration:none;font-weight:800;letter-spacing:-.02em;font-size:1.15rem}
    .brand span{color:#168bff}.hero{margin-bottom:24px}
    h1{font-size:2rem;line-height:1.15;margin:0 0 8px}.sub{color:#9aa7b8;margin:0}
    .status{display:flex;gap:8px;align-items:center;border-left:3px solid #168bff;background:#10192a;padding:12px 14px;margin:20px 0}
    section{border:1px solid #334155;background:#121a2b;padding:22px;margin:0 0 16px;border-radius:8px}
    .section-head{border-bottom:1px solid #263247;padding-bottom:12px;margin-bottom:16px}
    h2{margin:0 0 4px;font-size:1.1rem}.section-head small{display:block}
    label{display:block;margin:12px 0 5px;font-weight:600}
    input{width:100%;padding:11px;border:1px solid #475569;border-radius:6px;background:#0b1020;color:#f3f6fa}
    input:focus{outline:2px solid #168bff;outline-offset:2px;border-color:#168bff}
    input[type=checkbox]{width:auto;margin-right:8px}.check{display:flex;align-items:center;margin-top:16px}
    button{width:100%;border:0;border-radius:6px;background:#168bff;color:white;font-weight:700;padding:13px 18px;cursor:pointer}
    button:hover{background:#0878e5}small{color:#9aa7b8}.grid,.project-fields{display:grid;gap:12px}
    .field>small{display:block;margin-top:6px}.actions{margin-top:20px}
    footer{color:#9aa7b8;text-align:center;padding:28px 0 0;font-size:.9rem}
    footer a{color:#55c7ff;text-decoration:none}@media(min-width:620px){.grid,.project-fields{grid-template-columns:1fr 1fr}}
    .error{color:#fca5a5}.ok{color:#86efac}code{font-family:ui-monospace,monospace}
  </style>
</head>
<body><main class="wrap">
  <header class="brandbar"><a class="brand" href="https://spacepc.dev">SpacePC<span>.dev</span></a></header>
  <div class="hero">
    <h1>{{PROJECT_NAME}}</h1>
    <p class="sub">Configure this device locally. No cloud account is required.</p>
  </div>
  <p id="status" class="status">Loading device status…</p>
  <form method="post" action="/save">
    <section>
      <div class="section-head"><h2>Device</h2><small>Name this ESP32 and choose how often it reports new values.</small></div>
      <label for="deviceName">Device name</label>
      <input id="deviceName" name="deviceName" maxlength="48" required value="{{DEVICE_NAME}}">
      <label for="interval">Transmission interval in seconds</label>
      <input id="interval" name="interval" type="number" min="5" max="86400" required value="{{INTERVAL}}">
    </section>
    <section>
      <div class="section-head"><h2>{{PROJECT_SETTINGS_TITLE}}</h2><small>Configure the hardware connected to this ESP32.</small></div>
      <div class="project-fields">{{PROJECT_FIELDS}}</div>
    </section>
    <section>
      <div class="section-head"><h2>Wi-Fi</h2><small>The ESP32 connects directly to your local network.</small></div>
      <label for="wifiSsid">Network name (SSID)</label>
      <input id="wifiSsid" name="wifiSsid" maxlength="32" required value="{{WIFI_SSID}}">
      <label for="wifiPassword">Password</label>
      <input id="wifiPassword" name="wifiPassword" type="password" maxlength="63" placeholder="Leave empty to keep the stored password">
      <label class="check"><input name="clearWifiPassword" type="checkbox" value="1">Clear the stored Wi-Fi password (open network)</label>
    </section>
    <section>
      <div class="section-head"><h2>MQTT <small>(optional)</small></h2><small>Only needed for MQTT clients. The native Home Assistant integration works without a broker.</small></div>
      <div class="grid">
        <div><label for="mqttHost">Broker host or IP</label><input id="mqttHost" name="mqttHost" maxlength="128" value="{{MQTT_HOST}}"></div>
        <div><label for="mqttPort">Port</label><input id="mqttPort" name="mqttPort" type="number" min="1" max="65535" required value="{{MQTT_PORT}}"></div>
      </div>
      <div class="grid">
        <div><label for="mqttUsername">Username</label><input id="mqttUsername" name="mqttUsername" maxlength="64" value="{{MQTT_USERNAME}}"></div>
        <div><label for="mqttPassword">Password</label><input id="mqttPassword" name="mqttPassword" type="password" maxlength="128" placeholder="Leave empty to keep the stored password"></div>
      </div>
      <label class="check"><input name="clearMqttPassword" type="checkbox" value="1">Clear the stored MQTT password</label>
      <label for="mqttBaseTopic">Base topic</label>
      <input id="mqttBaseTopic" name="mqttBaseTopic" maxlength="128" required value="{{MQTT_TOPIC}}">
      <label class="check"><input name="homeAssistantDiscovery" type="checkbox" value="1" {{HA_CHECKED}}>Publish Home Assistant MQTT Discovery</label>
      <small>Leave the broker empty when using only the native SpacePC Home Assistant integration. MQTT traffic is unencrypted and intended for trusted local networks.</small>
    </section>
    <div class="actions"><button type="submit">Save settings and restart</button></div>
  </form>
  <footer>Firmware and projects by <a href="https://spacepc.dev">SpacePC.dev</a></footer>
</main>
<script>
fetch('/api/status').then(r=>r.json()).then(s=>{
  const el=document.querySelector('#status');
  el.textContent=`Project: ${s.project} · Wi-Fi: ${s.wifi} · MQTT: ${s.mqtt} · IP: ${s.ip}`;
  el.className=s.project==='ready'?'status ok':'status error';
}).catch(()=>document.querySelector('#status').textContent='Status unavailable');
</script></body></html>
)HTML";
