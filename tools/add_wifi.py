#!/usr/bin/env python3
"""把 WiFi 交给板子（插着 USB 时用）。默认用 Mac 现在连的那个 WiFi 名字，密码自己输（不显示）。
  python3 add_wifi.py            加 / 改一个 WiFi
  python3 add_wifi.py --list     看板子存了哪些（屏幕上也会显示）
  python3 add_wifi.py --del 名字  删一个
"""
import getpass
import glob
import os
import re
import subprocess
import sys
import termios
import time


def mac_ssid():
    for cmd in (['ipconfig', 'getsummary', 'en0'], ['networksetup', '-getairportnetwork', 'en0']):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=5).stdout
        except Exception:
            continue
        m = re.search(r'^\s*SSID\s*:\s*(.+)$', out, re.M) or re.search(r'Current Wi-Fi Network:\s*(.+)$', out, re.M)
        if m and '<redacted>' not in m.group(1):
            return m.group(1).strip()
    return ''


def send(line, wait=2.5):
    ports = glob.glob('/dev/cu.usbmodem*')
    if not ports:
        sys.exit('找不到板子：用 USB 线插到 Mac 上再试')
    fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    os.write(fd, (line + '\n').encode())
    time.sleep(wait)
    os.close(fd)


args = sys.argv[1:]
if args[:1] == ['--list']:
    send('Nlist')  # 屏幕上会显示已存的
    print('看板子的屏幕。')
elif args[:1] == ['--del'] and len(args) > 1:
    send('Ndel:' + args[1])
    print(f'已让板子删掉：{args[1]}')
else:
    cur = mac_ssid()
    ssid = input(f'WiFi 名字{f"（回车 = Mac 现在连的「{cur}」）" if cur else ""}：').strip() or cur
    if not ssid:
        sys.exit('没有 WiFi 名字')
    pw = getpass.getpass(f'「{ssid}」的密码（不显示）：')
    send(f'N{ssid}|{pw}')
    print(f'已交给板子：{ssid}。屏幕上会显示"记下 WiFi"。')
