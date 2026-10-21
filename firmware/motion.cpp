#include "motion.h"
#include "log.h"
#include <Wire.h>
#include "SensorQMI8658.hpp"
#include "pin_config.h"

MotionState mot;
bool motionLog = false;
static SensorQMI8658 qmi;

void motionBegin() {
  if (!qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    logf("imu not found\n");
    return;
  }
  // 和官方示例同样的配置（125Hz 时 enable 不成功）
  int c = qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_1000Hz, SensorQMI8658::LPF_MODE_0);
  bool e = qmi.enableAccelerometer();
  logf("imu config %d enable %d\n", c, e);
  mot.ok = true;
  mot.stillSince = millis();
  logf("imu ok\n");
}

// 屏幕法线的轴和朝向（实测后再定；+1 = 正面朝上时 gz 为正）
#ifndef SCREEN_Z_SIGN
#define SCREEN_Z_SIGN 1
#endif

void motionStep(uint32_t now) {
  if (!mot.ok) return;  // getDataReady() 在 125Hz 下一直是 false，直接按帧读
  float x, y, z;
  if (!qmi.getAccelerometer(x, y, z)) return;

  static bool first = true;
  if (first) { mot.gx = x; mot.gy = y; mot.gz = z; first = false; }
  float dx = x - mot.gx, dy = y - mot.gy, dz = z - mot.gz;
  float dev = sqrtf(dx * dx + dy * dy + dz * dz);  // 和慢速重力的偏差 = 在动
  mot.gx += dx * 0.08f; mot.gy += dy * 0.08f; mot.gz += dz * 0.08f;
  mot.activity = mot.activity * 0.85f + dev * 0.15f;

  // 拿起：静止 8 秒以上后开始动，接下来 1.5 秒内晃动够大
  static uint32_t movingSince = 0, armedUntil = 0;
  if (mot.activity < 0.035f) {
    if (movingSince) { movingSince = 0; mot.stillSince = now; }
  } else {
    if (!movingSince) {
      movingSince = now;
      if (now - mot.stillSince > 8000) armedUntil = now + 1500;
    }
    mot.stillSince = now;
  }
  if (armedUntil && now < armedUntil && mot.activity > 0.06f) { mot.pickedUp = true; armedUntil = 0; }
  if (armedUntil && now >= armedUntil) armedUntil = 0;

  // 摇晃：1 秒内大幅偏差（>1.4g）≥ 8 次。原来 0.8g×6 次太敏感，还没使劲摇就嫌弃了
  static uint8_t hits = 0;
  static uint32_t hitWindow = 0, lastShake = 0;
  if (now - hitWindow > 1000) { hitWindow = now; hits = 0; }
  if (dev > 1.4f && ++hits >= 8 && now - lastShake > 3000) { mot.shaken = true; lastShake = now; hits = 0; }

  float up = mot.gz * SCREEN_Z_SIGN;
  mot.flat = fabsf(mot.gz) > 0.85f;
  static uint32_t downSince = 0;
  if (up < -0.85f) { if (!downSince) downSince = now; }
  else downSince = 0;
  bool wasDown = mot.faceDown;
  mot.faceDown = downSince && now - downSince > 3000;
  if (mot.faceDown != wasDown) logf("faceDown %d (gz %.2f)\n", mot.faceDown, mot.gz);

  static uint32_t lastLog = 0;
  if (motionLog && now - lastLog > 1000) {
    lastLog = now;
    logf("acc %.2f %.2f %.2f act %.3f still %lus\n", mot.gx, mot.gy, mot.gz, mot.activity, (now - mot.stillSince) / 1000);
  }
}
