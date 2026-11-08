#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing secrets.h - copy secrets.example.h to secrets.h and fill in your WiFi credentials and API keys."
#endif

struct ModelInfo { const char* id; const char* note; };
const ModelInfo MODELS[] = {
  { "gemini-3.5-flash",      "balanced · ~5 s" },
  { "gemini-3.1-flash-lite", "fast, weaker · ~2 s" },
  { "groq",                  "⚡ fastest · ~1 s" },
  { "gemini-3.8-flash",      "💪 strongest · often overloaded" },
};
const size_t MODEL_COUNT = sizeof(MODELS) / sizeof(MODELS[0]);
const char* GEMINI_THINKING = "low";
const uint16_t GEMINI_TIMEOUT_MS = 20000;

const char* GROQ_MODEL   = "openai/gpt-oss-120b";
const bool  GROQ_REASONING_LOW = true;

const int    GEMINI_MAX_TOKENS = 2048;
const int    GROQ_MAX_TOKENS   = 1024;
const size_t MAX_PROMPT_CHARS  = 2000;

const char* SYSTEM_PROMPT =
  "You are an assistant that solves math, logic and grammar tasks (Slovak and English). "
  "Answer in the language the task is written in. Be concise. "
  "Write plain text: no LaTeX, no headings, no tables. "
  "Write math readably with Unicode characters (√, ², ³, ×, ÷, ≈, ≤, ≥, π), fractions as (a+b)/c, "
  "and use the decimal separator customary in the task's language. "
  "Format for math and logic: a line 'Steps:' followed by numbered steps, "
  "then a final separate line 'Result: ...'. "
  "Format for grammar: a line 'Answer: ...' followed by a line 'Rule: ...' with one sentence.";

