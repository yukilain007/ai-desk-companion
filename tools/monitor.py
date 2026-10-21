# 看板子的日志若干秒。用法: python3 monitor.py [秒数]
import os, sys, time, select
from serialport import open_board
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8
fd = open_board()
end = time.time() + secs
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.3)
    if r:
        try: sys.stdout.write(os.read(fd, 4096).decode(errors='replace')); sys.stdout.flush()
        except BlockingIOError: pass
