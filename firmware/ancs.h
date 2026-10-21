#pragma once
#include <Arduino.h>

// iPhone 通知（ANCS，手表用的那个通道）：板子在 iPhone 蓝牙里叫 BLE_NAME（config.h），配对后自动重连。
// 只挑电话、未接来电、微信，只显示是谁，不显示内容。主循环只读下面这几个。
extern volatile bool ancsReady;      // 有一条要显示
extern char ancsText[96];            // 要显示的话（"<名字>，来电话了：xxx"）
extern volatile bool ancsIsCall;     // 是来电（要响铃）
extern volatile bool ancsCallGone;   // 来电结束了（接了/挂了）
extern volatile bool ancsConnected;  // iPhone 连着

void ancsBegin();
