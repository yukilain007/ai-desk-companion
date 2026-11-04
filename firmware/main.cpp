// 桌面小伙伴 — 固件（Waveshare ESP32-S3-Touch-AMOLED-1.75C）
// 一张会动的脸：表情之间有动画过渡，会眨眼、东张西望、听你说话时看着你。
// 点屏幕切下一个表情；串口发 0-6 直接指定表情；
// 连上 WiFi 后，claude.ai 里的 Claude 可以通过云端中转（cloud/）给它换表情、说一句话；
// 局域网里还能连 Mac 上的对话桥（bridge/），对着它说话。
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>  // 先引入，GFX 库才会打开 U8g2 中文字体支持
#include "Arduino_GFX_Library.h"
#include "log.h"
#include "TouchDrvCSTXXX.hpp"
#include "pin_config.h"
#include "net.h"
#include "audio.h"
#include "power.h"
#include "motion.h"
#include "ancs.h"
#include "esp_wifi.h"
#include <Preferences.h>
#include "settings.h"
#if __has_include("wake_voice.h")
#include "wake_voice.h"  // 定制的叫醒声音；没有这个文件就用提示音
#define HAVE_WAKE_VOICE 1
#endif

// ---------- display ----------
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
// 只刷有脸的区域（四角本来就是黑的），每帧推送的像素少约四成
// 两块画布：平时只刷脸附近（快，~32 帧）；专注时换大画布，环才能靠近屏幕边缘（~23 帧）
Arduino_Canvas *gfxSmall = new Arduino_Canvas(386, 370, panel, 40, 40);
Arduino_Canvas *gfxBig = new Arduino_Canvas(442, 442, panel, 12, 12);
Arduino_Canvas *gfx = gfxSmall;
int OX = 40, OY = 40, CW = 386, CH = 370;

void useCanvas(bool big) {
  if ((gfx == gfxBig) == big) return;
  gfx = big ? gfxBig : gfxSmall;
  OX = OY = big ? 12 : 40;
  CW = big ? 442 : 386;
  CH = big ? 442 : 370;
  panel->fillScreen(0);  // 擦掉小画布外面残留的环
}

TouchDrvCST92xx touch;
volatile bool touchFlag = false;

// ---------- palette ----------
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t BLACK = 0x0000;
const uint16_t WHITE = rgb(255, 255, 255);
// 点缀色（专注点阵、未读圆点、录音点）：config.h 里 ACCENT_R/G/B 可以改
const uint8_t AR = ACCENT_R, AG = ACCENT_G, AB = ACCENT_B;
const uint16_t ACCENT = rgb(AR, AG, AB);
uint16_t accentDim(float k) { return rgb((uint8_t)(AR * k), (uint8_t)(AG * k), (uint8_t)(AB * k)); }

// ---------- 脸的坐标 -> 像素 ----------
// 脸画在一个 -72..72 的方格里（0,0 是屏幕中心），S = 每个单位多少像素
const float S = 466.0f / 144.0f * 1.15f;  // ≈3.72 px/unit
inline float PX(float u) { return 233.0f - OX + u * S; }
float faceDY = 0;  // 说话时脸往上挪，给字幕让位
float nodY = 0;    // 听你说话时点头
inline float PY(float v) { return 233.0f - OY + (v + faceDY + nodY) * S; }

// 粗线工具（单位同上）
void dot(float u, float v, float w, uint16_t c) {
  gfx->fillCircle((int)PX(u), (int)PY(v), max(1, (int)(w * S / 2)), c);
}
void line(float u1, float v1, float u2, float v2, float w, uint16_t c) {
  float len = hypotf(u2 - u1, v2 - v1) * S;
  int n = max(2, (int)(len / 1.5f));
  for (int i = 0; i <= n; i++) {
    float t = (float)i / n;
    dot(u1 + (u2 - u1) * t, v1 + (v2 - v1) * t, w, c);
  }
}
void quad(float u0, float v0, float cu, float cv, float u1, float v1, float w, uint16_t c) {
  for (int i = 0; i <= 40; i++) {
    float t = i / 40.0f, a = (1 - t) * (1 - t), b = 2 * t * (1 - t), d = t * t;
    dot(a * u0 + b * cu + d * u1, a * v0 + b * cv + d * v1, w, c);
  }
}

// ---------- expressions ----------
enum Expr { CALM, WINK, CURIOUS, MEH, MUSIC, SLEEP, SMILE, EXPR_COUNT };
const char *EXPR_NAME[] = { "calm", "wink", "curious", "meh", "music", "sleep", "smile" };

struct Brow { float u1, v1, u2, v2, a; };  // a = 显示多少 0..1

struct Pose {
  float topL, botL, topR, botR;  // 眼睛上沿 / 下沿
  float lid;                     // 0 睁开 .. 1 闭上（闭上后画成一道弧）
  float arc;                     // 闭眼弧线：<0 = ^ 开心，>0 = ∪ 困
  float wink;                    // 右眼单独闭上（眨一只眼）
  float happy;                   // 笑眼：下半边被弯弯地切掉，变成 ◠
  Brow bl, br;
  float smile;                   // 嘴角：0 = 平，1 = 微笑，2 = 大笑
  float blush;                   // 脸红 0..1
  float bright;
};

const float EYE_X = 26, EYE_HW = 9;  // 眼睛离中心多远、半宽
const Brow BL_HIDE = { -35, -19, -17, -19, 0 }, BR_HIDE = { 17, -19, 35, -19, 0 };

Pose basePose() {
  return { -12, 12, -12, 12, 0, 2, 0, 0, BL_HIDE, BR_HIDE, 0.6f, 0, 200 };
}

Pose cur = basePose(), tgt = cur;
bool petting = false;  // 被摸：手指贴着屏幕滑动
uint32_t petStart = 0;
bool faceDownSleep = false;  // 屏幕扣着睡的（翻过来才醒，不走早上自动醒）
bool listening = false;  // 听见你在说话
bool pttActive = false;  // 按住 BOOT 键在说话
bool covered = false;    // 被捂住眼睛
bool touchIsDown = false;  // 手指/手掌还在屏幕上（触摸那段每帧更新）
uint32_t coveredSince = 0;
const int PTT_PIN = 0;   // BOOT 键
bool focusActive = false;  // 专注中（番茄钟）
uint32_t focusStart = 0, focusMs = 0;
uint32_t nodAt = 0, perkAt = 0;
bool startled = false;              // 掉下去：瞪眼"啊！"
uint32_t startleAt = 0, landedAt = 0;
uint32_t dizzyUntil = 0, dizzyStart = 0;  // 被转晕：蚊香眼
Expr expr = CALM;
uint32_t exprStart = 0;
float exprDur = 0;            // 自动回到平静的时长（秒），0 = 一直保持
bool autoNight = false;       // 是自己睡的（点一下会醒）
uint32_t lastInteract = 0;    // 最后一次被点 / 被电脑指挥
uint32_t nextIdle = 0;        // 下一次自己做小动作的时间

// 时间：板子没有 RTC，联网后 NTP 对时；也可以由电脑串口发 "T<本地时间epoch>\n" 同步
bool timeKnown = false;
int64_t timeBase = 0;         // 本地 epoch - millis()/1000
int localHour() { return (int)(((timeBase + millis() / 1000) % 86400) / 3600); }
bool isNight() { int h = localHour(); return timeKnown && (h >= BEDTIME_HOUR || h < 7); }
uint32_t nextBlink = 0, blinkStart = 0, nextSaccade = 0;
bool blinking = false;
float blink = 0, lookX = 0, lookY = 0, lookTX = 0, lookTY = 0;

// 表情已演了多少秒；exprStart 可能比本帧的 now 晚几毫秒，按有符号处理避免溢出
float exprT(uint32_t now) {
  int32_t d = (int32_t)(now - exprStart);
  return d < 0 ? 0 : d / 1000.0f;
}

void setExpr(Expr e, float dur = -1) {
  expr = e;
  exprStart = millis();
  if (dur < 0) dur = (e == WINK || e == CURIOUS || e == MEH) ? 4 : (e == SMILE ? 8 : 0);
  exprDur = dur;
  if (e != SLEEP) autoNight = false;
  if (e == CALM) nextIdle = millis() + random(25000, 70000);
  logf("expr %d %s\n", e, EXPR_NAME[e]);
}

Pose poseFor(Expr e, float t) {
  Pose p = basePose();
  switch (e) {
    case CALM: break;
    case WINK:  // 眨一只眼 + 笑一下："嗯，知道了"
      p.wink = (t > 0.1f && t < 0.9f) ? 1 : 0;
      p.smile = 1.2f;
      break;
    case CURIOUS:  // 一边眉毛挑起来，那只眼睁大："嗯？"
      p.topR = -16; p.botR = 13;
      p.topL = -8; p.botL = 10;
      p.br = { 16, -27, 36, -24, 1 };
      p.smile = 0.2f;
      break;
    case MEH:  // 眼皮耷拉、眉毛压平：嫌弃
      p.topL = p.topR = 2; p.botL = p.botR = 11;
      p.bl = { -35, -6, -17, -7, 1 }; p.br = { 17, -7, 35, -6, 1 };
      p.smile = -0.3f;
      break;
    case MUSIC:  // 眯眼享受，音符往上飘
      p.lid = 1; p.arc = -6;
      p.smile = 1.3f;
      break;
    case SLEEP:  // 慢慢闭眼、屏幕变暗
      p.lid = min(1.0f, t / 1.2f); p.arc = 4;
      p.smile = 0.2f;
      p.bright = t < 2.0f ? 200 : max(25.0f, 200 - (t - 2.0f) * 60);
      break;
    case SMILE:  // 笑：笑眼 ◠、脸红
      p.happy = 1;
      p.smile = 2;
      p.blush = 1;
      break;
    default: break;
  }
  return p;
}

// ---------- drawing ----------
// 眼睛：椭圆，从 top 画到 bot
void drawEye(float cx, float top, float bot) {
  float h = bot - top;
  if (h < 0.3f) return;
  int y0 = (int)PY(top), y1 = (int)PY(bot);
  for (int y = y0; y <= y1; y++) {
    float f = constrain((y - PY(top)) / (h * S), 0.0f, 1.0f);
    float e = 2 * f - 1;
    int w = (int)(EYE_HW * sqrtf(max(0.0f, 1 - e * e)) * S);
    if (w > 0) gfx->drawFastHLine((int)PX(cx) - w, y, 2 * w + 1, WHITE);
  }
}

void drawEyeSide(float cx, float top, float bot, float lid, float arc, float happy = 0) {
  if (lid > 0.85f) {  // 闭上：一道弧
    float y0 = (top + bot) / 2 + 2;
    quad(cx - EYE_HW, y0, cx, y0 + arc, cx + EYE_HW, y0, 3.2f, WHITE);
    return;
  }
  drawEye(cx, top + (bot - top) * lid, bot);  // 眼皮从上往下盖
  if (happy > 0.05f) {  // 笑眼：用一个黑色椭圆从下面往上"咬"掉一块
    float h = (bot - top) / 2, cy = (top + bot) / 2 + h * (1.6f - 0.8f * happy);
    gfx->fillEllipse((int)PX(cx), (int)PY(cy), (int)(EYE_HW * 1.5f * S), (int)(h * S), BLACK);
  }
}

void drawBrow(const Brow &b) {
  if (b.a < 0.05f) return;
  float mu = (b.u1 + b.u2) / 2, mv = (b.v1 + b.v2) / 2;
  float k = b.a;
  line(mu + (b.u1 - mu) * k, mv + (b.v1 - mv) * k, mu + (b.u2 - mu) * k, mv + (b.v2 - mv) * k, 2.5f, WHITE);
}

void drawMouth(float smile) {
  float w = 5 + max(0.0f, smile) * 1.5f;  // 笑得越开嘴越宽
  quad(-w, 25, 0, 25 + smile * 3, w, 25, 2.2f, WHITE);
}

void drawBlush(float k) {
  if (k < 0.05f) return;
  uint16_t c = rgb((uint8_t)(0xF2 * k * 0.8f), (uint8_t)(0x8C * k * 0.8f), (uint8_t)(0x9A * k * 0.8f));
  for (int sx = -1; sx <= 1; sx += 2)
    gfx->fillEllipse((int)PX(sx * 39), (int)PY(19), (int)(6 * S), (int)(3 * S), c);
}

void drawNote(float ph) {  // 听歌：白色八分音符往上飘
  float du = sinf(ph * 6.28f) * 2 - 8, dv = -ph * 8 + 4;
  gfx->fillEllipse((int)PX(44 + du), (int)PY(-28 + dv), (int)(4.5f * S), (int)(3.5f * S), WHITE);
  gfx->fillRect((int)PX(47 + du), (int)PY(-44 + dv), max(2, (int)(1.8f * S)), (int)(16 * S), WHITE);
  quad(48.8f + du, -44 + dv, 55 + du, -41 + dv, 53 + du, -35 + dv, 1.6f, WHITE);
}

