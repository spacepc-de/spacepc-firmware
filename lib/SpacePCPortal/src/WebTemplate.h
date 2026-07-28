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
    *{box-sizing:border-box}body{margin:0;padding:24px}.wrap{max-width:760px;margin:auto}
    h1{font-size:2rem;margin-bottom:8px}.sub{color:#9aa7b8;margin:0 0 24px}
    section{border:1px solid #334155;background:#121a2b;padding:20px;margin:0 0 16px;border-radius:8px}
    h2{margin:0 0 16px;font-size:1.1rem}label{display:block;margin:12px 0 5px}
    input{width:100%;padding:11px;border:1px solid #475569;border-radius:6px;background:#0b1020;color:#f3f6fa}
    input[type=checkbox]{width:auto;margin-right:8px}.check{display:flex;align-items:center;margin-top:16px}
    button{border:0;border-radius:6px;background:#168bff;color:white;font-weight:700;padding:12px 18px;cursor:pointer}
    small,.status{color:#9aa7b8}.grid{display:grid;gap:12px}@media(min-width:620px){.grid{grid-template-columns:1fr 1fr}}
    .error{color:#fca5a5}.ok{color:#86efac}code{font-family:ui-monospace,monospace}
  </style>
</head>
<body><main class="wrap">
  <h1>{{PROJECT_NAME}}</h1>
  <p class="sub">Configure this SpacePC device. Settings are stored locally on the ESP32.</p>
  <p id="status" class="status">Loading device status…</p>
  <form method="post" action="/save">
    <section>
      <h2>Device</h2>
      <label for="deviceName">Device name</label>
      <input id="deviceName" name="deviceName" maxlength="48" required value="{{DEVICE_NAME}}">
      <label for="interval">Transmission interval in seconds</label>
      <input id="interval" name="interval" type="number" min="5" max="86400" required value="{{INTERVAL}}">
    </section>
    <section>
      <h2>Project settings</h2>
      {{PROJECT_FIELDS}}
    </section>
    <section>
      <h2>Wi-Fi</h2>
      <label for="wifiSsid">Network name (SSID)</label>
      <input id="wifiSsid" name="wifiSsid" maxlength="32" required value="{{WIFI_SSID}}">
      <label for="wifiPassword">Password</label>
      <input id="wifiPassword" name="wifiPassword" type="password" maxlength="63" placeholder="Leave empty to keep the stored password">
      <label class="check"><input name="clearWifiPassword" type="checkbox" value="1">Clear the stored Wi-Fi password (open network)</label>
    </section>
    <section>
      <h2>MQTT <small>(optional)</small></h2>
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
    <button type="submit">Save and restart</button>
  </form>
</main>
<script>
fetch('/api/status').then(r=>r.json()).then(s=>{
  const el=document.querySelector('#status');
  el.textContent=`Project: ${s.project} · Wi-Fi: ${s.wifi} · MQTT: ${s.mqtt} · IP: ${s.ip}`;
  el.className=s.project==='ready'?'status ok':'status error';
}).catch(()=>document.querySelector('#status').textContent='Status unavailable');
</script></body></html>
)HTML";
