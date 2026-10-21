#pragma once
#include <Arduino.h>

// 网络在核心 0 上单独跑；主循环只读这几个变量
extern volatile int netPendingFace;      // 云端发来的表情，-1 = 没有
extern volatile bool netTimeReady;       // NTP 对上时了
extern volatile int64_t netLocalEpoch;   // 对时那一刻的本地时间（按 config.h 的时区）
extern volatile bool netOnline;          // 和云端中转连着
extern char netSayBuf[640];              // 云端发来的一句话（UTF-8）
extern volatile bool netSayReady;
extern volatile bool netSayChime;       // 这句是云端发来的消息（不是当面对话的回复）：响一声提示
extern volatile bool netClipIncoming;    // 正在收一段语音（收完会播，不用再提示）
extern volatile bool netSayHold;        // 这句要挂着（点一下或 10 分钟才收）
extern volatile int netBatPercent;      // 主循环填，网络任务每分钟报给云端
extern volatile bool netBatCharging, netBatUsb;

extern volatile bool netBridgeOnline;   // 连着 Mac 上的对话桥（局域网）
extern volatile bool netEarOn;
extern volatile uint32_t netThinkingSince;
extern volatile bool netBridgeMissed;      // 按键说了话但对话桥没连着（主循环提示一下）  // 对话桥说"我在想"（K1）的时间，0 = 没在想          // 主循环填：耳朵开着（长按屏幕开关）

extern const char *netResetReason;
extern volatile int netFocusCmd;           // 云端/对话桥："M<分钟>" 开始专注，"M0" 结束；-1 = 没有
extern volatile int netFocusDone;
extern volatile bool netTimerCmd;          // 收到计时命令 "W<秒>|<内容>"（秒=0 表示全部取消）
extern volatile uint32_t netTimerSec;
extern char netTimerLabel[48];
extern volatile bool netAlarmCmd;          // 闹钟命令 "L<hh>:<mm>|<掩码>"（netAlarmH<0 = 全部取消）
extern volatile int netAlarmH, netAlarmM, netAlarmMask;
void parseAlarmCmd(const char *s);         // 解析 "L" 后面的部分          // 主循环填：完成了一次 N 分钟的专注，网络任务报给云端     // 开机时填：上次重启原因，连上云端后报一次

extern char netWifiCmd[128];              // 蓝牙/串口来的 WiFi 设置："名字|密码" / "del:名字" / "clear"
extern volatile bool netWifiCmdReady;
extern String netWifiStatus;              // 已存哪些 WiFi、当前连的哪个（蓝牙读这个）

void netBegin();
