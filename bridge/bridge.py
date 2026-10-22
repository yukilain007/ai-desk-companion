#!/usr/bin/env python3
"""桌面小伙伴 · 对话桥：板子录到的话 → Mac 本地转文字 → 叫了名字才交给 Claude Code → 表情/字幕/语音回板子。

- 板子在局域网里连 ws://<Mac名字>.local:8765/board/<BOARD_TOKEN>
  上行："U<字节数>" + 二进制 PCM（16kHz 单声道 16bit）+ "u"；按住 BOOT 键说的用 "V" 开头（不用叫名字、不比声纹）
  下行：和云端同一套协议 —— "F<n>" 表情、"S<文字>" 字幕、"P<字节数>"+二进制+"E" 语音、"K1"/"K0" 在想/想完
- 转文字：本地 whisper-server（不联网）。没叫名字的话转完就丢，不记录内容。
- 两种叫法（config.env 里设）：公共名字 = 只用表情和字幕、不出声（在外面 / 有别人在）；私人名字 = 放松聊天，可以开口。
  叫过以后 60 秒内的话不用再叫。
- 回答的是本机的 Claude Code（claude -p），用你自己的 Claude 账号登录。
- 所有设置在同目录的 config.env（照 config.example.env 抄一份）。
"""
import asyncio
import io
import json
import shutil
import os
import re
import subprocess
import sys
import threading
import time
import unicodedata
import urllib.request
import uuid
import wave

import websockets

HERE = os.path.dirname(os.path.abspath(__file__))


def read_kv(path):
    out = {}
    try:
        for line in open(path, encoding='utf-8'):
            m = re.match(r'^\s*([A-Z_]+)\s*=\s*(.*?)\s*$', line)
            if m and not line.lstrip().startswith('#'):
                out[m.group(1)] = re.sub(r'\s+#.*$', '', m.group(2)).strip('"\'')
    except FileNotFoundError:
        pass
    return out


CFG = read_kv(os.path.join(HERE, 'config.env'))


def cfg(key, default=''):
    return os.environ.get(key) or CFG.get(key) or default


def path_cfg(key, default=''):
    p = os.path.expanduser(cfg(key, default))
    return os.path.join(HERE, p) if p and not os.path.isabs(p) else p  # 相对路径按对话桥这个文件夹算


def names(key):
    return [n.strip() for n in re.split(r'[,，]', cfg(key)) if n.strip()]


BOARD_TOKEN = cfg('BOARD_TOKEN')
ELEVEN_KEY = cfg('ELEVENLABS_API_KEY')
VOICE_ID = cfg('VOICE_ID')
VOICE_SPEED = float(cfg('VOICE_SPEED', '1.0'))
VOICE_LANG = cfg('VOICE_LANG', '中文')  # 开口说话用什么语言
USER_NAME = cfg('USER_NAME', '用户')
PERSONA_DIR = path_cfg('PERSONA_DIR', 'persona')  # 里面的 CLAUDE.md = 人设
CLAUDE_BIN = shutil.which(cfg('CLAUDE_BIN', 'claude')) or os.path.expanduser('~/.local/bin/claude')
CLAUDE_MODEL = cfg('CLAUDE_MODEL', 'sonnet')  # 语音对话要快，Sonnet 比较合适
MCP_CONFIG = path_cfg('MCP_CONFIG')  # 可选：给它接记忆之类的 MCP（.mcp.json 路径）
MCP_ALLOWED = cfg('MCP_ALLOWED')  # 允许用的 MCP 工具，逗号分隔
NOTES_DIR = path_cfg('NOTES_DIR')  # 可选："记一下……"写到这个文件夹
# 语音聊天用不着读写文件；不禁的话它会顺手翻文件（又慢又不该）
NO_TOOLS = 'Bash,Read,Glob,Grep,Edit,Write,NotebookEdit,WebFetch,WebSearch,Task,Agent,TodoWrite'
WHISPER_BIN = shutil.which('whisper-server') or '/opt/homebrew/bin/whisper-server'
WHISPER_MODEL = path_cfg('WHISPER_MODEL', 'models/ggml-small.bin')
WHISPER_PORT = 8178
PORT = 8765
DEBUG = cfg('COMPANION_DEBUG') == '1'  # 调试：没叫名字的话也记下识别结果（平时别开）
SESSION_SECONDS = 60          # 叫一次名字后多久内不用再叫
CONTEXT_SECONDS = 30 * 60     # 多久内的对话接着同一个 Claude 会话

