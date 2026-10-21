# 往板子发一行串口命令（自动补换行）并读回若干秒。用法: python3 send.py "命令" [秒数] [过滤前缀]
# 例：python3 send.py "S你好" 　显示一句字幕；python3 send.py P 2 status 　看电量和连接状态
import os, sys, time, select
from serialport import open_board
fd = open_board()
os.write(fd, (sys.argv[1] + '\n').encode())
buf = b''; end = time.time() + (float(sys.argv[2]) if len(sys.argv) > 2 else 3)
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try: buf += os.read(fd, 4096)
        except BlockingIOError: pass
pre = sys.argv[3] if len(sys.argv) > 3 else ''
for l in buf.decode(errors='replace').splitlines():
    if l.startswith(pre): print(l)
