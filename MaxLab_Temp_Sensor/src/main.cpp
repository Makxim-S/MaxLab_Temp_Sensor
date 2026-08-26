#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

constexpr uint8_t ONE_WIRE_PIN = 4;
constexpr uint8_t BUTTON_PIN = 2;
constexpr uint8_t LED_R_PIN = 6;
constexpr uint8_t LED_G_PIN = 7;
constexpr uint8_t LED_B_PIN = 5;

constexpr bool DEFAULT_RGB_COMMON_ANODE = true;
constexpr float LED_TEMP_MIN_C = 20.0f;
constexpr float LED_TEMP_MAX_C = 127.0f;
constexpr uint32_t SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t TEMP_CONVERSION_MS = 750;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 25;
constexpr size_t HISTORY_SIZE = 7200;
constexpr uint16_t GRAPH_MAX_POINTS = 240;
constexpr uint16_t DEFAULT_GRAPH_DIV_SEC = 60;

const char *STA_SSID = "";
const char *STA_PASSWORD = "";
const char *AP_SSID = "MaxLab-Temp";
const char *AP_PASSWORD = "12345678";

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature sensors(&oneWire);
WebServer server(80);
Preferences prefs;

struct Sample {
  float sensor1 = NAN;
  float sensor2 = NAN;
  float average = NAN;
  float hottest = NAN;
  uint32_t seconds = 0;
};

Sample history[HISTORY_SIZE];
size_t historyStart = 0;
size_t historyCount = 0;

float currentTemps[2] = {NAN, NAN};
float maxTemps[2] = {NAN, NAN};
float maxAverage = NAN;
float maxHottest = NAN;
uint8_t sensorCount = 0;
bool paused = false;
bool ledCommonAnode = DEFAULT_RGB_COMMON_ANODE;
bool ledManualMode = false;
uint8_t manualLed[3] = {0, 0, 0};
uint16_t graphDivSec = DEFAULT_GRAPH_DIV_SEC;
uint32_t lastSampleAt = 0;
bool tempConversionPending = false;
uint32_t tempConversionStartedAt = 0;

bool stableButtonState = HIGH;
bool rawButtonState = HIGH;
uint32_t lastButtonChangeAt = 0;

String ipText;

void saveLedSettings() {
  prefs.putBool("anode", ledCommonAnode);
  prefs.putBool("manual", ledManualMode);
  prefs.putUChar("r", manualLed[0]);
  prefs.putUChar("g", manualLed[1]);
  prefs.putUChar("b", manualLed[2]);
  prefs.putUShort("gdiv", graphDivSec);
}

