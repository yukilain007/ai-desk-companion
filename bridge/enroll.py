#!/usr/bin/env python3
"""录你的声纹（存在这台 Mac 上，不上传）。
  python3 enroll.py           开始录：板子会提示，对着它说 8 句话，每句 2 秒以上
  python3 enroll.py --status  看有没有录过
  python3 enroll.py --reset   删掉声纹（之后不做声纹比对）
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
FLAG, PRINT = os.path.join(HERE, 'enroll.flag'), os.path.join(HERE, 'voiceprint.npz')
arg = sys.argv[1] if len(sys.argv) > 1 else ''
if arg == '--status':
    print('声纹：已录' if os.path.exists(PRINT) else '声纹：未录', '| 录音中' if os.path.exists(FLAG) else '')
elif arg == '--reset':
    for f in (PRINT, FLAG):
        if os.path.exists(f): os.remove(f)
    os.system(f'launchctl kickstart -k gui/{os.getuid()}/com.desk-companion.bridge 2>/dev/null')
    print('已删除声纹。重启对话桥后生效（不再比对声音）')
else:
    open(FLAG, 'w').write('enroll')
    print('开始录声纹：看板子上的提示，对着它说 8 句话，每句 2 秒以上。录完板子会说"记住你的声音了"。')
