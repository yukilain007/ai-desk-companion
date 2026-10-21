// 电源芯片 AXP2101：读电量、是否在充电、是否插着 USB。只读不改充电设置。
#include "power.h"
#include "log.h"
#include <Wire.h>
#include "pin_config.h"  // 里面定义了 XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

static XPowersPMU pmu;
static bool pmuOk = false;

void powerBegin() {
  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!pmuOk) {
    logf("pmu not found\n");
    return;
  }
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
  // 开机键要按住 2 秒才开机（原来一碰就开，放口袋里会误开机）。存在电源芯片里，接着电池就一直有效
  uint8_t before = pmu.getPowerKeyPressOnTime();
  pmu.setPowerKeyPressOnTime(XPOWERS_POWERON_2S);
  logf("pmu ok (power-on press: %d -> %d)\n", before, pmu.getPowerKeyPressOnTime());
}

PowerState powerRead() {
  PowerState s;
  if (!pmuOk) return s;
  s.ok = true;
  s.battery = pmu.isBatteryConnect();
  s.percent = s.battery ? pmu.getBatteryPercent() : -1;
  s.charging = pmu.isCharging();
  s.usb = pmu.isVbusIn();
  s.mv = pmu.getBattVoltage();
  return s;
}
