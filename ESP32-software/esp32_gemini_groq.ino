#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <vector>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Chýba secrets.h – skopíruj secrets.example.h ako secrets.h a vyplň svoje kľúče."
#endif

struct ModelInfo { const char* id; const char* note; };
const ModelInfo MODELS[] = {
  { "gemini-3.5-flash",      "vyvážený · ~5 s" },
  { "gemini-3.1-flash-lite", "rýchly, slabší · ~2 s" },
  { "groq",                  "⚡ najrýchlejší · ~1 s" },
  { "gemini-3.8-flash",      "💪 najsilnejší · často preťažený" },
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
  "Si asistent na riešenie úloh z matematiky, logiky a gramatiky (slovenčina, angličtina). "
  "Odpovedaj v jazyku, v ktorom je úloha zadaná. Buď stručný. "
  "Píš obyčajným textom: nepoužívaj LaTeX, nadpisy ani tabuľky. "
  "Matematiku zapisuj čitateľne cez Unicode znaky (√, ², ³, ×, ÷, ≈, ≤, ≥, π), zlomky ako (a+b)/c "
  "a desatinnú čiarku v slovenských úlohách. "
  "Formát pri matematike a logike: riadok 'Postup:' a pod ním očíslované kroky, "
  "potom na konci samostatný riadok 'Výsledok: ...'. "
  "Formát pri gramatike: riadok 'Odpoveď: ...' a potom riadok 'Pravidlo: ...' s jednou vetou.";

WebServer server(80);

const uint8_t LCD_SDA = 4;
const uint8_t LCD_SCL = 5;
const uint8_t LCD_COLS = 16;
const uint16_t LCD_PAGE_MS = 3000;
LiquidCrystal_I2C* lcd = nullptr;
std::vector<String> lcdLines;
size_t lcdPos = 0;
uint32_t lcdLastPage = 0;
bool lcdNeedReinit = false;

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="sk">
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
<div id="models">Ako prvý:</div>
<textarea id="p" placeholder="Zadaj úlohu (matematika, logika, gramatika SK/EN)... Ctrl+Enter odošle."></textarea>
<button id="b">Odoslať</button>
<label style="margin-left:12px;font-size:14px;color:#9ca3af"><input type="checkbox" id="lcd"> Vypísať odpoveď na LCD</label>
<div id="meta"></div>
<div id="out"></div>
<script>
const b=document.getElementById('b'),p=document.getElementById('p'),o=document.getElementById('out'),m=document.getElementById('meta');
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
const mb=document.getElementById('models');
const lc=document.getElementById('lcd');
let first='';
try{ first=localStorage.getItem('first')||''; lc.checked=localStorage.getItem('lcd')==='1'; }catch(e){}
lc.onchange=()=>{ try{ localStorage.setItem('lcd',lc.checked?'1':'0'); }catch(e){} };
function pick(id){
  first=id;
  try{ localStorage.setItem('first',id); }catch(e){}
  for(const x of mb.querySelectorAll('button')) x.classList.toggle('on',x.dataset.id===id);
}
fetch('/models').then(r=>r.json()).then(d=>{
  for(const md of d.models){
    const x=document.createElement('button');
    x.textContent=md.label; x.dataset.id=md.id; x.title='Tento model sa skúsi ako prvý';
    if(md.note){ const s=document.createElement('small'); s.textContent=md.note; x.appendChild(s); }
    x.onclick=()=>pick(md.id);
    mb.appendChild(x);
  }
  pick(d.models.some(md=>md.id===first)?first:d.models[0].id);
}).catch(()=>{ mb.textContent='Zoznam modelov sa nenačítal, použije sa predvolené poradie.'; });
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
  const secs=()=>((performance.now()-t0)/1000).toFixed(1).replace('.',',')+' s';
  m.textContent='Počítam... 0,0 s';
  const tick=setInterval(()=>{ m.textContent='Počítam... '+secs(); },100);
  try{
    const r=await fetch('/ask',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompt,first,lcd:lc.checked})});
    const d=await r.json();
    clearInterval(tick);
    if(d.ok){
      m.textContent='Odpovedal: '+d.provider+' | čas: '+secs()+(d.note?(' | '+d.note):'');
      show(d.answer);
    } else {
      m.textContent='Chyba | čas: '+secs();
      o.textContent=d.error;
    }
  }catch(e){
    clearInterval(tick);
    m.textContent='Chyba | čas: '+secs();
    o.textContent='Spojenie s ESP32 zlyhalo: '+e;
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
  if (code <= 0) return String("spojenie zlyhalo (") + HTTPClient::errorToString(code) + ")";
  JsonDocument filter;
  filter["error"]["message"] = true;
  JsonDocument doc;
  if (!deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    const char* msg = doc["error"]["message"];
    if (msg) return "HTTP " + String(code) + ": " + cutUtf8(String(msg), 200);
  }
  return "HTTP " + String(code);
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
  if (strlen(GEMINI_KEY) == 0 || strncmp(GEMINI_KEY, "SEM_", 4) == 0) {
    err = "chýba Gemini kľúč";
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
    err = String("neplatná JSON odpoveď: ") + e.c_str();
    return false;
  }

  answer = "";
  for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
    const char* t = part["text"];
    if (t) answer += t;
  }
  answer.trim();
  if (answer.length() == 0) {
    err = "prázdna odpoveď (zablokovaná alebo príliš nízky limit tokenov)";
    return false;
  }
  return true;
}

