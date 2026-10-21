# 找板子的串口（USB 插上后是 /dev/cu.usbmodem 开头）
import glob, os, sys, termios


def open_board():
    ports = sorted(glob.glob('/dev/cu.usbmodem*'))
    if not ports:
        sys.exit('找不到板子：用 USB 线插到 Mac 上再试')
    fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd
