// iPhone 通知（ANCS）。参考 NimBLE-Arduino 的 ANCS 示例，改成单独的任务、不卡主循环。
// 流程：板子广播（声明要用 ANCS）→ iPhone 在蓝牙设置里点它（BUDDY_NAME）配对 → 订阅通知来源和数据来源
// → 有新通知时按类别过滤，要显示的再去问"哪个 App、标题是什么"（不问正文）→ 交给主循环。
#include "ancs.h"
#include "log.h"
#include "NimBLEDevice.h"
#include "net.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include "settings.h"

volatile bool ancsReady = false;
char ancsText[96];
volatile bool ancsIsCall = false;
volatile bool ancsCallGone = false;
volatile bool ancsConnected = false;

static NimBLEUUID ANCS_SVC("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static NimBLEUUID NOTIF_SRC("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static NimBLEUUID CTRL_PT("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static NimBLEUUID DATA_SRC("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");

enum : uint8_t { CAT_OTHER = 0, CAT_CALL = 1, CAT_MISSED = 2, CAT_SOCIAL = 4 };

struct Pending { uint32_t uid; uint8_t cat; };
static QueueHandle_t pendQ;
static volatile uint16_t connHandle = 0xFFFF;
static NimBLEClient *volatile client = nullptr;
static volatile bool needSetup = false;
static volatile uint32_t callUid = 0xFFFFFFFF;

// 数据来源的回包：CommandID(1) UID(4) 然后 [AttrID(1) Len(2) 数据]...
static uint8_t dsBuf[512];
static size_t dsLen = 0;
static volatile bool dsDone = false;

static void onDataSource(NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) {
  if (dsLen + n > sizeof(dsBuf)) dsLen = 0;
  memcpy(dsBuf + dsLen, d, n);
  dsLen += n;
  // 我们只要 AppIdentifier(0) 和 Title(1) 两个属性：解析到两个就算收齐
  size_t p = 5;
  int got = 0;
  while (p + 3 <= dsLen) {
    uint16_t len = dsBuf[p + 1] | (dsBuf[p + 2] << 8);
    if (p + 3 + len > dsLen) break;
    p += 3 + len;
    got++;
  }
  if (got >= 2) dsDone = true;
}

static void onNotifSource(NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) {
  if (n < 8) return;
  uint8_t event = d[0], cat = d[2];
  uint32_t uid = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
  if (event == 2 && uid == callUid) {  // 来电那条被移除：接了或挂了
    ancsCallGone = true;
    callUid = 0xFFFFFFFF;
    return;
  }
  if (event != 0) return;  // 只管新通知
  if (d[1] & 0x04) return; // PreExisting：配对时手机里原有的旧通知，不报
  if (cat == CAT_CALL || cat == CAT_MISSED || cat == CAT_SOCIAL) {
    Pending pd = { uid, cat };
    xQueueSend(pendQ, &pd, 0);
  }
}

// ---- Claude 桌面端 Hardware Buddy ----
volatile bool buddyConnected = false;
volatile uint32_t buddyLastBeat = 0;
volatile int buddyTotal = 0, buddyRunning = 0, buddyWaiting = 0;
char buddyMsg[80] = "";
char buddyPromptId[64] = "", buddyTool[32] = "", buddyHint[96] = "";
volatile uint32_t buddyPromptSeq = 0;
volatile int buddyDecision = 0;
volatile uint32_t buddyPasskey = 0, buddyPasskeySeq = 0;
volatile int64_t buddyTimeEpoch = 0;
static volatile uint16_t buddyHandle = 0xFFFF;
static NimBLECharacteristic *nusTx = nullptr;
static QueueHandle_t lineQ;
static int statAppr = 0, statDeny = 0;

class Callbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &ci) override {
    if (!ancsConnected) {  // 还没有 iPhone：这个连接可能是 iPhone，试试 ANCS（Mac 上没有，会失败，不影响）
      connHandle = ci.getConnHandle();
      client = s->getClient(ci);
      needSetup = true;
    }
    logf("ble: connected (%d now)\n", s->getConnectedCount());
    if (s->getConnectedCount() < 3) NimBLEDevice::startAdvertising();  // iPhone 和 Mac 可以同时连
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &ci, int reason) override {
    if (ci.getConnHandle() == connHandle) {
      connHandle = 0xFFFF;
      ancsConnected = false;
      client = nullptr;
    }
    if (ci.getConnHandle() == buddyHandle) { buddyHandle = 0xFFFF; buddyConnected = false; }
    logf("ble: disconnected (%d)\n", reason);
  }
  uint32_t onPassKeyDisplay() override {  // 配对：屏幕上显示 6 位数，在 Mac / iPhone 上输入
    uint32_t pk = 100000 + esp_random() % 900000;
    buddyPasskey = pk;
    buddyPasskeySeq++;
    return pk;
  }
  void onAuthenticationComplete(NimBLEConnInfo &ci) override {
    logf("ble: %s\n", ci.isEncrypted() ? "paired/encrypted" : "pairing failed");
  }
} callbacks;

