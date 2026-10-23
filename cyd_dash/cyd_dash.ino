// Dashboard sessioni Claude per ESP32-2432S028R (Cheap Yellow Display)
// Dati: JSON da server/dash_server.py via Wi-Fi (HTTP) o via USB seriale.
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"

#define LED_R 4
#define LED_G 16
#define LED_B 17
#define XPT_IRQ 36
#define XPT_MOSI 32
#define XPT_MISO 39
#define XPT_CLK 25
#define XPT_CS 33
// calibrazione touch (raw -> pixel) con rotazione 1
#define T_XMIN 200
#define T_XMAX 3700
#define T_YMIN 240
#define T_YMAX 3800

#define C_BG     0x0000   // nero puro
#define C_CARD   0x0000
#define C_LINE   0x2945
#define C_PILL   0x1082
#define C_DIM    0x8CBA
#define C_TXT    0xFFFF
#define C_ORANGE 0xFB40   // #FF6A00 arancione fluo
#define C_GREEN  0x3FE2   // #39FF14 verde neon
#define C_TEAL   0x073F   // #00E5FF ciano
#define C_RED    0xF9CC   // #FF3864 rosa-rosso
#define C_YEL    0xFEA1   // #FFD60A giallo
#define C_PURPLE 0xC3FF   // #C77DFF viola

TFT_eSPI tft;
TFT_eSprite spr(&tft);
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT_CS, XPT_IRQ);

struct Sess {
  int pid; char name[16]; char proj[20]; char cwd[56]; bool busy; char kind[5]; char ver[10];
  int age, idle, ctx; long ctxk, win, out, cr; int turns; char model[14]; char last[56]; char tool[14];
};
#define MAXS 16
#define PER_PAGE 8
Sess S[MAXS];
int nS = 0, nTotal = 0, nBusy = 0, codexProcs = -1;
char clockStr[8] = "--:--";
struct Usage { int cl5 = -1, cl7 = -1, cx5 = -1, cxw = -1; char clPlan[12] = ""; char cxPlan[12] = ""; char clErr[64] = ""; char cxErr[64] = ""; } U;
bool dataOk = false, dirty = true;
unsigned long lastData = 0;
enum Screen { GRID, DETAIL, USAGE };
Screen scr = GRID;
int page = 0, sel = -1, selPid = -1;
const char* transport = "usb";
char dashUrl[96];

// ---- mascotte pixel-art 12x10 ----
const uint16_t SPR_CRAB[10]  = {0b001000000100,0b001000000100,0b011111111110,0b111111111111,0b111111111111,0b111111111111,0b011111111110,0b101010010101,0b101010010101,0b000000000000};
const uint16_t SPR_ALIEN[10] = {0b001000000100,0b000100001000,0b001111111100,0b011011110110,0b111111111111,0b101111111101,0b101000000101,0b000110011000,0b000000000000,0b000000000000};
const uint16_t SPR_GHOST[10] = {0b000111111000,0b001111111100,0b011111111110,0b011111111110,0b111111111111,0b111111111111,0b111111111111,0b111111111111,0b110110110110,0b100100100100};
const uint16_t SPR_BOT[10]   = {0b000001100000,0b000111111000,0b011111111110,0b010111111010,0b011111111110,0b000111111000,0b011111111110,0b010111111010,0b000110011000,0b001110011100};
const uint16_t* SHAPES[4] = {SPR_CRAB, SPR_ALIEN, SPR_GHOST, SPR_BOT};
const int8_t EYES[4][4] = {{3,3,8,3},{4,3,7,3},{3,4,8,4},{4,3,7,3}};
const uint16_t PALETTE[6] = {C_GREEN, C_ORANGE, C_TEAL, C_PURPLE, C_YEL, C_RED};

