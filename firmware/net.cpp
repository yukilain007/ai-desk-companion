// WiFi + 和云端中转的 WebSocket 长连接 + NTP 对时
// + 局域网里连 Mac 上的对话桥（bridge/）：把录好的整句话发过去，收回表情/字幕/语音
// 没有 config.h 也照常跑：WiFi 可以之后用手机 / Mac 脚本加；没配云端就只连 Mac。
#include "net.h"
#include "log.h"
#include "audio.h"

volatile int netPendingFace = -1;
volatile bool netTimeReady = false;
volatile int64_t netLocalEpoch = 0;
volatile bool netOnline = false;
char netSayBuf[640];
volatile bool netSayReady = false;
volatile bool netSayHold = false;
volatile int netBatPercent = -1;
volatile bool netBatCharging = false, netBatUsb = false;
volatile bool netBridgeOnline = false;
volatile bool netEarOn = true;
volatile uint32_t netThinkingSince = 0;
volatile bool netBridgeMissed = false;
const char *netResetReason = "unknown";
volatile int netFocusCmd = -1;
volatile int netFocusDone = 0;
volatile bool netTimerCmd = false;
volatile uint32_t netTimerSec = 0;
char netTimerLabel[48];
volatile bool netAlarmCmd = false;
volatile int netAlarmH = -1, netAlarmM = 0, netAlarmMask = 0;

void parseAlarmCmd(const char *s) {
  int h = -1, m = 0, mask = 128;
  if (s[0] == '0' && s[1] == 0) h = -1;
  else sscanf(s, "%d:%d|%d", &h, &m, &mask);
  if (h > 23 || m > 59) return;
  netAlarmH = h;
  netAlarmM = m;
  netAlarmMask = mask;
  netAlarmCmd = true;
}

#include "settings.h"
#include <WiFi.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include <WebSocketsClient.h>
#include <ESPmDNS.h>
#include <time.h>

static const bool cloudOn = sizeof(CLOUD_HOST) > 1;
#define BRIDGE_PORT 8765

struct WifiCred { const char *ssid; const char *pass; };
static const WifiCred NETS[] = WIFI_LIST;

// ---------- WiFi 列表与选网 ----------
// config.h 里的 + 用手机（LightBlue）/ Mac 脚本加的（存 NVS，最多 6 个）。
// 选网：优先"上次找到 Mac 的网络"，其次信号强的；连上 2 分钟找不到 Mac 就换下一个认识的网络，
// 都试过就停在信号最好的那个，10 分钟后再轮一遍（比如在店里：Mac 连店里的网，板子先试店里的，找不到 Mac 再试手机热点）
struct KnownNet { String ssid, pass; bool stored; };
static std::vector<KnownNet> nets;
static Preferences wprefs;
static String preferredSsid;
char netWifiCmd[128];
volatile bool netWifiCmdReady = false;
String netWifiStatus = "";  // 给蓝牙"读"用：已存哪些、当前连的哪个

static void loadNets() {
  nets.clear();
  for (auto &n : NETS) if (n.ssid) nets.push_back({ n.ssid, n.pass, false });
  wprefs.begin("wifi", true);
  int n = wprefs.getUChar("n", 0);
  for (int i = 0; i < n; i++) {
    String sid = wprefs.getString(("s" + String(i)).c_str(), "");
    String pw = wprefs.getString(("p" + String(i)).c_str(), "");
    bool dup = false;
    for (auto &k : nets) if (k.ssid == sid) { k.pass = pw; k.stored = true; dup = true; }
    if (sid.length() && !dup) nets.push_back({ sid, pw, true });
  }
  preferredSsid = wprefs.getString("pref", "");
  wprefs.end();
}

static void saveStored() {
  wprefs.begin("wifi", false);
  int n = 0;
  for (auto &k : nets) {
    if (!k.stored || n >= 6) continue;
    wprefs.putString(("s" + String(n)).c_str(), k.ssid);
    wprefs.putString(("p" + String(n)).c_str(), k.pass);
    n++;
  }
  wprefs.putUChar("n", n);
  wprefs.end();
}

static void savePreferred(const String &sid) {
  if (sid == preferredSsid) return;
  preferredSsid = sid;
  wprefs.begin("wifi", false);
  wprefs.putString("pref", sid);
  wprefs.end();
  logf("wifi: remember %s (Mac found here)\n", sid.c_str());
}

static void updateStatus() {
  String st = "已存：";
  for (size_t i = 0; i < nets.size(); i++) st += (i ? "、" : "") + nets[i].ssid;
  st += "；当前：" + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("未连接"));
  netWifiStatus = st;
}