void loadLedSettings() {
  prefs.begin("led", false);
  ledCommonAnode = prefs.getBool("anode", DEFAULT_RGB_COMMON_ANODE);
  ledManualMode = prefs.getBool("manual", false);
  manualLed[0] = prefs.getUChar("r", 0);
  manualLed[1] = prefs.getUChar("g", 0);
  manualLed[2] = prefs.getUChar("b", 0);
  graphDivSec = prefs.getUShort("gdiv", DEFAULT_GRAPH_DIV_SEC);
  if (graphDivSec < 10 || graphDivSec > 3600) {
    graphDivSec = DEFAULT_GRAPH_DIV_SEC;
  }
}

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>MaxLab Temperature</title>
  <style>
    :root{color-scheme:dark;--bg:#101114;--panel:#181a1f;--line:#2b3038;--text:#f2f4f8;--muted:#9098a5;--sensor1:#36d6a8;--sensor2:#3a86ff;--hot:#ff4b45;--warn:#ffcf45}
    *{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
    main{width:min(860px,100%);margin:0 auto;padding:22px}
    header{display:flex;align-items:flex-end;justify-content:space-between;gap:18px;margin-bottom:18px}
    h1{margin:0;font-size:clamp(1.35rem,3vw,2.2rem);font-weight:720;letter-spacing:0}
    .status{display:flex;gap:8px;align-items:center;color:var(--muted);font-size:.94rem}
    .dot{width:10px;height:10px;border-radius:50%;background:var(--sensor1);box-shadow:0 0 18px var(--sensor1)}
    .paused .dot{background:var(--warn);box-shadow:0 0 18px var(--warn)}
    .grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;margin-bottom:12px}
    .tile,.chart{background:var(--panel);border:1px solid var(--line);border-radius:8px}
    .tile{padding:16px;min-height:112px}
    .tile-wide{grid-column:1/-1;text-align:center;min-height:132px}
    .label{color:var(--muted);font-size:.82rem;text-transform:uppercase;letter-spacing:.08em}
    .sensor1-label{color:var(--sensor1)}
    .sensor2-label{color:var(--sensor2)}
    .value{margin-top:8px;font-size:clamp(1.7rem,4vw,3rem);font-weight:760;line-height:1}
    .tile-wide .value{font-size:clamp(2.25rem,7vw,4.1rem)}
    .unit{font-size:.48em;color:var(--muted);margin-left:3px}
    .sub{margin-top:9px;color:var(--muted);font-size:.92rem}
    .settings-btn{position:fixed;top:12px;right:12px;z-index:5;width:42px;height:42px;border:1px solid var(--line);border-radius:8px;background:var(--panel);color:var(--text);font-size:1.25rem}
    .panel{position:fixed;top:60px;right:12px;z-index:5;width:min(330px,calc(100vw - 24px));padding:14px;background:var(--panel);border:1px solid var(--line);border-radius:8px;box-shadow:0 18px 42px #0009;display:none}
    .panel.open{display:block}.panel-title{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;color:var(--text);font-weight:700}.panel-row{display:grid;grid-template-columns:54px 1fr 42px;gap:8px;align-items:center;margin:10px 0;color:var(--muted)}.panel-row input{width:100%}
    .btns{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:10px 0}.btn{border:1px solid var(--line);border-radius:8px;background:#20232a;color:var(--text);min-height:38px}.btn.active{border-color:var(--sensor1);box-shadow:0 0 0 1px var(--sensor1) inset}.btn-red{color:#ff6b66}.btn-green{color:#36d6a8}.btn-blue{color:#64a2ff}
    .chart{padding:14px;height:420px}
    canvas{display:block;width:100%;height:100%}
    .select{width:100%;min-height:38px;border:1px solid var(--line);border-radius:8px;background:#20232a;color:var(--text);padding:0 10px}
    @media (max-width:560px){main{padding:14px}.grid{grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.tile{padding:12px;min-height:104px}.tile-wide{grid-column:1/-1;min-height:126px}.label{font-size:.68rem}.value{font-size:clamp(1.45rem,9vw,2.35rem)}.tile-wide .value{font-size:clamp(2.35rem,13vw,3.6rem)}.sub{font-size:.78rem}.chart{height:330px}header{align-items:flex-start;flex-direction:column}}
  </style>
</head>
<body>
<button class="settings-btn" id="settingsBtn" title="LED settings">&#9881;</button>
<aside class="panel" id="settingsPanel">
  <div class="panel-title"><span>LED RGB</span><button class="btn" id="closeSettings">OK</button></div>
  <div class="btns"><button class="btn active" id="autoLed">Auto</button><button class="btn" id="manualLed">Manual</button><button class="btn" id="anodeLed">Anode</button></div>
  <div class="btns"><button class="btn btn-red" data-rgb="255,0,0">Red</button><button class="btn btn-green" data-rgb="0,255,0">Green</button><button class="btn btn-blue" data-rgb="0,0,255">Blue</button></div>
  <div class="btns"><button class="btn" data-rgb="255,255,255">White</button><button class="btn" data-rgb="255,80,0">Orange</button><button class="btn" data-rgb="0,0,0">Off</button></div>
  <div class="panel-row"><span>R</span><input id="ledR" type="range" min="0" max="255" value="0"><span id="ledRv">0</span></div>
  <div class="panel-row"><span>G</span><input id="ledG" type="range" min="0" max="255" value="0"><span id="ledGv">0</span></div>
  <div class="panel-row"><span>B</span><input id="ledB" type="range" min="0" max="255" value="0"><span id="ledBv">0</span></div>
  <div class="panel-row"><span>X</span><select class="select" id="graphDiv"><option value="60">1 min</option><option value="300">5 min</option><option value="600">10 min</option><option value="1800">30 min</option><option value="3600">1 hour</option></select><span></span></div>
</aside>
<main>
  <header>
    <div><h1>MaxLab Temperature</h1><div class="sub" id="ip">ESP32-C3</div></div>
    <div class="status" id="status"><span class="dot"></span><span id="statusText">&#1080;&#1079;&#1084;&#1077;&#1088;&#1077;&#1085;&#1080;&#1077;</span></div>
  </header>
  <section class="grid">
    <div class="tile tile-wide"><div class="label">&#1057;&#1088;&#1077;&#1076;&#1085;&#1103;&#1103; &#1090;&#1077;&#1084;&#1087;&#1077;&#1088;&#1072;&#1090;&#1091;&#1088;&#1072;</div><div class="value" id="avg">--<span class="unit">&#176;C</span></div><div class="sub" id="maxAvg">&#1084;&#1072;&#1082;&#1089; -- &#176;C</div></div>
    <div class="tile"><div class="label sensor1-label">&#1044;&#1072;&#1090;&#1095;&#1080;&#1082; 1</div><div class="value" id="t1">--<span class="unit">&#176;C</span></div><div class="sub" id="m1">&#1084;&#1072;&#1082;&#1089; -- &#176;C</div></div>
    <div class="tile"><div class="label sensor2-label">&#1044;&#1072;&#1090;&#1095;&#1080;&#1082; 2</div><div class="value" id="t2">--<span class="unit">&#176;C</span></div><div class="sub" id="m2">&#1084;&#1072;&#1082;&#1089; -- &#176;C</div></div>
    <div class="tile"><div class="label">&#1057;&#1072;&#1084;&#1099;&#1081; &#1075;&#1086;&#1088;&#1103;&#1095;&#1080;&#1081;</div><div class="value" id="hot">--<span class="unit">&#176;C</span></div><div class="sub" id="maxHot">&#1084;&#1072;&#1082;&#1089; -- &#176;C</div></div>
    <div class="tile"><div class="label">&#1057;&#1086;&#1089;&#1090;&#1086;&#1103;&#1085;&#1080;&#1077;</div><div class="value" id="state">OK</div><div class="sub" id="sensors">&#1076;&#1072;&#1090;&#1095;&#1080;&#1082;&#1086;&#1074;: --</div></div>
  </section>
  <section class="chart"><canvas id="chart"></canvas></section>
</main>
<script>
const els={avg:q('avg'),t1:q('t1'),t2:q('t2'),hot:q('hot'),maxAvg:q('maxAvg'),m1:q('m1'),m2:q('m2'),maxHot:q('maxHot'),state:q('state'),status:q('status'),statusText:q('statusText'),sensors:q('sensors'),ip:q('ip')};
const canvas=q('chart'),ctx=canvas.getContext('2d');
const led={panel:q('settingsPanel'),r:q('ledR'),g:q('ledG'),b:q('ledB'),rv:q('ledRv'),gv:q('ledGv'),bv:q('ledBv'),auto:q('autoLed'),manual:q('manualLed'),anode:q('anodeLed'),graphDiv:q('graphDiv')};
function q(id){return document.getElementById(id)}
function temp(v){return Number.isFinite(v)?`${v.toFixed(1)}<span class="unit">&#176;C</span>`:`--<span class="unit">&#176;C</span>`}
function plain(v){return Number.isFinite(v)?`${v.toFixed(1)} \u00b0C`:'-- \u00b0C'}
function fmtTime(sec){const m=Math.floor(sec/60),s=Math.floor(sec%60),h=Math.floor(m/60);return h>0?`${h}:${String(m%60).padStart(2,'0')}`:`${m}:${String(s).padStart(2,'0')}`}
function draw(history,meta){
  const dpr=devicePixelRatio||1,w=canvas.clientWidth,h=canvas.clientHeight;canvas.width=w*dpr;canvas.height=h*dpr;ctx.setTransform(dpr,0,0,dpr,0,0);
  ctx.clearRect(0,0,w,h);ctx.fillStyle='#181a1f';ctx.fillRect(0,0,w,h);
  const pad={l:46,r:16,t:16,b:46},plotW=w-pad.l-pad.r,plotH=h-pad.t-pad.b;
  const vals=history.flatMap(p=>[p.t1,p.t2,p.avg]).filter(Number.isFinite);
  let min=Math.min(10,...vals),max=Math.max(40,...vals);if(max-min<8){max+=4;min-=4}
  ctx.strokeStyle='#2b3038';ctx.lineWidth=1;ctx.fillStyle='#9098a5';ctx.font='12px system-ui';
  for(let i=0;i<=4;i++){const y=pad.t+plotH*i/4;const v=max-(max-min)*i/4;ctx.beginPath();ctx.moveTo(pad.l,y);ctx.lineTo(w-pad.r,y);ctx.stroke();ctx.fillText(v.toFixed(0)+'\u00b0',8,y+4)}
  const start=meta.start||0,end=meta.end||start,span=Math.max(1,end-start);
  const div=Math.max(10,meta.div||60),firstTick=Math.ceil(start/div)*div;
  for(let s=firstTick;s<=end;s+=div){const x=pad.l+((s-start)/span)*plotW;ctx.strokeStyle='#2b3038';ctx.beginPath();ctx.moveTo(x,pad.t);ctx.lineTo(x,pad.t+plotH);ctx.stroke();ctx.fillStyle='#9098a5';ctx.fillText(fmtTime(s),Math.max(2,Math.min(w-44,x-14)),h-20)}
  line(history,'t1','#36d6a8');line(history,'t2','#3a86ff');
  ctx.fillStyle='#36d6a8';ctx.fillText('\u0434\u0430\u0442\u0447\u0438\u043a 1',pad.l,h-8);ctx.fillStyle='#3a86ff';ctx.fillText('\u0434\u0430\u0442\u0447\u0438\u043a 2',pad.l+76,h-8);
  function line(points,key,color){ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();let open=false;points.forEach(p=>{const v=p[key];if(!Number.isFinite(v))return;const x=pad.l+((p.s-start)/span)*plotW;const y=pad.t+(max-v)/(max-min)*plotH;if(x<pad.l-2||x>w-pad.r+2)return;if(!open){ctx.moveTo(x,y);open=true}else ctx.lineTo(x,y)});ctx.stroke()}
}
async function refresh(){
  try{
    const r=await fetch('/api');const d=await r.json();
    els.avg.innerHTML=temp(d.average);els.t1.innerHTML=temp(d.temps[0]);els.t2.innerHTML=temp(d.temps[1]);els.hot.innerHTML=temp(d.hottest);
    els.maxAvg.textContent='\u043c\u0430\u043a\u0441 '+plain(d.maxAverage);els.m1.textContent='\u043c\u0430\u043a\u0441 '+plain(d.max[0]);els.m2.textContent='\u043c\u0430\u043a\u0441 '+plain(d.max[1]);els.maxHot.textContent='\u043c\u0430\u043a\u0441 '+plain(d.maxHottest);
    els.state.textContent=d.paused?'\u041f\u0430\u0443\u0437\u0430':'OK';els.status.classList.toggle('paused',d.paused);els.statusText.textContent=d.paused?'\u043f\u0430\u0443\u0437\u0430':'\u0438\u0437\u043c\u0435\u0440\u0435\u043d\u0438\u0435';
    els.sensors.textContent='\u0434\u0430\u0442\u0447\u0438\u043a\u043e\u0432: '+d.sensorCount;els.ip.textContent=d.ip;draw(d.history,d.graph);
  }catch(e){els.state.textContent='\u041d\u0435\u0442 \u0441\u0432\u044f\u0437\u0438';}
}
function syncLedValues(r,g,b){led.r.value=r;led.g.value=g;led.b.value=b;led.rv.textContent=r;led.gv.textContent=g;led.bv.textContent=b}
async function setLed(args){
  const r=await fetch('/led?'+new URLSearchParams(args));const d=await r.json();
  syncLedValues(d.r,d.g,d.b);led.auto.classList.toggle('active',!d.manual);led.manual.classList.toggle('active',d.manual);led.anode.textContent=d.commonAnode?'Anode':'Cathode';led.anode.classList.toggle('active',d.commonAnode);
  led.graphDiv.value=d.graphDiv;
}
async function loadLed(){try{await setLed({})}catch(e){}}
q('settingsBtn').onclick=()=>led.panel.classList.toggle('open');
q('closeSettings').onclick=()=>led.panel.classList.remove('open');
led.auto.onclick=()=>setLed({manual:0});
led.manual.onclick=()=>setLed({manual:1,r:led.r.value,g:led.g.value,b:led.b.value});
led.anode.onclick=()=>setLed({commonAnode:led.anode.classList.contains('active')?0:1});
led.graphDiv.onchange=()=>setLed({graphDiv:led.graphDiv.value});
document.querySelectorAll('[data-rgb]').forEach(btn=>btn.onclick=()=>{const [r,g,b]=btn.dataset.rgb.split(',');setLed({manual:1,r,g,b})});
[led.r,led.g,led.b].forEach(input=>input.oninput=()=>{syncLedValues(led.r.value,led.g.value,led.b.value);setLed({manual:1,r:led.r.value,g:led.g.value,b:led.b.value})});
refresh();setInterval(refresh,1000);
loadLed();
</script>
</body>
</html>
)HTML";

float validTemp(float value) {
  if (value == DEVICE_DISCONNECTED_C || value < -100.0f || value > 300.0f) {
    return NAN;
  }
  return value;
}

float averageOf(float a, float b) {
  if (isnan(a)) return b;
  if (isnan(b)) return a;
  return (a + b) * 0.5f;
}

float hottestOf(float a, float b) {
  if (isnan(a)) return b;
  if (isnan(b)) return a;
  return max(a, b);
}

void updateMax(float value, float &maxValue) {
  if (!isnan(value) && (isnan(maxValue) || value > maxValue)) {
    maxValue = value;
  }
}

uint8_t pwmValue(uint8_t brightness) {
  return ledCommonAnode ? 255 - brightness : brightness;
}

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(0, pwmValue(r));
  ledcWrite(1, pwmValue(g));
  ledcWrite(2, pwmValue(b));
}

void applyManualLed() {
  setLed(manualLed[0], manualLed[1], manualLed[2]);
}

void setLedForTemperature(float tempC) {
  if (ledManualMode) {
    applyManualLed();
    return;
  }

  if (isnan(tempC)) {
    setLed(0, 0, 0);
    return;
  }

  if (tempC <= LED_TEMP_MIN_C) {
    setLed(0, 0, 255);
    return;
  }

  if (tempC >= LED_TEMP_MAX_C) {
    setLed(255, 0, 0);
    return;
  }

  float t = (tempC - LED_TEMP_MIN_C) / (LED_TEMP_MAX_C - LED_TEMP_MIN_C);
  uint8_t r = static_cast<uint8_t>(255.0f * t);
  uint8_t b = static_cast<uint8_t>(255.0f * (1.0f - t));
  uint8_t g = static_cast<uint8_t>(110.0f * (1.0f - fabsf(t * 2.0f - 1.0f)));
  setLed(r, g, b);
}

void pushHistory(float t1, float t2) {
  size_t index = (historyStart + historyCount) % HISTORY_SIZE;
  if (historyCount == HISTORY_SIZE) {
    index = historyStart;
    historyStart = (historyStart + 1) % HISTORY_SIZE;
  } else {
    historyCount++;
  }

  history[index].sensor1 = t1;
  history[index].sensor2 = t2;
  history[index].average = averageOf(t1, t2);
  history[index].hottest = hottestOf(t1, t2);
  history[index].seconds = millis() / 1000;
}

void requestTemperatureConversion() {
  sensors.requestTemperatures();
  tempConversionPending = true;
  tempConversionStartedAt = millis();
}

void readTemperatureSample() {
  currentTemps[0] = sensorCount > 0 ? validTemp(sensors.getTempCByIndex(0)) : NAN;
  currentTemps[1] = sensorCount > 1 ? validTemp(sensors.getTempCByIndex(1)) : NAN;

  updateMax(currentTemps[0], maxTemps[0]);
  updateMax(currentTemps[1], maxTemps[1]);
  updateMax(averageOf(currentTemps[0], currentTemps[1]), maxAverage);
  maxHottest = hottestOf(maxHottest, hottestOf(currentTemps[0], currentTemps[1]));
  pushHistory(currentTemps[0], currentTemps[1]);
  setLedForTemperature(hottestOf(currentTemps[0], currentTemps[1]));
  tempConversionPending = false;
  lastSampleAt = millis();
}

void handleButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != rawButtonState) {
    rawButtonState = reading;
    lastButtonChangeAt = millis();
  }

  if (millis() - lastButtonChangeAt >= BUTTON_DEBOUNCE_MS && rawButtonState != stableButtonState) {
    stableButtonState = rawButtonState;
    if (stableButtonState == LOW) {
      paused = !paused;
      if (paused) {
        tempConversionPending = false;
        setLedForTemperature(hottestOf(currentTemps[0], currentTemps[1]));
      } else {
        lastSampleAt = 0;
        requestTemperatureConversion();
      }
    }
  }
}

void appendJsonFloat(String &json, float value) {
  if (isnan(value)) {
    json += "null";
  } else {
    json += String(value, 1);
  }
}

uint32_t firstHistorySecond() {
  if (historyCount == 0) {
    return millis() / 1000;
  }
  return history[historyStart].seconds;
}

uint32_t latestHistorySecond() {
  if (historyCount == 0) {
    return millis() / 1000;
  }
  size_t index = (historyStart + historyCount - 1) % HISTORY_SIZE;
  return history[index].seconds;
}

void sendJsonFloat(float value) {
  String part;
  appendJsonFloat(part, value);
  server.sendContent(part);
}

void handleApi() {
  uint32_t firstSecond = firstHistorySecond();
  uint32_t latestSecond = latestHistorySecond();
  uint32_t startSecond = firstSecond;
  uint32_t endSecond = latestSecond;
  size_t pointStep = historyCount > GRAPH_MAX_POINTS ? (historyCount + GRAPH_MAX_POINTS - 1) / GRAPH_MAX_POINTS : 1;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  String json;
  json.reserve(512);
  json += "{\"paused\":";
  json += paused ? "true" : "false";
  json += ",\"sensorCount\":";
  json += sensorCount;
  json += ",\"ip\":\"";
  json += ipText;
  json += "\",\"temps\":[";
  appendJsonFloat(json, currentTemps[0]);
  json += ",";
  appendJsonFloat(json, currentTemps[1]);
  json += "],\"average\":";
  appendJsonFloat(json, averageOf(currentTemps[0], currentTemps[1]));
  json += ",\"hottest\":";
  appendJsonFloat(json, hottestOf(currentTemps[0], currentTemps[1]));
  json += ",\"max\":[";
  appendJsonFloat(json, maxTemps[0]);
  json += ",";
  appendJsonFloat(json, maxTemps[1]);
  json += "],\"maxAverage\":";
  appendJsonFloat(json, maxAverage);
  json += ",\"maxHottest\":";
  appendJsonFloat(json, maxHottest);
  json += ",\"graph\":{\"first\":";
  json += firstSecond;
  json += ",\"latest\":";
  json += latestSecond;
  json += ",\"start\":";
  json += startSecond;
  json += ",\"end\":";
  json += endSecond;
  json += ",\"div\":";
  json += graphDivSec;
  json += ",\"step\":";
  json += pointStep;
  json += "},\"history\":[";
  server.sendContent(json);

  bool firstPoint = true;
  for (size_t i = 0; i < historyCount; i += pointStep) {
    size_t index = (historyStart + i) % HISTORY_SIZE;
    String point;
    point.reserve(72);
    if (!firstPoint) point += ",";
    firstPoint = false;
    point += "{\"s\":";
    point += history[index].seconds;
    point += ",\"t1\":";
    appendJsonFloat(point, history[index].sensor1);
    point += ",\"t2\":";
    appendJsonFloat(point, history[index].sensor2);
    point += ",\"avg\":";
    appendJsonFloat(point, history[index].average);
    point += "}";
    server.sendContent(point);
  }
  server.sendContent("]}");
  server.sendContent("");
}

uint8_t argByte(const char *name, uint8_t fallback) {
  if (!server.hasArg(name)) {
    return fallback;
  }
  return static_cast<uint8_t>(constrain(server.arg(name).toInt(), 0, 255));
}

void handleLed() {
  bool settingsChanged = false;
  if (server.hasArg("commonAnode")) {
    bool value = server.arg("commonAnode").toInt() != 0;
    settingsChanged = settingsChanged || value != ledCommonAnode;
    ledCommonAnode = value;
  }

  if (server.hasArg("manual")) {
    bool value = server.arg("manual").toInt() != 0;
    settingsChanged = settingsChanged || value != ledManualMode;
    ledManualMode = value;
  }

  if (server.hasArg("graphDiv")) {
    uint16_t value = static_cast<uint16_t>(constrain(server.arg("graphDiv").toInt(), 10, 3600));
    settingsChanged = settingsChanged || value != graphDivSec;
    graphDivSec = value;
  }

  uint8_t nextR = argByte("r", manualLed[0]);
  uint8_t nextG = argByte("g", manualLed[1]);
  uint8_t nextB = argByte("b", manualLed[2]);
  settingsChanged = settingsChanged || nextR != manualLed[0] || nextG != manualLed[1] || nextB != manualLed[2];
  manualLed[0] = nextR;
  manualLed[1] = nextG;
  manualLed[2] = nextB;
  if (settingsChanged) {
    saveLedSettings();
  }

  if (ledManualMode) {
    applyManualLed();
  } else {
    setLedForTemperature(hottestOf(currentTemps[0], currentTemps[1]));
  }

  String json;
  json.reserve(96);
  json += "{\"manual\":";
  json += ledManualMode ? "true" : "false";
  json += ",\"commonAnode\":";
  json += ledCommonAnode ? "true" : "false";
  json += ",\"r\":";
  json += manualLed[0];
  json += ",\"g\":";
  json += manualLed[1];
  json += ",\"b\":";
  json += manualLed[2];
  json += ",\"graphDiv\":";
  json += graphDivSec;
  json += "}";
  server.send(200, "application/json", json);
}

void startWifi() {
  if (strlen(STA_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(STA_SSID, STA_PASSWORD);
    uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 12000) {
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
      ipText = WiFi.localIP().toString();
      return;
    }
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  ipText = WiFi.softAPIP().toString();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  loadLedSettings();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  stableButtonState = digitalRead(BUTTON_PIN);
  rawButtonState = stableButtonState;

  ledcSetup(0, 5000, 8);
  ledcSetup(1, 5000, 8);
  ledcSetup(2, 5000, 8);
  ledcAttachPin(LED_R_PIN, 0);
  ledcAttachPin(LED_G_PIN, 1);
  ledcAttachPin(LED_B_PIN, 2);
  setLed(0, 0, 0);

  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(false);
  sensorCount = min<uint8_t>(sensors.getDeviceCount(), 2);

  startWifi();
  server.on("/", []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api", handleApi);
  server.on("/led", handleLed);
  server.begin();

  requestTemperatureConversion();

  Serial.println();
  Serial.println("MaxLab temperature server started");
  Serial.print("Sensors: ");
  Serial.println(sensorCount);
  Serial.print("Open: http://");
  Serial.println(ipText);
}

void loop() {
  server.handleClient();
  handleButton();

  if (!paused && tempConversionPending && millis() - tempConversionStartedAt >= TEMP_CONVERSION_MS) {
    readTemperatureSample();
  }

  if (!paused && !tempConversionPending && millis() - lastSampleAt >= SAMPLE_INTERVAL_MS) {
    requestTemperatureConversion();
  }
}