// 取出某个属性（id）的字符串
static String attr(uint8_t id) {
  size_t p = 5;
  while (p + 3 <= dsLen) {
    uint8_t aid = dsBuf[p];
    uint16_t len = dsBuf[p + 1] | (dsBuf[p + 2] << 8);
    if (p + 3 + len > dsLen) break;
    if (aid == id) return String((const char *)dsBuf + p + 3).substring(0, len);
    p += 3 + len;
  }
  return "";
}

// Nordic UART：桌面端写进来的是一行一行的 JSON，攒到 \n 再交给任务处理
class NusRx : public NimBLECharacteristicCallbacks {
  String buf;
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &ci) override {
    buddyHandle = ci.getConnHandle();
    std::string v = c->getValue();
    for (char ch : v) {
      if (ch == '\n') {
        if (buf.length()) { char *line = strdup(buf.c_str()); if (xQueueSend(lineQ, &line, 0) != pdTRUE) free(line); }
        buf = "";
      } else if (buf.length() < 6000) buf += ch;
    }
  }
} nusRx;

static void buddySend(const String &line) {  // 按 MTU 切开发
  uint16_t h = buddyHandle;
  if (!nusTx || h == 0xFFFF) return;
  String l = line + "\n";
  NimBLEServer *srv = NimBLEDevice::getServer();
  size_t mtu = srv ? srv->getPeerMTU(h) : 23;
  size_t step = mtu > 3 ? mtu - 3 : 20;
  for (size_t i = 0; i < l.length(); i += step) nusTx->notify((const uint8_t *)l.c_str() + i, min(step, l.length() - i), h);
}

static void buddyLine(const char *line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  if (doc["time"].is<JsonArray>()) buddyTimeEpoch = (int64_t)doc["time"][0].as<long long>() + doc["time"][1].as<long>();
  const char *cmd = doc["cmd"] | "";
  if (*cmd) {
    String c = cmd;
    if (c == "char_begin" || c == "file" || c == "chunk" || c == "file_end" || c == "char_end") return;  // 不收推送的文件夹：不回，桌面端会超时
    JsonDocument ack;
    ack["ack"] = c;
    ack["ok"] = true;
    ack["n"] = 0;
    if (c == "status") {
      JsonObject d = ack["data"].to<JsonObject>();
      d["name"] = BUDDY_NAME;
      NimBLEServer *srv = NimBLEDevice::getServer();
      d["sec"] = srv && buddyHandle != 0xFFFF && srv->getPeerInfoByHandle(buddyHandle).isEncrypted();
      if (netBatPercent >= 0) { d["bat"]["pct"] = netBatPercent; d["bat"]["usb"] = (bool)netBatUsb; }
      d["sys"]["up"] = millis() / 1000;
      d["sys"]["heap"] = ESP.getFreeHeap();
      d["stats"]["appr"] = statAppr;
      d["stats"]["deny"] = statDeny;
    } else if (c == "unpair") {  // 只删 Mac 这一个的配对，iPhone 的留着
      NimBLEServer *srv = NimBLEDevice::getServer();
      if (srv && buddyHandle != 0xFFFF) NimBLEDevice::deleteBond(srv->getPeerInfoByHandle(buddyHandle).getIdAddress());
    }
    String out;
    serializeJson(ack, out);
    buddySend(out);
    return;
  }
  if (doc["total"].is<int>()) {  // 心跳
    buddyLastBeat = millis();
    buddyConnected = true;
    buddyTotal = doc["total"] | 0;
    buddyRunning = doc["running"] | 0;
    buddyWaiting = doc["waiting"] | 0;
    strlcpy(buddyMsg, doc["msg"] | "", sizeof(buddyMsg));
    const char *id = doc["prompt"]["id"] | "";
    if (*id && strcmp(id, buddyPromptId)) {
      strlcpy(buddyTool, doc["prompt"]["tool"] | "", sizeof(buddyTool));
      strlcpy(buddyHint, doc["prompt"]["hint"] | "", sizeof(buddyHint));
      strlcpy(buddyPromptId, id, sizeof(buddyPromptId));
      buddyPromptSeq++;
      logf("buddy: prompt %s\n", buddyTool);
    } else if (!*id) buddyPromptId[0] = 0;
  }
}