static void tell(const String &msg) {  // 在屏幕上说一句（复用字幕通道）
  if (netSayReady) return;
  strncpy(netSayBuf, msg.c_str(), sizeof(netSayBuf) - 1);
  netSayBuf[sizeof(netSayBuf) - 1] = 0;
  netSayHold = false;
  netSayReady = true;
}

// "名字|密码" 加/改；"del:名字" 删；"clear" 清掉手机/Mac 加的（config.h 里的不动）
static void handleWifiCmd(const char *c) {
  String cmd = String(c);
  cmd.trim();
  if (cmd == "list") {
    updateStatus();
    tell(netWifiStatus);
    return;
  }
  if (cmd == "clear") {
    nets.erase(std::remove_if(nets.begin(), nets.end(), [](const KnownNet &k) { return k.stored; }), nets.end());
    saveStored();
    tell("手机加的 WiFi 都删了。");
  } else if (cmd.startsWith("del:")) {
    String sid = cmd.substring(4);
    size_t before = nets.size();
    nets.erase(std::remove_if(nets.begin(), nets.end(), [&](const KnownNet &k) { return k.stored && k.ssid == sid; }), nets.end());
    saveStored();
    tell(nets.size() < before ? "删了 WiFi：" + sid : "没找到 " + sid + "（代码里写死的删不了）");
  } else {
    int bar = cmd.lastIndexOf('|');
    if (bar <= 0) { tell("格式是：WiFi名字|密码"); return; }
    String sid = cmd.substring(0, bar), pw = cmd.substring(bar + 1);
    bool found = false;
    for (auto &k : nets) if (k.ssid == sid) { k.pass = pw; k.stored = true; found = true; }
    if (!found) {
      int stored = 0;
      for (auto &k : nets) stored += k.stored;
      if (stored >= 6) { tell("已经存了 6 个了，先删一个（del:名字）"); return; }
      nets.push_back({ sid, pw, true });
    }
    saveStored();
    tell("记下 WiFi：" + sid);
    logf("wifi: added %s\n", sid.c_str());  // 不打印密码
  }
  updateStatus();
}
static WebSocketsClient ws;
static WebSocketsClient bws;  // 对话桥
static volatile uint32_t lastPong = 0;

// 语音：云端先发 "P<字节数>"，再分块发二进制 PCM，最后 "E"
static int16_t *clipBuf = nullptr;
static size_t clipLen = 0, clipPos = 0;

static void onWs(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      netOnline = true;
      lastPong = millis();
      logf("ws connected\n");
      break;
    case WStype_DISCONNECTED:
      if (netOnline) logf("ws disconnected\n");
      netOnline = false;
      break;
    case WStype_TEXT:
      if (len == 4 && memcmp(payload, "pong", 4) == 0) { lastPong = millis(); break; }
      if (len >= 2 && payload[0] == 'Q') {  // 云端的定时问候带编号：回执 "A<编号>"，云端才删掉存的那份
        char a[24];
        snprintf(a, sizeof(a), "A%.*s", (int)min(len - 1, (size_t)20), (const char *)payload + 1);
        ws.sendTXT(a);
        break;
      }
      if (len >= 2 && payload[0] == 'F' && payload[1] >= '0' && payload[1] <= '9') netPendingFace = payload[1] - '0';
      if (len >= 2 && payload[0] == 'M') { netFocusCmd = atoi((const char *)payload + 1); break; }
      if (len >= 2 && payload[0] == 'L' && !netAlarmCmd) {  // 闹钟 "L<hh>:<mm>|<掩码>" / "L0"
        char tmp[24];
        size_t n = min(len - 1, sizeof(tmp) - 1);
        memcpy(tmp, payload + 1, n);
        tmp[n] = 0;
        parseAlarmCmd(tmp);
        break;
      }
      if (len >= 2 && payload[0] == 'W' && !netTimerCmd) {  // 计时 "W<秒>|<内容>"
        char tmp[80];
        size_t n = min(len, sizeof(tmp) - 1);
        memcpy(tmp, payload, n);
        tmp[n] = 0;
        char *bar = strchr(tmp, '|');
        netTimerSec = (uint32_t)atol(tmp + 1);
        strncpy(netTimerLabel, bar ? bar + 1 : "", sizeof(netTimerLabel) - 1);
        netTimerLabel[sizeof(netTimerLabel) - 1] = 0;
        netTimerCmd = true;
        break;
      }
      if (len == 2 && payload[0] == 'K') netThinkingSince = payload[1] == '1' ? max((uint32_t)1, (uint32_t)millis()) : 0;
      if (len >= 2 && payload[0] == 'P') {
        if (clipBuf) free(clipBuf);
        clipLen = strtoul((const char *)payload + 1, nullptr, 10);
        clipPos = 0;
        clipBuf = clipLen && clipLen < 4 * 1024 * 1024 ? (int16_t *)ps_malloc(clipLen) : nullptr;
        if (!clipBuf) logf("clip alloc failed (%u)\n", (unsigned)clipLen);
        break;
      }
      if (len == 1 && payload[0] == 'E') {
        if (clipBuf && clipPos > 0) {
          audioPlay(clipBuf, clipPos);  // 交给播放任务，它播完会 free
          clipBuf = nullptr;
        }
        break;
      }
      if (len >= 2 && (payload[0] == 'S' || payload[0] == 'P')) netThinkingSince = 0;
      // "S" 普通字幕；"H" 挂着的字幕（定时问候：挂到点一下或 10 分钟）
      if (len >= 2 && (payload[0] == 'S' || payload[0] == 'H') && !netSayReady) {
        size_t n = min(len - 1, sizeof(netSayBuf) - 1);
        memcpy(netSayBuf, payload + 1, n);
        netSayBuf[n] = 0;
        netSayHold = payload[0] == 'H';
        netSayReady = true;
      }
      break;
    case WStype_BIN:
      if (clipBuf && clipPos + len <= clipLen) {
        memcpy((uint8_t *)clipBuf + clipPos, payload, len);
        clipPos += len;
      }
      break;
    default:
      break;
  }
}

