#pragma once
#include <Arduino.h>

struct PowerState {
  bool ok = false;       // 电源芯片在线
  bool battery = false;  // 接了电池
  int percent = -1;      // 电量 %
  bool charging = false;
  bool usb = false;      // 插着 USB
  int mv = 0;            // 电池电压
};

void powerBegin();
PowerState powerRead();
