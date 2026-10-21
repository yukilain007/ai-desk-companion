# 把 Mac 的本地时间发给板子（没联网时用；联网后板子自己对时）
import os, time, select
from serialport import open_board
fd = open_board()
local = int(time.time()) + time.localtime().tm_gmtoff
os.write(fd, f"T{local}\n".encode())
buf = b''; end = time.time() + 1.5
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.3)
    if r:
        try: buf += os.read(fd, 4096)
        except BlockingIOError: pass
print([l for l in buf.decode(errors='replace').splitlines() if 'time' in l] or 'no reply')