void drawZ(float u, float v, float w, float h) {
  line(u, v, u + w, v, 1.8f, WHITE);
  line(u + w, v, u, v + h, 1.8f, WHITE);
  line(u, v + h, u + w, v + h, 1.8f, WHITE);
}

// ---------- subtitle ----------
// 一页两行，脸上移 12 个单位后字幕放在 y=330 / 368（屏幕绝对坐标），宽度按圆屏弦长收窄
// 字体 u8g2_font_unifont_h_chinese（Unifont 16px，22145 字）由 GFX 库在装了 U8g2 时自带
const int SUB_TOP[2] = { 330, 368 };
const int SUB_W[2] = { 360, 320 };
const float FACE_UP = -12;

String subLines[24];
int subLineW[24];
int subLineC[24];       // 每行字数（按字符，不按字节）
String subText = "";    // 最近一句，点圆点重播用
bool subUnread = false; // 说完了但可能没看到：脸下面留一个金色圆点
int subLineN = 0, subPage = 0;
bool subActive = false;
uint32_t subPageStart = 0;
float subAlpha = 0;

int glyphW(uint32_t cp) { return cp >= 0x2E80 ? 32 : 16; }  // unifont: CJK 全角 16px，其它半角 8px，×2

int pageChars(int page) {
  int n = 0;
  for (int i = page * 2; i < min(subLineN, page * 2 + 2); i++) n += subLineC[i];
  return n;
}
bool subHold = false;  // 挂着的字幕（定时问候）：最后一页挂到点一下或 10 分钟
const float HOLD_SECONDS = 600;
float pageDur(int page) {
  if (subHold && (page + 1) * 2 >= subLineN) return HOLD_SECONDS;
  return max(4.0f, 1.5f + pageChars(page) * 0.3f);
}

void sayText(const char *txt, bool hold = false) {
  subHold = hold;
  subText = txt;
  subUnread = false;
  subLineN = 0;
  String cur = "";
  int w = 0, c = 0;
  const uint8_t *p = (const uint8_t *)txt;
  while (*p && subLineN < 24) {
    // 解一个 UTF-8 字符
    uint32_t cp = *p;
    int n = cp < 0x80 ? 1 : cp < 0xE0 ? 2 : cp < 0xF0 ? 3 : 4;
    if (n == 2) cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    if (n == 3) cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    if (n == 4) cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    int limit = SUB_W[subLineN % 2];
    if (cp == '\n' || w + glyphW(cp) > limit) {
      subLineW[subLineN] = w;
      subLineC[subLineN] = c;
      subLines[subLineN++] = cur;
      cur = "";
      w = 0;
      c = 0;
      if (cp == '\n') { p += n; continue; }
      if (cp == ' ') { p += n; continue; }  // 行首不留空格
    }
    for (int i = 0; i < n; i++) cur += (char)p[i];
    w += glyphW(cp);
    c++;
    p += n;
  }
  if (cur.length() && subLineN < 24) { subLineW[subLineN] = w; subLineC[subLineN] = c; subLines[subLineN++] = cur; }
  subPage = 0;
  subPageStart = millis();
  subActive = subLineN > 0;
  logf("say %d lines\n", subLineN);
}

void stepSubtitle(uint32_t now) {
  if (subActive) {
    float t = (now - subPageStart) / 1000.0f;
    if (t > pageDur(subPage)) {
      if ((subPage + 1) * 2 < subLineN) { subPage++; subPageStart = now; subAlpha = 0.25f; }
      else { subActive = false; subUnread = true; }
    }
  }
  subAlpha += ((subActive ? 1.0f : 0.0f) - subAlpha) * 0.15f;
  faceDY += ((subActive || subAlpha > 0.3f ? FACE_UP : 0) - faceDY) * 0.15f;
}

void noteActivity(uint32_t now);  // 定义在 low power 一节
// ---------- 阅读视图 ----------
// 多于两行的消息：点一下 → 脸让开，整条铺开（一屏约 7 行），上下划滚动，左右划翻历史，点一下关，1 分钟不动自己关
bool readerOn = false;
String readerLines[60];
int readerN = 0, readerTop = 0;
uint32_t readerAt = 0;
const int READER_W = 330, READER_LH = 36, READER_Y0 = 112, READER_ROWS = 7;

void openReader(uint32_t now, const String &txt) {
  readerN = 0;
  String cur = "";
  int w = 0;
  const uint8_t *p = (const uint8_t *)txt.c_str();
  while (*p && readerN < 60) {
    uint32_t cp = *p;
    int n = cp < 0x80 ? 1 : cp < 0xE0 ? 2 : cp < 0xF0 ? 3 : 4;
    if (n == 3) cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    int gw = cp >= 0x2E80 ? 32 : 16;
    if (cp == '\n' || w + gw > READER_W) {
      readerLines[readerN++] = cur;
      cur = ""; w = 0;
      if (cp == '\n' || cp == ' ') { p += n; continue; }
    }
    for (int i = 0; i < n; i++) cur += (char)p[i];
    w += gw;
    p += n;
  }
  if (cur.length() && readerN < 60) readerLines[readerN++] = cur;
  readerTop = 0;
  readerOn = true;
  readerAt = now;
  subActive = false;
  subUnread = false;
  noteActivity(now);
}

void readerScroll(uint32_t now, int d) {
  readerAt = now;
  readerTop = constrain(readerTop + d, 0, max(0, readerN - READER_ROWS));
}

void drawReader(uint32_t now) {
  gfx->setFont(u8g2_font_unifont_h_chinese);
  gfx->setUTF8Print(true);
  gfx->setTextSize(2);
  gfx->setTextColor(rgb(235, 235, 226));
  for (int k = 0; k < READER_ROWS && readerTop + k < readerN; k++) {
    gfx->setCursor(233 - READER_W / 2 - OX, READER_Y0 + k * READER_LH + 28 - OY);
    gfx->print(readerLines[readerTop + k]);
  }
  if (readerN > READER_ROWS) {  // 右边一条小滚动条
    int h = 230, y = 118 + h * readerTop / readerN, hh = max(20, h * READER_ROWS / readerN);
    gfx->fillRoundRect(424 - OX, 118 - OY, 4, h, 2, rgb(50, 50, 50));
    gfx->fillRoundRect(424 - OX, y - OY, 4, hh, 2, ACCENT);
  }
}

// 点屏幕时先问字幕：长消息 → 打开阅读视图；正在说 → 翻下一页 / 结束；有未读圆点 → 重播。返回 true 表示这次点击被字幕用掉了
bool subtitleTap(uint32_t now) {
  if ((subActive || subUnread) && subLineN > 2) { String t = subText; openReader(now, t); return true; }  // 长消息：点开看全文
  if (subActive) {
    if ((subPage + 1) * 2 < subLineN) { subPage++; subPageStart = now; subAlpha = 0.25f; }
    else subActive = false;  // 看过了，不留圆点
    return true;
  }
  if (subUnread) {
    String again = subText;  // sayText 会改写 subText，先复制一份，别自己覆盖自己
    sayText(again.c_str());
    return true;
  }
  return false;
}

void drawPttDot(uint32_t now) {  // 按键录音中：下方一个实心金点，轻轻脉动
  if (!pttActive) return;
  int r = 7 + (int)(1.5f * sinf(now / 180.0f));
  gfx->fillCircle(233 - OX, 388 - OY, r, ACCENT);
}

void drawUnreadDot(uint32_t now) {
  if (!subUnread || subAlpha > 0.1f) return;
  float k = 0.55f + 0.45f * sinf(now / 600.0f);  // 慢慢呼吸
  gfx->fillCircle(233 - OX, 388 - OY, 6, accentDim(k));
}

void drawSubtitle() {
  if (subAlpha < 0.03f) return;
  uint8_t g = (uint8_t)(230 * subAlpha);
  gfx->setFont(u8g2_font_unifont_h_chinese);
  gfx->setUTF8Print(true);
  gfx->setTextSize(2);
  gfx->setTextColor(rgb(g, g, (uint8_t)(g * 0.96f)));
  for (int k = 0; k < 2; k++) {
    int i = subPage * 2 + k;
    if (i >= subLineN) break;
    gfx->setCursor(233 - subLineW[i] / 2 - OX, SUB_TOP[k] + 28 - OY);  // u8g2 字体以基线定位
    gfx->print(subLines[i]);
  }
  if (subLineN > 2 && subActive) {  // 还有下一屏：提示可以点开看全文
    gfx->setTextSize(1);
    gfx->setTextColor(rgb(g / 2, g / 2, g / 2));
    gfx->setCursor(233 - 40 - OX, 418 - OY);
    gfx->print("点开看全文");
  }
}

void drawBattery();  // 定义在 battery 一节
void drawFocusRing(uint32_t now);  // 定义在 focus 一节
void drawSpiral(float cx, float cy, float rot);  // 定义在 motion 一节
void updateNod(uint32_t now);      // 定义在 listening 一节
extern uint32_t chargeAt;          // 定义在 battery 一节
void playChime(float f1, float f2, float len, float gain);  // 定义在 timers 一节
int minutesToAlarm(int within, String &when);  // 定义在 alarm 一节：多久后响（分钟），-1 = within 分钟内没有
void startDraw(uint32_t now);      // 摇一摇抽签
void swipeHist(uint32_t now, bool older);
void loadHist();
void addHist(const char *txt);
extern uint32_t lastTouchAt, msgArrivedAt, msgRemindAt;
extern int msgRemindN;
void drawBuddy(uint32_t now);      // Claude 桌面端
extern bool buddyAsking;
void buddyDecide(uint32_t now, bool approve);
extern uint32_t touchDownAtG;
extern bool alarmRinging;  // 定义在 alarm 一节
void stopRinging(uint32_t now, bool snooze);
void loadAlarms();
void noteActivity(uint32_t now);  // 定义在 low power 一节
uint32_t tDraw = 0, tFlush = 0;

void render(uint32_t now) {
  uint32_t r0 = micros();
  float t = exprT(now);
  gfx->fillScreen(BLACK);

  float sway = (expr == MUSIC) ? sinf(t * 2.4f) * 2.5f : 0;
  float lid = max(cur.lid, blink);
  float arc = (blink > cur.lid) ? 2 : cur.arc;

  if (readerOn) drawReader(now);  // 阅读视图：脸让开
  else {
    if (dizzyUntil) {  // 晕：蚊香眼，左右晃
      float wob = sinf(now / 180.0f) * 2.0f;
      float rot = (now - dizzyStart) / 1000.0f * 9.0f;
      drawSpiral(-EYE_X + wob, 0, rot);
      drawSpiral(EYE_X + wob, 0, rot);
    } else {
      drawEyeSide(-EYE_X + lookX + sway, cur.topL + lookY, cur.botL + lookY, lid, arc, cur.happy);
      if (cur.wink > 0.5f && lid < 0.85f) drawEyeSide(EYE_X + lookX + sway, cur.topR + lookY, cur.botR + lookY, 1, -5);
      else drawEyeSide(EYE_X + lookX + sway, cur.topR + lookY, cur.botR + lookY, lid, arc, cur.happy);
    }
    drawBlush(cur.blush);  // 画在眼睛后面：笑眼"咬"的那一块不会把脸红擦掉
    drawBrow(cur.bl);
    drawBrow(cur.br);
    if (startled) {  // 掉下去："啊！"嘴张成小圆
      for (int i = 0; i < 28; i++) { float a = i * 6.283f / 28; dot(3.2f * cosf(a), 27 + 3.2f * sinf(a), 2.0f, WHITE); }
    } else if (dizzyUntil) {  // 晕：嘴是一道波浪
      for (int i = 0; i <= 30; i++) { float u = -7 + 14.0f * i / 30; dot(u, 26 + sinf(u * 1.3f + now / 150.0f) * 1.2f, 2.0f, WHITE); }
    } else {
      drawMouth(cur.smile);
    }

    if (expr == MUSIC) drawNote(fmodf(t / 2.2f, 1.0f));
    if (expr == SLEEP && t > 1.2f) {
      if (fmodf(t, 2.4f) > 0.3f) drawZ(26, -27, 7, 7);
      if (fmodf(t, 2.4f) > 1.0f) drawZ(37, -37, 5, 5);
    }
  }
  drawBuddy(now);
  drawSubtitle();
  drawUnreadDot(now);
  drawPttDot(now);
  drawFocusRing(now);
  drawBattery();
  uint32_t r1 = micros();
  gfx->flush();
  tDraw += r1 - r0; tFlush += micros() - r1;
}