uint16_t dim565(uint16_t c, uint8_t pct) {
  uint16_t r = ((c >> 11) & 31) * pct / 100, g = ((c >> 5) & 63) * pct / 100, b = (c & 31) * pct / 100;
  return (r << 11) | (g << 5) | b;
}
uint32_t hashStr(const char* s) { uint32_t h = 5381; while (*s) h = ((h << 5) + h) + (uint8_t)*s++; return h; }
int shapeOf(const Sess& s) { return hashStr(s.proj) % 4; }
uint16_t colorOf(const Sess& s) { return PALETTE[(hashStr(s.proj) / 7) % 6]; }

void drawMascot(TFT_eSPI& g, int x, int y, int shape, uint16_t col, int sc = 2) {
  const uint16_t* rows = SHAPES[shape];
  for (int r = 0; r < 10; r++) for (int c = 0; c < 12; c++)
    if (rows[r] & (1 << (11 - c))) g.fillRect(x + c * sc, y + r * sc, sc, sc, col);
  const int8_t* e = EYES[shape];
  g.fillRect(x + e[0] * sc, y + e[1] * sc, sc, sc, TFT_BLACK);
  g.fillRect(x + e[2] * sc, y + e[3] * sc, sc, sc, TFT_BLACK);
}

String fmtK(long v) {
  if (v >= 1000000000L) return String(v / 1000000000.0, 1) + "b";
  if (v >= 1000000L) return String(v / 1000000.0, 1) + "m";
  if (v >= 1000L) return String(v / 1000) + "k";
  return String(v);
}
String fmtAge(int s) {
  if (s < 60) return String(s) + "s";
  if (s < 3600) return String(s / 60) + "m";
  if (s < 86400) return String(s / 3600) + "h " + String((s % 3600) / 60) + "m";
  return String(s / 86400) + "d";
}
String trunc(const char* s, int n) { String t(s); if ((int)t.length() > n) { t = t.substring(0, n - 1); t += "~"; } return t; }
uint16_t pctColor(int p) { return p < 0 ? C_DIM : p < 60 ? C_GREEN : p < 85 ? C_YEL : C_RED; }
uint16_t remainingColor(int used) { return used < 0 ? C_DIM : used < 60 ? C_GREEN : used < 85 ? C_YEL : C_RED; }
String remainingText(int used) { return used < 0 ? String("--") : String(100 - constrain(used, 0, 100)) + "%"; }

// ---------------------------------------------------------------- GRID
#define CW 74
#define CH 88
#define GX0 3
#define GY0 34
void drawCard(int i, int x, int y) {
  Sess& s = S[i];
  spr.createSprite(CW, CH);
  spr.fillSprite(C_BG);
  uint16_t col = colorOf(s);
  spr.fillRoundRect(0, 0, CW, CH, 7, C_CARD);
  spr.drawRoundRect(0, 0, CW, CH, 7, s.busy ? col : dim565(col, 48));
  spr.fillRect(7, 0, CW - 14, 2, s.busy ? col : dim565(col, 35));
  spr.fillCircle(CW - 9, 11, 3, s.busy ? C_ORANGE : C_DIM);
  drawMascot(spr, (CW - 24) / 2, 8, shapeOf(s), s.busy ? col : dim565(col, 70));
  spr.setTextDatum(TC_DATUM);
  spr.setTextFont(1);
  spr.setTextColor(C_TXT, C_CARD);
  spr.drawString(trunc(s.name, 12), CW / 2, 32);
  spr.setTextColor(s.busy ? C_ORANGE : C_DIM, C_CARD);
  spr.drawString(String(s.busy ? "LIVE " : "IDLE ") + fmtAge(s.idle), CW / 2, 45);
  spr.fillRect(7, 58, CW - 14, 1, C_LINE);
  spr.setTextDatum(TL_DATUM); spr.setTextColor(C_DIM, C_CARD);
  spr.drawString(s.tool[0] ? trunc(s.tool, 10) : String(s.kind) + " / " + fmtAge(s.age), 7, 64);
  spr.setTextDatum(TR_DATUM); spr.setTextColor(col, C_CARD);
  spr.drawString(String(s.ctx) + "%", CW - 7, 76);
  spr.fillRoundRect(7, 78, 43, 3, 1, C_LINE);
  spr.fillRoundRect(7, 78, 43 * constrain(s.ctx, 0, 100) / 100, 3, 1, col);
  spr.pushSprite(x, y);
  spr.deleteSprite();
}