static void buddyStep() {
  static uint32_t lastAdvCheck = 0;  // 连着一个设备时也要继续广播，另一个（iPhone / Mac）才连得进来
  if (millis() - lastAdvCheck > 3000) {
    lastAdvCheck = millis();
    NimBLEServer *srv = NimBLEDevice::getServer();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (srv && adv && srv->getConnectedCount() < 3 && !adv->isAdvertising()) adv->start();
  }
  char *line;
  while (xQueueReceive(lineQ, &line, 0) == pdTRUE) { buddyLine(line); free(line); }
  if (buddyConnected && millis() - buddyLastBeat > 30000) buddyConnected = false;
  int d = buddyDecision;
  if (d && buddyPromptId[0]) {
    JsonDocument j;
    j["cmd"] = "permission";
    j["id"] = buddyPromptId;
    j["decision"] = d == 1 ? "once" : "deny";
    String out;
    serializeJson(j, out);
    buddySend(out);
    if (d == 1) statAppr++; else statDeny++;
    logf("buddy: %s\n", d == 1 ? "approved" : "denied");
    buddyPromptId[0] = 0;
  }
  buddyDecision = 0;
}

static void ancsTask(void *) {
  NimBLERemoteCharacteristic *ctrl = nullptr;
  for (;;) {
    buddyStep();
    if (needSetup && client) {
      needSetup = false;
      vTaskDelay(pdMS_TO_TICKS(800));
      // 等的这 800ms 里 iPhone 可能已经断开（client 被清空）：会空指针崩溃
      NimBLEClient *c = client;
      if (!c || !c->isConnected()) { logf("ble: gone before setup\n"); continue; }
      NimBLERemoteService *svc = c->getService(ANCS_SVC);  // 访问受保护的服务时 iPhone 会弹配对
      if (svc) {
        NimBLERemoteCharacteristic *ns = svc->getCharacteristic(NOTIF_SRC);
        NimBLERemoteCharacteristic *ds = svc->getCharacteristic(DATA_SRC);
        ctrl = svc->getCharacteristic(CTRL_PT);
        if (ns && ds && ctrl && ds->subscribe(true, onDataSource) && ns->subscribe(true, onNotifSource)) {
          ancsConnected = true;
          logf("ble: ancs ready\n");
        } else {
          logf("ble: ancs subscribe failed\n");
        }
      } else {
        logf("ble: no ancs service (not paired yet?)\n");
      }
    }
    Pending pd;
    if (ancsConnected && ctrl && xQueueReceive(pendQ, &pd, pdMS_TO_TICKS(50)) == pdTRUE) {
      // 问 App 是谁、标题（来电人 / 微信发消息的人），标题最多 40 字节；不问正文
      uint8_t req[9] = { 0, (uint8_t)pd.uid, (uint8_t)(pd.uid >> 8), (uint8_t)(pd.uid >> 16), (uint8_t)(pd.uid >> 24),
                         0 /*AppIdentifier*/, 1 /*Title*/, 40, 0 /*Title 最长 40 字节*/ };
      dsLen = 0;
      dsDone = false;
      if (ctrl->writeValue(req, 9, true)) {
        for (int i = 0; i < 40 && !dsDone; i++) vTaskDelay(pdMS_TO_TICKS(25));
      }
      String app = attr(0), title = attr(1);
      title.trim();
      String msg;
      if (pd.cat == CAT_CALL) msg = USER_NAME "，来电话了" + (title.length() ? "：" + title : String(""));
      else if (pd.cat == CAT_MISSED) msg = USER_NAME "，有个未接来电" + (title.length() ? "：" + title : String(""));
      else if (app == "com.tencent.xin") msg = USER_NAME "，微信：" + (title.length() ? title : String("有新消息"));
      if (msg.length() && !ancsReady) {
        strncpy(ancsText, msg.c_str(), sizeof(ancsText) - 1);
        ancsText[sizeof(ancsText) - 1] = 0;
        ancsIsCall = pd.cat == CAT_CALL;
        if (ancsIsCall) callUid = pd.uid;
        ancsReady = true;
        logf("ble: notify cat %d app %s\n", pd.cat, app.c_str());
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// ---------- 蓝牙配 WiFi ----------
// LightBlue 里连上板子 → "WiFi 设置"这一项：写 "名字|密码" 加网络、"del:名字" 删、"clear" 清空；读出来是已存的和当前连的。
// 读写都要求已配对（加密）：在公开场合别人装了 LightBlue 也改不了
static NimBLEUUID NUS_SVC("6e400001-b5a3-f393-e0a9-e50e24dcca9e"), NUS_RX("6e400002-b5a3-f393-e0a9-e50e24dcca9e"), NUS_TX("6e400003-b5a3-f393-e0a9-e50e24dcca9e");
static NimBLEUUID WIFI_SVC("B8E0A001-5C3D-4F2A-9E1B-7A2C3D4E5F60");
static NimBLEUUID WIFI_CHR("B8E0A002-5C3D-4F2A-9E1B-7A2C3D4E5F60");

class WifiCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    std::string v = c->getValue();
    if (v.empty() || netWifiCmdReady) return;
    size_t n = std::min(v.size(), sizeof(netWifiCmd) - 1);
    memcpy(netWifiCmd, v.data(), n);
    netWifiCmd[n] = 0;
    netWifiCmdReady = true;
    logf("ble: wifi command received\n");  // 不打印内容（有密码）
  }
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    c->setValue(netWifiStatus.c_str());
  }
} wifiCallbacks;

void ancsLowPower(bool on) {
  uint16_t h = connHandle;
  if (h == 0xFFFF) return;
  NimBLEServer *srv = NimBLEDevice::getServer();
  if (!srv) return;
  // 单位：间隔 1.25ms，超时 10ms。iOS 要求：间隔 ≤ 2 秒，间隔 ×（延迟 + 1）≤ 6 秒，超时 ≥ 间隔 ×（延迟 + 1）× 3
  if (on) srv->updateConnParams(h, 384, 576, 2, 700);  // 480–720ms，可跳过 2 次
  else srv->updateConnParams(h, 24, 40, 0, 400);       // 30–50ms
  logf("ble: conn params %s\n", on ? "slow (deep sleep)" : "normal");
}

void ancsDebug(bool restart) {  // 串口 "G?" / "G!"：看蓝牙广播状态 / 重新开广播
  NimBLEServer *srv = NimBLEDevice::getServer();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (restart) { adv->stop(); bool ok = adv->start(); logf("ble: re-advertise %d\n", ok); }
  logf("ble: advertising=%d connected=%d bonds=%d ancs=%d buddyHandle=%d\n", adv->isAdvertising(), srv ? srv->getConnectedCount() : -1,
       NimBLEDevice::getNumBonds(), (int)ancsConnected, (int)buddyHandle);
}

void ancsBegin() {
  pendQ = xQueueCreate(6, sizeof(Pending));
  NimBLEDevice::init(BUDDY_NAME);  // 正式名字也要以 Claude 开头：Mac 配对后会记住这个名字，桌面端按它过滤
  // 换一个固定的随机静态地址（存 NVS）：升级前 Mac / iPhone 记住的是旧地址下不带 Claude 的旧名字，桌面端会把它过滤掉。
  // 新地址 = 全新设备，名字重新读；旧配对作废，第一次换地址时清掉（iPhone 要重新配对一次）
  {
    Preferences bp;
    bp.begin("ble", false);
    uint8_t addr[6];
    if (bp.getBytesLength("addr") == 6) bp.getBytes("addr", addr, 6);
    else {
      esp_fill_random(addr, 6);
      addr[5] |= 0xC0;  // 随机静态地址：最高两位为 1（NimBLE 地址是小端，addr[5] 是最高字节）
      bp.putBytes("addr", addr, 6);
      NimBLEDevice::deleteAllBonds();
      logf("ble: new random address, bonds cleared\n");
    }
    bp.end();
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    NimBLEDevice::setOwnAddr(addr);
  }
  NimBLEDevice::setSecurityAuth(true, false, true);          // 绑定 + 安全连接，不用输配对码
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);  // 配对码显示在屏幕上（Claude 桌面端要求加密配对；已配对的 iPhone 不受影响）
  NimBLEDevice::setMTU(247);
  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(&callbacks);
  srv->advertiseOnDisconnect(true);
  NimBLEService *ws = srv->createService(WIFI_SVC);
  NimBLECharacteristic *wc = ws->createCharacteristic(
      WIFI_CHR, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  wc->setCallbacks(&wifiCallbacks);
  wc->createDescriptor("2901")->setValue("WiFi设置：写 名字|密码");  // LightBlue 里显示的名字
  ws->start();
  // Claude 桌面端 Hardware Buddy：Nordic UART，读写都要加密
  lineQ = xQueueCreate(8, sizeof(char *));
  NimBLEService *nus = srv->createService(NUS_SVC);
  NimBLECharacteristic *rx = nus->createCharacteristic(NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
  rx->setCallbacks(&nusRx);
  nusTx = nus->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
  nus->start();
  NimBLEAdvertising *adv = srv->getAdvertising();
  NimBLEAdvertisementData ad;
  ad.setFlags(0x06);
  ad.addServiceUUID(ANCS_SVC);  // 声明要用 ANCS，iPhone 才会把通知给它
  adv->setAdvertisementData(ad);
  NimBLEAdvertisementData sr;
  sr.addServiceUUID(NUS_SVC);  // Claude 桌面端按 Nordic UART + 名字以 Claude 开头来找设备
  sr.setName(BUDDY_ADV_NAME);
  adv->setScanResponseData(sr);
  bool advOk = adv->start();
  logf("ble: adv start %d\n", advOk);
  xTaskCreatePinnedToCore(ancsTask, "ancs", 6144, nullptr, 1, nullptr, 0);
  logf("ble: advertising as %s\n", BUDDY_NAME);
}