// ---------- behaviour ----------
float ap(float c, float t, float k) { return c + (t - c) * k; }
void apBrow(Brow &c, const Brow &t, float k) {
  c.u1 = ap(c.u1, t.u1, k); c.v1 = ap(c.v1, t.v1, k);
  c.u2 = ap(c.u2, t.u2, k); c.v2 = ap(c.v2, t.v2, k);
  c.a = ap(c.a, t.a, k);
}

void step(uint32_t now) {
  float t = exprT(now);
  if (exprDur > 0 && t > exprDur) setExpr(CALM);

  // 自己的小动作：平静久了偶尔眨只眼、挑个眉
  if (listening || petting) nextIdle = max(nextIdle, now + 8000);  // 听你说话 / 被摸时不自己做小动作
  if (focusActive) { nextIdle = max(nextIdle, now + 10000); nextSaccade = max(nextSaccade, now + 8000); }  // 专注中安静
  if (expr == CALM && now > nextIdle) {
    int r = random(100);
    if (r < 50) setExpr(WINK, 2.5f);
    else if (r < 75) setExpr(CURIOUS, 2.5f);
    else if (r < 90) setExpr(MEH, 2.5f);
    else setExpr(SMILE, 3.5f);
    t = 0;
  }
  // 夜里（BEDTIME_HOUR 到 7 点）一分钟没人理就自己睡；7 点醒来眨一下眼
  if (expr == CALM && isNight() && now - lastInteract > 60000) {
    setExpr(SLEEP);
    autoNight = true;
    t = 0;
  }
  if (expr == SLEEP && autoNight && timeKnown && !isNight() && !faceDownSleep) {
    setExpr(WINK, 2.5f);
    t = 0;
  }
  tgt = poseFor(expr, exprT(now));
  // 对话桥在想：抬眼看向斜上方，眼睛微眯（最多 25 秒，免得卡在这个样子）
  bool thinking = netThinkingSince && now - netThinkingSince < 25000;
  if (thinking && !petting) {
    tgt.topL += 1; tgt.topR += 1;
    lookTX = -3.0f; lookTY = -1.5f;
    nextSaccade = max(nextSaccade, now + 2000);
    nextIdle = max(nextIdle, now + 8000);
  }
  if ((listening || pttActive) && !petting) {  // 听你说话：眼睛睁大，直直看着你
    tgt.topL -= 4.5f; tgt.topR -= 4.5f;
    tgt.botL += 1; tgt.botR += 1;
  }
  // 待机：放着 2 分钟没人碰没人动 → 犯困，眼皮半垂，跟着呼吸慢慢起伏
  bool standby = expr == CALM && !listening && !petting && !subActive && mot.ok &&
                 now - mot.stillSince > 120000 && now - lastInteract > 120000;
  if (standby) {
    tgt.topL = max(tgt.topL, tgt.botL - 7.0f); tgt.topR = max(tgt.topR, tgt.botR - 7.0f);  // 上沿压低 = 眼皮半垂
    float br = sinf(now / 2600.0f * 6.283f) * 0.9f;
    tgt.topL += br; tgt.botL += br; tgt.topR += br; tgt.botR += br;
    nextSaccade = max(nextSaccade, now + 6000);
  }
  if (covered) {  // 被捂住：闭眼
    tgt.lid = 1;
    tgt.arc = 3;
  }
  if (focusActive) tgt.bright = min(tgt.bright, 110.0f);  // 专注中屏幕调暗
  if (chargeAt && now - chargeAt < 2500) {  // 刚插上电：满足地眯一下眼
    tgt.lid = 1;
    tgt.arc = -6;
  }
  if (petting) {  // 被摸的时候舒服地眯眼（^ ^）、脸红
    tgt.lid = 1;
    tgt.arc = -6;
    tgt.smile = max(tgt.smile, 1.2f);
    tgt.blush = 1;
    tgt.bright = max(tgt.bright, 180.0f);
  }

  if (startled) {  // 掉下去：眼睛瞪圆
    tgt.topL = tgt.topR = -17; tgt.botL = tgt.botR = 15;
    tgt.lid = 0; tgt.wink = 0; tgt.happy = 0; tgt.blush = 0;
    tgt.bl.a = tgt.br.a = 0;
  }

  // 眼睛、眉毛用弹簧跟过去：会微微冲过头再落回（阻尼比约 0.65），比直接滑过去更有弹性；吓一跳时弹簧更硬
  static uint32_t lastStepAt = 0;
  float dt = lastStepAt ? constrain((now - lastStepAt) / 1000.0f, 0.005f, 0.06f) : 0.033f;
  lastStepAt = now;
  float SK = startled ? 700 : 170, SD = startled ? 40 : 17;
  auto spr = [&](float &c, float &v, float t) { v += ((t - c) * SK - v * SD) * dt; c += v * dt; };
  static float sv[12];
  spr(cur.topL, sv[0], tgt.topL); spr(cur.botL, sv[1], tgt.botL);
  spr(cur.topR, sv[2], tgt.topR); spr(cur.botR, sv[3], tgt.botR);
  spr(cur.bl.u1, sv[4], tgt.bl.u1); spr(cur.bl.v1, sv[5], tgt.bl.v1); spr(cur.bl.u2, sv[6], tgt.bl.u2); spr(cur.bl.v2, sv[7], tgt.bl.v2);
  spr(cur.br.u1, sv[8], tgt.br.u1); spr(cur.br.v1, sv[9], tgt.br.v1); spr(cur.br.u2, sv[10], tgt.br.u2); spr(cur.br.v2, sv[11], tgt.br.v2);
  float k = startled ? 0.6f : 0.25f;
  cur.wink = tgt.wink;
  cur.happy = ap(cur.happy, tgt.happy, k);
  cur.smile = ap(cur.smile, tgt.smile, startled ? 0.6f : 0.2f);
  cur.blush = ap(cur.blush, tgt.blush, 0.1f);
  cur.lid = ap(cur.lid, tgt.lid, expr == SLEEP ? 0.5f : 0.3f);
  cur.arc = ap(cur.arc, tgt.arc, 0.4f);
  cur.bl.a = ap(cur.bl.a, tgt.bl.a, k); cur.br.a = ap(cur.br.a, tgt.br.a, k);
  cur.bright = ap(cur.bright, tgt.bright, 0.08f);

  // 平静时眼睛偶尔看看别处
  if (listening || pttActive) { lookTX = 0; lookTY = -1.2f; nextSaccade = now + 2500; }  // 直直看着你
  else if (expr == CALM && now > nextSaccade) {
    lookTX = random(-20, 21) / 10.0f;
    lookTY = random(-8, 9) / 10.0f;
    nextSaccade = now + random(1500, 4000);
  }
  if (expr != CALM && !listening) lookTX = lookTY = 0;
  // 拿在手里立着：眼睛往低的那边滑（y 轴是屏幕左右，实测向左歪 y≈-1）
  if (mot.ok && !mot.flat && mot.gx > 0.4f && !listening && !petting) {
    lookTX = constrain(mot.gy * 7.0f, -5.0f, 5.0f);
    nextSaccade = max(nextSaccade, now + 1500);
  }
  lookX = ap(lookX, lookTX, 0.2f);
  lookY = ap(lookY, lookTY, 0.2f);

  // blink (not while eyes are already closed)
  bool canBlink = expr != MUSIC && expr != SLEEP && !petting && !covered && !startled && !dizzyUntil;
  if (!blinking && canBlink && now > nextBlink) { blinking = true; blinkStart = now; }
  if (blinking) {  // 快闭（70ms，越闭越快）→ 停一下 → 慢睁（140ms，最后慢慢睁开）；15% 连眨两下
    static bool doubleDone = false;
    uint32_t e = now - blinkStart;
    if (e < 70) { float u = e / 70.0f; blink = u * u; }
    else if (e < 110) blink = 1;
    else if (e < 250) { float u = (e - 110) / 140.0f; blink = (1 - u) * (1 - u); }
    else {
      blinking = false; blink = 0;
      if (!doubleDone && random(100) < 15) { doubleDone = true; nextBlink = now + 110; }
      else { doubleDone = false; nextBlink = now + random(2500, 6000); }
    }
  }
}

// ---------- battery ----------
// 每 10 秒读一次；没插电时 20% / 10% 各用字幕提醒一次；低于 15% 在下方露一个小电池
PowerState bat;

// 插电反应：像吃到东西，眯眼"嗯↗"；电快没了时插上 → 长叹一口气"……活过来了。"；夜里有闹钟 → 顺便确认明早几点叫
uint32_t chargeAt = 0;
void chargeReact(uint32_t now) {
  chargeAt = now;
  noteActivity(now);
  lastInteract = now;
  if (expr == SLEEP && autoNight) setExpr(CALM);
  String when;
  int h = localHour();
  if (bat.percent >= 0 && bat.percent <= 20) {
    playChime(660, 440, 0.9f, 0.25f);
    sayText("……活过来了。");
  } else if (timeKnown && (h >= 21 || h < 6) && minutesToAlarm(12 * 60, when) >= 0) {
    playChime(523, 784, 0.5f, 0.25f);
    sayText(("插上了。明早 " + when + " 叫你。").c_str());
  } else {
    playChime(523, 784, 0.5f, 0.25f);
    const char *l[] = { "嗯——。", "吃饭了。", "充上了。" };
    if (random(100) < 60) sayText(l[random(3)]);
  }
  logf("plugged in (%d%%)\n", bat.percent);
}

void stepBattery(uint32_t now) {
  static uint32_t lastRead = 0;
  static bool warned20 = false, warned10 = false;
  extern bool lowPower;
  if (lastRead && now - lastRead < (lowPower ? 10000UL : 2000UL)) return;  // 醒着 2 秒读一次：插电马上有反应
  lastRead = now;
  bat = powerRead();
  netBatPercent = bat.percent;
  netBatCharging = bat.charging;
  netBatUsb = bat.usb;
  if (!bat.battery || bat.percent < 0) return;
  // 夜里有闹钟要响却没插电：拔线时马上提醒；电低于 30% 再急一点（电池撑不了一整夜，半夜没电关机早上就叫不了）
  static bool lastUsb = true, warnedNight = false, firstRead = true;
  if (firstRead) { firstRead = false; lastUsb = bat.usb; }  // 开机第一次读：不算插拔
  bool justUnplugged = lastUsb && !bat.usb;
  bool justPlugged = !lastUsb && bat.usb;
  lastUsb = bat.usb;
  if (justPlugged) chargeReact(now);
  if (bat.usb) { warned20 = warned10 = warnedNight = false; return; }
  int h = localHour();
  bool evening = timeKnown && (h >= 21 || h < 6);
  String when;
  if (evening && minutesToAlarm(12 * 60, when) >= 0) {
    if (justUnplugged) {
      setExpr(CURIOUS, 4);
      sayText(("拔电了？明早 " + when + " 还要叫你，插着吧。").c_str(), true);
      playChime(1318, 1760, 0.5f, 0.3f);
      logf("unplugged with alarm %s\n", when.c_str());
    } else if (bat.percent <= 30 && !warnedNight) {
      warnedNight = true;
      setExpr(CURIOUS, 5);
      sayText(("电只剩 " + String(bat.percent) + "%，撑不到明早 " + when + " 叫你。快插电。").c_str(), true);
      playChime(988, 740, 0.8f, 0.4f);
      logf("low battery before alarm\n");
    }
  }
  if (bat.percent <= 10 && !warned10) {
    warned10 = warned20 = true;
    setExpr(CURIOUS, 4);
    sayText("只剩10%了。再不充电，我就要睡着了。");
  } else if (bat.percent <= 20 && !warned20) {
    warned20 = true;
    sayText("电量还剩20%，找个时间给我充一下。");
  }
}

void drawBattery() {
  if (!bat.battery || bat.usb || bat.percent < 0 || bat.percent >= 15) return;
  int x = 233 - 12 - OX, y = 398 - OY;
  uint16_t c = accentDim(0.6f);
  gfx->drawRoundRect(x, y, 22, 11, 2, c);
  gfx->fillRect(x + 22, y + 3, 2, 5, c);
  gfx->fillRect(x + 2, y + 2, max(1, 18 * bat.percent / 100), 7, c);
}

// ---------- bedtime ----------
// BEDTIME_HOUR 整点提醒一次；之后到凌晨 5 点你还在摸它 / 跟它说话，隔半小时再催一次，一晚最多催三次
const char *BED_FIRST[] = {
  "该睡了。剩下的事明天再说。",
  "到点了。手机放下，去洗漱。",
  "今天辛苦了，去睡吧。",
};
const char *BED_AGAIN[] = {
  "还不睡？我都看见了。",
  "已经很晚了。再玩下去，明天起不来。",
  "明天还有事呢，睡觉。",
  "我先闭眼了，你也是。",
};