void drawPill(int x, int y, int w, int h, const String& txt, uint16_t fill, uint16_t fg) {
  tft.fillRoundRect(x, y, w, h, h / 2, fill);
  tft.setTextDatum(MC_DATUM); tft.setTextFont(1); tft.setTextColor(fg, fill);
  tft.drawString(txt, x + w / 2, y + h / 2 + 1);
  tft.setTextDatum(TL_DATUM);
}

void drawGrid(bool full) {
  if (full) tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 30, C_BG);
  tft.fillRect(0, 29, 320, 1, C_LINE);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(C_GREEN, C_BG);
  tft.drawString("AGENTS", 8, 6);
  tft.setTextFont(1); tft.setTextColor(C_DIM, C_BG);
  tft.drawString(String(nBusy) + "/" + String(nTotal) + " active", 91, 11);
  drawPill(216, 6, 47, 17, "CL " + remainingText(U.cl5), C_PILL, remainingColor(U.cl5));
  drawPill(267, 6, 47, 17, "CX " + remainingText(U.cx5), C_PILL, remainingColor(U.cx5));
  // cards
  int pages = max(1, (nS + PER_PAGE - 1) / PER_PAGE);
  if (page >= pages) page = pages - 1;
  for (int k = 0; k < PER_PAGE; k++) {
    int i = page * PER_PAGE + k;
    int x = GX0 + (k % 4) * (CW + 6), y = GY0 + (k / 4) * (CH + 7);
    if (i < nS) drawCard(i, x, y); else tft.fillRect(x, y, CW, CH, C_BG);
  }
  if (nS == 0) {
    tft.setTextDatum(MC_DATUM); tft.setTextFont(2); tft.setTextColor(C_DIM, C_BG);
    tft.drawString(dataOk ? "nessuna sessione" : "in attesa del server...", 160, 120);
  }
  // footer
  tft.fillRect(0, 219, 320, 21, C_BG);
  tft.fillRect(0, 219, 320, 1, C_LINE);
  tft.setTextFont(1); tft.setTextDatum(ML_DATUM); tft.setTextColor(C_DIM, C_BG);
  tft.fillCircle(9, 230, 2, dataOk ? C_GREEN : C_RED);
  tft.drawString(String(transport) + (dataOk ? " online" : " offline"), 16, 230);
  tft.setTextDatum(MR_DATUM); tft.drawString(clockStr, 312, 230);
  tft.setTextDatum(MC_DATUM);
  if (pages > 1) {
    tft.drawString("<", 96, 230); tft.drawString(">", 224, 230);
    for (int p = 0; p < pages; p++) tft.fillRoundRect(160 - pages * 6 + p * 12, 228, 8, 4, 2, p == page ? C_ORANGE : C_LINE);
  }
}

// ---------------------------------------------------------------- DETAIL
void label(int x, int y, const char* t) { tft.setTextFont(1); tft.setTextDatum(TL_DATUM); tft.setTextColor(C_DIM, C_BG); tft.drawString(t, x, y); }
void value(int x, int y, const String& t, bool big = true) {
  tft.setTextDatum(TL_DATUM); tft.setTextColor(C_TXT, C_BG);
  if (big) tft.setFreeFont(&FreeSansBold9pt7b); else tft.setTextFont(2);
  tft.drawString(t, x, y);
}

