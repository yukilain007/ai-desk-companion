#pragma once
// 读 config.h（有的话），没填的项用默认值
#if __has_include("config.h")
#include "config.h"
#define HAVE_CONFIG 1
#endif

#ifndef USER_NAME
#define USER_NAME "你"
#endif
#ifndef BLE_NAME
#define BLE_NAME "桌面小伙伴"
#endif
#ifndef TZ_OFFSET_HOURS
#define TZ_OFFSET_HOURS 8
#endif
#ifndef MAC_HOST
#define MAC_HOST "MacBook-Air"  // Mac 的局域网名（scutil --get LocalHostName）
#endif
#ifndef CLOUD_HOST  // 不用云端：只连 Mac 上的对话桥
#define CLOUD_HOST ""
#endif
#ifndef BOARD_TOKEN
#define BOARD_TOKEN ""
#endif
#ifndef WIFI_LIST
#define WIFI_LIST { { nullptr, nullptr } }
#endif
#ifndef ACCENT_R  // 点缀色，默认薄荷绿
#define ACCENT_R 0x7F
#define ACCENT_G 0xD1
#define ACCENT_B 0xC1
#endif
#ifndef BEDTIME_HOUR  // 几点提醒睡觉（24 小时制），之后到早上 7 点算夜里
#define BEDTIME_HOUR 23
#endif