void stepBedtime(uint32_t now) {
  static int lastNightDay = -1;  // 记录提醒过的是哪一晚
  static int nags = 0;
  static uint32_t lastNag = 0;
  if (!timeKnown) return;
  int64_t local = timeBase + millis() / 1000;
  int h = (int)((local % 86400) / 3600), m = (int)((local % 3600) / 60);
  int night = (int)((local - 5 * 3600) / 86400);  // 凌晨 5 点前算前一晚
  bool late = h >= BEDTIME_HOUR || h < 5;
  if (!late) return;
  if (night != lastNightDay) {  // 今晚第一次：整点提醒
    lastNightDay = night;
    nags = 1;
    lastNag = now;
    if (expr == SLEEP && autoNight) setExpr(CALM);
    setExpr(CURIOUS, 4);
    String when;
    if (bat.battery && !bat.usb && minutesToAlarm(12 * 60, when) >= 0)  // 有闹钟却没插电：提醒插电（不然半夜没电，早上叫不了）
      sayText(("该睡了。明早 " + when + " 叫你，先给我插上电。").c_str(), true);
    else
      sayText(BED_FIRST[random(3)]);
    logf("bedtime reminder\n");
    return;
  }
  bool active = now - lastInteract < 5000;  // 刚刚还在摸它 / 跟它说话
  bool past2330 = h < 5 || h > BEDTIME_HOUR || m >= 30;  // 第一次提醒半小时以后
  if (active && past2330 && nags < 3 && now - lastNag > 30UL * 60 * 1000) {
    nags++;
    lastNag = now;
    setExpr(CURIOUS, 4);
    sayText(BED_AGAIN[random(4)]);
    logf("bedtime nag\n");
  }
}

// ---------- motion ----------
// 拿起 → 醒过来"嗯？"；用力摇 → 嫌弃地瞥一眼，然后抽一支签；屏幕扣在桌上 3 秒 → 睡觉变暗，翻过来就醒
// 掉下去 → 瞪眼"啊！"，接住 / 落地后嘟囔一句；转一圈半以上 → 蚊香眼晕 3 秒多，缓过来眨眨眼
void drawSpiral(float cx, float cy, float rot) {
  for (int i = 0; i <= 70; i++) {
    float f = i / 70.0f, th = f * 11.0f + rot, r = 0.8f + 8.0f * f;
    dot(cx + r * cosf(th), cy + r * sinf(th) * 0.95f, 1.8f, WHITE);
  }
}

void playStartle() {  // 很短的"！"：音调一下蹿上去
  const int N = 16000 * 16 / 100;
  int16_t *pcm = (int16_t *)ps_malloc(N * 2);
  if (!pcm) return;
  float ph = 0;
  for (int i = 0; i < N; i++) {
    float t = i / 16000.0f;
    ph += 2 * PI * (700.0f + 900.0f * min(1.0f, t / 0.09f)) / 16000.0f;
    float env = min(1.0f, t * 300.0f) * min(1.0f, (0.16f - t) * 40.0f);
    pcm[i] = (int16_t)(sinf(ph) * env * 5000);
  }
  audioPlay(pcm, N * 2);
}

void stepMotion(uint32_t now) {
  if (!mot.ok) return;
  if (mot.faceDown && !faceDownSleep) {
    faceDownSleep = true;
    setExpr(SLEEP);
    autoNight = true;
    logf("face down: sleep\n");
    return;
  }
  if (!mot.faceDown && faceDownSleep && mot.gz > -0.5f) {
    faceDownSleep = false;
    perkAt = now;
    lastInteract = now;
    setExpr(CALM);
    logf("face up: wake\n");
  }
  if (mot.dropped) {
    mot.dropped = false;
    startled = true;
    startleAt = now;
    landedAt = 0;
    lastInteract = now;
    perkAt = now;
    if (expr == SLEEP && autoNight) setExpr(CALM);
    playStartle();
  }
  if (mot.landed) {
    mot.landed = false;
    if (startled && !landedAt) landedAt = now;
  }
  if (startled && ((landedAt && now - landedAt > 700) || now - startleAt > 3000)) {
    startled = false;
    const char *lines[] = { "……吓我一跳。", "接住了？", "下次说一声。" };
    setExpr(MEH, 3);
    sayText(lines[random(3)]);
  }
  if (mot.dizzy) {
    mot.dizzy = false;
    dizzyStart = now;
    dizzyUntil = max((uint32_t)1, now + 3500);
    lastInteract = now;
    if (expr == SLEEP && autoNight) setExpr(CALM);
  }
  if (dizzyUntil && (int32_t)(now - dizzyUntil) >= 0) {
    if (mot.spin > 180) dizzyUntil = now + 1500;  // 还在转：接着晕
    else {
      dizzyUntil = 0;
      nextBlink = now;
      setExpr(WINK, 2.5f);
      if (random(100) < 60) { const char *l[] = { "……别转了。", "天在转。", "我没事。" }; sayText(l[random(3)]); }
      logf("dizzy over\n");
    }
  }
  // 掉落 / 转晕前后，拿起和摇晃都不算（接住那一下、转的时候受力都很大）
  bool motionBusy = startled || dizzyUntil || (landedAt && now - landedAt < 2000);
  if (motionBusy) { mot.shaken = false; mot.pickedUp = false; }
  if (mot.shaken) {
    mot.shaken = false;
    lastInteract = now;
    setExpr(MEH, 1.2f);  // 先嫌弃地瞥一眼，再抽签
    startDraw(now);
    logf("shaken\n");
  }
  if (mot.pickedUp && alarmRinging) {  // 拿起来 = 起来了
    mot.pickedUp = false;
    stopRinging(now, false);
  }
  if (mot.pickedUp) {
    mot.pickedUp = false;
    lastInteract = now;
    perkAt = now;
    if (expr == SLEEP && autoNight) setExpr(CALM);
    if (expr == CALM) {
      int r = random(100);
      if (r < 60) setExpr(CURIOUS, 2.5f);
      else if (r < 90) setExpr(WINK, 2.5f);
      else setExpr(SMILE, 3);
    }
    lastTouchAt = now;  // 拿起来看了：算看过消息
    logf("picked up\n");
  }
}

// ---------- listening ----------
// 你开始说话 → 抬眼看你；说话中间停顿后接着说 → 点一下头；说完（说了 1.5 秒以上）→ 眨只眼"嗯，听到了"
void stepListen(uint32_t now) {
  static bool wasVoice = false;
  static uint32_t listenStart = 0, voiceLastOn = 0, pauseAt = 0;
  static uint32_t sessionSpeech = 0;  // 一次对话里累计说了多久（几段话算一次）
  static bool inSession = false;
  bool v = micVoice;
  bool allowed = !(expr == SLEEP && autoNight) && expr != MUSIC && netEarOn && !focusActive;  // 长按关耳朵：录音和"听见就抬眼"一起关
  if (!allowed) {
    listening = false;
    wasVoice = v;
    updateNod(now);
    return;
  }
  if (v && !listening) {
    listening = true;
    listenStart = now;
    if (!inSession) perkAt = now;  // 刚开口：整张脸轻轻一提
    inSession = true;
    logf("listen start (level %d, floor %d)\n", (int)micLevel, (int)micFloor);
  }
  if (v) voiceLastOn = now;
  if (listening) {
    lastInteract = now;
    if (!v && wasVoice) pauseAt = now;
    if (v && !wasVoice && now - pauseAt > 150 && now - pauseAt < 1200 && now - nodAt > 1500) {
      nodAt = now;
      logf("nod\n");
    }
    if (!v && now - voiceLastOn > 1200) {  // 一段话结束，但对话可能还没完
      listening = false;
      sessionSpeech += voiceLastOn - listenStart;
    }
  }
  // 安静超过 2.2 秒才算这次对话说完：说得够多（>1.5 秒）就眨只眼"嗯，听到了"，一次对话只眨一次
  if (inSession && !listening && now - voiceLastOn > 2200) {
    logf("heard %lums\n", sessionSpeech);
    if (sessionSpeech > 1500 && expr == CALM) setExpr(WINK, 2.5f);
    inSession = false;
    sessionSpeech = 0;
  }
  wasVoice = v;
  updateNod(now);
}

// ---------- 跟着节拍点头 ----------
// 听歌的脸时，用麦克风听鼓点：音量突然比最近的平均高四成 = 一拍，点一下头；
// 记住拍子间隔，安静的几小节里按节奏接着点（最多 4 下）。戴耳机听不到声音时就只左右摇
uint32_t beatAt = 0;
void stepBeat(uint32_t now) {
  static float avg = 0, period = 0;
  static uint32_t lastBeat = 0;
  static int predicted = 0;
  if (expr != MUSIC) { avg = 0; period = 0; return; }
  float e = micLevel;
  if (avg <= 0) avg = e;
  bool onset = e > avg * 1.4f && e > 30 && now - lastBeat > 280;
  avg = avg * 0.94f + e * 0.06f;
  if (onset) {
    uint32_t iv = now - lastBeat;
    if (iv > 300 && iv < 1000) period = period ? period * 0.8f + iv * 0.2f : iv;
    lastBeat = beatAt = now;
    predicted = 0;
  } else if (period && now - lastBeat > period * 1.05f && predicted < 4) {
    lastBeat = beatAt = now;
    predicted++;
  }
}

void updateNod(uint32_t now) {
  float nt = (now - nodAt) / 380.0f;
  float pt = (now - perkAt) / 300.0f;
  float bt = (now - beatAt) / 220.0f;  // 跟着拍子：短一点、轻一点
  nodY = (nt < 1 ? 2.2f * sinf(nt * PI) : 0) - (pt < 1 ? 2.5f * sinf(pt * PI) : 0) + (bt < 1 ? 1.6f * sinf(bt * PI) : 0);
}

// ---------- touch ----------
// CST9217：有触摸时拉中断线，只在中断后去读（一直轮询会让芯片卡死，之前就是这样）
bool touchAttached = false;
volatile uint32_t coverAt = 0;  // 触摸芯片报"整个屏幕被手掌盖住"的最近时间

void initTouch() {
  if (touchAttached) detachInterrupt(TP_INT);
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW); delay(30);
  digitalWrite(TP_RST, HIGH); delay(80);
  touch.setPins(TP_RST, TP_INT);
  if (touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL)) {
    touch.setMaxCoordinates(466, 466);
    touch.setMirrorXY(true, true);
    attachInterrupt(TP_INT, [] { touchFlag = true; }, FALLING);
    touch.setCoverScreenCallback([](void *) { coverAt = millis(); }, NULL);  // 手掌盖住屏幕（在 getPoint 里触发）
    touchAttached = true;
    logf("touch ok\n");
  } else {
    logf("touch not found\n");
  }
}

// ---------- setup / loop ----------
const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // 拔掉 USB 后没人收日志：直接丢掉，别卡住主循环（之前拔线后点屏幕没反应就是这个）
  Wire.begin(IIC_SDA, IIC_SCL);

  if (!gfxSmall->begin(80000000)) logf("gfx begin failed\n");
  if (!gfxBig->begin(GFX_SKIP_OUTPUT_BEGIN)) logf("big canvas alloc failed\n");  // 只分配内存，不再初始化屏幕
  panel->fillScreen(BLACK);
  panel->setBrightness(200);

  initTouch();
  pinMode(PTT_PIN, INPUT_PULLUP);
  audioBegin();
  powerBegin();
  motionBegin();

  logf("i2c:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) logf(" 0x%02X", addr);
  }
  logf("\n");

  // 上次为什么重启（卡死被看门狗救起来 = task_wdt，崩溃 = panic）：报给云端存着
  netResetReason = resetReasonName(esp_reset_reason());
  logf("reset reason: %s\n", netResetReason);
  loadAlarms();
  loadHist();
  netBegin();
  ancsBegin();
  // 看门狗：主循环 5 秒没转就自动重启（曾经夜里卡死过一整夜）
  enableLoopWDT();
  randomSeed(esp_random());
  uint32_t now = millis();
  nextBlink = now + 2000;
  nextSaccade = now + 1500;
  setExpr(CALM);
}

uint32_t lastTap = 0;
uint32_t msgChimeAt = 0;  // 云端消息的提示音什么时候响（0 = 不响）
bool pendingTap = false;
uint32_t pendingTapAt = 0;
int lastBright = -1;