void drawDetail(bool full) {
  if (sel < 0 || sel >= nS) { scr = GRID; drawGrid(true); return; }
  Sess& s = S[sel];
  tft.fillScreen(C_BG);
  uint16_t col = colorOf(s);
  tft.fillRect(0, 29, 320, 1, C_LINE);
  tft.setTextFont(2); tft.setTextDatum(TL_DATUM); tft.setTextColor(C_TXT, C_BG);
  tft.drawString("<", 9, 5);
  tft.setTextFont(1); tft.setTextColor(C_DIM, C_BG); tft.drawString("AGENT / DETAIL", 31, 11);
  drawPill(207, 6, 106, 17, String(s.busy ? "LIVE " : "IDLE ") + fmtAge(s.idle), s.busy ? C_ORANGE : C_PILL, s.busy ? C_BG : C_DIM);

  tft.fillRoundRect(7, 36, 306, 57, 7, C_CARD);
  tft.drawRoundRect(7, 36, 306, 57, 7, dim565(col, 75));
  tft.fillRect(8, 36, 4, 57, col);
  drawMascot(tft, 20, 47, shapeOf(s), col, 3);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b); tft.setTextColor(C_TXT, C_CARD);
  tft.drawString(trunc(s.name, 19), 67, 43);
  tft.setTextFont(1); tft.setTextColor(C_DIM, C_CARD);
  tft.drawString(trunc(s.cwd, 39), 68, 70);

  tft.fillRoundRect(7, 100, 149, 42, 6, C_CARD);
  tft.fillRoundRect(164, 100, 149, 42, 6, C_CARD);
  tft.drawRoundRect(7, 100, 149, 42, 6, dim565(C_ORANGE, 65));
  tft.drawRoundRect(164, 100, 149, 42, 6, dim565(C_TEAL, 65));
  label(17, 106, "DOING NOW");
  tft.setTextFont(2); tft.setTextColor(s.busy ? C_ORANGE : C_TXT, C_CARD);
  tft.drawString(s.busy ? (s.tool[0] ? trunc(s.tool, 16) : String("thinking")) : String("waiting"), 17, 117);
  label(174, 106, "CONTEXT");
  tft.setTextFont(2); tft.setTextColor(pctColor(s.ctx), C_CARD);
  tft.drawString(String(s.ctx) + "%", 174, 116);
  tft.setTextFont(1); tft.setTextColor(C_DIM, C_CARD);
  tft.drawString(fmtK(s.ctxk * 1000L) + "/" + fmtK(s.win * 1000L), 221, 122);
  tft.fillRoundRect(174, 135, 128, 3, 1, C_LINE);
  tft.fillRoundRect(174, 135, 128 * constrain(s.ctx, 0, 100) / 100, 3, 1, pctColor(s.ctx));

  tft.fillRoundRect(7, 149, 306, 42, 6, C_CARD);
  tft.drawRoundRect(7, 149, 306, 42, 6, C_LINE);
  tft.fillRect(84, 155, 1, 29, C_LINE);
  tft.fillRect(160, 155, 1, 29, C_LINE);
  tft.fillRect(236, 155, 1, 29, C_LINE);
  const char* names[4] = {"OUT", "CACHE", "TURNS", "UPTIME"};
  String vals[4] = {fmtK(s.out), fmtK(s.cr), String(s.turns), fmtAge(s.age)};
  for (int k = 0; k < 4; k++) {
    int x = 16 + k * 76;
    tft.setTextFont(1); tft.setTextColor(C_DIM, C_CARD); tft.drawString(names[k], x, 155);
    tft.setTextFont(2); tft.setTextColor(C_TXT, C_CARD); tft.drawString(vals[k], x, 166);
  }
  tft.setTextFont(1); tft.setTextColor(C_DIM, C_BG);
  tft.drawString("LAST PROMPT", 8, 196);
  tft.setTextColor(C_TXT, C_BG);
  tft.drawString(s.last[0] ? trunc(s.last, 49) : String("-"), 8, 206);
  tft.fillRoundRect(7, 220, 149, 18, 5, C_ORANGE);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(C_BG, C_ORANGE); tft.drawString("FOCUS EDITOR", 81, 229);
  tft.fillRoundRect(164, 220, 149, 18, 5, C_PILL);
  tft.drawRoundRect(164, 220, 149, 18, 5, C_TEAL);
  tft.setTextColor(C_TXT, C_PILL); tft.drawString("USAGE  >", 238, 229);
}

