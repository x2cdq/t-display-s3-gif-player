/*
 * LilyGO T-Display-S3 — GIF player
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <AnimatedGIF.h>
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include <DNSServer.h>

// Settings
#define AP_SSID            "ESP-32 GIF"
#define AP_PASS            "12345678"
#define SERVER_TIME_MS     (4UL * 60UL * 1000UL) // 4 minutes
#define INFO_SCREEN_MS     60000UL
#define SLEEP_HOLD_MS      1500UL

#define FS_LABEL           "ffat"
#define GIF_DIR            "/gifs"
#define FS_BLOCK           4096

#define PIN_BOOT           0
#define PIN_WAKE           14
#define PIN_POWER_ON       15
#define PIN_LCD_BL         38
#define PIN_BAT_VOLT       4

#define SCREEN_W           320
#define SCREEN_H           170

// Global objects
TFT_eSPI     tft = TFT_eSPI();
AnimatedGIF  gif;
WebServer    server(80);
DNSServer    dns;
Preferences  prefs;

String   curName = "";
bool     gifOpen = false;
int      xOff = 0, yOff = 0;
uint32_t nextFrameAt = 0;

bool     serverOn = false;
uint32_t serverUntil = 0;
uint32_t infoUntil = 0;
bool     needReload = true;

bool     wantSleep = false;
uint32_t sleepAt = 0;

String   uploadErr = "";
File     uploadFile;
String   uploadPath = "";

// Utility functions
void touchServer() { serverUntil = millis() + SERVER_TIME_MS; }

void hideInfo() {
  infoUntil = 0;
  needReload = true;
}

const char *translitCp(uint32_t cp) {
  static const char *const ru[32] = {
    "a","b","v","g","d","e","zh","z","i","y","k","l","m","n","o","p",
    "r","s","t","u","f","kh","ts","ch","sh","shch","","y","","e","yu","ya"
  };
  if (cp >= 0x0410 && cp <= 0x042F) return ru[cp - 0x0410];
  if (cp >= 0x0430 && cp <= 0x044F) return ru[cp - 0x0430];
  switch (cp) {
    case 0x0401: case 0x0451: return "e";
    case 0x0406: case 0x0456: return "i";
    case 0x0407: case 0x0457: return "yi";
    case 0x0404: case 0x0454: return "ye";
    case 0x0490: case 0x0491: return "g";
  }
  return nullptr;
}

String cleanName(String n) {
  int s = max(n.lastIndexOf('/'), n.lastIndexOf('\\'));
  n = n.substring(s + 1);
  String o;
  size_t i = 0, len = n.length();
  while (i < len) {
    uint8_t c = (uint8_t)n[i++];
    const char *add = nullptr;
    char one[2] = {0, 0};
    if (c < 0x80) {
      if (isalnum(c) || c == '.' || c == '-' || c == '_') { one[0] = (char)c; add = one; }
    } else {
      uint32_t cp = 0;
      int extra = 0;
      if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
      else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
      else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
      for (int k = 0; k < extra && i < len; k++) cp = (cp << 6) | ((uint8_t)n[i++] & 0x3F);
      add = translitCp(cp);
    }
    if (add) o += add;
    else if (!o.endsWith("_")) o += "_";
  }
  o.toLowerCase();
  if (o.length() > 40) o = o.substring(o.length() - 40);
  return o;
}

bool gifExists(const String &name) {
  if (name.length() == 0) return false;
  return LittleFS.exists(String(GIF_DIR) + "/" + name);
}

String uniqueGifName(const String &name) {
  if (!gifExists(name)) return name;
  String stem = name.substring(0, name.length() - 4);
  if (stem.length() == 0) stem = "gif";
  for (int i = 1; i < 100; i++) {
    String suf = "_" + String(i) + ".gif";
    String st = stem;
    if (st.length() + suf.length() > 40) st = st.substring(0, 40 - suf.length());
    String cand = st + suf;
    if (!gifExists(cand)) return cand;
  }
  return name;
}

String firstGif() {
  File dir = LittleFS.open(GIF_DIR);
  if (!dir) return "";
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String n = f.name();
      f.close();
      dir.close();
      return n;
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return "";
}

void saveCurrent() {
  prefs.putString("cur", curName);
}

int getBatteryPercent() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_BAT_VOLT);
  int mv = (int)(sum / 8) * 2;

  if (mv > 4300) return -1;

  static const int curve[][2] = {
    {4200, 100}, {4100, 90}, {4000, 77}, {3920, 62}, {3860, 48}, {3820, 38},
    {3780, 28},  {3730, 18}, {3680, 10}, {3600, 4},  {3300, 0}
  };
  const int n = sizeof(curve) / sizeof(curve[0]);
  if (mv >= curve[0][0]) return 100;
  if (mv <= curve[n - 1][0]) return 0;
  for (int i = 0; i < n - 1; i++) {
    if (mv <= curve[i][0] && mv > curve[i + 1][0]) {
      int dv = curve[i][0] - curve[i + 1][0];
      int dp = curve[i][1] - curve[i + 1][1];
      return curve[i + 1][1] + (mv - curve[i + 1][0]) * dp / dv;
    }
  }
  return 0;
}

// AnimatedGIF
static File gifFile;

void *GIFOpenFile(const char *fname, int32_t *pSize) {
  gifFile = LittleFS.open(fname, "r");
  if (gifFile) {
    *pSize = gifFile.size();
    return (void *)&gifFile;
  }
  return NULL;
}

void GIFCloseFile(void *pHandle) {
  File *f = static_cast<File *>(pHandle);
  if (f) f->close();
}

int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead = iLen;
  File *f = static_cast<File *>(pFile->fHandle);
  if ((pFile->iSize - pFile->iPos) < iLen) iBytesRead = pFile->iSize - pFile->iPos - 1;
  if (iBytesRead <= 0) return 0;
  iBytesRead = (int32_t)f->read(pBuf, iBytesRead);
  pFile->iPos = f->position();
  return iBytesRead;
}

int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition) {
  File *f = static_cast<File *>(pFile->fHandle);
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

void GIFDraw(GIFDRAW *pDraw) {
  static uint16_t line[SCREEN_W];
  int y = pDraw->iY + pDraw->y + yOff;
  int x0 = pDraw->iX + xOff;
  int w = pDraw->iWidth;
  if (y < 0 || y >= SCREEN_H || x0 >= SCREEN_W || w < 1) return;
  if (x0 + w > SCREEN_W) w = SCREEN_W - x0;

  uint8_t *s = pDraw->pPixels;
  uint16_t *pal = pDraw->pPalette;

  if (pDraw->ucDisposalMethod == 2) {
    for (int x = 0; x < w; x++)
      if (s[x] == pDraw->ucTransparent) s[x] = pDraw->ucBackground;
    pDraw->ucHasTransparency = 0;
  }

  if (pDraw->ucHasTransparency) {
    uint8_t t = pDraw->ucTransparent;
    int x = 0;
    while (x < w) {
      if (s[x] == t) { x++; continue; }
      int start = x;
      while (x < w && s[x] != t) {
        line[x - start] = pal[s[x]];
        x++;
      }
      tft.pushImage(x0 + start, y, x - start, 1, line);
    }
  } else {
    for (int x = 0; x < w; x++) line[x] = pal[s[x]];
    tft.pushImage(x0, y, w, 1, line);
  }
}

// Messages on screen
void closeGif() {
  if (gifOpen) {
    gif.close();
    gifOpen = false;
  }
}

void showMessage(const char *l1, const char *l2 = nullptr) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(l1, SCREEN_W / 2, l2 ? 65 : 85, 4);
  if (l2) {
    tft.setTextColor(0xA979, TFT_BLACK);
    tft.drawString(l2, SCREEN_W / 2, 105, 2);
  }
}

void openCurrent() {
  closeGif();
  tft.fillScreen(TFT_BLACK);

  if (!gifExists(curName)) {
    curName = firstGif();
    saveCurrent();
  }
  if (curName.length() == 0) {
    showMessage("Net GIF :(", "Nazhmi na nizhnyuyu knopku");
    return;
  }

  String path = String(GIF_DIR) + "/" + curName;
  if (gif.open(path.c_str(), GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
    gifOpen = true;
    xOff = max(0, (SCREEN_W - gif.getCanvasWidth()) / 2);
    yOff = max(0, (SCREEN_H - gif.getCanvasHeight()) / 2);
    nextFrameAt = 0;
  } else {
    showMessage("Oshibka GIF", curName.c_str());
  }
}

void playGif() {
  if (!gifOpen) return;
  if ((int32_t)(millis() - nextFrameAt) < 0) return;
  int frameDelay = 0;
  int r = gif.playFrame(false, &frameDelay);
  if (frameDelay < 10) frameDelay = 10;
  nextFrameAt = millis() + frameDelay;
  if (r == 0) {
    gif.reset();
  } else if (r < 0) {
    closeGif();
    showMessage("Oshibka vosproizvedeniya", curName.c_str());
  }
}

// Settings screen
void showTextScreen() {
  tft.fillScreen(0x1084);

  tft.fillRect(0, 0, SCREEN_W, 30, 0xA979);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, 0xA979);
  tft.drawString("PODKLYUCHENIE K TELEFONU", SCREEN_W / 2, 15, 2);

  tft.setTextDatum(TL_DATUM);

  tft.setTextColor(TFT_LIGHTGREY, 0x1084);
  tft.drawString("1. Podklyuchis k Wi-Fi:", 16, 38, 2);
  tft.setTextColor(0xA979, 0x1084);
  tft.drawString("   " AP_SSID, 16, 56, 4);
  tft.setTextColor(TFT_LIGHTGREY, 0x1084);
  tft.drawString("   Parol: " AP_PASS, 20, 88, 2);

  tft.setTextColor(TFT_LIGHTGREY, 0x1084);
  tft.drawString("2. Stranica otkroetsya sama, ili:", 16, 112, 2);
  tft.setTextColor(0xA979, 0x1084);
  tft.drawString("   192.168.4.1", 16, 130, 4);
}

// QR generator
// MiniQR begin
#define MQR_MAXV 6
#define MQR_MAXN (17 + 4 * MQR_MAXV)

static int     mqrN = 0;
static uint8_t mqrMod[MQR_MAXN * MQR_MAXN];
static uint8_t mqrFn[MQR_MAXN * MQR_MAXN];

static const uint8_t MQR_DATA_CW[7] = {0, 19, 34, 55, 80, 108, 136};
static const uint8_t MQR_ECC_CW[7]  = {0,  7, 10, 15, 20,  26,  18};
static const uint8_t MQR_BLOCKS[7]  = {0,  1,  1,  1,  1,   1,   2};

static inline bool mqrGet(int x, int y) { return mqrMod[y * mqrN + x]; }

static void mqrSetFn(int x, int y, bool dark) {
  if (x < 0 || y < 0 || x >= mqrN || y >= mqrN) return;
  mqrMod[y * mqrN + x] = dark ? 1 : 0;
  mqrFn[y * mqrN + x] = 1;
}

static int mqrGfMul(int x, int y) {
  int z = 0;
  for (int i = 7; i >= 0; i--) {
    z = (z << 1) ^ ((z >> 7) * 0x11D);
    z ^= ((y >> i) & 1) * x;
  }
  return z;
}

static void mqrRsDivisor(uint8_t *res, int deg) {
  for (int i = 0; i < deg; i++) res[i] = 0;
  res[deg - 1] = 1;
  int root = 1;
  for (int i = 0; i < deg; i++) {
    for (int j = 0; j < deg; j++) {
      res[j] = (uint8_t)mqrGfMul(res[j], root);
      if (j + 1 < deg) res[j] ^= res[j + 1];
    }
    root = mqrGfMul(root, 2);
  }
}

static void mqrRsRemainder(const uint8_t *data, int len, const uint8_t *div, int deg, uint8_t *res) {
  for (int i = 0; i < deg; i++) res[i] = 0;
  for (int k = 0; k < len; k++) {
    int factor = data[k] ^ res[0];
    for (int i = 0; i < deg - 1; i++) res[i] = res[i + 1];
    res[deg - 1] = 0;
    for (int i = 0; i < deg; i++) res[i] ^= (uint8_t)mqrGfMul(div[i], factor);
  }
}

static void mqrPut(uint8_t *buf, int &pos, uint32_t val, int len) {
  for (int i = len - 1; i >= 0; i--, pos++)
    if ((val >> i) & 1) buf[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7));
}

static void mqrDrawFormat(int mask) {
  int n = mqrN;
  int data = (1 << 3) | mask;
  int rem = data;
  for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
  int bits = ((data << 10) | rem) ^ 0x5412;
  for (int i = 0; i <= 5; i++) mqrSetFn(8, i, (bits >> i) & 1);
  mqrSetFn(8, 7, (bits >> 6) & 1);
  mqrSetFn(8, 8, (bits >> 7) & 1);
  mqrSetFn(7, 8, (bits >> 8) & 1);
  for (int i = 9; i < 15; i++) mqrSetFn(14 - i, 8, (bits >> i) & 1);
  for (int i = 0; i < 8; i++) mqrSetFn(n - 1 - i, 8, (bits >> i) & 1);
  for (int i = 8; i < 15; i++) mqrSetFn(8, n - 15 + i, (bits >> i) & 1);
  mqrSetFn(8, n - 8, true);
}

static void mqrApplyMask(int mask) {
  for (int y = 0; y < mqrN; y++) {
    for (int x = 0; x < mqrN; x++) {
      bool inv;
      switch (mask) {
        case 0: inv = (x + y) % 2 == 0; break;
        case 1: inv = y % 2 == 0; break;
        case 2: inv = x % 3 == 0; break;
        case 3: inv = (x + y) % 3 == 0; break;
        case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
        case 5: inv = x * y % 2 + x * y % 3 == 0; break;
        case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
        default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
      }
      if (inv && !mqrFn[y * mqrN + x]) mqrMod[y * mqrN + x] ^= 1;
    }
  }
}

static int mqrPenalty() {
  int n = mqrN, p = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int a = 0; a < n; a++) {
      int run = 1;
      for (int b = 1; b < n; b++) {
        bool cur  = pass ? mqrGet(a, b)     : mqrGet(b, a);
        bool prev = pass ? mqrGet(a, b - 1) : mqrGet(b - 1, a);
        if (cur == prev) {
          run++;
          if (run == 5) p += 3;
          else if (run > 5) p++;
        } else {
          run = 1;
        }
      }
    }
  }
  int dark = 0;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      bool c = mqrGet(x, y);
      if (c) dark++;
      if (x + 1 < n && y + 1 < n && c == mqrGet(x + 1, y) && c == mqrGet(x, y + 1) && c == mqrGet(x + 1, y + 1)) p += 3;
    }
  }
  int total = n * n;
  int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
  if (k > 0) p += k * 10;
  return p;
}

static bool mqrEncode(const char *text) {
  int len = strlen(text);
  int v = 0;
  for (int i = 1; i <= MQR_MAXV; i++) {
    if (len <= MQR_DATA_CW[i] - 2) { v = i; break; }
  }
  if (!v) return false;

  int dataCw = MQR_DATA_CW[v], nb = MQR_BLOCKS[v], ecl = MQR_ECC_CW[v], per = dataCw / nb;

  uint8_t data[136];
  memset(data, 0, sizeof(data));
  int pos = 0;
  mqrPut(data, pos, 4, 4);
  mqrPut(data, pos, (uint32_t)len, 8);
  for (int i = 0; i < len; i++) mqrPut(data, pos, (uint8_t)text[i], 8);
  int cap = dataCw * 8;
  int term = cap - pos;
  if (term > 4) term = 4;
  mqrPut(data, pos, 0, term);
  pos = (pos + 7) & ~7;
  for (uint8_t pad = 0xEC; pos < cap; pad ^= (0xEC ^ 0x11)) mqrPut(data, pos, pad, 8);

  uint8_t div[26], ecc[2][26], all[172];
  mqrRsDivisor(div, ecl);
  for (int b = 0; b < nb; b++) mqrRsRemainder(data + b * per, per, div, ecl, ecc[b]);
  int total = 0;
  for (int i = 0; i < per; i++)
    for (int b = 0; b < nb; b++) all[total++] = data[b * per + i];
  for (int i = 0; i < ecl; i++)
    for (int b = 0; b < nb; b++) all[total++] = ecc[b][i];

  int n = 17 + 4 * v;
  mqrN = n;
  memset(mqrMod, 0, sizeof(mqrMod));
  memset(mqrFn, 0, sizeof(mqrFn));
  for (int i = 0; i < n; i++) {
    mqrSetFn(6, i, i % 2 == 0);
    mqrSetFn(i, 6, i % 2 == 0);
  }
  const int cx[3] = {3, n - 4, 3};
  const int cy[3] = {3, 3, n - 4};
  for (int k = 0; k < 3; k++) {
    for (int dy = -4; dy <= 4; dy++) {
      for (int dx = -4; dx <= 4; dx++) {
        int d = max(abs(dx), abs(dy));
        mqrSetFn(cx[k] + dx, cy[k] + dy, d != 2 && d != 4);
      }
    }
  }
  if (v >= 2) {
    for (int dy = -2; dy <= 2; dy++)
      for (int dx = -2; dx <= 2; dx++)
        mqrSetFn(n - 7 + dx, n - 7 + dy, max(abs(dx), abs(dy)) != 1);
  }
  mqrDrawFormat(0);

  int bit = 0;
  for (int right = n - 1; right >= 1; right -= 2) {
    if (right == 6) right = 5;
    for (int vert = 0; vert < n; vert++) {
      for (int j = 0; j < 2; j++) {
        int x = right - j;
        bool upward = ((right + 1) & 2) == 0;
        int y = upward ? n - 1 - vert : vert;
        if (!mqrFn[y * n + x] && bit < total * 8) {
          mqrMod[y * n + x] = (all[bit >> 3] >> (7 - (bit & 7))) & 1;
          bit++;
        }
      }
    }
  }

  int best = 0, bestPen = 0x7FFFFFFF;
  for (int m = 0; m < 8; m++) {
    mqrApplyMask(m);
    mqrDrawFormat(m);
    int p = mqrPenalty();
    if (p < bestPen) { bestPen = p; best = m; }
    mqrApplyMask(m);
  }
  mqrApplyMask(best);
  mqrDrawFormat(best);
  return true;
}
// MiniQR end

// QR Screen
bool showQrScreen() {
  String text = String("WIFI:T:WPA;S:") + AP_SSID + ";P:" + AP_PASS + ";;";
  if (!mqrEncode(text.c_str())) return false;

  tft.fillScreen(0x1084);

  const int quiet = 10;
  const int scale = (SCREEN_H - 2 * quiet) / mqrN;
  const int side  = mqrN * scale;
  const int box   = side + quiet * 2;
  const int bx = 6, by = (SCREEN_H - box) / 2;

  tft.fillRect(bx, by, box, box, TFT_WHITE);
  for (int y = 0; y < mqrN; y++)
    for (int x = 0; x < mqrN; x++)
      if (mqrGet(x, y))
        tft.fillRect(bx + quiet + x * scale, by + quiet + y * scale, scale, scale, TFT_BLACK);

  const int tx = bx + box + 10;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, 0x1084);
  tft.drawString("Navedi kameru", tx, 14, 2);
  tft.drawString("na QR-kod", tx, 32, 2);

  tft.setTextColor(TFT_LIGHTGREY, 0x1084);
  tft.drawString("Ili vruchnuyu:", tx, 64, 2);
  tft.setTextColor(0xA979, 0x1084);
  tft.drawString(AP_SSID, tx, 84, 2);
  tft.setTextColor(TFT_LIGHTGREY, 0x1084);
  tft.drawString("Parol: " AP_PASS, tx, 104, 2);

  tft.drawString("Stranica otkroetsya", tx, 134, 1);
  tft.drawString("sama (ili 192.168.4.1)", tx, 146, 1);
  return true;
}

void showServerInfo() {
  if (showQrScreen()) return;
  showTextScreen();
}

// WIFI and Server
void startServer() {
  bool wasOn = serverOn;
  if (!serverOn) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    dns.start(53, "*", IPAddress(192, 168, 4, 1));
    server.begin();
    serverOn = true;
  }
  touchServer();

  if (wasOn && WiFi.softAPgetStationNum() > 0) return;
  infoUntil = millis() + INFO_SCREEN_MS;
  showServerInfo();
}

void stopServer() {
  if (!serverOn) return;
  dns.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  serverOn = false;
  if (infoUntil) hideInfo();
}

// Web-page
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#ece6fb">
<title>GIF loader</title>
<style>
  :root {
    --bg: #ece6fb;
    --card: #f6f2ff;
    --tile: #ddd2f7;
    --ink: #1e1240;
    --muted: #7a6aa6;
    --deep: #2b1666;
    --deep-2: #5a35c8;
    --soft: #c8b5f5;
    --soft-ink: #2b1666;
    --danger: #d6336c;
    --ok-bg: #d9f5e5;
    --ok-ink: #17884a;
    --shadow: 0 10px 30px rgba(60, 30, 140, 0.14);
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #140c2b;
      --card: #1f1440;
      --tile: #2a1c57;
      --ink: #f3eeff;
      --muted: #a99bd6;
      --deep: #3a1f8f;
      --deep-2: #7a52ee;
      --soft: #c8b5f5;
      --soft-ink: #1e1240;
      --danger: #ff7aa8;
      --ok-bg: #173a2a;
      --ok-ink: #6fe0a2;
      --shadow: 0 10px 30px rgba(0, 0, 0, 0.4);
    }
  }
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  html { background: var(--bg); }
  body {
    margin: 0;
    padding: 16px 16px calc(110px + env(safe-area-inset-bottom, 0px));
    background: var(--bg);
    color: var(--ink);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
    max-width: 460px;
    margin-inline: auto;
  }
  button { font-family: inherit; border: none; cursor: pointer; transition: transform .1s ease, opacity .2s; }
  button:active { transform: scale(.97); }
  button:disabled { opacity: .45; cursor: default; }

  /* ---- Hero ---- */
  .hero {
    position: relative;
    overflow: hidden;
    border-radius: 32px;
    padding: 20px 20px 22px;
    color: #fff;
    background: linear-gradient(155deg, #6a43d8 0%, #3b1f8f 55%, #2b1666 100%);
    box-shadow: var(--shadow);
  }
  .hero::before, .hero::after {
    content: ""; position: absolute; border-radius: 50%; pointer-events: none;
  }
  .hero::before { width: 220px; height: 220px; right: -70px; top: -60px; background: radial-gradient(circle, rgba(255,255,255,.22), transparent 70%); }
  .hero::after  { width: 180px; height: 180px; left: -60px; bottom: -80px; background: radial-gradient(circle, rgba(200,181,245,.28), transparent 70%); }
  .hero > * { position: relative; z-index: 1; }
  .chip {
    display: inline-flex; align-items: center; gap: 6px;
    padding: 8px 14px; border-radius: 999px;
    background: rgba(255,255,255,.88); color: var(--soft-ink);
    font-size: 13px; font-weight: 600;
  }
  .eyebrow { margin-top: 26px; font-size: 13px; font-weight: 600; letter-spacing: .04em; opacity: .85; }
  .hero h1 { margin: 4px 0 8px; font-size: 38px; line-height: 1.05; font-weight: 800; letter-spacing: -.02em; }
  .hero p { margin: 0 0 18px; font-size: 14px; line-height: 1.35; opacity: .85; }
  .pick {
    display: flex; align-items: center; justify-content: space-between; gap: 10px;
    width: 100%; padding: 14px 18px; border-radius: 999px;
    background: rgba(255,255,255,.16); border: 1px solid rgba(255,255,255,.35);
    color: #fff; font-size: 15px; font-weight: 600; cursor: pointer;
  }
  .pick small { font-size: 12px; font-weight: 500; opacity: .8; }
  input[type=file] { display: none; }
  .btn-main {
    width: 100%; margin-top: 10px; padding: 15px; border-radius: 999px;
    background: var(--soft); color: var(--soft-ink);
    font-size: 16px; font-weight: 700;
  }
  progress {
    width: 100%; height: 8px; margin-top: 12px; display: none;
    accent-color: #fff; border-radius: 4px;
  }
  #upMsg { margin-top: 10px; text-align: center; font-size: 13px; opacity: .9; min-height: 16px; }

  /* ---- Stats ---- */
  .stats { display: flex; gap: 10px; margin: 14px 0 6px; }
  .stat {
    flex: 1; padding: 12px 14px; border-radius: 20px;
    background: var(--card); box-shadow: var(--shadow);
  }
  .stat span { display: block; font-size: 11px; font-weight: 600; color: var(--muted); text-transform: uppercase; letter-spacing: .05em; }
  .stat b { display: block; margin-top: 3px; font-size: 16px; font-weight: 800; }

  /* ---- Section title ---- */
  .section { display: flex; align-items: baseline; justify-content: space-between; margin: 22px 4px 12px; }
  .section h2 { margin: 0; font-size: 30px; line-height: 1.05; font-weight: 800; letter-spacing: -.02em; }
  .count { font-size: 13px; font-weight: 700; padding: 6px 12px; border-radius: 999px; background: var(--deep); color: #fff; }

  /* ---- Grid of GIFs ---- */
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
  .tile {
    display: flex; flex-direction: column; gap: 6px;
    padding: 10px; border-radius: 24px; background: var(--tile);
    border: 2px solid transparent;
  }
  .tile.cur { border-color: var(--deep-2); background: var(--card); box-shadow: var(--shadow); }
  .thumb {
    position: relative; aspect-ratio: 16 / 9; border-radius: 16px;
    display: grid; place-items: center;
    background: linear-gradient(150deg, #7a52ee, #2b1666);
  }
  .thumb svg { width: 22px; height: 22px; fill: #fff; margin-left: 2px; }
  .badge {
    position: absolute; left: 6px; top: 6px;
    padding: 3px 8px; border-radius: 999px; font-size: 10px; font-weight: 800;
    background: var(--ok-bg); color: var(--ok-ink);
  }
  .name { margin: 4px 4px 0; font-size: 13px; font-weight: 700; word-break: break-all; line-height: 1.25; }
  .size { margin: 0 4px; font-size: 12px; color: var(--muted); }
  .tile-actions { display: flex; gap: 6px; margin-top: auto; padding-top: 6px; }
  .btn-pick {
    flex: 1; padding: 10px 6px; border-radius: 999px;
    background: var(--deep); color: #fff; font-size: 13px; font-weight: 700;
  }
  .btn-del {
    width: 40px; border-radius: 50%; padding: 0; aspect-ratio: 1;
    display: grid; place-items: center;
    background: transparent; color: var(--danger); border: 1.5px solid currentColor;
  }
  .btn-del svg { width: 17px; height: 17px; fill: none; stroke: currentColor; stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; }
  .empty { grid-column: 1 / -1; text-align: center; padding: 26px 10px; color: var(--muted); font-size: 14px; background: var(--tile); border-radius: 24px; }

  .hint { margin: 18px 8px 0; text-align: center; font-size: 12px; line-height: 1.5; color: var(--muted); }

  /* ---- Bottom bar ---- */
  .dock {
    position: fixed; left: 0; right: 0; bottom: 0; z-index: 10;
    padding: 12px 16px calc(14px + env(safe-area-inset-bottom, 0px));
    display: flex; justify-content: center;
    background: linear-gradient(to top, var(--bg) 55%, transparent);
    pointer-events: none;
  }
  .sleep {
    pointer-events: auto;
    display: flex; align-items: center; justify-content: space-between; gap: 12px;
    width: 100%; max-width: 428px; padding: 8px 8px 8px 24px; border-radius: 999px;
    background: var(--deep); color: #fff; font-size: 16px; font-weight: 700;
    box-shadow: 0 10px 26px rgba(43,22,102,.45);
  }
  .sleep i {
    width: 44px; height: 44px; border-radius: 50%;
    background: var(--soft); display: grid; place-items: center;
  }
  .sleep i svg { width: 20px; height: 20px; fill: var(--soft-ink); }

  [hidden] { display: none !important; }
  .offline .pick, .offline .btn-main, .offline .tile-actions, .offline .sleep { opacity: .4; pointer-events: none; }
  #upErr { margin-top: 6px; text-align: center; font-size: 13px; line-height: 1.35; color: #ffc2d4; white-space: pre-line; }

  /* ---- Connection lost ---- */
  .lost {
    position: fixed; z-index: 20; left: 12px; right: 12px;
    top: calc(12px + env(safe-area-inset-top, 0px));
    max-width: 436px; margin-inline: auto;
    padding: 16px 18px; border-radius: 24px;
    display: flex; flex-direction: column; gap: 6px;
    background: var(--deep); color: #fff;
    box-shadow: 0 14px 34px rgba(20,8,60,.5);
  }
  .lost b { font-size: 17px; font-weight: 800; }
  .lost span { font-size: 13px; line-height: 1.45; opacity: .9; }
  .lost button {
    margin-top: 8px; padding: 12px; border-radius: 999px;
    background: var(--soft); color: var(--soft-ink); font-size: 15px; font-weight: 700;
  }

  /* ---- Confirmation and notification ---- */
  .modal {
    position: fixed; inset: 0; z-index: 30;
    display: flex; align-items: flex-end; justify-content: center;
    background: rgba(20,10,50,.5);
  }
  .sheet {
    width: 100%; max-width: 460px;
    padding: 24px 20px calc(20px + env(safe-area-inset-bottom, 0px));
    border-radius: 28px 28px 0 0; background: var(--card);
  }
  .sheet p { margin: 0 0 18px; text-align: center; font-size: 18px; font-weight: 700; }
  .m-actions { display: flex; gap: 10px; }
  .m-actions button { flex: 1; padding: 14px; border-radius: 999px; font-size: 15px; font-weight: 700; }
  .m-no { background: var(--tile); color: var(--ink); }
  .m-yes { background: var(--deep); color: #fff; }
  .toast {
    position: fixed; z-index: 40; left: 50%;
    bottom: calc(100px + env(safe-area-inset-bottom, 0px));
    transform: translate(-50%, 20px); opacity: 0; pointer-events: none;
    max-width: 90%; padding: 12px 18px; border-radius: 999px;
    background: var(--ink); color: var(--bg);
    font-size: 14px; font-weight: 600; text-align: center;
    transition: opacity .2s, transform .2s;
  }
  .toast.show { opacity: 1; transform: translate(-50%, 0); }
</style>
</head>
<body>

<section class="hero">
  <span class="chip" id="timer">Связь активна</span>
  <div class="eyebrow">GIF · T-Display-S3</div>
  <h1>Загрузить гифку.</h1>
  <p>Размер 320×170.<br>Можно выбрать сразу несколько файлов.</p>

  <label class="pick" for="file">
    <span>Выбрать гифки</span>
    <small id="picked">.gif</small>
  </label>
  <input type="file" id="file" accept=".gif,image/gif" multiple onchange="onFilePicked()">
  <button class="btn-main" id="upBtn" onclick="upload()" disabled>Загрузить</button>
  <progress id="prog" value="0" max="100"></progress>
  <div id="upMsg"></div>
  <div id="upErr"></div>
</section>

<div class="stats">
  <div class="stat"><span>Заряд</span><b id="battery">…</b></div>
  <div class="stat"><span>Свободно</span><b id="space">…</b></div>
</div>

<div class="section">
  <h2>Мои гифки</h2>
  <span class="count" id="count">0</span>
</div>
<div class="grid" id="list"><div class="empty">Загрузка…</div></div>

<div class="hint">
  Выключить: зажми верхнюю кнопку на 1.5 сек.<br>
  Включить: нажми на верхнюю кнопку или Reset.
</div>

<div class="dock">
  <button class="sleep" onclick="goSleep()">
    <span>Выключить экран</span>
    <i><svg viewBox="0 0 24 24"><path d="M20.5 14.5A8.5 8.5 0 0 1 9.5 3.5a.7.7 0 0 0-.9-.9A10 10 0 1 0 21.4 15.4a.7.7 0 0 0-.9-.9z"/></svg></i>
  </button>
</div>

<div class="lost" id="lost" hidden>
  <b id="lostTitle">Связь потеряна</b>
  <span id="lostText"></span>
  <button id="lostBtn" onclick="recheck()">Проверить связь</button>
</div>

<div class="modal" id="modal" hidden>
  <div class="sheet">
    <p id="mText"></p>
    <div class="m-actions">
      <button class="m-no" id="mNo">Отмена</button>
      <button class="m-yes" id="mYes">ОК</button>
    </div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
let left = 0, alive = true, busy = false, checking = false, recovering = false, quietUntil = 0;
const $ = s => document.querySelector(s);
const formatSize = n => n > 1048576 ? (n/1048576).toFixed(1)+' Мб' : (n/1024).toFixed(0)+' Кб';

async function api(url, opt, ms) {
  const c = new AbortController();
  const t = setTimeout(() => c.abort(), ms || 4000);
  try { return await fetch(url, Object.assign({ signal: c.signal }, opt || {})); }
  finally { clearTimeout(t); }
}

function toast(msg) {
  const t = $('#toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => t.classList.remove('show'), 3500);
}

function ask(text, okLabel) {
  return new Promise(res => {
    $('#mText').textContent = text;
    $('#mYes').textContent = okLabel || 'ОК';
    $('#modal').hidden = false;
    const close = v => {
      $('#modal').hidden = true;
      $('#mYes').onclick = $('#mNo').onclick = $('#modal').onclick = null;
      res(v);
    };
    $('#mYes').onclick = () => close(true);
    $('#mNo').onclick = () => close(false);
    $('#modal').onclick = e => { if (e.target.id === 'modal') close(false); };
  });
}

/* Connection state */
function setAlive(v) {
  alive = v;
  document.body.classList.toggle('offline', !v);
  if (v) $('#lost').hidden = true;
}

function dead(kind) {
  setAlive(false);
  const sleep = kind === 'sleep';
  $('#timer').textContent = sleep ? 'Экран выключен' : 'Связь потеряна';
  $('#lostTitle').textContent = sleep ? 'Экран выключен' : 'Связь потеряна';
  $('#lostText').textContent = sleep
    ? 'Разбудить: короткое нажатие на верхнюю кнопку или Reset. Потом нажми BOOT, чтобы снова включить Wi-Fi.'
    : 'Wi-Fi на плате отключается сам. Чтобы вернуть связь: нажми BOOT на плате и убедись, что телефон подключён к её Wi-Fi. Страница подключится сама.';
  $('#lost').hidden = false;
}

async function recheck() {
  const b = $('#lostBtn');
  b.disabled = true;
  b.textContent = 'Проверяю…';
  const ok = await load();
  b.disabled = false;
  b.textContent = 'Проверить связь';
  if (!ok) toast('Плата пока не отвечает');
}

function onFilePicked() {
  const f = $('#file').files;
  if (f.length > 0) {
    $('#upBtn').disabled = false;
    $('#picked').textContent = 'Файлов: ' + f.length;
  }
}

async function load() {
  let res;
  try {
    const r = await api('/list');
    res = await r.json();
  } catch(e) {
    if (alive) dead();
    return false;
  }
  if (!alive) setAlive(true);

  left = res.left;
  $('#space').textContent = formatSize(res.total - res.used);
  $('#battery').textContent = res.bat === -1 ? '⚡ Сеть' : res.bat + '%';
  $('#count').textContent = res.files.length;

  const box = $('#list');
  box.innerHTML = '';

  if (!res.files.length) {
    box.innerHTML = '<div class="empty">Загруженных гифок нет</div>';
    return true;
  }

  res.files.forEach(f => {
    const isCur = f.name === res.current;
    const tile = document.createElement('div');
    tile.className = 'tile' + (isCur ? ' cur' : '');
    tile.innerHTML =
      '<div class="thumb"><svg viewBox="0 0 24 24"><path d="M7 4.5v15l13-7.5z"/></svg>' +
      (isCur ? '<span class="badge">Активно</span>' : '') + '</div>' +
      '<div class="name"></div>' +
      '<div class="size">' + formatSize(f.size) + '</div>';
    tile.querySelector('.name').textContent = f.name;

    const actions = document.createElement('div');
    actions.className = 'tile-actions';

    const showBtn = document.createElement('button');
    showBtn.className = 'btn-pick';
    showBtn.textContent = isCur ? 'Выбрано' : 'Выбрать';
    showBtn.disabled = isCur;
    showBtn.onclick = () => act('/select', f.name);

    const delBtn = document.createElement('button');
    delBtn.className = 'btn-del';
    delBtn.setAttribute('aria-label', 'Удалить');
    delBtn.innerHTML = '<svg viewBox="0 0 24 24"><path d="M4 7h16M10 11v6M14 11v6M6 7l1 12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-12M9 7V4h6v3"/></svg>';
    delBtn.onclick = async () => { if (await ask('Удалить эту гифку?', 'Удалить')) act('/delete', f.name); };

    actions.append(showBtn, delBtn);
    tile.append(actions);
    box.append(tile);
  });
  return true;
}

async function act(url, name) {
  try {
    const r = await api(url + '?name=' + encodeURIComponent(name), { method: 'POST' }, 8000);
    if (!r.ok) toast(await r.text());
  } catch(e) { dead(); return; }
  load();
}

async function upload() {
  const files = [...$('#file').files];
  if (!files.length || busy) return;
  busy = true;
  $('#upBtn').disabled = true;
  $('#upErr').textContent = '';
  $('#prog').style.display = 'block';

  const errors = [];
  let done = 0;

  for (let i = 0; i < files.length; i++) {
    const f = files[i];
    $('#upMsg').textContent = `Загружается ${i+1}/${files.length}...`;
    $('#prog').value = 0;

    const r = await new Promise(res => {
      const x = new XMLHttpRequest(), fd = new FormData();
      fd.append('file', f, f.name);
      x.open('POST', '/upload');
      x.upload.onprogress = e => { if (e.lengthComputable) $('#prog').value = (e.loaded / e.total) * 100; };
      x.onload = () => res({ ok: x.status === 200, msg: x.responseText });
      x.onerror = () => res({ ok: false, lost: true, msg: 'нет связи с платой' });
      x.send(fd);
    });

    if (r.ok) done++;
    else errors.push(`${f.name}: ${r.msg || 'ошибка загрузки'}`);
    if (r.lost) break;
  }

  busy = false;
  $('#prog').style.display = 'none';
  $('#file').value = '';
  $('#picked').textContent = '.gif';
  $('#upMsg').textContent = errors.length ? `Загружено ${done} из ${files.length}` : 'Всё загружено!';
  $('#upErr').textContent = errors.join('\n');
  await load();
}

async function goSleep() {
  if (!(await ask('Выключить экран?', 'Выключить'))) return;
  try { await api('/sleep', { method: 'POST' }, 3000); } catch(e) {}
  quietUntil = Date.now() + 8000;   // пока плата засыпает, не проверяем связь
  dead('sleep');
}

setInterval(() => {
  if (!alive) return;
  if (busy) { $('#timer').textContent = '⏱ Идёт загрузка…'; return; }
  if (left > 0) left--;
  const m = Math.floor(left / 60), s = left % 60;
  $('#timer').textContent = `⏱ Wi-Fi отключится через ${m}:${String(s).padStart(2, '0')}`;
  if (left <= 0 && !checking) {
    checking = true;
    load().finally(() => { checking = false; });
  }
}, 1000);

/* List update */
setInterval(() => { if (alive && !busy) load(); }, 15000);

setInterval(async () => {
  if (alive || recovering || Date.now() < quietUntil) return;
  recovering = true;
  try { await load(); } finally { recovering = false; }
}, 4000);

load();
</script>
</body>
</html>
)rawliteral";

// HTTP
void handleNotFound() {
  if (server.uri() == "/favicon.ico") { server.send(404, "text/plain", ""); return; }
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}

void handleRoot() {
  touchServer();
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleList() {
  String j = "{\"files\":[";
  File dir = LittleFS.open(GIF_DIR);
  bool first = true;
  uint32_t used = 0;
  if (dir) {
    File f = dir.openNextFile();
    while (f) {
      if (!f.isDirectory()) {
        if (!first) j += ",";
        first = false;
        uint32_t sz = (uint32_t)f.size();
        used += ((sz + FS_BLOCK - 1) / FS_BLOCK) * FS_BLOCK;
        j += "{\"name\":\"" + String(f.name()) + "\",\"size\":" + String(sz) + "}";
      }
      f.close();
      f = dir.openNextFile();
    }
    dir.close();
  }
  j += "],\"current\":\"" + curName + "\"";
  j += ",\"total\":" + String((uint32_t)LittleFS.totalBytes());
  j += ",\"used\":" + String(used);

  int bat = getBatteryPercent();
  j += ",\"bat\":" + String(bat);

  int32_t left = (int32_t)(serverUntil - millis()) / 1000;
  if (left < 0) left = 0;
  j += ",\"left\":" + String(left) + "}";
  server.send(200, "application/json", j);
}

void handleSelect() {
  touchServer();
  String name = cleanName(server.arg("name"));
  if (!gifExists(name)) { server.send(404, "text/plain; charset=utf-8", "Файл не найден"); return; }
  curName = name;
  saveCurrent();
  needReload = true;
  infoUntil = 0;
  server.send(200, "text/plain", "OK");
}

void handleDelete() {
  touchServer();
  String name = cleanName(server.arg("name"));
  if (!gifExists(name)) { server.send(404, "text/plain; charset=utf-8", "Файл не найден"); return; }
  if (name == curName) closeGif();
  LittleFS.remove(String(GIF_DIR) + "/" + name);
  if (name == curName) {
    curName = firstGif();
    saveCurrent();
    needReload = true;
  }
  server.send(200, "text/plain", "OK");
}

void handleSleep() {
  server.send(200, "text/plain", "OK");
  wantSleep = true;
  sleepAt = millis() + 400;
}

void handleUploadDone() {
  touchServer();
  if (uploadErr.length()) server.send(500, "text/plain; charset=utf-8", uploadErr);
  else server.send(200, "text/plain", "OK");
}

void handleUpload() {
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    uploadErr = "";
    String name = cleanName(up.filename);
    if (!name.endsWith(".gif")) {
      uploadErr = "Только .gif файлы";
      uploadPath = "";
      return;
    }
    name = uniqueGifName(name);
    uploadPath = String(GIF_DIR) + "/" + name;
    uploadFile = LittleFS.open(uploadPath, "w");
    if (!uploadFile) {
      uploadErr = "Не удалось сохранить файл";
      uploadPath = "";
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    touchServer();
    if (uploadFile && !uploadErr.length()) {
      if (uploadFile.write(up.buf, up.currentSize) != up.currentSize) {
        uploadErr = "Закончилось место на плате";
        uploadFile.close();
        LittleFS.remove(uploadPath);
        uploadPath = "";
      }
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    if (uploadPath.length() && !uploadErr.length()) {
      String name = uploadPath.substring(String(GIF_DIR).length() + 1);
      if (curName.length() == 0) {
        curName = name;
        saveCurrent();
        needReload = true;
      }
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    if (uploadPath.length()) LittleFS.remove(uploadPath);
    uploadErr = "Загрузка прервана";
    needReload = true;
  }
}

// Sleep mode
void goToSleep() {
  stopServer();
  closeGif();
  showMessage("Zasypayu...", "Razbudit: GPIO14 ili RESET");
  delay(1200);

  tft.fillScreen(TFT_BLACK);
  tft.writecommand(0x28);
  tft.writecommand(0x10);
  delay(120);

  digitalWrite(PIN_LCD_BL, LOW);
  digitalWrite(PIN_POWER_ON, LOW);
  gpio_hold_en((gpio_num_t)PIN_LCD_BL);
  gpio_hold_en((gpio_num_t)PIN_POWER_ON);
  gpio_deep_sleep_hold_en();

  uint32_t t0 = millis();
  while (digitalRead(PIN_WAKE) == LOW && millis() - t0 < 10000) delay(10);
  delay(80);

  rtc_gpio_pullup_en((gpio_num_t)PIN_WAKE);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_WAKE);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_WAKE, 0);
  esp_deep_sleep_start();
}

// BOOT button
void handleButton() {
  static bool last = true;
  static uint32_t lastChange = 0;
  bool now = digitalRead(PIN_BOOT);
  if (now != last && millis() - lastChange > 50) {
    lastChange = millis();
    last = now;
    if (!now) {
      startServer();
    }
  }
}

// Sleep
void handleSleepButton() {
  static bool armed = false;
  static bool wasDown = false;
  static uint32_t downAt = 0;

  bool down = (digitalRead(PIN_WAKE) == LOW);

  if (!armed) {
    if (!down) armed = true;
    return;
  }

  if (down) {
    if (!wasDown) {
      wasDown = true;
      downAt = millis();
    } else if (!wantSleep && millis() - downAt >= SLEEP_HOLD_MS) {
      wantSleep = true;
      sleepAt = millis();
    }
  } else {
    wasDown = false;
  }
}

// setup / loop
void setup() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_LCD_BL);
  gpio_hold_dis((gpio_num_t)PIN_POWER_ON);

  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  rtc_gpio_deinit((gpio_num_t)PIN_WAKE);
  pinMode(PIN_WAKE, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  gif.begin(LITTLE_ENDIAN_PIXELS);

  if (!LittleFS.begin(true, "/littlefs", 10, FS_LABEL)) {
    showMessage("FS Error", "Check partition scheme");
    while (true) delay(1000);
  }
  if (!LittleFS.exists(GIF_DIR)) LittleFS.mkdir(GIF_DIR);

  prefs.begin("gif", false);
  curName = prefs.getString("cur", "");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/list", HTTP_GET, handleList);
  server.on("/select", HTTP_POST, handleSelect);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/sleep", HTTP_POST, handleSleep);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  server.onNotFound(handleNotFound);

  WiFi.mode(WIFI_OFF);
}

void loop() {
  handleButton();
  handleSleepButton();

  if (serverOn) {
    dns.processNextRequest();
    server.handleClient();
    if ((int32_t)(millis() - serverUntil) >= 0) stopServer();
  }

  if (wantSleep && (int32_t)(millis() - sleepAt) >= 0) goToSleep();

  if (infoUntil) {
    static uint32_t lastPoll = 0;
    if (serverOn && millis() - lastPoll > 300) {
      lastPoll = millis();
      if (WiFi.softAPgetStationNum() > 0) hideInfo();
    }
  }

  if (infoUntil) {
    if ((int32_t)(millis() - infoUntil) >= 0) {
      hideInfo();
    } else {
      delay(2);
      return;
    }
  }

  if (needReload) {
    needReload = false;
    openCurrent();
  }

  playGif();
  delay(1);
}