// ---------- focus（专注陪伴 / 番茄钟）----------
// 双击屏幕 / 对着它说"专注 N 分钟" / Claude 在聊天里开。外圈一圈点阵随时间减少；
// 专注中它很安静（不东张西望、不做小动作、不理周围的声音，屏幕调暗）；点一下显示还剩几分钟。
// 到点"叮"一声（板子自己生成的音，不花额度），挂一句提醒，并报给云端记今天完成了几次。
void startFocus(uint32_t now, int minutes) {
  useCanvas(true);
  focusActive = true;
  focusStart = now;
  focusMs = (uint32_t)constrain(minutes, 1, 180) * 60000UL;
  setExpr(WINK, 2.5f);
  sayText(("开始专注 " + String(minutes) + " 分钟。我在旁边。").c_str());
  logf("focus start %d min\n", minutes);
}

void stopFocus(uint32_t now, bool done) {
  if (!focusActive) return;
  focusActive = false;
  useCanvas(false);
  int minutes = (int)((now - focusStart) / 60000UL);
  if (done) {
    // 两声"叮"：880Hz + 1320Hz，带淡入淡出
    const int N = 16000 * 9 / 10;
    int16_t *pcm = (int16_t *)ps_malloc(N * 2);
    if (pcm) {
      for (int i = 0; i < N; i++) {
        float t = i / 16000.0f;
        float f = t < 0.4f ? 880.0f : 1320.0f;
        float lt = t < 0.4f ? t : t - 0.45f;
        float env = lt < 0 ? 0 : expf(-lt * 6.0f) * min(1.0f, lt * 200.0f);
        pcm[i] = (int16_t)(sinf(2 * PI * f * t) * env * 9000);
      }
      audioPlay(pcm, N * 2);
    }
    setExpr(WINK, 3);
    sayText((String(focusMs / 60000UL) + " 分钟到了，起来动一动。").c_str(), true);
    netFocusDone = (int)(focusMs / 60000UL);
    logf("focus done %lu min\n", focusMs / 60000UL);
  } else {
    sayText(("提前结束了，专注了 " + String(minutes) + " 分钟。").c_str());
    logf("focus stopped after %d min\n", minutes);
  }
}

void stepFocus(uint32_t now) {
  if (netFocusCmd >= 0) {  // 云端 / 对话桥："M<分钟>" 开始，"M0" 结束
    int m = netFocusCmd;
    netFocusCmd = -1;
    if (m == 0) stopFocus(now, false);
    else startFocus(now, m);
  }
  if (focusActive && now - focusStart >= focusMs) stopFocus(now, true);
}

// 点阵倒计时：像钟表的分钟刻度，一圈 60 个位置，一个点 = 一分钟，从 12 点顺时针挨着排
// （点少时挨在一起，不分散成一圈）。超过 60 分钟就排满一圈、一颗代表几分钟。
// 从 12 点那颗开始一颗一颗消失；下一颗要消失的会轻轻呼吸，看得出时间在走
bool knobMode = false;  // 双击后拧它：调专注时间（定义在下面的旋钮一节）
int knobMin = 25;

void drawFocusRing(uint32_t now) {
  if (knobMode) {  // 拧的时候先预览：几分钟就几颗点
    int cx = 233 - OX, cy = 233 - OY;
    for (int i = 0; i < min(knobMin, 60); i++) {
      float a = (-90.0f + i * 6.0f) * DEG_TO_RAD;
      gfx->fillCircle(cx + (int)lroundf(cosf(a) * 215), cy + (int)lroundf(sinf(a) * 215), 5, ACCENT);
    }
    return;
  }
  if (!focusActive) return;
  int minutes = (int)(focusMs / 60000UL);
  int n = min(minutes, 60);
  float perDot = (float)focusMs / n;
  uint32_t el = now - focusStart;
  int gone = (int)(el / perDot);              // 已经消失的点
  float frac = (el - gone * perDot) / perDot;  // 下一颗走了多少
  int cx = 233 - OX, cy = 233 - OY;
  const float R = 215;
  for (int i = gone; i < n; i++) {
    float a = (-90.0f + i * 6.0f) * DEG_TO_RAD;  // 6° = 一个分钟刻度
    int x = cx + (int)lroundf(cosf(a) * R), y = cy + (int)lroundf(sinf(a) * R);
    if (i == gone) {  // 下一颗：慢慢暗下去，同时轻轻呼吸
      float k = (1.0f - frac * 0.7f) * (0.75f + 0.25f * sinf(now / 400.0f));
      gfx->fillCircle(x, y, 5, accentDim(k));
    } else {
      gfx->fillCircle(x, y, 5, ACCENT);
    }
  }
}

// ---------- timers（计时提醒）----------
// "15 分钟后提醒我看面团"（对话桥解析）/ Claude 的 set_timer 工具 → "W<秒>|<内容>"，"W0" 全部取消。
// 最多 4 个同时跑；到点"叮——叮"（比专注的低一点、长一点，听得出区别）+ 挂着提醒。
// 睡着也照算，到点自己醒。有提醒在等时，单击显示最近那个还剩多久。
struct Timer { bool on; uint32_t endAt; char label[48]; };
Timer timers[4];

void playChime(float f1, float f2, float len, float gain = 1.0f) {
  const int N = (int)(16000 * len);
  int16_t *pcm = (int16_t *)ps_malloc(N * 2);
  if (!pcm) return;
  float half = len / 2;
  for (int i = 0; i < N; i++) {
    float t = i / 16000.0f;
    float f = t < half ? f1 : f2;
    float lt = t < half ? t : t - half;
    float env = expf(-lt * 5.0f) * min(1.0f, lt * 200.0f);
    pcm[i] = (int16_t)(sinf(2 * PI * f * t) * env * 9000 * gain);
  }
  audioPlay(pcm, N * 2);
}

void addTimer(uint32_t now, uint32_t seconds, const char *label) {
  int slot = -1;
  for (int i = 0; i < 4; i++) if (!timers[i].on) { slot = i; break; }
  if (slot < 0) { sayText("已经有 4 个提醒在等了，先取消一个。"); return; }
  timers[slot].on = true;
  timers[slot].endAt = now + seconds * 1000UL;
  strncpy(timers[slot].label, label, sizeof(timers[slot].label) - 1);
  timers[slot].label[sizeof(timers[slot].label) - 1] = 0;
  uint32_t m = (seconds + 59) / 60;
  String what = strlen(label) ? String(label) : String("提醒你");
  sayText((String(m) + " 分钟后" + what + "。记下了。").c_str());
  setExpr(WINK, 2.5f);
  logf("timer %u s: %s\n", (unsigned)seconds, label);
}

void stepTimers(uint32_t now) {
  if (netTimerCmd) {  // 网络任务收到的 "W..."
    netTimerCmd = false;
    if (netTimerSec == 0) {
      for (auto &t : timers) t.on = false;
      sayText("提醒都取消了。");
    } else {
      addTimer(now, netTimerSec, netTimerLabel);
    }
  }
  for (auto &t : timers) {
    if (t.on && (int32_t)(now - t.endAt) >= 0) {
      t.on = false;
      playChime(660, 990, 1.2f);
      setExpr(WINK, 3);
      String msg = strlen(t.label) ? ("到点了：" + String(t.label)) : String("时间到了。");
      sayText(msg.c_str(), true);
      logf("timer done: %s\n", t.label);
    }
  }
}

bool timersActive() {
  for (auto &t : timers) if (t.on) return true;
  return false;
}

void sayNearestTimer(uint32_t now) {
  int best = -1;
  for (int i = 0; i < 4; i++)
    if (timers[i].on && (best < 0 || (int32_t)(timers[i].endAt - timers[best].endAt) < 0)) best = i;
  if (best < 0) return;
  uint32_t left = (timers[best].endAt - now + 59999) / 60000;
  int n = 0;
  for (auto &t : timers) n += t.on;
  String head = strlen(timers[best].label) ? String(timers[best].label) + " " : String("");
  sayText((head + "还剩 " + String(left) + " 分钟" + (n > 1 ? "（一共 " + String(n) + " 个提醒）" : "") + "。").c_str());
}

// ---------- 旋钮（双击后拧它调专注时间）----------
// 双击屏幕 → 外圈亮 25 颗点、显示"25 分钟"，这时平放在桌上拧它：顺时针加、逆时针减，每 30° 加减 5 分钟（0–120），每变一档"嗒"一声。
// 不动 3 秒就开始；点一下马上开始；拧到 0 = 取消。（不做成"平放一拧就进入"：拿起来时会误触）
uint32_t knobLastMove = 0;
float knobAngle = 0;

void enterKnob(uint32_t now) {
  knobMode = true;
  knobAngle = 0;
  knobMin = 25;
  knobLastMove = now;
  useCanvas(true);
  setExpr(WINK, 2.5f);
  sayText("25 分钟。拧我调时间。");
  logf("knob start\n");
}

void finishKnob(uint32_t now) {
  knobMode = false;
  logf("knob done %d\n", knobMin);
  if (knobMin >= 5) startFocus(now, knobMin);
  else { useCanvas(false); sayText("好，不专注了。"); }
}

void stepKnob(uint32_t now) {
  static uint32_t last = 0;
  float dt = last ? constrain((now - last) / 1000.0f, 0.0f, 0.1f) : 0;
  last = now;
  if (!knobMode) return;
  if (focusActive || alarmRinging || dizzyUntil || startled || covered) {
    knobMode = false;
    if (!focusActive) useCanvas(false);
    logf("knob cancelled\n");
    return;
  }
  float r = -mot.rateZ;  // 正面朝上时 rateZ 为正 = 逆时针，取反让顺时针为正
  if (fabsf(r) > 15 && fabsf(r) < 260) { knobAngle += r * dt; knobLastMove = now; lastInteract = now; noteActivity(now); }
  int m = constrain(25 + (int)lroundf(knobAngle / 30.0f) * 5, 0, 120);
  if (m != knobMin) {
    knobMin = m;
    playChime(2200, 2200, 0.03f, 0.2f);  // 很轻的"嗒"
    sayText(m ? (String(m) + " 分钟").c_str() : "取消");
  }
  if (now - knobLastMove > 3000) finishKnob(now);
}

// ---------- alarm（叫起床）----------
// 最多 3 个闹钟，存在 NVS 里（重启不丢）。"L<hh>:<mm>|<星期掩码>"：bit0=周日…bit6=周六，128=只响一次；"L0" 全部取消。
// 到点：先轻提示音（每 8 秒、越来越响）→ 1 分钟后换叫醒声音（有 wake_voice.h 就放录好的声音，每 30 秒，越来越大声）→ 最多 10 分钟。
// 点一下 / 拿起来 = 起来了；捂住 = 再睡 5 分钟。
struct Alarm { uint8_t on, h, m, mask; };
Alarm alarms[3];
Preferences prefs;
bool alarmRinging = false;
uint32_t ringStart = 0, snoozeUntil = 0, lastRingSound = 0;
int ringVoiceIdx = 0, ringPlays = 0;
int32_t alarmLastDay[3] = { -1, -1, -1 };

void saveAlarms() {
  prefs.begin("companion", false);
  prefs.putBytes("alarms", alarms, sizeof(alarms));
  prefs.end();
}

void loadAlarms() {
  prefs.begin("companion", true);
  if (prefs.getBytesLength("alarms") == sizeof(alarms)) prefs.getBytes("alarms", alarms, sizeof(alarms));
  prefs.end();
  for (auto &a : alarms) if (a.on) logf("alarm %02d:%02d mask %d\n", a.h, a.m, a.mask);
}

int64_t localNow() { return timeBase + millis() / 1000; }

String alarmDesc(const Alarm &a) {
  char t[8];
  snprintf(t, sizeof(t), "%d:%02d", a.h, a.m);
  if (a.mask & 128) {
    int64_t l = localNow();
    int nowMin = (int)((l % 86400) / 60);
    return String(a.h * 60 + a.m > nowMin ? "今天 " : "明早 ") + t;
  }
  if ((a.mask & 0x7F) == 0x7F) return String("每天 ") + t;
  if ((a.mask & 0x7F) == 0x3E) return String("工作日 ") + t;
  if ((a.mask & 0x7F) == 0x41) return String("周末 ") + t;
  return String(t);
}

int minutesToAlarm(int within, String &when) {
  if (!timeKnown) return -1;
  int64_t l = localNow();
  int32_t today = (int32_t)(l / 86400);
  int nowMin = (int)((l % 86400) / 60);
  int best = -1;
  for (auto &a : alarms) {
    if (!a.on) continue;
    for (int d = 0; d <= 1; d++) {
      int wday = (int)((today + d + 4) % 7);
      bool fires = (a.mask & 128) || (a.mask & (1 << wday));
      int mins = d * 1440 + a.h * 60 + a.m - nowMin;
      if (fires && mins > 0 && mins <= within && (best < 0 || mins < best)) {
        best = mins;
        char t[8];
        snprintf(t, sizeof(t), "%d:%02d", a.h, a.m);
        when = t;
      }
    }
  }
  return best;
}

