#pragma once
#include <Arduino.h>

// QMI8658 加速度：判断放着不动 / 被拿起 / 摇晃 / 屏幕朝下，以及拿在手里时的倾斜
struct MotionState {
  bool ok = false;
  float gx = 0, gy = 0, gz = 0;  // 低通后的重力方向（g）
  float activity = 0;            // 晃动程度
  uint32_t stillSince = 0;       // 从什么时候开始一直没动
  bool pickedUp = false;         // 事件：静止很久后被拿起（读后清零）
  bool shaken = false;           // 事件：被用力摇（读后清零）
  bool faceDown = false;         // 屏幕朝下扣着超过 3 秒
  bool flat = false;             // 平放（正面朝上或朝下）
};
extern MotionState mot;
extern bool motionLog;           // 串口发 A 切换：每秒打印一次加速度，校准方向用

void motionBegin();
void motionStep(uint32_t now);