# 声纹：只理你的声音。没录过声纹（voiceprint.npz 不存在）时不启用。
# 录法：python3 enroll.py → 对着板子说 8 句、每句 2 秒以上。声纹只存在这台 Mac 上。
SPK_MODEL = os.path.join(HERE, 'models', '3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx')
VOICEPRINT = os.path.join(HERE, 'voiceprint.npz')
ENROLL_FLAG = os.path.join(HERE, 'enroll.flag')
ENROLL_N = 8
# 实测：同一个人 0.6~0.85、不同的人 0.45~0.57。先用 0.50，看日志里的分数再调
VOICE_THRESHOLD = float(cfg('VOICE_THRESHOLD', '0.50'))
VERIFY_MIN_SECONDS = 1.0  # 太短的（只喊一声名字）比不准，先放行，正文再比

FACES = ['calm', 'wink', 'curious', 'meh', 'music', 'sleep', 'smile']


def name_re(ns, head=False):
    if not ns:
        return re.compile(r'(?!x)x')  # 什么都不匹配
    alt = '|'.join(re.escape(n) for n in sorted(ns, key=len, reverse=True))
    return re.compile((r'^\W*((hi|hey|hello|嗨|嘿)\W*)?' if head else '') + f'({alt})', re.I)


# 名字和 whisper 常听错的写法都写进 config.env（逗号分隔），比如 NAMES_PUBLIC=Momo,摸摸,模模
PUBLIC_NAMES, PRIVATE_NAMES = names('NAMES_PUBLIC'), names('NAMES_PRIVATE')
PUBLIC_RE, PRIVATE_RE = name_re(PUBLIC_NAMES), name_re(PRIVATE_NAMES)
ANY_NAME_HEAD_RE = name_re(PUBLIC_NAMES + PRIVATE_NAMES, head=True)
# 中文模式常把开头的英文名吞掉：有英文名字的话，开头 1.5 秒再用英文识别一次
EN_PUBLIC = [n for n in PUBLIC_NAMES if n.isascii()]
EN_PRIVATE = [n for n in PRIVATE_NAMES if n.isascii()]
EN_PUBLIC_RE, EN_PRIVATE_RE = name_re(EN_PUBLIC, head=True), name_re(EN_PRIVATE, head=True)
# 在要它开口说话
VOICE_RE = re.compile(r'用声音|说给我听|讲给我听|念给我听|读给我听|让我听听|听听你的声音|出个声|说出来听|speak|用语音|开口说', re.I)

# whisper 在杂音/听不清时的经典幻觉（训练数据里大量视频字幕），整句丢掉
# 提示词里千万别放名字：听不清时它会把提示词原样吐出来，等于替你叫了名字
# 夹在正文里的署名式幻觉（"(词:xx)""(字幕:xx)"）：剥掉括号这段，正文照常处理
CREDIT_RE = re.compile(r'[（(\[【][^）)\]】]{0,20}(词|曲|作词|作曲|编曲|字幕|翻译|校对|演唱)[:：][^）)\]】]{0,20}[）)\]】]')
HALLUCINATION_RE = re.compile(
    r'字幕|訂閱|订阅|點讚|点赞|謝謝觀看|谢谢观看|感謝收看|感谢收看|明鏡|明镜|請不吝|请不吝|'
    r'thank(s| you) for watching|subscribe|amara\.org|^\W*$', re.I)


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


