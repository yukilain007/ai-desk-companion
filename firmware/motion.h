#pragma once
#include <Arduino.h>

// QMI8658 加速度 + 陀螺仪：判断放着不动 / 被拿起 / 摇晃 / 屏幕朝下 / 掉落 / 被转晕，以及拿在手里时的倾斜
struct MotionState {
  bool ok = false;
  float gx = 0, gy = 0, gz = 0;  // 低通后的重力方向（g）
  float activity = 0;            // 晃动程度
  uint32_t stillSince = 0;       // 从什么时候开始一直没动
  bool pickedUp = false;         // 事件：静止很久后被拿起（读后清零）
  bool shaken = false;           // 事件：被用力摇（读后清零）
  bool faceDown = false;         // 屏幕朝下扣着超过 3 秒
  bool flat = false;             // 平放（正面朝上或朝下）
  float tiltY = 0;               // 屏幕左右方向的倾斜（快速版，游戏用；向左歪为负）
  bool dropped = false;          // 事件：开始自由落体（失重）（读后清零）
  bool landed = false;           // 事件：掉落后被接住 / 落地（读后清零）
  bool dizzy = false;            // 事件：被转了一圈半以上（读后清零）
  float spin = 0;                // 现在转得多快（度/秒，三个轴合起来）
  float rateZ = 0;               // 绕屏幕法线转的速度（度/秒，平放在桌上拧它）
  uint32_t lastSpinAt = 0;       // 最近一次在转的时间（转的时候不算"摇"）
};
extern MotionState mot;
extern bool motionLog;           // 串口发 A 切换：每秒打印一次加速度，校准方向用

void motionBegin();
void motionStep(uint32_t now);
void motionLowPower(bool on);    // 深睡时关陀螺仪省电（拿起靠加速度计，照样能叫醒）