// 对话桥发来的东西和云端是同一套协议（F/S/P/E/二进制），交给同一个处理函数
static void onBridge(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      netBridgeOnline = true;
      logf("bridge connected\n");
      break;
    case WStype_DISCONNECTED:
      if (netBridgeOnline) logf("bridge disconnected\n");
      netBridgeOnline = false;
      break;
    case WStype_TEXT:
    case WStype_BIN:
      onWs(type, payload, len);
      break;
    default:
      break;
  }
}

// 局域网里找 Mac：mDNS 解析 MAC_HOST，找到就连；连不上隔一会儿再找（Mac 可能换了网络/IP）
static void bridgeStep(uint32_t now) {
  static bool mdnsUp = false, started = false;
  static uint32_t lastTry = 0;
  static IPAddress ip;
  if (!mdnsUp) { mdnsUp = MDNS.begin("desk-companion"); }
  if (!netBridgeOnline && (lastTry == 0 || now - lastTry > (started ? 60000UL : 15000UL))) {
    lastTry = now;
    IPAddress found = MDNS.queryHost(MAC_HOST, 1500);
    if (found != IPAddress(0, 0, 0, 0) && (!started || found != ip)) {
      ip = found;
      if (started) bws.disconnect();
      bws.begin(ip.toString(), BRIDGE_PORT, "/board/" BOARD_TOKEN);
      bws.onEvent(onBridge);
      bws.setReconnectInterval(5000);
      bws.enableHeartbeat(15000, 3000, 2);
      started = true;
      logf("bridge at %s\n", ip.toString().c_str());
    }
  }
  if (started) bws.loop();
  micRecordEnabled = netBridgeOnline && netEarOn;

  // 录好的整句："U<字节数>" + 8KB 二进制块 + "u"
  Utterance u;
  if (audioTakeUtterance(u)) {
    if (netBridgeOnline) {
      char h[24];
      snprintf(h, sizeof(h), "%c%u", u.ptt ? 'V' : 'U', (unsigned)u.bytes);  // V = 按键说的，不用叫名字
      bws.sendTXT(h);
      for (size_t i = 0; i < u.bytes; i += 8192) bws.sendBIN((uint8_t *)u.buf + i, min((size_t)8192, u.bytes - i));
      bws.sendTXT("u");
      logf("sent %s %u bytes\n", u.ptt ? "ptt" : "utterance", (unsigned)u.bytes);
    } else if (u.ptt) {
      netBridgeMissed = true;  // 按键说了但 Mac 不在
    }
    free(u.buf);
  }
}

// 扫一遍，挑一个认识的网络连：优先上次找到 Mac 的，跳过这一轮试过但没找到 Mac 的
static std::vector<String> skipped;  // 这一轮试过、没找到 Mac 的
static uint32_t roundStart = 0;

static bool connectBest() {
  int n = WiFi.scanNetworks();
  if (n <= 0) { WiFi.scanDelete(); return false; }
  int best = -1, bestScore = -10000;
  String seen;
  for (int i = 0; i < n; i++) {
    String sid = WiFi.SSID(i);
    for (size_t k = 0; k < nets.size(); k++) {
      if (nets[k].ssid != sid) continue;
      bool skip = std::find(skipped.begin(), skipped.end(), sid) != skipped.end();
      int score = WiFi.RSSI(i) + (sid == preferredSsid ? 1000 : 0) - (skip ? 5000 : 0);
      if (score > bestScore) { bestScore = score; best = (int)k; }
    }
    if (i < 10) seen += " [" + sid + " " + String(WiFi.RSSI(i)) + "]";
  }
  WiFi.scanDelete();
  if (best < 0) {
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 120000) { lastLog = millis(); logf("wifi: no known network. visible:%s\n", seen.c_str()); }
    return false;
  }
  logf("wifi: connecting %s\n", nets[best].ssid.c_str());
  WiFi.begin(nets[best].ssid.c_str(), nets[best].pass.c_str());
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(250));  // 最多 15 秒
  return WiFi.status() == WL_CONNECTED;
}