void addAlarm(int h, int m, int mask) {
  int slot = -1;
  for (int i = 0; i < 3; i++) if (alarms[i].on && alarms[i].h == h && alarms[i].m == m) slot = i;  // 同一时间就覆盖
  for (int i = 0; i < 3 && slot < 0; i++) if (!alarms[i].on) slot = i;
  if (slot < 0) { sayText("已经有 3 个闹钟了，先取消一个。"); return; }
  alarms[slot] = { 1, (uint8_t)h, (uint8_t)m, (uint8_t)mask };
  alarmLastDay[slot] = -1;
  saveAlarms();
  setExpr(WINK, 2.5f);
  sayText((alarmDesc(alarms[slot]) + " 叫你起床。记下了。").c_str());
  logf("alarm set %02d:%02d mask %d\n", h, m, mask);
}

void stopRinging(uint32_t now, bool snooze) {
  if (!alarmRinging) return;
  alarmRinging = false;
  if (snooze) {
    snoozeUntil = now + 5UL * 60 * 1000;
    sayText("好，再睡五分钟。", true);
    logf("alarm snooze\n");
  } else {
    snoozeUntil = 0;
#ifdef HAVE_WAKE_VOICE
    // 起来了：放一句录好的鼓励，字幕同步
    int k = random(sizeof(ENC_CLIPS) / sizeof(ENC_CLIPS[0]));
    const WakeClip &c = ENC_CLIPS[k];
    int16_t *pcm = (int16_t *)ps_malloc(c.n * 2);
    if (pcm) {
      memcpy(pcm, c.pcm, c.n * 2);
      audioPlay(pcm, c.n * 2);
    }
    setExpr(SMILE, 4);
    sayText(ENC_LINES[k], true);
#else
    setExpr(WINK, 3);
    sayText("早。");
#endif
    logf("alarm stopped\n");
  }
}

void ringSound(uint32_t now) {
  uint32_t t = now - ringStart;
  if (t < 60000) {  // 前 1 分钟：轻提示音，每 8 秒，越来越响
    if (now - lastRingSound >= 8000) {
      lastRingSound = now;
      playChime(784, 1046, 0.9f, 0.25f + 0.35f * t / 60000.0f);
    }
    return;
  }
  if (now - lastRingSound >= 30000 || ringPlays == 0) {  // 之后：叫醒声音，每 30 秒，越来越大声
    lastRingSound = now;
    float gain = min(1.0f, 0.6f + 0.1f * ringPlays++);
#ifdef HAVE_WAKE_VOICE
    const WakeClip &c = WAKE_CLIPS[ringVoiceIdx++ % (sizeof(WAKE_CLIPS) / sizeof(WAKE_CLIPS[0]))];
    int16_t *pcm = (int16_t *)ps_malloc(c.n * 2);
    if (pcm) {
      for (uint32_t i = 0; i < c.n; i++) pcm[i] = (int16_t)(c.pcm[i] * gain);
      audioPlay(pcm, c.n * 2);
    }
#else
    playChime(784, 1046, 1.2f, gain);
#endif
  }
}

void stepAlarm(uint32_t now) {
  if (netAlarmCmd) {  // 网络 / 串口收到的 "L..."
    netAlarmCmd = false;
    if (netAlarmH < 0) {
      for (auto &a : alarms) a.on = 0;
      saveAlarms();
      stopRinging(now, false);
      snoozeUntil = 0;
      sayText("闹钟都取消了。");
    } else {
      addAlarm(netAlarmH, netAlarmM, netAlarmMask);
    }
  }
  if (alarmRinging) {
    ringSound(now);
    if (now - ringStart > 10UL * 60 * 1000) {  // 叫了 10 分钟
      alarmRinging = false;
      sayText("叫了你十分钟了。起来了没？", true);
    }
    return;
  }
  if (snoozeUntil && (int32_t)(now - snoozeUntil) >= 0) {  // 赖床时间到
    snoozeUntil = 0;
    alarmRinging = true;
    ringStart = now - 60000;  // 直接从叫名字那段开始
    lastRingSound = 0;
    ringPlays = 0;
    return;
  }
  if (!timeKnown) return;
  int64_t l = localNow();
  int32_t day = (int32_t)(l / 86400);
  int wday = (int)((day + 4) % 7);  // 1970-01-01 是周四
  int hm = (int)((l % 86400) / 60);
  for (int i = 0; i < 3; i++) {
    Alarm &a = alarms[i];
    if (!a.on || alarmLastDay[i] == day) continue;
    bool today = (a.mask & 128) || (a.mask & (1 << wday));
    if (today && hm == a.h * 60 + a.m) {
      alarmLastDay[i] = day;
      if (a.mask & 128) { a.on = 0; saveAlarms(); }  // 只响一次的用完就关
      alarmRinging = true;
      ringStart = now;
      lastRingSound = 0;
      ringPlays = 0;
      if (expr == SLEEP && autoNight) setExpr(CALM);
      perkAt = now;
      sayText("起床了。", true);
      logf("alarm ringing %02d:%02d\n", a.h, a.m);
    }
  }
}

// ---------- 摇一摇抽签 ----------
// 使劲摇 → 嫌弃地瞥一眼 → 字幕像老虎机一样翻，越翻越慢，"嗒"停在一个答案上。夜里"先睡觉"抽到得多
const char *DRAW_ANSWERS[] = {
  "可以。", "别。", "去吧。", "再想想。", "明天再说。", "先吃饭。", "先睡觉。", "你心里已经有答案了。",
  "听你的。", "不行。", "行，就这个。", "少吃一点就行。", "先喝口水。", "问我的话：要。", "不急，等等再定。",
  "这个可以有。", "选第一个。", "选你刚才犹豫的那个。", "去做，我在。", "抽到这个，就别再摇了。",
};
const int DRAW_N = sizeof(DRAW_ANSWERS) / sizeof(DRAW_ANSWERS[0]);
const uint16_t DRAW_GAPS[] = { 80, 85, 95, 110, 130, 155, 190, 240, 310 };  // 每翻一下之后等多久（越来越慢）
const int DRAW_FLIPS = sizeof(DRAW_GAPS) / sizeof(DRAW_GAPS[0]);
uint32_t drawStart = 0, drawNextAt = 0;
int drawFlip = 0, drawLast = -1;

int pickAnswer() {
  for (;;) {
    int i;
    if (isNight() && random(100) < 35) i = 6;  // 夜里：先睡觉
    else i = random(DRAW_N);
    if (i != drawLast) { drawLast = i; return i; }
  }
}

void startDraw(uint32_t now) {
  drawStart = max((uint32_t)1, now);
  drawNextAt = now + 600;  // 先让嫌弃的表情露一下
  drawFlip = 0;
}

void stepDraw(uint32_t now) {
  if (!drawStart || (int32_t)(now - drawNextAt) < 0) return;
  noteActivity(now);
  lastInteract = now;
  int i = pickAnswer();
  sayText(DRAW_ANSWERS[i]);
  if (drawFlip < DRAW_FLIPS) {
    playChime(2200, 2200, 0.025f, 0.15f);  // 翻一下"嗒"
    drawNextAt = now + DRAW_GAPS[drawFlip++];
  } else {  // 停：就是它
    drawStart = 0;
    playChime(988, 1319, 0.5f, 0.3f);
    setExpr(WINK, 3);
    logf("draw: %s\n", DRAW_ANSWERS[i]);
  }
}

// ---------- 消息：没看再提醒 + 历史 ----------
// 没看再提醒：云端发来的消息 5 分钟后还没看（没点开、没碰过、没拿起）→ 再响一声、脸一提、亮屏；
// 还没看 10 分钟后再一次，最多两次。夜里不提醒，专注中往后推。
// 历史：最近 10 条（云端消息 + 当面聊天的回复，不含歌名），存 NVS 重启不丢。
// 在屏幕上快速左划 = 上一条（带时间），右划 = 往回翻；15 秒没动就退出
uint32_t lastTouchAt = 0;           // 最近一次碰屏幕 / 拿起来
uint32_t msgArrivedAt = 0, msgRemindAt = 0;
int msgRemindN = 0;

struct HistMsg { uint32_t t; char text[160]; };  // t = 本地时间（epoch 秒），0 = 不知道几点
HistMsg hist[10];
int histN = 0, histIdx = -1;
uint32_t histLastAt = 0;

void saveHist() {
  prefs.begin("companion", false);
  prefs.putBytes("hist", hist, sizeof(hist));
  prefs.putUChar("histN", histN);
  prefs.end();
}

void loadHist() {
  prefs.begin("companion", true);
  if (prefs.getBytesLength("hist") == sizeof(hist)) {
    prefs.getBytes("hist", hist, sizeof(hist));
    histN = min(10, (int)prefs.getUChar("histN", 0));
  }
  prefs.end();
}

void addHist(const char *txt) {
  if (!strcmp(txt, "在。") || !strcmp(txt, "在听。") || !strncmp(txt, "正在听：", strlen("正在听："))) return;
  for (int i = min(histN, 9); i > 0; i--) hist[i] = hist[i - 1];  // 最新的放在 0
  hist[0].t = timeKnown ? (uint32_t)localNow() : 0;
  size_t n = strlen(txt);
  if (n > sizeof(hist[0].text) - 1) {  // 截断时别切断半个汉字
    n = sizeof(hist[0].text) - 1;
    while (n > 0 && ((uint8_t)txt[n] & 0xC0) == 0x80) n--;
  }
  memcpy(hist[0].text, txt, n);
  hist[0].text[n] = 0;
  histN = min(histN + 1, 10);
  saveHist();
}

String histWhen(uint32_t t) {
  if (!t || !timeKnown) return "";
  int32_t today = (int32_t)(localNow() / 86400), day = (int32_t)(t / 86400);
  char hm[8];
  snprintf(hm, sizeof(hm), "%d:%02d", (int)((t % 86400) / 3600), (int)((t % 3600) / 60));
  if (day == today) return String("今天 ") + hm;
  if (day == today - 1) return String("昨天 ") + hm;
  return String(today - day) + " 天前 " + hm;
}

void showHist(uint32_t now, int idx) {
  histIdx = idx;
  histLastAt = now;
  String head = histWhen(hist[idx].t);
  String t = head + "（" + String(idx + 1) + "/" + String(histN) + "）\n" + hist[idx].text;
  if (readerOn || strlen(hist[idx].text) > 60) openReader(now, t);  // 长的直接用阅读视图
  else sayText(t.c_str());
}

void swipeHist(uint32_t now, bool older) {
  lastTouchAt = now;
  if (!histN) { sayText("还没有消息。"); return; }
  if (older) {
    int i = histIdx < 0 ? 0 : histIdx + 1;
    if (i >= histN) { histLastAt = now; sayText("没有更早的了。"); return; }
    showHist(now, i);
  } else {
    if (histIdx < 0) return;  // 没在翻历史：右划不做事
    if (histIdx == 0) { histIdx = -1; subActive = false; subUnread = false; readerOn = false; return; }  // 翻回最新之后再右划：退出
    showHist(now, histIdx - 1);
  }
  playChime(2200, 2200, 0.025f, 0.15f);
}

void stepMsgRemind(uint32_t now) {
  if (histIdx >= 0 && now - histLastAt > 15000) histIdx = -1;  // 翻历史 15 秒没动：退出
  if (!msgRemindAt || (int32_t)(now - msgRemindAt) < 0) return;
  bool unseen = (subUnread || (subActive && subHold)) && lastTouchAt < msgArrivedAt;
  if (!unseen || isNight()) { msgRemindAt = 0; return; }
  if (focusActive || alarmRinging || audioSpeaking) { msgRemindAt = now + 5UL * 60 * 1000; return; }
  playChime(1318, 1760, 0.5f, 0.35f);
  noteActivity(now);
  perkAt = now;
  msgRemindN++;
  msgRemindAt = msgRemindN < 2 ? now + 10UL * 60 * 1000 : 0;
  logf("unread reminder %d\n", msgRemindN);
}

// ---------- Claude 桌面端 Hardware Buddy ----------
// Claude 桌面端（Code / Cowork）在干活：脸下面三颗小点轮流跳。需要批准：挑眉"嗯？"，显示工具和命令，
// 长按 1 秒批准（外圈点阵走满），点一下拒绝。配对时屏幕上显示 6 位配对码
bool buddyAsking = false;
uint32_t buddySeenSeq = 0, buddyPkSeen = 0, touchDownAtG = 0;

