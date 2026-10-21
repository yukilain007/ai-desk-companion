#pragma once
// 复制一份改名为 config.h 再填。⚠️ config.h 里有 WiFi 密码和密钥：不要发给别人、不要传到网上（已在 .gitignore 里）。
// 没有 config.h 也能编译：小伙伴照样能动、能摸、能计时，只是不联网。

// ---- 怎么称呼你 ----
// 屏幕上的提醒会用这个名字，比如"小明，来电话了"
#define USER_NAME "你"

// ---- 蓝牙里显示的名字（iPhone 设置 → 蓝牙 里点它配对）----
#define BLE_NAME "桌面小伙伴"

// ---- WiFi ----
// 按 {"名字", "密码"} 一行一个，会自动连信号最好的那个。记得加上手机热点，出门开热点就能连。
// 也可以不写在这里，之后用手机（LightBlue）或 tools/add_wifi.py 加。
#define WIFI_LIST { \
  {"家里WiFi名字", "家里WiFi密码"}, \
  {"手机热点名字", "手机热点密码"}, \
}

// ---- 时区：相对 UTC 的小时数（北京 / 新加坡 = 8）----
#define TZ_OFFSET_HOURS 8

// ---- 云端中转（cloud/ 部署后得到的域名和板子密钥），不用云端就删掉这两行 ----
#define CLOUD_HOST "face.example.com"
#define BOARD_TOKEN "和 cloud 里 BOARD_TOKEN 一样的一串随机字符"

// ---- Mac 上的对话桥（bridge/）：Mac 的局域网名字，终端里跑 scutil --get LocalHostName 查 ----
#define MAC_HOST "MacBook-Air"

// ---- 外观和作息（可以不改）----
#define ACCENT_R 0x7F  // 点缀色 RGB（专注点阵、未读圆点），默认薄荷绿
#define ACCENT_G 0xD1
#define ACCENT_B 0xC1
#define BEDTIME_HOUR 23  // 几点提醒睡觉
