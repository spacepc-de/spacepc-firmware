#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_arduino_version.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <soc/soc_caps.h>

namespace {
constexpr uint16_t dnsPort = 53;
constexpr uint32_t serialReportIntervalMs = 5000;

DNSServer dnsServer;
WebServer webServer(80);
String accessPointName;
String sketchMd5;
uint32_t lastSerialReport = 0;

const char dashboardPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="de">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="theme-color" content="#07111f">
  <title>SpacePC Test Firmware</title>
  <style>
    :root{color-scheme:dark;font-family:Inter,ui-sans-serif,system-ui,sans-serif;background:#07111f;color:#eaf2ff}
    *{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at top right,#14345c 0,transparent 35%),#07111f}
    main{width:min(1080px,calc(100% - 28px));margin:auto;padding:34px 0 60px}
    header{display:flex;gap:18px;align-items:flex-start;justify-content:space-between;margin-bottom:24px}
    h1{font-size:clamp(1.8rem,5vw,3.2rem);line-height:1;margin:0 0 10px;letter-spacing:-.04em}
    p{margin:0;color:#9eb0c7}.live{display:flex;align-items:center;gap:8px;color:#8cf0bd;font-size:.9rem;white-space:nowrap}
    .dot{width:9px;height:9px;border-radius:50%;background:#35dc88;box-shadow:0 0 14px #35dc88}
    .hero,.card{border:1px solid #243c59;background:#0c1a2bcc;border-radius:16px;box-shadow:0 16px 50px #0004}
    .hero{display:grid;grid-template-columns:repeat(4,1fr);gap:1px;overflow:hidden;margin-bottom:18px;background:#243c59}
    .metric{background:#0c1a2b;padding:20px}.metric span{display:block;color:#8fa4bd;font-size:.78rem;text-transform:uppercase;letter-spacing:.08em}
    .metric strong{display:block;font-size:1.35rem;margin-top:7px;overflow-wrap:anywhere}
    .grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px}.card{padding:20px}
    h2{font-size:1rem;margin:0 0 13px;color:#cce0fb}dl{margin:0}
    .row{display:grid;grid-template-columns:minmax(120px,.8fr) minmax(0,1.2fr);gap:12px;padding:9px 0;border-top:1px solid #1b3049}
    .row:first-child{border-top:0}dt{color:#8fa4bd}dd{margin:0;text-align:right;overflow-wrap:anywhere}
    .bar{height:7px;background:#172a41;border-radius:9px;overflow:hidden;margin-top:12px}.bar i{height:100%;display:block;background:linear-gradient(90deg,#35dc88,#32a8ff)}
    .actions{display:flex;gap:10px;margin-top:18px;flex-wrap:wrap}
    button,input{font:inherit;border:1px solid #315071;border-radius:9px;color:#eaf2ff;background:#13263c;padding:9px 13px}
    button{cursor:pointer}button:hover{background:#1a3452}button.danger{border-color:#7d3f4d;color:#ffc2cb}
    input{width:90px}.tool{padding-top:14px;margin-top:14px;border-top:1px solid #1b3049}.tool:first-of-type{border-top:0;margin-top:0;padding-top:0}
    .tool p{font-size:.88rem;margin:7px 0 11px}.result{margin-top:11px;color:#8cf0bd;white-space:pre-wrap;overflow-wrap:anywhere}
    .error{color:#ff9eae}.wide{grid-column:1/-1}
    footer{color:#71879f;font-size:.82rem;margin-top:18px}
    @media(max-width:760px){header{display:block}.live{margin-top:14px}.hero{grid-template-columns:repeat(2,1fr)}.grid{grid-template-columns:1fr}}
  </style>
</head>
<body><main>
  <header><div><h1>Test Firmware</h1></div><div class="live"><i class="dot"></i><span id="connection">connected</span></div></header>
  <section class="hero">
    <div class="metric"><span>Chip</span><strong id="chip">–</strong></div>
    <div class="metric"><span>Uptime</span><strong id="uptime">–</strong></div>
    <div class="metric"><span>Free heap</span><strong id="heap">–</strong></div>
    <div class="metric"><span>Portal clients</span><strong id="clients">–</strong></div>
  </section>
  <div class="grid">
    <section class="card"><h2>Processor & system</h2><dl id="system"></dl></section>
    <section class="card"><h2>Memory</h2><dl id="memory"></dl><div class="bar"><i id="heapbar"></i></div></section>
    <section class="card"><h2>Firmware & Flash</h2><dl id="firmware"></dl></section>
    <section class="card"><h2>Wi-Fi & captive portal</h2><dl id="network"></dl></section>
    <section class="card wide">
      <h2>Tools</h2>
      <div class="tool">
        <strong>Scan the I²C bus</strong>
        <p>Choose the SDA and SCL GPIOs. Only use pins wired correctly for 3.3 V logic.</p>
        <div class="actions">
          <label>SDA <input id="sda" type="number" min="0" max="48"></label>
          <label>SCL <input id="scl" type="number" min="0" max="48"></label>
          <button id="scan">Start I²C scan</button>
        </div>
        <div id="scanresult" class="result"></div>
      </div>
      <div class="tool">
        <strong>Test PSRAM and clear free space</strong>
        <p>Allocates free PSRAM in blocks, verifies it with a test pattern, overwrites the allocated blocks with zeros, and releases them. Memory used by the running firmware remains untouched.</p>
        <button id="psramtest">Start PSRAM test</button>
        <div id="psramresult" class="result"></div>
      </div>
      <div class="tool">
        <strong>Storage locations</strong>
        <p>Firmware, bootloader, NVS, and the partition table reside in internal flash. This test firmware does not write settings to NVS, but Wi-Fi credentials or settings left by previous firmware can survive a normal reflash. Heap and PSRAM are volatile.</p>
        <button id="erasenvs" class="danger">Erase NVS and old Wi-Fi data</button>
        <div id="nvsresult" class="result"></div>
      </div>
      <div class="tool">
        <button id="restart" class="danger">Restart ESP32</button>
        <div id="restartresult" class="result"></div>
      </div>
    </section>
  </div>
  <div class="actions"><button id="refresh">Refresh now</button><button id="download">Download diagnostics as JSON</button></div>
  <footer>Automatically refreshes every 2 seconds · Serial output at 115200 baud</footer>
</main>
<script>
let latest;
const bytes=n=>n>=1048576?(n/1048576).toFixed(2)+' MiB':n>=1024?(n/1024).toFixed(1)+' KiB':n+' B';
const duration=ms=>{let s=Math.floor(ms/1000),d=Math.floor(s/86400);s%=86400;let h=Math.floor(s/3600);s%=3600;let m=Math.floor(s/60);s%=60;return(d?d+'d ':'')+[h,m,s].map(v=>String(v).padStart(2,'0')).join(':')};
const rows=(id,values)=>document.querySelector(id).innerHTML=Object.entries(values).map(([k,v])=>`<div class="row"><dt>${k}</dt><dd>${v}</dd></div>`).join('');
const callTool=async(url,options,result)=>{
  const el=document.querySelector(result);el.className='result';el.textContent='Please wait…';
  try{const r=await fetch(url,options);const data=await r.json();if(!r.ok)throw new Error(data.error||('HTTP '+r.status));el.textContent=data.message}
  catch(e){el.className='result error';el.textContent='Error: '+e.message}
};
async function update(){
  try{
    const response=await fetch('/api/status',{cache:'no-store'});
    if(!response.ok)throw new Error(response.status);
    const s=latest=await response.json();
    chip.textContent=s.chip.model;uptime.textContent=duration(s.runtime.uptime_ms);heap.textContent=bytes(s.memory.free_heap);clients.textContent=s.network.clients;
    rows('#system',{'Chip model':s.chip.model,'Revision':s.chip.revision,'CPU cores':s.chip.cores,'CPU frequency':s.chip.cpu_mhz+' MHz','Features':s.chip.features.join(', ')||'–','Temperature':s.chip.temperature_c.toFixed(1)+' °C','Reset reason':s.runtime.reset_reason,'ESP ID':s.chip.efuse_id});
    rows('#memory',{'Total heap':bytes(s.memory.heap_size),'Free heap':bytes(s.memory.free_heap),'Minimum free heap':bytes(s.memory.min_free_heap),'Largest block':bytes(s.memory.max_alloc_heap),'Total PSRAM':bytes(s.memory.psram_size),'Free PSRAM':bytes(s.memory.free_psram)});
    rows('#firmware',{'Project':s.firmware.name,'Built':s.firmware.build,'Arduino Core':s.firmware.arduino,'ESP-IDF':s.firmware.idf,'Sketch size':bytes(s.firmware.sketch_size),'Free sketch space':bytes(s.firmware.free_sketch_space),'Flash size':bytes(s.firmware.flash_size),'NVS partition':bytes(s.firmware.nvs_size),'Flash frequency':(s.firmware.flash_speed/1000000)+' MHz','Flash mode':s.firmware.flash_mode,'Sketch MD5':s.firmware.sketch_md5});
    rows('#network',{'SSID':s.network.ssid,'Security':'Open (no password)','Portal address':'http://'+s.network.ip,'AP MAC':s.network.mac,'Channel':s.network.channel,'Connected devices':s.network.clients,'Hostname':s.network.hostname});
    if(!sda.dataset.initialized){sda.value=s.pins.default_sda;scl.value=s.pins.default_scl;sda.dataset.initialized='1'}
    heapbar.style.width=Math.max(0,Math.min(100,s.memory.free_heap/s.memory.heap_size*100))+'%';
    connection.textContent='connected · '+new Date().toLocaleTimeString();connection.className='';
  }catch(e){connection.textContent='connection lost';connection.className='error'}
}
refresh.onclick=update;
download.onclick=()=>{if(!latest)return;const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(latest,null,2)],{type:'application/json'}));a.download='esp32-diagnose.json';a.click();URL.revokeObjectURL(a.href)};
scan.onclick=()=>callTool('/api/i2c-scan?sda='+encodeURIComponent(sda.value)+'&scl='+encodeURIComponent(scl.value),{method:'POST'},'#scanresult');
psramtest.onclick=()=>callTool('/api/psram-test',{method:'POST'},'#psramresult').then(update);
restart.onclick=()=>callTool('/api/restart',{method:'POST'},'#restartresult');
erasenvs.onclick=()=>{if(confirm('Erase the entire NVS partition? Old Wi-Fi credentials and all application settings stored there will be permanently lost.'))callTool('/api/erase-nvs',{method:'POST'},'#nvsresult')};
update();setInterval(update,2000);
</script></body></html>
)HTML";

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "\\r");
  value.replace("\t", "\\t");
  return value;
}

String formatEfuseId() {
  const uint64_t id = ESP.getEfuseMac();
  char output[17];
  snprintf(
    output,
    sizeof(output),
    "%08lx%08lx",
    static_cast<unsigned long>(id >> 32),
    static_cast<unsigned long>(id)
  );
  return String(output);
}

String resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software";
    case ESP_RST_PANIC: return "Software panic";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "Unknown";
  }
}

String flashModeName() {
  switch (ESP.getFlashChipMode()) {
    case FM_QIO: return "QIO";
    case FM_QOUT: return "QOUT";
    case FM_DIO: return "DIO";
    case FM_DOUT: return "DOUT";
    case FM_FAST_READ: return "FAST_READ";
    case FM_SLOW_READ: return "SLOW_READ";
    default: return "UNKNOWN";
  }
}

String arduinoVersion() {
  return String(ESP_ARDUINO_VERSION_MAJOR) + "." +
    String(ESP_ARDUINO_VERSION_MINOR) + "." +
    String(ESP_ARDUINO_VERSION_PATCH);
}

String featureList() {
  esp_chip_info_t info;
  esp_chip_info(&info);
  String result = "[";
  bool hasFeature = false;
#if defined(CHIP_FEATURE_EMB_FLASH)
  if (info.features & CHIP_FEATURE_EMB_FLASH) {
    result += "\"embedded flash\"";
    hasFeature = true;
  }
#endif
#if defined(CHIP_FEATURE_WIFI_BGN)
  if (info.features & CHIP_FEATURE_WIFI_BGN) {
    result += hasFeature ? ",\"Wi-Fi\"" : "\"Wi-Fi\"";
    hasFeature = true;
  }
#endif
#if defined(CHIP_FEATURE_BLE)
  if (info.features & CHIP_FEATURE_BLE) {
    result += hasFeature ? ",\"BLE\"" : "\"BLE\"";
    hasFeature = true;
  }
#endif
#if defined(CHIP_FEATURE_BT)
  if (info.features & CHIP_FEATURE_BT) {
    result += hasFeature ? ",\"Bluetooth\"" : "\"Bluetooth\"";
  }
#endif
  result += "]";
  return result;
}

String diagnosticsJson() {
  esp_chip_info_t chipInfo;
  esp_chip_info(&chipInfo);
  const esp_partition_t *nvsPartition = esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA,
    ESP_PARTITION_SUBTYPE_DATA_NVS,
    nullptr
  );
  String payload;
  payload.reserve(1600);
  payload += "{";
  payload += "\"chip\":{";
  payload += "\"model\":\"" + jsonEscape(ESP.getChipModel()) + "\",";
  payload += "\"revision\":" + String(ESP.getChipRevision()) + ",";
  payload += "\"cores\":" + String(chipInfo.cores) + ",";
  payload += "\"cpu_mhz\":" + String(ESP.getCpuFreqMHz()) + ",";
  payload += "\"features\":" + featureList() + ",";
  payload += "\"temperature_c\":" + String(temperatureRead(), 1) + ",";
  payload += "\"efuse_id\":\"" + formatEfuseId() + "\"},";
  payload += "\"runtime\":{";
  payload += "\"uptime_ms\":" + String(millis()) + ",";
  payload += "\"reset_reason\":\"" + resetReasonName() + "\"},";
  payload += "\"memory\":{";
  payload += "\"heap_size\":" + String(ESP.getHeapSize()) + ",";
  payload += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  payload += "\"min_free_heap\":" + String(ESP.getMinFreeHeap()) + ",";
  payload += "\"max_alloc_heap\":" + String(ESP.getMaxAllocHeap()) + ",";
  payload += "\"psram_size\":" + String(ESP.getPsramSize()) + ",";
  payload += "\"free_psram\":" + String(ESP.getFreePsram()) + "},";
  payload += "\"pins\":{";
  payload += "\"default_sda\":" + String(SDA) + ",";
  payload += "\"default_scl\":" + String(SCL) + "},";
  payload += "\"firmware\":{";
  payload += "\"name\":\"SpacePC Test Firmware\",";
  payload += "\"build\":\"" __DATE__ " " __TIME__ "\",";
  payload += "\"arduino\":\"" + arduinoVersion() + "\",";
  payload += "\"idf\":\"" + jsonEscape(ESP.getSdkVersion()) + "\",";
  payload += "\"sketch_size\":" + String(ESP.getSketchSize()) + ",";
  payload += "\"free_sketch_space\":" + String(ESP.getFreeSketchSpace()) + ",";
  payload += "\"flash_size\":" + String(ESP.getFlashChipSize()) + ",";
  payload += "\"flash_speed\":" + String(ESP.getFlashChipSpeed()) + ",";
  payload += "\"flash_mode\":\"" + flashModeName() + "\",";
  payload += "\"nvs_size\":" + String(nvsPartition ? nvsPartition->size : 0) + ",";
  payload += "\"sketch_md5\":\"" + sketchMd5 + "\"},";
  payload += "\"network\":{";
  payload += "\"mode\":\"access-point\",";
  payload += "\"ssid\":\"" + jsonEscape(accessPointName) + "\",";
  payload += "\"open\":true,";
  payload += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
  payload += "\"mac\":\"" + WiFi.softAPmacAddress() + "\",";
  payload += "\"channel\":" + String(WiFi.channel()) + ",";
  payload += "\"clients\":" + String(WiFi.softAPgetStationNum()) + ",";
  payload += "\"hostname\":\"" + jsonEscape(WiFi.getHostname()) + "\"}";
  payload += "}";
  return payload;
}

void sendToolResult(int status, const String &message, bool error = false) {
  String payload = error
    ? "{\"error\":\"" + jsonEscape(message) + "\"}"
    : "{\"message\":\"" + jsonEscape(message) + "\"}";
  webServer.send(status, "application/json; charset=utf-8", payload);
}

void handleI2cScan() {
  if (!webServer.hasArg("sda") || !webServer.hasArg("scl")) {
    sendToolResult(400, "SDA and SCL are required.", true);
    return;
  }
  const int sda = webServer.arg("sda").toInt();
  const int scl = webServer.arg("scl").toInt();
  if (
    sda < 0 || scl < 0 ||
    sda >= SOC_GPIO_PIN_COUNT || scl >= SOC_GPIO_PIN_COUNT ||
    sda == scl ||
    !digitalPinCanOutput(sda) || !digitalPinCanOutput(scl)
  ) {
    sendToolResult(400, "Invalid GPIO combination for an I²C bus.", true);
    return;
  }

  Wire.end();
  if (!Wire.begin(sda, scl, 100000)) {
    sendToolResult(500, "Could not start the I²C bus on these GPIOs.", true);
    return;
  }

  String devices;
  uint8_t deviceCount = 0;
  uint8_t errorCount = 0;
  for (uint8_t address = 1; address < 127; address += 1) {
    Wire.beginTransmission(address);
    const uint8_t result = Wire.endTransmission();
    if (result == 0) {
      char formatted[7];
      snprintf(formatted, sizeof(formatted), "0x%02X", address);
      if (!devices.isEmpty()) {
        devices += ", ";
      }
      devices += formatted;
      deviceCount += 1;
    } else if (result == 4) {
      errorCount += 1;
    }
    delay(1);
  }
  Wire.end();

  String message =
    "Scan on SDA GPIO " + String(sda) +
    " / SCL GPIO " + String(scl) + ": ";
  message += deviceCount == 0
    ? "No I²C devices found."
    : String(deviceCount) + " device(s) found: " + devices;
  if (errorCount > 0) {
    message += " Bus errors occurred at " + String(errorCount) + " address(es).";
  }
  Serial.println("[I2C] " + message);
  sendToolResult(200, message);
}

void handlePsramTest() {
  constexpr size_t blockSize = 32 * 1024;
  constexpr size_t maxBlocks = 256;
  void *blocks[maxBlocks] = {};
  size_t blockCount = 0;
  size_t testedBytes = 0;
  bool verified = true;

  if (ESP.getPsramSize() == 0) {
    sendToolResult(409, "This board reports no available PSRAM.", true);
    return;
  }

  while (blockCount < maxBlocks) {
    void *block = heap_caps_malloc(
      blockSize,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!block) {
      break;
    }
    memset(block, 0xA5, blockSize);
    const uint8_t *bytes = static_cast<const uint8_t *>(block);
    for (size_t index = 0; index < blockSize; index += 1) {
      if (bytes[index] != 0xA5) {
        verified = false;
        break;
      }
    }
    memset(block, 0, blockSize);
    blocks[blockCount++] = block;
    testedBytes += blockSize;
    delay(0);
  }

  for (size_t index = 0; index < blockCount; index += 1) {
    heap_caps_free(blocks[index]);
  }

  String message =
    String(testedBytes) + " bytes of free PSRAM were allocated, ";
  message += verified
    ? "successfully verified, zeroed, and released."
    : "a memory error was detected; the allocated blocks were zeroed and released.";
  Serial.println("[PSRAM] " + message);
  sendToolResult(verified ? 200 : 500, message, !verified);
}

void handleRestart() {
  sendToolResult(200, "Restarting now. The portal will be available again shortly.");
  Serial.println("[SYSTEM] Restart requested from the portal.");
  delay(350);
  ESP.restart();
}

void handleEraseNvs() {
  const esp_err_t result = nvs_flash_erase();
  if (result != ESP_OK) {
    sendToolResult(
      500,
      "Could not erase NVS: " + String(esp_err_to_name(result)),
      true
    );
    return;
  }
  Serial.println("[NVS] The entire NVS partition was erased.");
  sendToolResult(
    200,
    "NVS was erased completely. Old Wi-Fi data and application settings were removed; the ESP32 is restarting."
  );
  delay(500);
  ESP.restart();
}

void redirectToPortal() {
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  webServer.send(302, "text/plain", "");
}

void startWebServer() {
  webServer.on("/", HTTP_GET, [] {
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send_P(200, "text/html; charset=utf-8", dashboardPage);
  });
  webServer.on("/api/status", HTTP_GET, [] {
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/json; charset=utf-8", diagnosticsJson());
  });
  webServer.on("/api/i2c-scan", HTTP_POST, handleI2cScan);
  webServer.on("/api/psram-test", HTTP_POST, handlePsramTest);
  webServer.on("/api/erase-nvs", HTTP_POST, handleEraseNvs);
  webServer.on("/api/restart", HTTP_POST, handleRestart);
  webServer.on("/generate_204", HTTP_ANY, redirectToPortal);
  webServer.on("/gen_204", HTTP_ANY, redirectToPortal);
  webServer.on("/hotspot-detect.html", HTTP_ANY, redirectToPortal);
  webServer.on("/library/test/success.html", HTTP_ANY, redirectToPortal);
  webServer.on("/connecttest.txt", HTTP_ANY, redirectToPortal);
  webServer.on("/redirect", HTTP_ANY, redirectToPortal);
  webServer.on("/canonical.html", HTTP_ANY, redirectToPortal);
  webServer.on("/success.txt", HTTP_ANY, redirectToPortal);
  webServer.on("/ncsi.txt", HTTP_ANY, redirectToPortal);
  webServer.onNotFound(redirectToPortal);
  webServer.begin();
}

void printBootReport() {
  Serial.println();
  Serial.println("================================================");
  Serial.println(" SpacePC Test Firmware - ESP32 Live Diagnostics");
  Serial.println("================================================");
  Serial.printf("Open Wi-Fi    : %s\n", accessPointName.c_str());
  Serial.printf("Captive Portal: http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("Chip           : %s Rev. %d, %d cores, %d MHz\n",
    ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("Flash / PSRAM  : %u / %u bytes\n", ESP.getFlashChipSize(), ESP.getPsramSize());
  Serial.printf("Arduino / IDF  : %s / %s\n", arduinoVersion().c_str(), ESP.getSdkVersion());
  Serial.printf("Reset reason   : %s\n", resetReasonName().c_str());
  Serial.println("API            : /api/status");
  Serial.println("Notice         : The test Wi-Fi network is intentionally open.");
  Serial.println("================================================");
}
}

void setup() {
  Serial.begin(115200);
  delay(400);

  const String suffix = formatEfuseId().substring(10);
  accessPointName = "SpacePC-Test-" + suffix;
  sketchMd5 = ESP.getSketchMD5();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setHostname(("spacepc-test-" + suffix).c_str());
  if (!WiFi.softAP(accessPointName.c_str())) {
    Serial.println("FEHLER: Access Point konnte nicht gestartet werden.");
    return;
  }

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(dnsPort, "*", WiFi.softAPIP());
  startWebServer();
  printBootReport();
  Serial.println(diagnosticsJson());
  lastSerialReport = millis();
}

void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();

  const uint32_t now = millis();
  if (now - lastSerialReport >= serialReportIntervalMs) {
    lastSerialReport = now;
    Serial.println(diagnosticsJson());
  }
  delay(2);
}