String clip(const char *s, int maxW) {  // 按字幕宽度截断（汉字 32、其它 16 像素）
  String out;
  int w = 0;
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    int n = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
    int gw = n >= 3 ? 32 : 16;
    if (w + gw > maxW - 32) { out += "…"; break; }
    for (int i = 0; i < n && p[i]; i++) out += (char)p[i];
    w += gw;
    p += n;
  }
  return out;
}

void stepBuddy(uint32_t now) {
  if (buddyPasskeySeq != buddyPkSeen) {
    buddyPkSeen = buddyPasskeySeq;
    noteActivity(now);
    setExpr(CURIOUS, 10);
    sayText(("蓝牙配对码\n" + String(buddyPasskey)).c_str(), true);
  }
  if (buddyTimeEpoch && !timeKnown) { timeBase = buddyTimeEpoch - millis() / 1000; timeKnown = true; }
  if (buddyPromptSeq != buddySeenSeq && buddyPromptId[0]) {
    buddySeenSeq = buddyPromptSeq;
    buddyAsking = true;
    noteActivity(now);
    lastInteract = now;
    readerOn = false;
    if (expr == SLEEP && autoNight) setExpr(CALM);
    setExpr(CURIOUS, 600);
    playChime(1318, 1760, 0.5f, 0.4f);
    String line1 = String("批准 ") + buddyTool + "？";
    sayText((clip(line1.c_str(), 360) + "\n" + clip(buddyHint, 320)).c_str(), true);
  }
  if (buddyAsking && (!buddyPromptId[0] || !buddyConnected)) {  // 在电脑上处理了，或者断开了
    buddyAsking = false;
    subActive = false;
    setExpr(CALM);
  }
}

void buddyDecide(uint32_t now, bool approve) {
  buddyDecision = approve ? 1 : 2;
  buddyAsking = false;
  lastInteract = now;
  if (approve) { setExpr(WINK, 3); playChime(988, 1319, 0.4f, 0.3f); sayText("批准了。"); }
  else { setExpr(MEH, 2.5f); playChime(523, 392, 0.4f, 0.3f); sayText("拒绝了。"); }
}

void drawBuddy(uint32_t now) {
  if (buddyAsking) {  // 提示 + 长按进度
    gfx->setFont(u8g2_font_unifont_h_chinese);
    gfx->setUTF8Print(true);
    gfx->setTextSize(1);
    gfx->setTextColor(rgb(0xA0, 0xA0, 0xA0));
    gfx->setCursor(233 - 84 - OX, 104 - OY);
    gfx->print("长按批准 · 点一下拒绝");
    if (touchIsDown && !covered) {
      float frac = min(1.0f, (now - touchDownAtG) / 1000.0f);
      int n = (int)(frac * 48);
      for (int i = 0; i < n; i++) {
        float a = (-90 + i * 7.5f) * DEG_TO_RAD;
        gfx->fillCircle(233 - OX + (int)(cosf(a) * 178), 233 - OY + (int)(sinf(a) * 178), 4, ACCENT);
      }
    }
    return;
  }
  bool working = buddyConnected && buddyRunning > 0;
  if (!working || subActive || subUnread || pttActive || readerOn || expr == SLEEP || focusActive || knobMode) return;
  for (int i = 0; i < 3; i++) {  // 三颗小点轮流往上跳：Claude 在干活
    float ph = fmodf(now / 900.0f - i * 0.18f, 1.0f);
    float up = ph < 0.3f ? sinf(ph / 0.3f * PI) : 0;
    gfx->fillCircle(233 - 18 + i * 18 - OX, 390 - (int)(up * 8) - OY, 4, accentDim(0.5f + 0.5f * up));
  }
}

// ---------- iPhone 通知 ----------
// 来电：挑眉 + 挂"<名字>，来电话了：xx" + 每 3 秒轻轻响一声（最多 30 秒，接了/挂了就停）；微信：只"嗒"一声 + 挂谁发的
bool callRinging = false;
uint32_t callStart = 0, lastCallChime = 0;

void stepAncs(uint32_t now) {
  if (ancsReady) {
    noteActivity(now);
    lastInteract = now;
    if (expr == SLEEP && autoNight) setExpr(CALM);
    setExpr(CURIOUS, 4);
    sayText(ancsText, true);
    if (ancsIsCall) {
      callRinging = true;
      callStart = now;
      lastCallChime = 0;
    } else {
      playChime(1568, 1568, 0.12f, 0.35f);  // 很短的"嗒"
    }
    ancsReady = false;
  }
  if (ancsCallGone) {
    ancsCallGone = false;
    callRinging = false;
  }
  if (callRinging) {
    if (now - callStart > 30000) callRinging = false;
    else if (now - lastCallChime > 3000) {
      lastCallChime = now;
      playChime(1046, 1318, 0.7f, 0.45f);
    }
  }
}

// ---------- cover（捂眼睛）----------
// 手掌盖住屏幕：闭眼"看不见了"；顺手把正在挂着的提醒收起（= 知道了）。
// 松手：眨眼、脸一提、"找到你了"。晚上 10 点以后捂住超过 3 秒：直接晚安。
void stepCover(uint32_t now) {
  // 芯片只在刚盖上时报"手掌"，之后当普通触摸点报：认出捂住以后，一直算捂着直到手离开屏幕
  bool nowCovered = (coverAt && now - coverAt < 400) || (covered && touchIsDown);
  if (nowCovered && !covered && alarmRinging) {  // 闹钟响着时捂住 = 再睡五分钟
    covered = true;
    coveredSince = now;
    pendingTap = false;
    stopRinging(now, true);
    return;
  }
  if (nowCovered && !covered) {
    covered = true;
    coveredSince = now;
    pendingTap = false;  // 手掌那一下不算点击
    petting = false;
    lastInteract = now;
    if (subActive) subtitleTap(now);  // 正在挂着的提醒/字幕：捂一下 = 知道了
    if (subActive) subActive = false;
    subUnread = false;
    // 捂着时屏幕被手挡住，看不到字：用一声很轻的"唔"（下滑的短音）让你听到
    {
      const int N = 16000 * 28 / 100;
      int16_t *pcm = (int16_t *)ps_malloc(N * 2);
      if (pcm) {
        float ph = 0;
        for (int i = 0; i < N; i++) {
          float t = (float)i / N;
          ph += 2 * PI * (820.0f - 300.0f * t) / 16000.0f;
          float env = sinf(PI * t);  // 轻轻起、轻轻落
          pcm[i] = (int16_t)(sinf(ph) * env * 4000);
        }
        audioPlay(pcm, N * 2);
      }
    }
    logf("covered\n");
  } else if (!nowCovered && covered) {
    covered = false;
    uint32_t held = now - coveredSince;
    lastInteract = now;
    int h = timeKnown ? (int)(((timeBase + millis() / 1000) % 86400) / 3600) : 12;
    logf("uncovered after %lums\n", held);
    if (held > 3000 && (h >= 22 || h < 5)) {
      setExpr(SLEEP);
      autoNight = true;
      sayText("晚安。");
    } else {
      perkAt = now;
      nextBlink = now;  // 睁眼马上眨一下
      const char *lines[] = { "找到你了。", "刚才是谁捂的？", "看见你了。" };
      sayText(lines[random(3)]);
    }
  }
}

// ---------- push to talk ----------
// BOOT 键（GPIO0，按下为低）：按住说话，松开发给 Mac，不用叫名字（PTT_PIN 定义在文件前面）

void stepPTT(uint32_t now) {
  static uint8_t stable = 0;
  static bool last = false;
  bool raw = digitalRead(PTT_PIN) == LOW;
  stable = (raw == last) ? min(stable + 1, 3) : 0;  // 连续几帧一样才算，去抖
  last = raw;
  if (stable < 2 || raw == pttActive) return;
  pttActive = raw;
  micPTT = raw;
  lastInteract = now;
  if (raw) {
    perkAt = now;
    if (expr == SLEEP && autoNight) setExpr(CALM);
    logf("ptt down\n");
  } else {
    logf("ptt up\n");
    if (!netBridgeOnline) sayText("Mac 不在，这句听不到。");
  }
}

// ---------- low power ----------
// 用电池时分两层睡（插着电一直醒着）：
//   浅睡：20 秒没动静 → 关屏、停画、CPU 80MHz。说话（连续 0.5 秒以上）、触摸、拿起、按键、云端消息都能叫醒
//   深睡：浅睡 5 分钟没人理 → 再关麦克风、WiFi 切最省电模式。喊它不醒，要触摸/拿起/按键/云端消息
//   夜里：深睡时再整个关掉 WiFi，每 20 分钟开 90 秒收消息、对时（闹钟、计时都在板子上跑，不受影响）
// 深睡时还会关掉陀螺仪（拿起靠加速度计照样能叫醒）、把和 iPhone 的蓝牙连接间隔放宽
// 只有浅睡的话，环境吵时麦克风会一直把它叫醒，实测每小时掉 12%
const uint32_t SLEEP_AFTER_MS = 20000;
const uint32_t DEEP_AFTER_MS = 5UL * 60 * 1000;
bool lowPower = false, deepSleep = false;
uint32_t lastActivity = 0, sleptAt = 0;
bool forceBattery = false;  // 调试：串口 B 切换"假装用电池"，插着线也能测睡眠
bool nightSleep = false, forceNight = false;  // forceNight：调试，串口 "Dn" 不管几点都按夜里算
uint32_t nightNextWin = 0, nightWinEnd = 0;
const uint32_t NIGHT_WIN_EVERY = 20UL * 60 * 1000, NIGHT_WIN_LEN = 90000;

void wakeUp(uint32_t now) {
  if (!lowPower) return;
  if (deepSleep) {
    audioMicEnable(true);
    motionLowPower(false);
    ancsLowPower(false);
    netSlowLoop = false;
    if (nightSleep) { nightSleep = false; netWifiPause = false; logf("night sleep end\n"); }
    wifi_mode_t wm;  // WiFi 被夜里关掉时没初始化，别去设它（会报错）；网络任务重开时默认就是省电模式
    if (esp_wifi_get_mode(&wm) == ESP_OK && wm != WIFI_MODE_NULL) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    deepSleep = false;
  }
  lowPower = false;
  setCpuFrequencyMhz(240);
  panel->displayOn();
  lastBright = -1;  // 下一帧重新设亮度
  perkAt = now;     // 像刚被叫醒，脸轻轻一提
  logf("wake\n");
}

void noteActivity(uint32_t now) {
  lastActivity = now;
  wakeUp(now);
}

void stepLowPower(uint32_t now) {
  // 醒着时：这些状态都算有动静
  // 睡着时：门槛高一点——说话要连续 0.5 秒、晃动要更明显（桌子被碰一下不算）
  static uint32_t voiceSince = 0;
  bool voiceNow = micVoice && netEarOn;
  if (!voiceNow) voiceSince = 0;
  else if (!voiceSince) voiceSince = now;
  bool sustainedVoice = voiceSince && now - voiceSince >= 500;

  // 挂着的字幕只让屏幕亮 30 秒，之后照常睡；叫醒时字幕还在
  bool subBusy = subActive && !(subHold && now - subPageStart > 30000);
  bool busy = readerOn || buddyAsking || knobMode || subBusy || focusActive || petting || covered || alarmRinging || callRinging || pttActive || audioSpeaking || (netThinkingSince && now - netThinkingSince < 25000);
  if (!lowPower) {
    if (busy || listening || voiceNow || mot.activity > 0.06f) noteActivity(now);
  } else {
    if (busy || sustainedVoice || mot.activity > 0.10f) noteActivity(now);
  }

  bool onBattery = (bat.battery && !bat.usb) || forceBattery;
  if (!onBattery) { noteActivity(now); return; }
  if (!lowPower && now - lastActivity > SLEEP_AFTER_MS) {
    lowPower = true;
    sleptAt = now;
    panel->setBrightness(0);
    panel->displayOff();
    setCpuFrequencyMhz(80);
    logf("sleep (low power)\n");
  }
  if (lowPower && !deepSleep && now - sleptAt > DEEP_AFTER_MS) {
    deepSleep = true;
    audioMicEnable(false);
    motionLowPower(true);
    ancsLowPower(true);
    netSlowLoop = true;
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    logf("deep sleep\n");
  }
  // 夜里深睡：关 WiFi，定时开窗
  bool night = deepSleep && (isNight() || forceNight);
  if (night && !nightSleep) {
    nightSleep = true;
    netWifiPause = true;
    nightNextWin = now + NIGHT_WIN_EVERY;
    nightWinEnd = 0;
    logf("night sleep: wifi off\n");
  } else if (!night && nightSleep) {
    nightSleep = false;
    netWifiPause = false;
    logf("night sleep end\n");
  }
  if (nightSleep) {
    if (netWifiPause && (int32_t)(now - nightNextWin) >= 0) {
      netWifiPause = false;
      nightWinEnd = now + NIGHT_WIN_LEN;
      logf("night: wifi window\n");
    } else if (!netWifiPause && nightWinEnd && (int32_t)(now - nightWinEnd) >= 0) {
      netWifiPause = true;
      nightWinEnd = 0;
      nightNextWin = now + NIGHT_WIN_EVERY;
    }
  }
}
uint32_t frames = 0, fpsT = 0;