// ---------------------------------------------------------------- USAGE
void bar(int y, const char* name, int pct, const char* extra) {
  tft.setTextFont(1); tft.setTextDatum(TL_DATUM); tft.setTextColor(C_TXT, C_CARD); tft.drawString(name, 18, y);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(remainingColor(pct), C_CARD); tft.drawString(pct < 0 ? "n/d" : remainingText(pct) + " left", 302, y);
  tft.fillRoundRect(18, y + 12, 284, 5, 2, C_LINE);
  if (pct >= 0 && pct < 100) tft.fillRoundRect(18, y + 12, 284 * (100 - constrain(pct, 0, 100)) / 100, 5, 2, remainingColor(pct));
  if (extra && extra[0]) { tft.setTextDatum(TL_DATUM); tft.setTextColor(C_DIM, C_CARD); tft.drawString(trunc(extra, 46), 18, y + 19); }
  tft.setTextDatum(TL_DATUM);
}
void drawUsage(bool full) {
  tft.fillScreen(C_BG);
  tft.setTextFont(2); tft.setTextDatum(TL_DATUM); tft.setTextColor(C_TXT, C_BG); tft.drawString("<", 9, 5);
  tft.setTextFont(1); tft.setTextColor(C_DIM, C_BG); tft.drawString("USAGE / LIMITS", 31, 11);
  tft.fillRect(0, 29, 320, 1, C_LINE);
  tft.setFreeFont(&FreeSansBold9pt7b); tft.setTextDatum(TL_DATUM); tft.setTextColor(C_TXT, C_BG);
  tft.fillRoundRect(7, 36, 306, 86, 7, C_CARD);
  tft.drawRoundRect(7, 36, 306, 86, 7, dim565(C_ORANGE, 75));
  tft.fillRect(8, 36, 4, 86, C_ORANGE);
  tft.setTextColor(C_ORANGE, C_BG);
  tft.drawString("CLAUDE", 18, 43);
  tft.setTextFont(1); tft.setTextColor(C_DIM, C_CARD); tft.drawString(U.clPlan, 110, 49);
  bar(65, "5 HOURS", U.cl5, U.cl5 < 0 ? U.clErr : "");
  bar(93, "7 DAYS", U.cl7, "");
  tft.setFreeFont(&FreeSansBold9pt7b); tft.setTextColor(C_TXT, C_BG);
  tft.fillRoundRect(7, 128, 306, 86, 7, C_CARD);
  tft.drawRoundRect(7, 128, 306, 86, 7, dim565(C_TEAL, 75));
  tft.fillRect(8, 128, 4, 86, C_TEAL);
  tft.setTextColor(C_TEAL, C_BG);
  tft.drawString("CODEX", 18, 135);
  tft.setTextFont(1); tft.setTextColor(C_DIM, C_CARD); tft.drawString(U.cxPlan, 110, 141);
  bar(157, "5 HOURS", U.cx5, U.cx5 < 0 ? U.cxErr : "");
  bar(185, "WEEKLY", U.cxw, "");
  tft.setTextFont(1); tft.setTextColor(C_DIM, C_BG);
  tft.drawString(String("CODEX PROCS ") + (codexProcs < 0 ? "-" : String(codexProcs)) + "  /  " + transport + "  /  " + clockStr, 8, 226);
}

void redraw(bool full) {
  if (scr == GRID) drawGrid(full); else if (scr == DETAIL) drawDetail(full); else drawUsage(full);
  dirty = false;
}

// ---------------------------------------------------------------- dati
void parseState(const char* json) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument doc;
#else
  DynamicJsonDocument doc(16384);