# ---------- whisper ----------
def ensure_whisper():
    try:
        urllib.request.urlopen(f'http://127.0.0.1:{WHISPER_PORT}/', timeout=1)
        return None
    except Exception:
        pass
    log('starting whisper-server')
    p = subprocess.Popen(
        [WHISPER_BIN, '-m', WHISPER_MODEL, '--host', '127.0.0.1', '--port', str(WHISPER_PORT),
         '-l', 'zh', '-t', '4', '--prompt', '以下是普通话的句子，使用简体中文。'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(60):
        time.sleep(0.5)
        try:
            urllib.request.urlopen(f'http://127.0.0.1:{WHISPER_PORT}/', timeout=1)
            return p
        except Exception:
            pass
    log('whisper-server did not start')
    return p


def pcm_to_wav(pcm):
    b = io.BytesIO()
    with wave.open(b, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm)
    return b.getvalue()


def transcribe(pcm, language='zh'):
    boundary = uuid.uuid4().hex
    body = io.BytesIO()
    def field(name, value):
        body.write(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode())
    field('temperature', '0.0')
    field('response_format', 'json')
    field('language', language)
    body.write(f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="a.wav"\r\nContent-Type: audio/wav\r\n\r\n'.encode())
    body.write(pcm_to_wav(pcm))
    body.write(f'\r\n--{boundary}--\r\n'.encode())
    req = urllib.request.Request(f'http://127.0.0.1:{WHISPER_PORT}/inference', data=body.getvalue(),
                                 headers={'Content-Type': f'multipart/form-data; boundary={boundary}'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode()).get('text', '').strip()


# ---------- Claude ----------
RULES = """你正在通过{user}桌上的小圆屏（桌面小伙伴）和{user}说话。这是语音对话桥：{user}对着板子说话，语音转文字后交给你，转写可能有错字，按意思理解。
你的回复会显示在板子上，所以：

每条消息开头会标【模式：PUBLIC】或【模式：PRIVATE】，按那一条的标注来：
- PUBLIC（叫的是公共名字）：可能在工作或公共场合，旁边有人。简短、干练、得体，不说私密的话。你不能出声，voice 必须为空。
- PRIVATE（叫的是私人名字）：只有你们两个。放松、有温度，按人设文件（CLAUDE.md）里的相处方式来。

你在这里能做的只有：回一句话（字幕/语音）、换一个表情{mcp}。
你**没有**发消息、查网页、读文件、管日程的能力。提醒/计时由系统处理："N 分钟后提醒我……""下午三点提醒我……"这类话会被直接设好，不会转给你。
如果一个提醒请求还是到了你这里，说明系统没听懂——绝不能说"已设好"，请对方换成"X 分钟后提醒我……"或"X 点提醒我……"这样的说法再说一次。其他做不到的事也直说。

只回一个 JSON 对象，不要任何别的文字、不要 markdown：
{{"face": "{faces}" 里选一个, "text": "显示在屏幕上的字幕，不超过 40 个字", "voice": "要念出来的话（{lang}），或空字符串"}}

voice 只有在 PRIVATE 模式、并且对方明确要听你的声音（比如"用声音说""说给我听""让我听听你的声音"）时才填，一到两句{lang}；其他时候一律空字符串。
表情要克制：大多数时候 calm 或 wink，smile 留给真正开心的时刻。"""


class Brain:
    """常驻的 Claude 进程（stream-json 进出）：不用每句冷启动、重读系统提示词，回复从 ~10 秒降到 ~6 秒。
    桥启动时就预热；闲置超过 CONTEXT_SECONDS 换新会话；进程挂了下一句自动重启。"""

    def __init__(self):
        self.mcp = MCP_CONFIG if MCP_CONFIG and os.path.exists(MCP_CONFIG) else None
        self.proc = None
        self.last_used = 0

    def _start(self):
        cmd = [CLAUDE_BIN, '-p', '--input-format', 'stream-json', '--output-format', 'stream-json', '--verbose',
               '--model', CLAUDE_MODEL, '--disallowedTools', NO_TOOLS,
               '--append-system-prompt', RULES.format(faces=' / '.join(FACES), user=USER_NAME, lang=VOICE_LANG,
                                                      mcp='，以及用接上的 MCP 工具（比如记忆）' if self.mcp else '')]
        if self.mcp:
            cmd += ['--mcp-config', self.mcp]
            if MCP_ALLOWED:
                cmd += ['--allowedTools', MCP_ALLOWED]
        os.makedirs(PERSONA_DIR, exist_ok=True)
        self.proc = subprocess.Popen(cmd, cwd=PERSONA_DIR, text=True, bufsize=1,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.last_used = time.time()
        log('claude process started')

    def _stop(self):
        if self.proc:
            try:
                self.proc.stdin.close()
                self.proc.wait(timeout=5)
            except Exception:
                self.proc.kill()
        self.proc = None

    def warm(self):
        self._start()

    def ask(self, transcript, mode):
        if self.proc and (self.proc.poll() is not None or time.time() - self.last_used > CONTEXT_SECONDS):
            self._stop()
        if not self.proc:
            self._start()
        t0 = time.time()
        msg = f'【模式：{mode}】{transcript}'
        # 60 秒没回就杀掉进程（readline 会因此返回空），免得卡住后面所有的话
        proc = self.proc
        watchdog = threading.Timer(60, lambda: (log('claude timeout, restarting'), proc.kill()))
        watchdog.start()
        try:
            self.proc.stdin.write(json.dumps({'type': 'user', 'message': {'role': 'user', 'content': msg}}, ensure_ascii=False) + '\n')
            self.proc.stdin.flush()
            text, err = None, False
            while True:
                line = self.proc.stdout.readline()
                if not line:
                    raise RuntimeError('claude process exited')
                ev = json.loads(line)
                if ev.get('type') == 'result':
                    text, err = ev.get('result', ''), ev.get('is_error', False)
                    break
        except Exception as e:
            log('claude error:', e)
            self._stop()
            return None
        finally:
            watchdog.cancel()
        self.last_used = time.time()
        if err or 'Not logged in' in text or 'Please run /login' in text:
            log('claude error:', text[:120])
            return {'face': 'curious', 'text': 'Mac 上的 Claude 还没登录（终端里跑 claude 登录一下）' if 'login' in text.lower() else '出错了，没想出来', 'voice': ''}
        m = re.search(r'\{.*\}', text, re.S)
        try:
            reply = json.loads(m.group(0)) if m else {'face': 'calm', 'text': text[:40], 'voice': ''}
        except Exception:
            reply = {'face': 'calm', 'text': text[:40], 'voice': ''}
        log(f'claude {time.time() - t0:.1f}s')
        return reply


# ---------- 随口记 ----------
# "记一下：xxx" → 直接写进 NOTES_DIR（一天一个文件，一条一行），不经过 Claude，又快又不会假装记了
NOTE_RE = re.compile(r'^\W*(记一下|记下来|记下|帮我记一下|帮我记|备忘一下|备忘|记个|记录一下)\W*', re.I)


def save_note(content):
    day = time.strftime('%Y-%m-%d')
    path = os.path.join(NOTES_DIR, f'{day}_随口记.md')
    new = not os.path.exists(path)
    with open(path, 'a', encoding='utf-8') as f:
        if new:
            f.write(f'# {day} 随口记\n\n')
        f.write(f'- {time.strftime("%H:%M")} {content}\n')
    return path


# ---------- 专注（番茄钟）----------
# "专注 40 分钟""开始番茄钟""结束专注" → 直接发给板子，不经过 Claude
FOCUS_STOP_RE = re.compile(r'(结束|停止|取消|不要|停一下|暂停)\W*(专注|番茄钟|番茄)')
FOCUS_START_RE = re.compile(r'(开始|开个|开一个|来个|来一个|帮我开)?\W*(专注|番茄钟|番茄)\W*([0-9零一二两三四五六七八九十百半]+)?\s*(个)?\s*(分钟|小时)?')
CN_NUM = {'零': 0, '一': 1, '二': 2, '两': 2, '三': 3, '四': 4, '五': 5, '六': 6, '七': 7, '八': 8, '九': 9}


def cn_to_int(t):
    if t.isdigit():
        return int(t)
    if t == '半':
        return 30
    if '十' in t:
        a, _, b = t.partition('十')
        return (CN_NUM.get(a, 1) if a else 1) * 10 + (CN_NUM.get(b, 0) if b else 0)
    return CN_NUM.get(t, 0)


def parse_focus(text):
    """返回 0 = 结束，N = 开始 N 分钟，None = 不是专注命令"""
    if FOCUS_STOP_RE.search(text):
        return 0
    m = FOCUS_START_RE.search(text)
    if not m or not (m.group(1) or m.group(3) or m.group(5)):  # 光说"专注"两个字不算，避免误触发
        return None
    n = cn_to_int(m.group(3)) if m.group(3) else 25
    if m.group(5) == '小时':
        n = n * 60 if m.group(3) != '半' else 30
    return max(1, min(180, n or 25))


# ---------- 计时提醒 ----------
# "15 分钟后提醒我看面团""一个半小时后叫我""下午三点提醒我回电话""取消提醒" → 直接发给板子 "W<秒>|<内容>"，不经过 Claude
NUM = r'[0-9零一二两三四五六七八九十百半]+'
TIMER_CANCEL_RE = re.compile(r'(取消|不用|别)\W*(所有|全部)?\W*(提醒|计时|闹钟|倒计时)')
TIMER_REL_RE = re.compile(rf'(?:({NUM})\s*个?\s*(半)?\s*(小时|钟头))?\s*(半\s*(?:小时|钟头))?\s*(?:({NUM})\s*(分钟|分))?\s*(?:({NUM})\s*秒)?\s*(?:钟)?\s*(?:以后|之后|后)\s*(?:再)?\s*(提醒|叫|喊)我\W*(.*)')
TIMER_ABS_RE = re.compile(rf'(早上|上午|中午|下午|晚上|今晚)?\s*({NUM})\s*点\s*(半|({NUM})\s*分?)?\s*(?:的时候|钟)?\s*(提醒|叫|喊)我\W*(.*)')
TIMER_SET_RE = re.compile(rf'(?:定|设|开)\s*(?:一)?个\s*({NUM})\s*(分钟|小时)\s*的?\s*(闹钟|计时|倒计时|提醒)')


def parse_timer(text):
    """返回 (秒, 内容)；(0, '') = 全部取消；None = 不是计时命令"""
    t = text.strip().rstrip('。.!！')
    if TIMER_CANCEL_RE.search(t):
        return (0, '')
    m = TIMER_REL_RE.search(t)
    if m and (m.group(1) or m.group(4) or m.group(5) or m.group(7)):
        secs = 0
        if m.group(1) == '半':  # "半小时"
            secs += 1800
        elif m.group(1):  # "一个小时" / "一个半小时"
            secs += cn_to_int(m.group(1)) * 3600 + (1800 if m.group(2) else 0)
        if m.group(4):  # "半小时"
            secs += 1800
        if m.group(5):
            secs += cn_to_int(m.group(5)) * 60
        if m.group(7):
            secs += cn_to_int(m.group(7))
        label = m.group(9).strip(' ，,。')
        return (max(5, min(secs, 12 * 3600)), label[:24]) if secs else None
    m = TIMER_ABS_RE.search(t)
    if m:
        h = cn_to_int(m.group(2))
        mi = 30 if m.group(3) == '半' else (cn_to_int(m.group(4)) if m.group(4) else 0)
        part = m.group(1) or ''
        if part in ('下午', '晚上', '今晚') and h < 12:
            h += 12
        if part == '中午' and h < 11:
            h += 12
        now = time.localtime()
        target = time.mktime((now.tm_year, now.tm_mon, now.tm_mday, h, mi, 0, 0, 0, -1))
        if not part and target <= time.time() and h < 12:  # 没说上下午、已经过了：当成下午/晚上
            target += 12 * 3600
        if target <= time.time():
            target += 24 * 3600
        return (int(target - time.time()), m.group(6).strip(' ，,。')[:24])
    m = TIMER_SET_RE.search(t)
    if m:
        n = cn_to_int(m.group(1))
        return (n * 3600 if m.group(2) == '小时' else n * 60, '')
    return None


# ---------- 闹钟（叫起床）----------
# "明早 7 点叫我""每天早上 7 点叫我""工作日 7 点半叫我""周末 9 点叫我起床""取消闹钟""明天不用叫我"
# → 板子 "L<hh>:<mm>|<掩码>"（bit0=周日…bit6=周六，128=只响一次），"L0" 全部取消。要在计时提醒之前判断
ALARM_CANCEL_RE = re.compile(r'(取消|关掉|关了|不用|别)\W*(闹钟|叫我|叫醒|喊我)')
ALARM_RE = re.compile(rf'(明早|明天早上|明天|每天早上|每天|天天|工作日|周一到周五|周末)?\s*(早上|上午|清早)?\s*({NUM})\s*点\s*(半|({NUM})\s*分?)?\s*(叫我|喊我|叫醒我)\s*(起床)?')


def parse_alarm(text):
    """返回 (时, 分, 掩码)；(-1, 0, 0) = 全部取消；None = 不是闹钟"""
    t = text.strip()
    if ALARM_CANCEL_RE.search(t):
        return (-1, 0, 0)
    m = ALARM_RE.search(t)
    if not m or not (m.group(1) or m.group(2) or m.group(7) or '起床' in t):
        return None  # 没说明早/每天/早上/起床：是计时提醒，不是闹钟
    h = cn_to_int(m.group(3))
    mi = 30 if m.group(4) == '半' else (cn_to_int(m.group(5)) if m.group(5) else 0)
    if h > 23 or mi > 59:
        return None
    when = m.group(1) or ''
    mask = 127 if when in ('每天早上', '每天', '天天') else 62 if when in ('工作日', '周一到周五') else 65 if when == '周末' else 128
    return (h, mi, mask)


# ---------- 声纹 ----------
class Voice:
    def __init__(self):
        self.ext = None
        self.mean = None
        self.samples = []
        if os.path.exists(VOICEPRINT):
            import numpy as np
            d = np.load(VOICEPRINT)
            self.mean = d['mean']
            log(f'voiceprint loaded ({int(d["n"])} samples, threshold {VOICE_THRESHOLD})')

    def embed(self, pcm):
        import numpy as np
        import sherpa_onnx
        if self.ext is None:
            self.ext = sherpa_onnx.SpeakerEmbeddingExtractor(
                sherpa_onnx.SpeakerEmbeddingExtractorConfig(model=SPK_MODEL, num_threads=2))
        x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768
        st = self.ext.create_stream()
        st.accept_waveform(16000, x)
        st.input_finished()
        e = np.array(self.ext.compute(st))
        return e / np.linalg.norm(e)

    def score(self, pcm):
        return float(self.embed(pcm) @ self.mean)

    def add_sample(self, pcm):
        self.samples.append(self.embed(pcm))
        return len(self.samples)

    def save(self):
        import numpy as np
        m = np.mean(self.samples, axis=0)
        self.mean = m / np.linalg.norm(m)
        np.savez(VOICEPRINT, mean=self.mean, n=len(self.samples))
        os.chmod(VOICEPRINT, 0o600)
        consistency = [float(e @ self.mean) for e in self.samples]
        log(f'voiceprint saved: {len(self.samples)} samples, self-score {min(consistency):.2f}~{max(consistency):.2f}')
        self.samples = []


# ---------- TTS ----------
def tts_pcm(text):
    if not ELEVEN_KEY or not VOICE_ID:
        return None
    req = urllib.request.Request(
        f'https://api.elevenlabs.io/v1/text-to-speech/{VOICE_ID}?output_format=pcm_16000',
        data=json.dumps({'text': text, 'model_id': 'eleven_flash_v2_5',
                         'voice_settings': {'stability': 0.5, 'similarity_boost': 0.75, 'speed': VOICE_SPEED}}).encode(),
        headers={'xi-api-key': ELEVEN_KEY, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def subtitle_safe(s):
    # 板子字库没有带重音的拉丁字母（café → cafe）
    return re.sub(r'[À-ɏ]', lambda m: unicodedata.normalize('NFD', m.group(0))[0], s)


# ---------- board connection ----------
brain = Brain()
voice = Voice()
board_ws = None  # 当前连着的板子（录声纹时主动发提示用）
session = {'mode': None, 'until': 0}
pending = []          # 我在想的时候又听到的话，攒着一起回
busy = None          # 在 main() 里建：Python 3.9 的 asyncio.Lock 创建时就绑定事件循环
whisper_lock = None  # whisper 一次只识别一段，避免杂音多时并发堆到超时
whisper_waiting = 0


async def drain(ws):
    # 一次只想一件事：想完发现又攒了话，就把攒的合在一起再想一次
    async with busy:
        while pending:
            texts = [t for t, _ in pending]
            mode = pending[-1][1]
            pending.clear()
            await reply_to(ws, ' / '.join(texts), mode)


async def reply_to(ws, transcript, mode):
    await ws.send('K1')
    loop = asyncio.get_running_loop()
    if mode == 'PRIVATE' and VOICE_RE.search(transcript):
        transcript += f'\n【这句在要你出声：这次 voice 必须填一到两句{VOICE_LANG}。】'
    reply = await loop.run_in_executor(None, brain.ask, transcript, mode)
    if not reply:
        await ws.send('K0')
        await ws.send('S没想好怎么回，再说一次？')
        return
    face = reply.get('face') if reply.get('face') in FACES else None
    text = subtitle_safe(str(reply.get('text', '')).strip())[:120]
    voice = str(reply.get('voice', '')).strip() if mode == 'PRIVATE' else ''
    if face:
        await ws.send(f'F{FACES.index(face)}')
    pcm = None
    if voice:
        try:
            pcm = await loop.run_in_executor(None, tts_pcm, voice[:300])
        except Exception as e:
            log('tts error:', e)
    await ws.send('S' + (text or subtitle_safe(voice) or '…'))
    if pcm:
        await ws.send(f'P{len(pcm)}')
        for i in range(0, len(pcm), 8192):
            await ws.send(pcm[i:i + 8192])
        await ws.send('E')
    log(f'reply face={face} voice={"yes" if pcm else "no"}')


async def handle(ws):
    path = ws.request.path if hasattr(ws, 'request') else ws.path
    if path != f'/board/{BOARD_TOKEN}' or not BOARD_TOKEN:
        await ws.close(1008, 'bad token')
        return
    global board_ws
    board_ws = ws
    log('board connected')
    buf, want, ptt = None, 0, False
    try:
        async for msg in ws:
            if isinstance(msg, str):
                if msg[:1] in ('U', 'V'):
                    want, buf, ptt = int(msg[1:]), bytearray(), msg[0] == 'V'
                elif msg == 'u' and buf is not None:
                    pcm, buf = bytes(buf), None
                    asyncio.create_task(on_utterance(ws, pcm, ptt))
            elif buf is not None:
                buf.extend(msg)
    except websockets.ConnectionClosed:
        pass
    log('board disconnected')


async def on_utterance(ws, pcm, ptt=False):
    global whisper_waiting
    loop = asyncio.get_running_loop()
    secs = len(pcm) / 32000

    # 录声纹模式：收 8 句 2 秒以上的话，不做别的
    if os.path.exists(ENROLL_FLAG):
        if secs < 2.0:
            await ws.send('S再说长一点，两秒以上。')
            return
        n = await loop.run_in_executor(None, voice.add_sample, pcm)
        if n >= ENROLL_N:
            await loop.run_in_executor(None, voice.save)
            os.remove(ENROLL_FLAG)
            await ws.send(f'F{FACES.index("smile")}')
            await ws.send('S记住你的声音了。以后只听你的。')
        else:
            await ws.send(f'S第 {n}/{ENROLL_N} 句记下了，再随便说一句。')
        return

    # 声纹比对：不像你就丢，连转文字都不做（只记分数）
    # 按键说的不比声纹：能按到键，人就在板子跟前（感冒时也能用）
    if voice.mean is not None and secs >= VERIFY_MIN_SECONDS and not ptt:
        sc = await loop.run_in_executor(None, voice.score, pcm)
        if sc < VOICE_THRESHOLD:
            log(f'not owner {secs:.1f}s (score {sc:.2f})')
            return
        log(f'voice ok {secs:.1f}s (score {sc:.2f})')
    if whisper_waiting >= 2:  # 前面已经排着两段了：这段多半也是杂音，丢掉
        log(f'skipped {secs:.1f}s (whisper busy)')
        return
    whisper_waiting += 1
    try:
        async with whisper_lock:
            text = await loop.run_in_executor(None, transcribe, pcm)
    except Exception as e:
        log('whisper error:', e)
        return
    finally:
        whisper_waiting -= 1
    text = CREDIT_RE.sub('', text).strip()
    if HALLUCINATION_RE.search(text):
        log(f'dropped {secs:.1f}s (whisper hallucination)' + (f' [debug] {text}' if DEBUG else ''))
        return
    now = time.time()
    name = 'PRIVATE' if PRIVATE_RE.search(text) else 'PUBLIC' if PUBLIC_RE.search(text) else None
    if not name and (EN_PUBLIC or EN_PRIVATE):
        try:
            head = await loop.run_in_executor(None, transcribe, pcm[:48000], 'en')
            if EN_PRIVATE_RE.search(head):
                name, text = 'PRIVATE', EN_PRIVATE[0] + '，' + text
            elif EN_PUBLIC_RE.search(head):
                name, text = 'PUBLIC', EN_PUBLIC[0] + '，' + text
        except Exception:
            pass
    if name:
        session.update(mode=name, until=now + SESSION_SECONDS)
    elif ptt:  # 按键说话不用叫名字：沿用刚才的模式，没有就公共模式（只出字幕，在公开场合最稳妥）
        if now > session['until']:
            session['mode'] = 'PUBLIC'
        session['until'] = now + SESSION_SECONDS
    elif now > session['until']:
        log(f'ignored {secs:.1f}s (no name)' + (f' [debug] {text}' if DEBUG else ''))  # 平时不记录内容
        return
    session['until'] = now + SESSION_SECONDS
    # 只喊了名字、后面没内容：打开 60 秒窗口，板子看着你等下一句，先不问 Claude
    rest = PRIVATE_RE.sub('', PUBLIC_RE.sub('', text))
    if not re.sub(r'[\W_]+', '', rest):
        log(f'called {secs:.1f}s [{session["mode"]}], waiting for the rest')
        await ws.send('F0')
        await ws.send('S在。' if session['mode'] == 'PRIVATE' else 'S在听。')
        return
    log(f'heard {secs:.1f}s [{session["mode"]}]{" (ptt)" if ptt else ""}: {text}')
    body = ANY_NAME_HEAD_RE.sub('', text, count=1).lstrip(' ，,。')
    al = parse_alarm(body)
    if al is not None:
        h, mi, mask = al
        await ws.send('L0' if h < 0 else f'L{h}:{mi:02d}|{mask}')
        log('alarm cancel all' if h < 0 else f'alarm {h}:{mi:02d} mask {mask}')
        return
    tm = parse_timer(body)
    if tm is not None:
        secs, label = tm
        await ws.send(f'W{secs}|{label}')
        log(f'timer: {secs}s "{label}"' if secs else 'timer cancel all')
        return
    f = parse_focus(text)
    if f is not None:
        await ws.send(f'M{f}')
        log(f'focus command: {f} min' if f else 'focus stop')
        return
    m = NOTE_RE.search(body)
    if m and NOTES_DIR and os.path.isdir(NOTES_DIR):
        content = body[m.end():].strip().rstrip('。.')
        if content:
            await loop.run_in_executor(None, save_note, content)
            log('note saved')
            await ws.send(f'F{FACES.index("wink")}')
            await ws.send('S记下了：' + subtitle_safe(content)[:30])
            return
    pending.append((text, session['mode']))
    await drain(ws)


# ---------- 听歌联动 ----------
# 每 4 秒看一眼 Mac 的"正在播放"（nowplaying-cli，网易云 App、浏览器都算）：
# 换歌 → 切听歌表情、挂歌名；暂停超过 30 秒 → 回平静（只撤自己切的）
NOWPLAYING = '/opt/homebrew/bin/nowplaying-cli'


def now_playing():
    try:
        out = subprocess.run([NOWPLAYING, 'get', 'title', 'artist', 'playbackRate'],
                             capture_output=True, text=True, timeout=5).stdout.splitlines()
        title, artist, rate = (out + ['', '', ''])[:3]
        title, artist = title.strip(), artist.strip()
        if title in ('null', ''):
            return None
        playing = rate.strip() not in ('0', '0.0', 'null', '')
        return {'title': title, 'artist': '' if artist == 'null' else artist, 'playing': playing}
    except Exception:
        return None


async def watch_music():
    loop = asyncio.get_running_loop()
    last_song, music_on, paused_since = None, False, None
    while True:
        await asyncio.sleep(4)
        if board_ws is None:
            continue
        np = await loop.run_in_executor(None, now_playing)
        try:
            if np and np['playing']:
                paused_since = None
                song = (np['title'], np['artist'])
                if song != last_song:
                    last_song, music_on = song, True
                    label = np['title'] + (f' — {np["artist"]}' if np['artist'] else '')
                    await board_ws.send(f'F{FACES.index("music")}')
                    await board_ws.send('S正在听：' + subtitle_safe(label)[:60])
                    log(f'music: {label}')
            elif music_on:
                paused_since = paused_since or time.time()
                if time.time() - paused_since > 30:
                    await board_ws.send('F0')
                    music_on, last_song, paused_since = False, None, None
                    log('music stopped')
        except Exception:
            pass


async def watch_enroll():
    # enroll.py 建了 enroll.flag → 提示板子开始录
    announced = False
    while True:
        await asyncio.sleep(2)
        on = os.path.exists(ENROLL_FLAG)
        if on and not announced and board_ws is not None:
            try:
                voice.samples = []
                await board_ws.send(f'F{FACES.index("wink")}')
                await board_ws.send(f'S开始录你的声音：随便说 {ENROLL_N} 句话，每句两秒以上。')
                log('enroll started')
                announced = True
            except Exception:
                pass
        if not on:
            announced = False


async def main():
    global busy, whisper_lock
    busy, whisper_lock = asyncio.Lock(), asyncio.Lock()
    if not BOARD_TOKEN:
        sys.exit('config.env 里没有 BOARD_TOKEN（照 config.example.env 抄一份再填）')
    if not PUBLIC_NAMES and not PRIVATE_NAMES:
        log('warning: config.env 里没有 NAMES_PUBLIC / NAMES_PRIVATE，只有按住 BOOT 键说话才会回应')
    wp = ensure_whisper()
    await asyncio.get_running_loop().run_in_executor(None, brain.warm)  # 预热，第一句也快
    log(f'bridge on :{PORT}  (whisper :{WHISPER_PORT}, voice {"on" if ELEVEN_KEY and VOICE_ID else "off"})')
    try:
        async with websockets.serve(handle, '0.0.0.0', PORT, max_size=2 ** 23, ping_interval=20):
            asyncio.create_task(watch_enroll())
            if os.path.exists(NOWPLAYING):
                asyncio.create_task(watch_music())
            await asyncio.Future()
    finally:
        if wp:
            wp.terminate()


if __name__ == '__main__':
    asyncio.run(main())