bool callGroq(const String& prompt, String& answer, String& err) {
  if (strlen(GROQ_KEY) == 0 || strncmp(GROQ_KEY, "SEM_", 4) == 0) {
    err = "chýba Groq kľúč";
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
    err = String("neplatná JSON odpoveď: ") + e.c_str();
    return false;
  }

  const char* c = doc["choices"][0]["message"]["content"];
  answer = c ? c : "";
  answer.trim();
  if (answer.length() == 0) {
    err = "prázdna odpoveď (príliš nízky limit tokenov?)";
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

String lcdFold(String s) {
  static const char* const MAP[][2] = {
    {"á","a"},{"ä","a"},{"č","c"},{"ď","d"},{"é","e"},{"ě","e"},{"í","i"},{"ĺ","l"},{"ľ","l"},
    {"ň","n"},{"ó","o"},{"ô","o"},{"ö","o"},{"ŕ","r"},{"ř","r"},{"š","s"},{"ť","t"},{"ú","u"},
    {"ü","u"},{"ý","y"},{"ž","z"},{"Á","A"},{"Ä","A"},{"Č","C"},{"Ď","D"},{"É","E"},{"Í","I"},
    {"Ĺ","L"},{"Ľ","L"},{"Ň","N"},{"Ó","O"},{"Ô","O"},{"Ŕ","R"},{"Š","S"},{"Ť","T"},{"Ú","U"},
    {"Ý","Y"},{"Ž","Z"},{"×","x"},{"÷","/"},{"√","sqrt"},{"²","^2"},{"³","^3"},{"≈","~"},
    {"≤","<="},{"≥",">="},{"≠","!="},{"π","pi"},{"−","-"},{"–","-"},{"—","-"},{"·","*"},
    {"„","\""},{"“","\""},{"”","\""},{"’","'"},{"…","..."}
  };
  for (const auto& m : MAP) s.replace(m[0], m[1]);
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\n') out += '\n';
    else if (c == '\t') out += ' ';
    else if (c >= 32 && c < 127) out += c;
  }
  return out;
}

const size_t LCD_MAX_LINES = 120;

void lcdPush(const String& s) {
  if (lcdLines.size() < LCD_MAX_LINES) lcdLines.push_back(s);
}

bool isDig(char c) { return c >= '0' && c <= '9'; }

size_t lcdBreakPos(const String& w) {
  for (size_t k = LCD_COLS; k >= 3; k--) {
    char a = w[k - 1], b = w[k];
    if (b == '(' && k > 1) return k;
    if (strchr("=+*/;:)", a)) return k;
    if (a == '-' && !strchr("=+-*/(", w[k - 2])) return k;
    if ((a == ',' || a == '.') && !(isDig(w[k - 2]) && isDig(b))) return k;
  }
  size_t k = LCD_COLS;
  while (k > 1 && isDig(w[k - 1]) && (isDig(w[k]) || w[k] == ',' || w[k] == '.')) k--;
  while (k > 1 && (w[k - 1] == ',' || w[k - 1] == '.') && isDig(w[k - 2])) {
    k--;
    while (k > 1 && isDig(w[k - 1])) k--;
  }
  return k > 1 ? k : LCD_COLS;
}

void lcdWrap(const String& text) {
  String line;
  int i = 0, len = text.length();
  while (i < len && lcdLines.size() < LCD_MAX_LINES) {
    while (i < len && text[i] == ' ') i++;
    int j = i;
    while (j < len && text[j] != ' ') j++;
    String word = text.substring(i, j);
    i = j;
    if (word.length() == 0) break;

    if (word.length() > LCD_COLS) {
      if (line.length()) { lcdPush(line); line = ""; }
      std::vector<String> parts;
      while (word.length() > LCD_COLS) {
        size_t k = lcdBreakPos(word);
        parts.push_back(word.substring(0, k));
        word = word.substring(k);
      }
      if (parts.size() == 1 && lcdLines.size() % 2 == 1) lcdPush("");
      for (const String& part : parts) lcdPush(part);
      line = word;
      continue;
    }

    if (line.length() && line.length() + 1 + word.length() > LCD_COLS) {
      lcdPush(line);
      line = "";
    }
    if (line.length()) line += ' ';
    line += word;
  }
  if (line.length()) lcdPush(line);
}

void lcdSetText(const String& answer) {
  String text = lcdFold(answer);
  text.replace("**", "");
  text.replace("$", "");

  lcdLines.clear();
  int start = 0, len = text.length();
  while (start < len && lcdLines.size() < LCD_MAX_LINES) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = len;
    lcdWrap(text.substring(start, nl));
    start = nl + 1;
  }
  if (lcdLines.empty()) lcdLines.push_back("(prazdna)");
  if (lcdLines.size() % 2) lcdLines.push_back("");

  lcdPos = 0;
  lcdLastPage = 0;
  lcdNeedReinit = true;
}