#endif
  DeserializationError err = deserializeJson(doc, json);
  if (err) { Serial.printf("ERR json %s\n", err.c_str()); return; }
  nTotal = doc["n"] | 0; nBusy = doc["busy"] | 0; codexProcs = doc["codex_procs"] | -1;
  strlcpy(clockStr, doc["clock"] | "--:--", sizeof clockStr);
  nS = 0;
  for (JsonObject o : doc["sessions"].as<JsonArray>()) {
    if (nS >= MAXS) break;
    Sess& s = S[nS];
    s.pid = o["pid"] | 0;
    strlcpy(s.name, o["name"] | "", sizeof s.name);
    strlcpy(s.proj, o["proj"] | "", sizeof s.proj);
    strlcpy(s.cwd, o["cwd"] | "", sizeof s.cwd);
    s.busy = strcmp(o["st"] | "", "busy") == 0;
    strlcpy(s.kind, o["kind"] | "", sizeof s.kind);
    strlcpy(s.ver, o["ver"] | "", sizeof s.ver);
    s.age = o["age"] | 0; s.idle = o["idle"] | 0; s.ctx = o["ctx"] | 0;
    s.ctxk = o["ctxk"] | 0L; s.win = o["win"] | 200L; s.out = o["out"] | 0L; s.cr = o["cr"] | 0L; s.turns = o["turns"] | 0;
    strlcpy(s.model, o["model"] | "", sizeof s.model);
    strlcpy(s.last, o["last"] | "", sizeof s.last);
    strlcpy(s.tool, o["tool"] | "", sizeof s.tool);
    nS++;
  }
  JsonObject u = doc["usage"];
  JsonObject c = u["claude"];
  if (!c.isNull()) { U.cl5 = c["five_hour"]["pct"] | -1; U.cl7 = c["seven_day"]["pct"] | -1; strlcpy(U.clPlan, c["plan"] | "", sizeof U.clPlan); }
  JsonObject x = u["codex"];
  if (!x.isNull()) { U.cx5 = x["five_hour"]["pct"] | -1; U.cxw = x["weekly"]["pct"] | -1; strlcpy(U.cxPlan, x["plan"] | "", sizeof U.cxPlan); }
  strlcpy(U.clErr, u["err"]["claude"] | "", sizeof U.clErr);
  strlcpy(U.cxErr, u["err"]["codex"] | "", sizeof U.cxErr);
  // mantieni la selezione sullo stesso pid
  if (scr == DETAIL) { sel = -1; for (int i = 0; i < nS; i++) if (S[i].pid == selPid) sel = i; }
  dataOk = true; lastData = millis(); dirty = true;
  Serial.printf("OK n=%d busy=%d shown=%d heap=%u\n", nTotal, nBusy, nS, ESP.getFreeHeap());
  digitalWrite(LED_B, LOW); delay(20); digitalWrite(LED_B, HIGH);
}

void readSerial() {
  static char buf[8192]; static int bl = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n') { buf[bl] = 0; if (bl > 0 && buf[0] == '{') { parseState(buf); transport = "usb"; } bl = 0; }
    else if (bl < (int)sizeof(buf) - 1) buf[bl++] = ch; else bl = 0;
  }
}

void pollHttp() {
  static unsigned long last = 0;
  if (WiFi.status() != WL_CONNECTED || millis() - last < 3000) return;
  last = millis();
  HTTPClient http; http.setTimeout(2500); http.begin(dashUrl);
  int code = http.GET();
  if (code == 200) { String body = http.getString(); transport = "wifi"; parseState(body.c_str()); }
  else Serial.printf("HTTP %d\n", code);
  http.end();
}

void sendFocus(int pid) {
  Serial.printf("ACT focus %d\n", pid);
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http; http.setTimeout(2000);
    http.begin(String("http://") + DASH_HOST + ":" + DASH_PORT + "/api/focus?pid=" + pid);
    http.POST(""); http.end();
  }
}

