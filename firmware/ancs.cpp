// iPhone 通知（ANCS）。参考 NimBLE-Arduino 的 ANCS 示例，改成单独的任务、不卡主循环。
// 流程：板子广播（声明要用 ANCS）→ iPhone 在蓝牙设置里点它（BLE_NAME）配对 → 订阅通知来源和数据来源
// → 有新通知时按类别过滤，要显示的再去问"哪个 App、标题是什么"（不问正文）→ 交给主循环。
#include "ancs.h"
#include "log.h"
#include "NimBLEDevice.h"
#include "net.h"
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

class Callbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &ci) override {
    connHandle = ci.getConnHandle();
    client = s->getClient(ci);
    needSetup = true;
    logf("ble: iphone connected\n");
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    connHandle = 0xFFFF;
    ancsConnected = false;
    client = nullptr;
    logf("ble: disconnected (%d)\n", reason);
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

static void ancsTask(void *) {
  NimBLERemoteCharacteristic *ctrl = nullptr;
  for (;;) {
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
// 读写都要求已配对（加密）：店里别人装了 LightBlue 也改不了
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

void ancsBegin() {
  pendQ = xQueueCreate(6, sizeof(Pending));
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setSecurityAuth(true, false, true);          // 绑定 + 安全连接，不用输配对码
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);  // iPhone 上点"配对"就行
  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(&callbacks);
  srv->advertiseOnDisconnect(true);
  NimBLEService *ws = srv->createService(WIFI_SVC);
  NimBLECharacteristic *wc = ws->createCharacteristic(
      WIFI_CHR, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  wc->setCallbacks(&wifiCallbacks);
  wc->createDescriptor("2901")->setValue("WiFi设置：写 名字|密码");  // LightBlue 里显示的名字
  ws->start();
  NimBLEAdvertising *adv = srv->getAdvertising();
  NimBLEAdvertisementData ad;
  ad.setFlags(0x06);
  ad.addServiceUUID(ANCS_SVC);  // 声明要用 ANCS，iPhone 才会把通知给它
  adv->setAdvertisementData(ad);
  NimBLEAdvertisementData sr;
  sr.setName(BLE_NAME);
  adv->setScanResponseData(sr);
  adv->start();
  xTaskCreatePinnedToCore(ancsTask, "ancs", 6144, nullptr, 1, nullptr, 0);
  logf("ble: advertising as %s\n", BLE_NAME);
}
