#pragma once
#include <Arduino.h>

// iPhone 通知（ANCS，手表用的那个通道）：板子在 iPhone 蓝牙里叫 BUDDY_NAME（settings.h），配对后自动重连。
// 只挑电话、未接来电、微信，只显示是谁，不显示内容。主循环只读下面这几个。
extern volatile bool ancsReady;      // 有一条要显示
extern char ancsText[96];            // 要显示的话（"<名字>，来电话了：xxx"）
extern volatile bool ancsIsCall;     // 是来电（要响铃）
extern volatile bool ancsCallGone;   // 来电结束了（接了/挂了）
extern volatile bool ancsConnected;  // iPhone 连着

// ---- Claude 桌面端 Hardware Buddy（蓝牙 Nordic UART）----
extern volatile bool buddyConnected;      // Mac 上的 Claude 桌面端连着（30 秒内收到过心跳）
extern volatile uint32_t buddyLastBeat;   // 最近一次心跳（millis）
extern volatile int buddyTotal, buddyRunning, buddyWaiting;
extern char buddyMsg[80];
extern char buddyPromptId[64], buddyTool[32], buddyHint[96];  // 需要批准时；没有时 buddyPromptId[0] == 0
extern volatile uint32_t buddyPromptSeq;  // 每来一个新的批准请求 +1
extern volatile int buddyDecision;        // 主循环设：1 = 批准一次，2 = 拒绝（蓝牙任务发出去后清零）
extern volatile uint32_t buddyPasskey, buddyPasskeySeq;  // 配对时要在屏幕上显示的 6 位数
extern volatile int64_t buddyTimeEpoch;   // 桌面端发来的本地时间（0 = 没有）

void ancsBegin();
void ancsDebug(bool restart);
void ancsLowPower(bool on);  // 深睡：把和 iPhone 的连接间隔放宽到 0.5–0.7 秒（通知晚一两秒到），醒来恢复
