#include "motion.h"
#include "log.h"
#include <Wire.h>
#include "SensorQMI8658.hpp"
#include "pin_config.h"

MotionState mot;
bool motionLog = false;
static SensorQMI8658 qmi;
static bool gyroOk = false;

void motionBegin() {
  if (!qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    logf("imu not found\n");
    return;
  }
  // 和官方示例同样的配置（125Hz 时 enable 不成功）
  int c = qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_1000Hz, SensorQMI8658::LPF_MODE_0);
  bool e = qmi.enableAccelerometer();
  logf("imu config %d enable %d\n", c, e);
  int gc = qmi.configGyroscope(SensorQMI8658::GYR_RANGE_1024DPS, SensorQMI8658::GYR_ODR_896_8Hz, SensorQMI8658::LPF_MODE_3);
  gyroOk = qmi.enableGyroscope();
  logf("gyro config %d enable %d\n", gc, gyroOk);
  mot.ok = true;
  mot.stillSince = millis();
  logf("imu ok\n");
}

void motionLowPower(bool on) {
  if (!mot.ok) return;
  static bool off = false;
  if (on == off) return;
  off = on;
  if (on) qmi.disableGyroscope();
  else gyroOk = qmi.enableGyroscope();
  mot.spin = 0; mot.rateZ = 0;
  logf("gyro %s\n", on ? "off (deep sleep)" : "on");
}

// 屏幕法线的轴和朝向（实测后再定；+1 = 正面朝上时 gz 为正）
#ifndef SCREEN_Z_SIGN
#define SCREEN_Z_SIGN 1
#endif

void motionStep(uint32_t now) {
  if (!mot.ok) return;  // getDataReady() 在 125Hz 下一直是 false，直接按帧读
  float x, y, z;
  if (!qmi.getAccelerometer(x, y, z)) return;

  mot.tiltY = mot.tiltY * 0.6f + y * 0.4f;
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

  // 掉落：总加速度接近 0（失重）连续两帧 → 开始掉；之后受力回来（>0.6g）= 被接住 / 落地
  static uint8_t lowN = 0;
  static bool falling = false;
  static uint32_t fallAt = 0, lastFall = 0;
  float mag = sqrtf(x * x + y * y + z * z);
  // 摇晃时手往下一甩也会"失重"，跟掉落分不开（只加"之前没晃"的条件不够）。
  // 所以落地后再判断：落地后 0.5 秒内还有 ≥2 次大偏差（>1.2g）= 是在摇，不算；安静下来 = 真掉落，才报 dropped + landed
  static uint32_t pendingSince = 0;
  static uint32_t lastShakeAt = 0;
  static int bigAfter = 0;
  if (mag < 0.35f) {
    if (++lowN >= 3 && !falling && !pendingSince && now - lastFall > 2000) { falling = true; fallAt = now; }
  } else {
    lowN = 0;
    if (falling && mag > 0.6f) { falling = false; pendingSince = now; bigAfter = 0; }
  }
  if (falling && now - fallAt > 1500) { falling = false; pendingSince = now; bigAfter = 0; }  // 保险
  if (pendingSince) {
    if (now - pendingSince > 120 && dev > 0.9f) bigAfter++;  // 落地那一下本身的冲击不算
    if (bigAfter >= 2) {  // 还在晃 = 是在摇：直接按摇晃算
      pendingSince = 0;
      if (now - lastShakeAt > 3000) { mot.shaken = true; lastShakeAt = now; }
      logf("zero-g was a shake, not a drop\n");
    } else if (now - pendingSince > 500) {
      pendingSince = 0;
      lastFall = now;
      mot.dropped = true;
      mot.landed = true;
      logf("drop confirmed (fell %lums)\n", now - fallAt);
    }
  }

  // 陀螺仪：每个轴分别累计"带方向"的转角，往同一个方向转才会越加越多；来回扭（摇晃时手腕在扭）会互相抵消。
  // 任一轴超过 540°（一圈半）= 转晕了（不分方向累加的话，摇几下就会被当成转晕）
  static uint32_t lastGyro = 0;
  static float ang[3] = { 0, 0, 0 };
  static uint32_t lastDizzy = 0;
  float wx, wy, wz;
  if (gyroOk && qmi.getGyroscope(wx, wy, wz)) {
    float dt = lastGyro ? constrain((now - lastGyro) / 1000.0f, 0.0f, 0.1f) : 0;
    lastGyro = now;
    float w = sqrtf(wx * wx + wy * wy + wz * wz);
    mot.spin = w;
    mot.rateZ = wz;
    float ws[3] = { wx, wy, wz };
    for (int k = 0; k < 3; k++) {
      if (fabsf(ang[k]) > 270) mot.lastSpinAt = now;  // 一直往一个方向转了大半圈：在转，不算摇
      if (fabsf(ws[k]) > 120) ang[k] += ws[k] * dt;
      else ang[k] *= powf(0.3f, dt);  // 慢下来就慢慢归零
    }
    if ((fabsf(ang[0]) > 540 || fabsf(ang[1]) > 540 || fabsf(ang[2]) > 540) && now - lastDizzy > 4000) {
      mot.dizzy = true; lastDizzy = now;
      ang[0] = ang[1] = ang[2] = 0;
      mot.lastSpinAt = now;
      logf("dizzy\n");
    }
  }

  // 摇晃：1 秒内偏差 >1.1g ≥ 5 次。0.8g×6 太敏感（还没使劲摇就嫌弃），1.4g×8 又太迟钝（摇了好几次都不算）
  // 转圈和掉落时受力也大：转的时候、掉落前后 2 秒内不算摇
  static uint8_t hits = 0;
  static uint32_t hitWindow = 0, lastShake = 0;
  if (now - hitWindow > 1000) { hitWindow = now; hits = 0; }
  bool quiet = (mot.lastSpinAt && now - mot.lastSpinAt < 1500) || (lastFall && now - lastFall < 2000);
  if (dev > 1.1f && ++hits >= 5 && now - lastShake > 3000 && !quiet && now - lastShakeAt > 3000) { mot.shaken = true; lastShake = lastShakeAt = now; hits = 0; }

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