// 卡顿排查：某一段超过 200ms 就打印是哪段
#define SLOW(name, code) { uint32_t _t = millis(); code; uint32_t _d = millis() - _t; if (_d > 200) logf("SLOW %s %lums\n", name, _d); }

void loop() {
  uint32_t now = millis();
  static uint32_t lastLoop = 0;
  if (lastLoop && now - lastLoop > 300) logf("SLOW loop gap %lums\n", now - lastLoop);
  lastLoop = now;

  // 单击要等 350ms 确认不是双击才执行
  if (pendingTap && now - pendingTapAt >= 350) {
    pendingTap = false;
    lastTap = now;
    if (alarmRinging) stopRinging(now, false);  // 点一下 = 起来了
    else if (knobMode) finishKnob(now);  // 调时间时点一下：马上开始
    else if (readerOn) { readerOn = false; lastInteract = now; }  // 阅读视图：点一下关
    else if (subtitleTap(now)) {}
    else if (focusActive) {
      int left = (int)((focusMs - (now - focusStart) + 59999) / 60000);
      sayText(("还剩 " + String(left) + " 分钟。").c_str());
    }
    else if (timersActive()) sayNearestTimer(now);
    else if (expr == SLEEP && autoNight) setExpr(CALM);
    else setExpr((Expr)((expr + 1) % EXPR_COUNT));
  }

  // 触摸：只在中断后读。按住时芯片会持续发中断；150ms 没有新中断就算松手
  static bool wasDown = false;
  static uint32_t lastDownAt = 0;
  static int badReads = 0;
  static int16_t downX = 0, downY = 0, lastX = 0, lastY = 0;
  static uint32_t downAt = 0;
  if (wasDown && now - lastDownAt > 150) {  // 松手：没摸起来、按得不久，才算"点一下"
    wasDown = false;
    touchIsDown = false;
    if (covered || (coverAt && now - coverAt < 1500)) { petting = false; pendingTap = false; }  // 捂眼睛那一下不算点/摸
    if (buddyAsking && !covered && !(coverAt && now - coverAt < 1500)) {  // 等批准：长按 1 秒批准，点一下拒绝
      uint32_t held = lastDownAt - downAt;
      pendingTap = false;
      if (held >= 1000) buddyDecide(now, true);
      else if (held < 600 && !petting) buddyDecide(now, false);
      petting = false;
    } else if (covered || (coverAt && now - coverAt < 1500)) {
      // 刚才是捂眼睛，什么都不做
    } else if (!petting && lastDownAt - downAt >= 1500) {  // 长按不动 1.5 秒：开关耳朵
      netEarOn = !netEarOn;
      lastInteract = now;
      sayText(netEarOn ? "耳朵开了。" : "耳朵关了，杂音再多也不理。");
      logf("ear %s\n", netEarOn ? "on" : "off");
    } else if (!petting && lastDownAt - downAt < 500 && abs(lastX - downX) > 80 && abs(lastX - downX) > 2 * abs(lastY - downY)) {
      // 快速横划：翻历史消息（左划 = 更早，右划 = 往回）
      pendingTap = false;
      lastInteract = now;
      logf("swipe %s\n", lastX < downX ? "left" : "right");
      swipeHist(now, lastX < downX);
    } else if (!petting && readerOn && lastDownAt - downAt < 500 && abs(lastY - downY) > 100 && abs(lastY - downY) > 2 * abs(lastX - downX)) {
      pendingTap = false;  // 阅读视图里快速上下划：滚动
      readerScroll(now, lastY < downY ? 3 : -3);
    } else if (!petting && lastDownAt - downAt < 600) {
      lastInteract = now;
      if (pendingTap && now - pendingTapAt < 400) {  // 双击：调时间 → 开始专注；专注中双击 = 结束
        pendingTap = false;
        if (focusActive) stopFocus(now, false);
        else if (knobMode) finishKnob(now);  // 调时间时再双击：马上开始
        else enterKnob(now);
      } else {
        pendingTap = true;  // 先等一下，看是不是双击
        pendingTapAt = now;
      }
    }
    if (petting) {
      petting = false;
      lastInteract = now;
      logf("pet end %lums\n", now - petStart);
      if (now - petStart > 3000) setExpr(SMILE, 3.5f);  // 摸得久了，松手露一下十一度
    }
  }
  if (touchFlag) {
    touchFlag = false;
    int16_t x[2], y[2];
    uint8_t n = 0;
    SLOW("touch", n = touch.getPoint(x, y, 1));
    bool down = n > 0;
    // 有中断却连续读不出东西：芯片可能卡住了，自动复位
    badReads = down ? 0 : badReads + 1;
    if (badReads > 40) {
      logf("touch stuck, resetting\n");
      SLOW("touch reset", initTouch());
      badReads = 0;
    }
    if (down) { lastDownAt = now; touchIsDown = true; noteActivity(now); lastTouchAt = now; lastX = x[0]; lastY = y[0]; }
    // 按住并移动超过 40px 且超过 0.4 秒 → 算"摸"
    if (down && wasDown && !petting && !covered && now - downAt > 400 && abs(x[0] - downX) + abs(y[0] - downY) > 40) {
      petting = true;
      petStart = now;
      if (expr == SLEEP && autoNight) setExpr(CALM);
      logf("pet start\n");
    }
    if (down && !wasDown) {
      downX = x[0]; downY = y[0]; downAt = now; touchDownAtG = now;
      logf("tap %d,%d\n", x[0], y[0]);
    }
    if (down) wasDown = true;
  }
  // 串口：单个数字 0-6 = 指定表情；"T<epoch>\n" = 同步本地时间
  static char line[640];
  static uint16_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (len == 0 && c >= '0' && c < '0' + EXPR_COUNT) {
      lastInteract = now;
      setExpr((Expr)(c - '0'));
    } else if (c == '\n' || c == '\r') {
      line[len] = 0;
      if (line[0] == 'S' && len > 1) {
        lastInteract = now;
        if (expr == SLEEP && autoNight) setExpr(CALM);
        sayText(line + 1);
      } else if (line[0] == 'P') {  // 状态：电池、耳朵、网络
        PowerState ps = powerRead();
        logf("status: bat %d%% %dmV charging=%d usb=%d batt=%d | ear=%d bridge=%d cloud=%d iphone=%d | heap %u psram %u\n",
             ps.percent, ps.mv, ps.charging, ps.usb, ps.battery, netEarOn, netBridgeOnline, netOnline,
             ancsConnected, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
        logf("buddy: connected=%d total=%d running=%d waiting=%d beat %lus ago msg=%s\n", buddyConnected, buddyTotal, buddyRunning,
             buddyWaiting, (unsigned long)((millis() - buddyLastBeat) / 1000), buddyMsg);
      } else if (line[0] == 'N') {  // WiFi：N名字|密码 / Ndel:名字 / Nclear（Mac 脚本用）
        strncpy(netWifiCmd, line + 1, sizeof(netWifiCmd) - 1);
        netWifiCmd[sizeof(netWifiCmd) - 1] = 0;
        netWifiCmdReady = true;
      } else if (line[0] == 'L') {  // 闹钟：L<hh>:<mm>|<掩码>，L0 全部取消
        parseAlarmCmd(line + 1);
      } else if (line[0] == 'W') {  // 计时：W<秒>|<内容>，W0 全部取消
        char *bar = strchr(line, '|');
        netTimerSec = (uint32_t)atol(line + 1);
        strncpy(netTimerLabel, bar ? bar + 1 : "", sizeof(netTimerLabel) - 1);
        netTimerLabel[sizeof(netTimerLabel) - 1] = 0;
        netTimerCmd = true;
      } else if (line[0] == 'M') {  // 专注：M<分钟> 开始，M0 结束
        netFocusCmd = atoi(line + 1);
      } else if (line[0] == 'O') {  // 调试：马上抽一次签（不用摇）
        startDraw(now);
      } else if (line[0] == 'G') {  // 蓝牙诊断：G? 看状态，G! 重新广播
        ancsDebug(line[1] == '!');
      } else if (line[0] == 'B') {
        forceBattery = !forceBattery;
        logf("force battery %d\n", forceBattery);
      } else if (line[0] == 'D') {  // 调试：马上进深睡；"Dn" 按夜里算（关 WiFi）
        forceNight = line[1] == 'n';
        lastActivity = now - SLEEP_AFTER_MS - 1;
        stepLowPower(now);
        sleptAt = now - DEEP_AFTER_MS - 1;
      } else if (line[0] == 'A') {
        motionLog = !motionLog;
      } else if (line[0] == 'T' && len > 1) {
        timeBase = atoll(line + 1) - millis() / 1000;
        timeKnown = true;
        logf("time synced, hour %d\n", localHour());
      }
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }

  // 云端 / 对话桥发来的表情、字幕
  if (netPendingFace >= 0) {
    noteActivity(now);
    int f = netPendingFace;
    netPendingFace = -1;
    if (f < EXPR_COUNT) {
      lastInteract = now;
      setExpr((Expr)f);
    }
  }
  if (netSayReady) {
    readerOn = false;
    noteActivity(now);
    lastInteract = now;
    if (expr == SLEEP && autoNight) setExpr(CALM);
    bool songTitle = strncmp(netSayBuf, "正在听：", strlen("正在听：")) == 0;
    if (!(focusActive && songTitle)) sayText(netSayBuf, netSayHold);  // 专注时换歌不弹歌名
    if (!songTitle) addHist(netSayBuf);
    if (netSayChime) {
      msgChimeAt = max((uint32_t)1, now + 500);  // 等半秒：后面跟着语音的话就不响
      msgArrivedAt = now;  // 没看再提醒：5 分钟后看一眼
      msgRemindAt = now + 5UL * 60 * 1000;
      msgRemindN = 0;
    }
    netSayChime = false;
    netSayReady = false;
  }
  // 新消息提示音：轻轻的"叮——"往上走（和计时、专注、闹钟、微信的声音都不一样），夜里更小声
  if (msgChimeAt && (int32_t)(now - msgChimeAt) >= 0) {
    msgChimeAt = 0;
    if (!audioSpeaking && !netClipIncoming && !alarmRinging) playChime(1318, 1760, 0.5f, isNight() ? 0.2f : 0.35f);
  }
  stepSubtitle(now);
  stepListen(now);
  stepBeat(now);
  SLOW("imu", motionStep(now));
  stepMotion(now);
  SLOW("battery", stepBattery(now));
  stepBedtime(now);

  if (netTimeReady) {
    netTimeReady = false;
    timeBase = netLocalEpoch - millis() / 1000;
    if (!timeKnown) logf("ntp time, hour %d\n", (int)(((timeBase + millis() / 1000) % 86400) / 3600));
    timeKnown = true;
  }

  stepPTT(now);
  stepCover(now);
  stepFocus(now);
  stepKnob(now);
  stepDraw(now);
  stepMsgRemind(now);
  stepBuddy(now);
  if (readerOn && now - readerAt > 60000) readerOn = false;  // 阅读视图 1 分钟不动自己关
  stepTimers(now);
  stepAlarm(now);
  stepAncs(now);
  stepLowPower(now);
  if (lowPower) {  // 睡着：不画，少转几圈，省电
    delay(deepSleep ? 100 : 40);
    return;
  }

  // 专注时很安静，12 帧就够看（呼吸的点、偶尔眨眼），少画一半省电
  static uint32_t lastFrame = 0;
  if (focusActive && now - lastFrame < 80) {
    delay(10);
    return;
  }
  lastFrame = now;

  step(now);
  int b = (int)cur.bright;
  if (b != lastBright) { panel->setBrightness(b); lastBright = b; }
  SLOW("render", render(now));

  frames++;
  if (now - fpsT > 5000) { logf("fps %.1f draw %lums flush %lums\n", frames * 1000.0f / (now - fpsT), tDraw / 1000 / max((uint32_t)1, frames), tFlush / 1000 / max((uint32_t)1, frames)); frames = 0; tDraw = tFlush = 0; fpsT = now; }
}