void lcdReinit() {
  if (!lcd) return;
  lcd->init();
  lcd->noCursor();
  lcd->noBlink();
  lcd->backlight();
}

void lcdRow(uint8_t row, String text) {
  while (text.length() < LCD_COLS) text += ' ';
  text = text.substring(0, LCD_COLS);
  lcd->setCursor(0, row);
  lcd->print(text);
}

void lcdShowPage() {
  lcdRow(0, lcdLines[lcdPos]);
  lcdRow(1, lcdLines[lcdPos + 1]);
}

void lcdMessage(const String& l1, const String& l2 = "") {
  if (!lcd) return;
  lcdRow(0, l1);
  lcdRow(1, l2);
}

void lcdSetup() {
  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setClock(100000);
  delay(100);

  uint8_t found = 0;
  Serial.print("I2C sken (SDA=" + String(LCD_SDA) + ", SCL=" + String(LCD_SCL) + "):");
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", a);
      if (!found && ((a >= 0x20 && a <= 0x27) || (a >= 0x38 && a <= 0x3F))) found = a;
    }
  }
  Serial.println();

  if (!found) {
    Serial.println("LCD sa na I2C nenašlo -> skontroluj zapojenie SDA/SCL, GND a napájanie 5V.");
    return;
  }
  Serial.printf("LCD nájdené na adrese 0x%02X\n", found);
  lcd = new LiquidCrystal_I2C(found, LCD_COLS, 2);
  lcdReinit();
}

void lcdTick() {
  if (!lcd || lcdLines.empty()) return;
  if (lcdNeedReinit) {
    lcdNeedReinit = false;
    lcdReinit();
  }
  if (lcdLastPage != 0 && millis() - lcdLastPage < LCD_PAGE_MS) return;
  if (lcdLastPage != 0 && lcdLines.size() <= 2) return;
  if (lcdLastPage != 0) lcdPos = (lcdPos + 2) % lcdLines.size();
  lcdShowPage();
  lcdLastPage = millis();
  if (lcdLastPage == 0) lcdLastPage = 1;
}

void handleAsk() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) {
    sendError(400, "Neplatná požiadavka.");
    return;
  }
  String prompt = in["prompt"] | "";
  prompt.trim();
  if (prompt.length() == 0) { sendError(400, "Prázdna úloha."); return; }
  if (prompt.length() > MAX_PROMPT_CHARS) {
    sendError(413, "Úloha je príliš dlhá (max " + String(MAX_PROMPT_CHARS) + " znakov).");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendError(503, "ESP32 nie je pripojené na WiFi.");
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
    Serial.printf("Volám %s...\n", order[i]);
    if (callModel(order[i], prompt, answer, err)) {
      JsonDocument res;
      res["ok"] = true;
      res["provider"] = modelLabel(order[i]);
      if (errors.length()) res["note"] = "zlyhalo: " + errors;
      res["answer"] = answer;
      sendJson(200, res);
      if (in["lcd"] | false) lcdSetText(answer);
      return;
    }
    Serial.println("  zlyhalo: " + err);
    if (errors.length()) errors += "; ";
    errors += String(order[i]) + ": " + err;
    if (WiFi.status() != WL_CONNECTED) break;
  }

  errors.replace("; ", "\n");
  sendError(502, "Zlyhali všetky modely.\n" + errors);
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
  Serial.printf("\nŠtart, dôvod resetu: %d (1=zapnutie, 3=softvér, 4/7=pád, 9=brownout/slabé napájanie)\n",
                (int)esp_reset_reason());

  lcdSetup();
  lcdMessage("Pripajam WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Pripájam sa na WiFi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t0 > 30000) {
      Serial.println("\nWiFi sa nepodarilo pripojiť, reštartujem...");
      ESP.restart();
    }
  }
  Serial.println();
  Serial.print("Hotovo. Otvor v prehliadači: http://");
  Serial.println(WiFi.localIP());
  lcdReinit();
  lcdMessage("WiFi OK, IP:", WiFi.localIP().toString());

  if (MDNS.begin("esp32-ai")) {
    Serial.println("alebo http://esp32-ai.local");
  }

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/ask", HTTP_POST, handleAsk);
  server.on("/models", HTTP_GET, handleModels);
  server.onNotFound([]() { server.send(404, "text/plain", "Nenájdené"); });
  server.begin();
}

void loop() {
  server.handleClient();
  lcdTick();
}
