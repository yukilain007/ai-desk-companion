#pragma once
#include <Arduino.h>
#include <stdarg.h>

// 日志只在 USB 发送缓冲区有空位时才写，否则直接丢掉。
// 插着 Mac 但没人读串口时，Serial.print 会把主循环卡住（实测卡过 8 分钟，一开串口才恢复）。
inline void logf(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(b) - 1) n = sizeof(b) - 1;
  if (Serial.availableForWrite() >= n) Serial.write((const uint8_t *)b, n);
}