WebServer server(80);

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-S3 AI</title>
<style>
  body{font-family:system-ui,sans-serif;max-width:720px;margin:0 auto;padding:16px;background:#111;color:#eee}
  textarea{width:100%;height:120px;box-sizing:border-box;padding:10px;font-size:16px;border-radius:8px;border:1px solid #444;background:#1c1c1c;color:#eee}
  button{margin-top:10px;padding:10px 18px;font-size:16px;border:0;border-radius:8px;background:#3b82f6;color:#fff}
  button:disabled{opacity:.5}
  #models{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin-bottom:10px;font-size:14px;color:#9ca3af}
  #models button{margin:0;padding:6px 12px;font-size:14px;background:#2a2a2a;color:#ddd;border:1px solid #444}
  #models button.on{background:#3b82f6;color:#fff;border-color:#3b82f6}
  #models button small{display:block;font-size:12px;opacity:.75;margin-top:2px}
  #meta{margin-top:16px;font-size:14px;color:#9ca3af}
  #out{white-space:pre-wrap;margin-top:8px;padding:12px;background:#1c1c1c;border-radius:8px;min-height:40px}
</style>
</head>
<body>
<h2>ESP32-S3 AI</h2>
<div id="models">Try first:</div>
<textarea id="p" placeholder="Enter a task (math, logic, grammar SK/EN)... Ctrl+Enter sends."></textarea>
<button id="b">Send</button>
<div id="meta"></div>
<div id="out"></div>
<script>
const b=document.getElementById('b'),p=document.getElementById('p'),o=document.getElementById('out'),m=document.getElementById('meta');
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
const mb=document.getElementById('models');
let first='';
try{ first=localStorage.getItem('first')||''; }catch(e){}
function pick(id){
  first=id;
  try{ localStorage.setItem('first',id); }catch(e){}
  for(const x of mb.querySelectorAll('button')) x.classList.toggle('on',x.dataset.id===id);
}
fetch('/models').then(r=>r.json()).then(d=>{
  for(const md of d.models){
    const x=document.createElement('button');
    x.textContent=md.label; x.dataset.id=md.id; x.title='This model is tried first';
    if(md.note){ const s=document.createElement('small'); s.textContent=md.note; x.appendChild(s); }
    x.onclick=()=>pick(md.id);
    mb.appendChild(x);
  }
  pick(d.models.some(md=>md.id===first)?first:d.models[0].id);
}).catch(()=>{ mb.textContent='Could not load the model list, the default order will be used.'; });
let katexReady=null;
function loadKatex(){
  if(katexReady) return katexReady;
  katexReady=new Promise(function(resolve){
    const base='https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/';
    const css=document.createElement('link');
    css.rel='stylesheet'; css.href=base+'katex.min.css';
    document.head.appendChild(css);
    const s1=document.createElement('script');
    s1.src=base+'katex.min.js';
    s1.onload=function(){
      const s2=document.createElement('script');
      s2.src=base+'contrib/auto-render.min.js';
      s2.onload=function(){ resolve(!!window.renderMathInElement); };
      s2.onerror=function(){ resolve(false); };
      document.head.appendChild(s2);
    };
    s1.onerror=function(){ resolve(false); };
    document.head.appendChild(s1);
    setTimeout(function(){ resolve(false); },8000);
  });
  return katexReady;
}
async function show(text){
  let h=esc(text).replace(/^#{1,6}\s*(.+)$/gm,'<b>$1</b>').replace(/\*\*([^*]+)\*\*/g,'<b>$1</b>');
  o.innerHTML=h;
  if(/\\\[|\\\(|\$\$|\\boxed|\\frac|\\text\{|\$[^$\n]+\$/.test(text)){
    if(await loadKatex()){
      try{
        renderMathInElement(o,{delimiters:[
          {left:'$$',right:'$$',display:true},
          {left:'\\[',right:'\\]',display:true},
          {left:'\\(',right:'\\)',display:false},
          {left:'$',right:'$',display:false}
        ],throwOnError:false});
      }catch(e){}
    }
  }
}
async function ask(){
  const prompt=p.value.trim();
  if(!prompt) return;
  b.disabled=true; o.textContent='';
  const t0=performance.now();
  const secs=()=>((performance.now()-t0)/1000).toFixed(1)+' s';
  m.textContent='Working... 0.0 s';
  const tick=setInterval(()=>{ m.textContent='Working... '+secs(); },100);
  try{
    const r=await fetch('/ask',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompt,first})});
    const d=await r.json();
    clearInterval(tick);
    if(d.ok){
      m.textContent='Answered by: '+d.provider+' | time: '+secs()+(d.note?(' | '+d.note):'');
      show(d.answer);
    } else {
      m.textContent='Error | time: '+secs();
      o.textContent=d.error;
    }
  }catch(e){
    clearInterval(tick);
    m.textContent='Error | time: '+secs();
    o.textContent='Connection to the ESP32 failed: '+e;
  }
  b.disabled=false;
}
b.onclick=ask;
p.addEventListener('keydown',e=>{ if(e.key==='Enter'&&(e.ctrlKey||e.metaKey)) ask(); });
</script>
</body>
</html>
)rawliteral";

String cutUtf8(const String& s, size_t maxLen) {
  if (s.length() <= maxLen) return s;
  size_t n = maxLen;
  while (n > 0 && (((uint8_t)s[n]) & 0xC0) == 0x80) n--;
  return s.substring(0, n) + "...";
}

String apiError(int code, const String& payload) {
  if (code <= 0) return String("connection failed (") + HTTPClient::errorToString(code) + ")";
  JsonDocument filter;
  filter["error"]["message"] = true;
  JsonDocument doc;
  if (!deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    const char* msg = doc["error"]["message"];
    if (msg) return "HTTP " + String(code) + ": " + cutUtf8(String(msg), 200);
  }
  return "HTTP " + String(code);
}

bool keyMissing(const char* key) {
  return strlen(key) == 0 || strncmp(key, "YOUR_", 5) == 0;
}

int httpsPost(const String& url, const char* headerName, const String& headerValue,
              const String& body, String& payload, uint16_t timeoutMs) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(timeoutMs / 1000);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.addHeader(headerName, headerValue);
  int code = http.POST(body);
  if (code > 0) payload = http.getString();
  http.end();
  return code;
}

bool callGemini(const char* model, const String& prompt, String& answer, String& err) {
  if (keyMissing(GEMINI_KEY)) {
    err = "Gemini key is missing";
    return false;
  }

  JsonDocument req;
  req["systemInstruction"]["parts"][0]["text"] = SYSTEM_PROMPT;
  req["contents"][0]["parts"][0]["text"] = prompt;
  req["generationConfig"]["maxOutputTokens"] = GEMINI_MAX_TOKENS;
  if (strlen(GEMINI_THINKING) > 0)
    req["generationConfig"]["thinkingConfig"]["thinkingLevel"] = GEMINI_THINKING;
  String body;
  serializeJson(req, body);

  String url = String("https://generativelanguage.googleapis.com/v1beta/models/") +
               model + ":generateContent";
  String payload;
  uint32_t t0 = millis();
  int code = httpsPost(url, "x-goog-api-key", GEMINI_KEY, body, payload, GEMINI_TIMEOUT_MS);
  Serial.printf("  %s -> %d (%lu ms)\n", model, code, (unsigned long)(millis() - t0));
  if (code != 200) {
    err = apiError(code, payload);
    return false;
  }

  JsonDocument filter;
  filter["candidates"][0]["content"]["parts"][0]["text"] = true;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (e) {
    err = String("invalid JSON response: ") + e.c_str();
    return false;
  }

  answer = "";
  for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
    const char* t = part["text"];
    if (t) answer += t;
  }
  answer.trim();
  if (answer.length() == 0) {
    err = "empty response (blocked or token limit too low)";
    return false;
  }
  return true;
}

bool callGroq(const String& prompt, String& answer, String& err) {
  if (keyMissing(GROQ_KEY)) {
    err = "Groq key is missing";
    return false;
  }

  JsonDocument req;
  req["model"] = GROQ_MODEL;
  req["messages"][0]["role"] = "system";
  req["messages"][0]["content"] = SYSTEM_PROMPT;
  req["messages"][1]["role"] = "user";
  req["messages"][1]["content"] = prompt;
  req["max_tokens"] = GROQ_MAX_TOKENS;
  if (GROQ_REASONING_LOW) req["reasoning_effort"] = "low";
  String body;
  serializeJson(req, body);

  String payload;
  uint32_t t0 = millis();
  int code = httpsPost("https://api.groq.com/openai/v1/chat/completions",
                       "Authorization", String("Bearer ") + GROQ_KEY, body, payload, 30000);
  Serial.printf("  groq -> %d (%lu ms)\n", code, (unsigned long)(millis() - t0));
  if (code != 200) {
    err = apiError(code, payload);
    return false;
  }

  JsonDocument filter;
  filter["choices"][0]["message"]["content"] = true;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (e) {
    err = String("invalid JSON response: ") + e.c_str();
    return false;
  }

  const char* c = doc["choices"][0]["message"]["content"];
  answer = c ? c : "";
  answer.trim();
  if (answer.length() == 0) {
    err = "empty response (token limit too low?)";
    return false;
  }
  return true;
}

bool isGroq(const char* id) { return strcmp(id, "groq") == 0; }

String modelLabel(const char* id) {
  if (isGroq(id)) return String("Groq (") + GROQ_MODEL + ")";
  return String("Gemini (") + id + ")";
}

bool callModel(const char* id, const String& prompt, String& answer, String& err) {
  if (isGroq(id)) return callGroq(prompt, answer, err);
  return callGemini(id, prompt, answer, err);
}

void sendJson(int status, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.send(status, "application/json; charset=utf-8", out);
}

void sendError(int status, const String& message) {
  JsonDocument res;
  res["ok"] = false;
  res["error"] = message;
  sendJson(status, res);
}

void handleAsk() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) {
    sendError(400, "Invalid request.");
    return;
  }
  String prompt = in["prompt"] | "";
  prompt.trim();
  if (prompt.length() == 0) { sendError(400, "Empty task."); return; }
  if (prompt.length() > MAX_PROMPT_CHARS) {
    sendError(413, "Task is too long (max " + String(MAX_PROMPT_CHARS) + " characters).");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendError(503, "ESP32 is not connected to WiFi.");
    return;
  }

  String first = in["first"] | "";
  const char* order[MODEL_COUNT];
  size_t n = 0;
  for (size_t i = 0; i < MODEL_COUNT; i++)
    if (first == MODELS[i].id) order[n++] = MODELS[i].id;
  for (size_t i = 0; i < MODEL_COUNT; i++)
    if (first != MODELS[i].id) order[n++] = MODELS[i].id;

  String answer, errors;
  for (size_t i = 0; i < n; i++) {
    String err;
    Serial.printf("Calling %s...\n", order[i]);
    if (callModel(order[i], prompt, answer, err)) {
      JsonDocument res;
      res["ok"] = true;
      res["provider"] = modelLabel(order[i]);
      if (errors.length()) res["note"] = "failed: " + errors;
      res["answer"] = answer;
      sendJson(200, res);
      return;
    }
    Serial.println("  failed: " + err);
    if (errors.length()) errors += "; ";
    errors += String(order[i]) + ": " + err;
    if (WiFi.status() != WL_CONNECTED) break;
  }

  errors.replace("; ", "\n");
  sendError(502, "All models failed.\n" + errors);
}

void handleModels() {
  JsonDocument res;
  for (size_t i = 0; i < MODEL_COUNT; i++) {
    res["models"][i]["id"] = MODELS[i].id;
    res["models"][i]["label"] = modelLabel(MODELS[i].id);
    res["models"][i]["note"] = MODELS[i].note;
  }
  sendJson(200, res);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t0 > 30000) {
      Serial.println("\nCould not connect to WiFi, restarting...");
      ESP.restart();
    }
  }
  Serial.println();
  Serial.print("Done. Open in your browser: http://");
  Serial.println(WiFi.localIP());

  if (MDNS.begin("esp32-ai")) {
    Serial.println("or http://esp32-ai.local");
  }

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/ask", HTTP_POST, handleAsk);
  server.on("/models", HTTP_GET, handleModels);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void loop() {
  server.handleClient();
}