static void netTask(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  loadNets();
  updateStatus();
  bool started = false;
  bool wasWifi = false;
  uint32_t lastPing = 0, lastTime = 0, lastBat = 0, lastTry = 0, connectedAt = 0;

  for (;;) {
    if (netWifiCmdReady) {  // 蓝牙 / 串口来的 WiFi 设置
      handleWifiCmd(netWifiCmd);
      netWifiCmdReady = false;
    }
    bool wifi = WiFi.status() == WL_CONNECTED;
    if (!wifi && (lastTry == 0 || millis() - lastTry > 8000)) {
      lastTry = millis();
      if (skipped.size() && millis() - roundStart > 600000) skipped.clear();  // 10 分钟后重新轮一遍
      wifi = connectBest();
    }
    // 连上 2 分钟还没找到 Mac，而附近还有别的认识的网络：换一个试（Mac 可能在那个网络上）
    if (wifi && !netBridgeOnline && connectedAt && millis() - connectedAt > 120000) {
      String cur = WiFi.SSID();
      int others = 0;
      for (auto &k : nets) if (k.ssid != cur && std::find(skipped.begin(), skipped.end(), k.ssid) == skipped.end()) others++;
      if (others && std::find(skipped.begin(), skipped.end(), cur) == skipped.end()) {
        if (skipped.empty()) roundStart = millis();
        skipped.push_back(cur);
        logf("wifi: no Mac on %s, trying another\n", cur.c_str());
        WiFi.disconnect();
        connectedAt = 0;
        lastTry = 0;
        continue;
      }
    }
    if (wifi && netBridgeOnline) { savePreferred(WiFi.SSID()); skipped.clear(); }
    if (wifi != wasWifi) {
      logf(wifi ? "wifi %s\n" : "wifi lost%s\n", wifi ? WiFi.SSID().c_str() : "");
      wasWifi = wifi;
      connectedAt = wifi ? millis() : 0;
      updateStatus();
    }
    if (wifi) {
      if (!started) {
        configTime(0, 0, "ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org");  // 系统时间按 UTC，本地时间自己加时区
        if (cloudOn) {
          ws.beginSSL(CLOUD_HOST, 443, "/ws/" BOARD_TOKEN);
          ws.onEvent(onWs);
          ws.setReconnectInterval(5000);
        }
        started = true;
      }
      if (cloudOn) ws.loop();
      uint32_t now = millis();
      bridgeStep(now);
      if (netOnline && netFocusDone > 0) {  // 完成一次专注："D<分钟>"
        char d[16];
        snprintf(d, sizeof(d), "D%d", netFocusDone);
        ws.sendTXT(d);
        netFocusDone = 0;
      }
      static bool resetReported = false;  // 开机后第一次连上云端：报上次重启原因
      if (netOnline && !resetReported) {
        char r[32];
        snprintf(r, sizeof(r), "R%s", netResetReason);
        ws.sendTXT(r);
        resetReported = true;
      }
      // 每分钟（以及刚连上时）报一次电量："B<百分比>,<充电中>,<插着USB>"
      if (netOnline && netBatPercent >= 0 && (lastBat == 0 || now - lastBat > 60000)) {
        char b[24];
        snprintf(b, sizeof(b), "B%d,%d,%d", netBatPercent, netBatCharging ? 1 : 0, netBatUsb ? 1 : 0);
        ws.sendTXT(b);
        lastBat = now;
      }
      if (!netOnline) lastBat = 0;
      if (netOnline && now - lastPing > 30000) {
        ws.sendTXT("ping");
        lastPing = now;
      }
      // 云端重新部署或网络切换时，连接可能"假活着"：70 秒没收到 pong 就主动断开重连
      if (netOnline && now - lastPong > 70000) {
        logf("ws stale, reconnecting\n");
        ws.disconnect();
        netOnline = false;
        lastPong = now;
      }
      // 每 10 分钟把系统时间交给主循环一次
      time_t t = time(nullptr);
      if (t > 1700000000 && (lastTime == 0 || now - lastTime > 600000)) {
        netLocalEpoch = (int64_t)t + TZ_OFFSET_HOURS * 3600;
        netTimeReady = true;
        lastTime = now;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void netBegin() {
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}