// ---------------------------------------------------------------- touch
void onTap(int x, int y) {
  if (scr == GRID) {
    if (y < 26 && x > 216) { scr = USAGE; redraw(true); return; }
    if (y >= 219) {
      int pages = max(1, (nS + PER_PAGE - 1) / PER_PAGE);
      if (x < 160 && page > 0) { page--; redraw(true); }
      else if (x >= 160 && page < pages - 1) { page++; redraw(true); }
      return;
    }
    for (int k = 0; k < PER_PAGE; k++) {
      int cx = GX0 + (k % 4) * (CW + 6), cy = GY0 + (k / 4) * (CH + 7);
      if (x >= cx && x < cx + CW && y >= cy && y < cy + CH) {
        int i = page * PER_PAGE + k;
        if (i < nS) { sel = i; selPid = S[i].pid; scr = DETAIL; redraw(true); }
        return;
      }
    }
  } else if (scr == DETAIL) {
    if (y < 28 && x < 90) { scr = GRID; redraw(true); return; }
    if (y >= 220) {
      if (x < 160) { if (sel >= 0) sendFocus(S[sel].pid); tft.fillRoundRect(7, 220, 149, 18, 5, C_GREEN); delay(120); drawDetail(true); }
      else { scr = USAGE; redraw(true); }
    }
  } else {
    if (y < 28 && x < 90) { scr = sel >= 0 ? DETAIL : GRID; redraw(true); }
  }
}

void handleTouch() {
  static bool touching = false; static int x0, y0, xl, yl;
  if (ts.tirqTouched() && ts.touched()) {
    TS_Point p = ts.getPoint();
    int x = constrain(map(p.x, T_XMIN, T_XMAX, 0, 320), 0, 319);
    int y = constrain(map(p.y, T_YMIN, T_YMAX, 0, 240), 0, 239);
    if (!touching) { touching = true; x0 = x; y0 = y; }
    xl = x; yl = y;
  } else if (touching) {
    touching = false;
    int dx = xl - x0;
    if (scr == GRID && abs(dx) > 70) {
      int pages = max(1, (nS + PER_PAGE - 1) / PER_PAGE);
      if (dx < 0 && page < pages - 1) { page++; redraw(true); }
      else if (dx > 0 && page > 0) { page--; redraw(true); }
    } else onTap(x0, y0);
    delay(60);
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  for (int p : {LED_R, LED_G, LED_B}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  touchSPI.begin(XPT_CLK, XPT_MISO, XPT_MOSI, XPT_CS);
  ts.begin(touchSPI); ts.setRotation(1);
  tft.init(); tft.setRotation(1); tft.fillScreen(C_BG);
  tft.setFreeFont(&FreeSansBold12pt7b); tft.setTextDatum(MC_DATUM); tft.setTextColor(C_TXT, C_BG);
  tft.drawString("agents", 160, 100);
  tft.fillRect(120, 160, 20, 20, TFT_RED); tft.fillRect(150, 160, 20, 20, TFT_GREEN); tft.fillRect(180, 160, 20, 20, TFT_BLUE);
  tft.setTextFont(1); tft.setTextColor(C_DIM, C_BG); tft.drawString("R", 130, 190); tft.drawString("G", 160, 190); tft.drawString("B", 190, 190);
  tft.setTextFont(1); tft.setTextColor(C_DIM, C_BG);
  snprintf(dashUrl, sizeof dashUrl, "http://%s:%d/api/state", DASH_HOST, DASH_PORT);
  if (strlen(WIFI_SSID) > 0) {
    tft.drawString(String("wifi: ") + WIFI_SSID, 160, 130);
    WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) { delay(200); readSerial(); }
    if (WiFi.status() == WL_CONNECTED) { transport = "wifi"; tft.drawString(WiFi.localIP().toString() + " -> " + dashUrl, 160, 145); }
    else tft.drawString("wifi non disponibile, uso USB", 160, 145);
  } else tft.drawString("solo USB: avvia dash_server.py --serial", 160, 130);
  delay(2500);
  Serial.println("READY");
  redraw(true);
}

void loop() {
  readSerial();
  pollHttp();
  handleTouch();
  if (dirty) redraw(scr == GRID);
  if (dataOk && millis() - lastData > 15000) { dataOk = false; dirty = true; lastData = millis(); }
  delay(10);
}
