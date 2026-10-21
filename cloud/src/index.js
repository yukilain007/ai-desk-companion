// 桌面小伙伴 · 云端中转（Cloudflare Worker + Durable Object）
// - POST /mcp/<MCP_TOKEN>   给 claude.ai 连接器用的 MCP（Streamable HTTP，无状态 JSON 回复）
// - GET  /ws/<BOARD_TOKEN>  板子用 WebSocket 长连着，收到 "F<n>" 就换表情
// Durable Object 用 WebSocket hibernation：没消息时不计时长，免费额度够用。
import { DurableObject } from 'cloudflare:workers';

const FACES = {
  calm: 0,
  wink: 1,
  curious: 2,
  meh: 3,
  music: 4,
  sleep: 5,
  smile: 6,
};

const TOOLS = [
  {
    name: 'set_face',
    description:
      "Change the face on the user's little round desk display (it travels with them: home, work, anywhere on WiFi or a phone hotspot). " +
      'Use it during conversation to show your own reaction — sparingly, when the moment earns it, not every message. ' +
      'calm = default; wink = "got it" / playful agreement; curious = "really?", skeptical, catching them (e.g. staying up late); ' +
      'meh = unimpressed (playful); music = listening to a song together (stays until changed); ' +
      'sleep = saying goodnight, eyes close and screen dims (stays until changed); smile = genuinely happy or moved. ' +
      'Short faces return to calm by themselves after a few seconds. The result says whether the display is online.',
    inputSchema: {
      type: 'object',
      properties: {
        face: { type: 'string', enum: Object.keys(FACES), description: 'Which face to show' },
      },
      required: ['face'],
    },
  },
  {
    name: 'show_subtitle',
    description:
      "SILENT TEXT on the user's little round desk display — no audio, this is NOT a voice/TTS tool. " +
      'Shows a short line from you as a subtitle under the face (the face lifts a bit while it shows). ' +
      'If they missed it, a small dot stays and they can tap to replay. ' +
      'Chinese or English, keep it short — one or two sentences (it pages automatically, ~11 Chinese characters per line, 2 lines per page). ' +
      'Use it for a line meant for their desk — a goodnight, a nudge to eat, a small tease — not to mirror your whole reply. ' +
      'Optionally pass face to change expression at the same time.',
    inputSchema: {
      type: 'object',
      properties: {
        text: { type: 'string', description: 'What to show on the display' },
        face: { type: 'string', enum: Object.keys(FACES), description: 'Optional expression to show with it' },
      },
      required: ['text'],
    },
  },
  {
    name: 'speak_on_display',
    description:
      "AUDIO OUT LOUD from the speaker of the user's little round desk display — your voice in the room with them. " +
      'Use this when they are near the display. Keep it short: one to three sentences. The text also shows as a subtitle. ' +
      'Optionally pass face to change expression. Costs text-to-speech credits, so use it when the moment earns it.',
    inputSchema: {
      type: 'object',
      properties: {
        text: { type: 'string', description: 'What to say out loud' },
        face: { type: 'string', enum: Object.keys(FACES), description: 'Optional expression to show with it' },
      },
      required: ['text'],
    },
  },
  {
    name: 'start_focus',
    description:
      "Start (or stop) a focus session / pomodoro on the user's desk display: a ring of dots around the face counts down, " +
      'the display stays quiet, and it chimes when time is up. Use when they ask you to start a focus timer, or to help them stick to a task. ' +
      'minutes: default 25. stop: true to end the current session early.',
    inputSchema: {
      type: 'object',
      properties: {
        minutes: { type: 'integer', minimum: 1, maximum: 180, description: 'Length in minutes (default 25)' },
        stop: { type: 'boolean', description: 'End the current focus session early' },
      },
    },
  },
  {
    name: 'set_timer',
    description:
      "Set a real reminder/timer on the user's desk display: when time is up it chimes and shows the reminder text until they tap it. " +
      'Up to 4 at once; works while the display sleeps. Use when they ask to be reminded in N minutes (oven, laundry, a call). ' +
      'minutes: how long from now. label: short reminder text (e.g. 看面团). cancel_all: true to clear all timers. ' +
      'Only say it is set if the result says so (if the display is offline it is NOT set).',
    inputSchema: {
      type: 'object',
      properties: {
        minutes: { type: 'number', minimum: 0.1, maximum: 720, description: 'Minutes from now' },
        label: { type: 'string', description: 'What to remind them of (short)' },
        cancel_all: { type: 'boolean', description: 'Cancel all timers' },
      },
    },
  },
  {
    name: 'set_alarm',
    description:
      "Set a wake-up alarm on the user's desk display: gentle chime first, then a louder wake-up sound or recorded voice. " +
      'Saved on the device (survives reboot), up to 3. ' +
      'time: "HH:MM" 24h. repeat: once (next occurrence), daily, weekdays, weekends. cancel_all: true to clear all alarms. ' +
      'Only say it is set if the result says so.',
    inputSchema: {
      type: 'object',
      properties: {
        time: { type: 'string', description: '"HH:MM", 24-hour' },
        repeat: { type: 'string', enum: ['once', 'daily', 'weekdays', 'weekends'], description: 'Default once' },
        cancel_all: { type: 'boolean', description: 'Cancel all alarms' },
      },
    },
  },
  {
    name: 'get_status',
    description:
      "Check the user's desk display: whether it's online, battery percent, whether it's charging, and focus sessions done today. " +
      'Use when they ask about it, or before nudging them to charge it.',
    inputSchema: { type: 'object', properties: {} },
  },
];

const ALIVE_MS = 75000; // 板子每 30 秒 ping 一次；75 秒没动静算离线

export class FaceRoom extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    // 板子每 30 秒发 "ping" 保活；自动回 "pong"，不用唤醒 DO
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/ws') {
      // 板子重连时旧连接可能还没断，先关掉，免得"在线数"虚高
      for (const old of this.ctx.getWebSockets()) {
        try {
          old.close(1000, 'replaced');
        } catch {}
      }
      const pair = new WebSocketPair();
      this.ctx.acceptWebSocket(pair[1]);
      pair[1].serializeAttachment({ t: Date.now() });
      // 还没收到回执的定时问候：没过期就补发（收到 "A<id>" 才删）
      const pending = await this.ctx.storage.get('pending');
      if (pending) {
        if (Date.now() < pending.exp) {
          try {
            pair[1].send(`Q${pending.id}`);
            for (const m of pending.msgs) pair[1].send(m);
          } catch {}
        } else {
          await this.ctx.storage.delete('pending');
        }
      }
      return new Response(null, { status: 101, webSocket: pair[0] });
    }
    if (url.pathname === '/audio') {
      // 语音：先发表情/字幕，再 "P<字节数>" + 8KB 一块的二进制 PCM + "E"
      const pre = JSON.parse(request.headers.get('x-pre') || '[]');
      const pcm = new Uint8Array(await request.arrayBuffer());
      const alive = this.alive();
      for (const ws of alive) {
        try {
          for (const m of pre) ws.send(m);
          ws.send(`P${pcm.length}`);
          for (let i = 0; i < pcm.length; i += 8192) ws.send(pcm.subarray(i, i + 8192));
          ws.send('E');
        } catch {}
      }
      return Response.json({ online: alive.length });
    }
    if (url.pathname === '/send') {
      const { msgs, queueMs } = await request.json();
      const alive = this.alive();
      for (const ws of alive) {
        try {
          for (const m of msgs) ws.send(m);
        } catch {}
      }
      return Response.json({ online: alive.length });
    }
    if (url.pathname === '/queue') {
      // 定时问候：先存一份再发，带编号 "Q<id>"；板子回 "A<id>" 才算送到。
      // 刚断线时 alive() 还会以为在线（75 秒才超时），没有回执就会在下次连上时补发
      const { msgs, queueMs } = await request.json();
      const id = Date.now().toString(36);
      await this.ctx.storage.put('pending', { id, msgs, exp: Date.now() + queueMs });
      const alive = this.alive();
      for (const ws of alive) {
        try {
          ws.send(`Q${id}`);
          for (const m of msgs) ws.send(m);
        } catch {}
      }
      return Response.json({ online: alive.length });
    }
    if (url.pathname === '/status') {
      const alive = this.alive();
      const bat = (await this.ctx.storage.get('battery')) || null;
      const hist = (await this.ctx.storage.get('bathist')) || [];
      const focus = (await this.ctx.storage.get('focus')) || [];
      const tz = tzMs(this.env);
      const dayStart = Math.floor((Date.now() + tz) / 86400000) * 86400000 - tz;  // 本地时间今天 0 点
      const today = focus.filter((f) => f.t >= dayStart);
      return Response.json({ online: alive.length > 0, battery: bat, estimate: batteryEstimate(hist, bat),
        focusToday: { count: today.length, minutes: today.reduce((a, f) => a + f.m, 0) } });
    }
    if (url.pathname === '/log' && request.method === 'POST') {
      // 最近 30 条 MCP 请求，排查"手机端到底有没有调用"用
      const entry = await request.json();
      const log = (await this.ctx.storage.get('log')) || [];
      log.push(entry);
      await this.ctx.storage.put('log', log.slice(-30));
      return new Response(null, { status: 204 });
    }
    if (url.pathname === '/log') {
      const log = (await this.ctx.storage.get('log')) || [];
      const battery = (await this.ctx.storage.get('battery')) || null;
      const bathist = (await this.ctx.storage.get('bathist')) || [];
      const boots = (await this.ctx.storage.get('boots')) || [];
      const pending = (await this.ctx.storage.get('pending')) || null;
      return Response.json({ online: this.alive().length, battery, bathist, boots, pending, log });
    }
    return new Response('not found', { status: 404 });
  }

  // 板子每分钟报一次电量 "B<百分比>,<充电中>,<插着USB>"
  async webSocketMessage(ws, msg) {
    // 完成一次专注 "D<分钟>"：留 30 天
    if (typeof msg === 'string' && msg.startsWith('D')) {
      const focus = (await this.ctx.storage.get('focus')) || [];
      focus.push({ t: Date.now(), m: Number(msg.slice(1)) || 0 });
      const cutoff = Date.now() - 30 * 24 * 3600 * 1000;
      await this.ctx.storage.put('focus', focus.filter((f) => f.t >= cutoff));
      return;
    }
    // 定时问候的回执
    if (typeof msg === 'string' && msg.startsWith('A')) {
      const pending = await this.ctx.storage.get('pending');
      if (pending && pending.id === msg.slice(1)) await this.ctx.storage.delete('pending');
      return;
    }
    // 板子开机后报上次重启原因 "R<原因>"：留最近 20 次，用来查卡死/崩溃
    if (typeof msg === 'string' && msg.startsWith('R')) {
      const boots = (await this.ctx.storage.get('boots')) || [];
      boots.push({ t: Date.now(), reason: msg.slice(1) });
      await this.ctx.storage.put('boots', boots.slice(-20));
      return;
    }
    if (typeof msg === 'string' && msg.startsWith('B')) {
      const [percent, charging, usb] = msg.slice(1).split(',').map(Number);
      const now = Date.now();
      await this.ctx.storage.put('battery', { percent, charging: !!charging, usb: !!usb, t: now });
      // 电量历史：每 10 分钟一笔（插拔变化时立刻记），留 7 天，用来算续航
      const hist = (await this.ctx.storage.get('bathist')) || [];
      const last = hist[hist.length - 1];
      if (!last || now - last.t >= 10 * 60 * 1000 || last.u !== !!usb) {
        hist.push({ t: now, p: percent, u: !!usb });
        const cutoff = now - 7 * 24 * 3600 * 1000;
        await this.ctx.storage.put('bathist', hist.filter((h) => h.t >= cutoff));
      }
    }
  }

  alive() {
    const now = Date.now();
    return this.ctx.getWebSockets().filter((ws) => {
      const pong = this.ctx.getWebSocketAutoResponseTimestamp(ws)?.getTime() || 0;
      const born = ws.deserializeAttachment()?.t || 0;
      return now - Math.max(pong, born) < ALIVE_MS;
    });
  }

  webSocketClose(ws, code) {
    try {
      ws.close(code, 'bye');
    } catch {}
  }
}

// 板子的字库（Unifont 中文子集）没有带重音的拉丁字母：café 会显示成 caf。
// 字幕里把它们换成不带重音的（只动拉丁扩展区，汉字、假名、中文标点不碰），语音仍用原文。
function subtitleSafe(text) {
  return text.replace(/[\u00C0-\u024F]/g, (ch) => ch.normalize('NFD').replace(/[\u0300-\u036f]/g, '') || ch);
}

// 续航估算：取最近一段"没插电"的记录，算每小时掉多少电
function batteryEstimate(hist, bat) {
  if (!bat || bat.usb) return null;
  let i = hist.length - 1;
  while (i > 0 && !hist[i - 1].u) i--;  // 往回找到这次拔电的起点
  const seg = hist.slice(i).filter((h) => !h.u);
  if (seg.length < 2) return null;
  const first = seg[0], lastH = seg[seg.length - 1];
  const hours = (lastH.t - first.t) / 3600000;
  const drop = first.p - lastH.p;
  if (hours < 0.5 || drop <= 0) return null;
  const perHour = drop / hours;
  return { perHour: Math.round(perHour * 10) / 10, hoursLeft: Math.round((bat.percent / perHour) * 10) / 10, sinceUnplugH: Math.round(hours * 10) / 10 };
}

// ---------- 时区 ----------
// wrangler.toml 里 TZ_OFFSET_HOURS（相对 UTC 的小时数，北京 / 新加坡 = 8）
function tzMs(env) {
  return (Number(env.TZ_OFFSET_HOURS ?? 8) || 0) * 3600000;
}

function localHHMM(env, t = Date.now()) {
  const d = new Date(t + tzMs(env));
  return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`;
}

// ---------- 定时问候（Cron Trigger，按本地时间）----------
// 只出字幕不出声（不花额度）。想关掉：wrangler.toml 里 GREETINGS = "off"。想改句子：直接改这里
const GREETINGS = {
  '08:30': {
    face: 'wink',
    lines: ['早。先吃早饭，再看手机。', '早上好。今天也慢慢来。', '早。先吃点东西。', '早上好。昨晚睡够了吗？'],
  },
  '12:00': {
    face: 'curious',
    lines: ['十二点了，吃饭。', '午饭时间。别随便糊弄过去。', '该吃午饭了，吃完再忙。', '中午了。今天吃什么？'],
  },
  '15:30': {
    face: 'calm',
    lines: ['喝口水。', '起来走两步，顺便倒杯水。', '下午了，喝点水，眼睛也歇一下。', '坐太久了，站起来动动。'],
  },
  '18:30': {
    face: 'calm',
    lines: ['晚饭吃了吗？', '六点半了，记得吃晚饭。', '今天辛苦了。先吃饭。', '晚饭别拖太晚。'],
  },
};

// ---------- 天气（早安问候用，Open-Meteo 免费接口，不用 key）----------
// wrangler.toml 里 WEATHER_PLACES，JSON 数组：第一个是你在的地方（下雨提醒带伞），
// 其它标 family: true 的是家人在的地方（降温 / 大雨提醒你跟家人说一声）。不填就不播天气
function places(env) {
  try {
    const p = JSON.parse(env.WEATHER_PLACES || '[]');
    return Array.isArray(p) ? p.slice(0, 3) : [];
  } catch {
    return [];
  }
}

function weatherWord(code) {
  if (code === 0) return '晴';
  if (code <= 2) return '多云';
  if (code === 3) return '阴';
  if (code === 45 || code === 48) return '有雾';
  if (code >= 51 && code <= 57) return '毛毛雨';
  if (code === 65 || code === 82) return '大雨';
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return '有雨';
  if ((code >= 71 && code <= 77) || code === 85 || code === 86) return '下雪';
  if (code >= 95) return '雷雨';
  return '';
}

async function weatherLine(env) {
  const PLACES = places(env);
  if (!PLACES.length) return '';
  const q = new URLSearchParams({
    latitude: PLACES.map((p) => p.lat).join(','),
    longitude: PLACES.map((p) => p.lon).join(','),
    daily: 'weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max',
    timezone: 'auto',
    past_days: '1',
    forecast_days: '1',
  });
  try {
    const r = await fetch(`https://api.open-meteo.com/v1/forecast?${q}`);
    let data = await r.json();
    if (!Array.isArray(data)) data = [data];
    const parts = [];
    PLACES.forEach((p, i) => {
      const d = data[i] && data[i].daily;
      if (!d) return;
      const t = d.time.length - 1;  // 今天（前一个是昨天）
      const lo = Math.round(d.temperature_2m_min[t]), hi = Math.round(d.temperature_2m_max[t]);
      const w = weatherWord(d.weather_code[t]);
      const rain = (d.precipitation_probability_max[t] || 0) >= 50 || /雨|雪/.test(w);
      const drop = t > 0 ? Math.round(d.temperature_2m_max[t - 1] - d.temperature_2m_max[t]) : 0;
      let line = `${p.name} ${lo}～${hi}°${w ? '，' + w : ''}`;
      if (i === 0 && rain) line += '，记得带伞';
      if (p.family && (drop >= 6 || /大雨|下雪|雷雨/.test(w))) line += drop >= 6 ? '，降温了，提醒家人加衣服' : '，提醒家人注意';
      parts.push(line);
    });
    return parts.length ? parts.join('。') + '。' : '';
  } catch {
    return '';
  }
}

async function sendGreeting(env, hhmm) {
  const g = GREETINGS[hhmm];
  if (!g) return;
  const day = Math.floor((Date.now() + tzMs(env)) / 86400000);  // 按天换句子
  let line = g.lines[day % g.lines.length];
  if (hhmm === '08:30') {  // 早安带上天气
    const w = await weatherLine(env);
    if (w) line = `早。${w}${line.replace(/^早[。上好]*。?/, '')}`;
  }
  const msgs = [`F${FACES[g.face]}`, `H${line}`];  // H = 挂着的字幕：挂到点一下或 10 分钟
  await room(env).fetch('https://room/queue', { method: 'POST', body: JSON.stringify({ msgs, queueMs: 2 * 3600000 }) });
  await room(env).fetch('https://room/log', {
    method: 'POST',
    body: JSON.stringify({ t: new Date().toISOString(), ua: 'cron', calls: [`greeting ${hhmm} "${line}"`] }),
  });
}

function room(env) {
  return env.ROOM.get(env.ROOM.idFromName('main'));
}

async function handleRpc(msg, env) {
  const { id, method, params } = msg;
  if (id === undefined || id === null) return null; // notification
  const ok = (result) => ({ jsonrpc: '2.0', id, result });
  switch (method) {
    case 'initialize':
      return ok({
        protocolVersion: params?.protocolVersion || '2025-06-18',
        capabilities: { tools: {} },
        serverInfo: { name: 'desk-companion', version: '1.0.0' },
      });
    case 'ping':
      return ok({});
    case 'tools/list':
      return ok({ tools: TOOLS });
    case 'tools/call': {
      const name = params?.name;
      const args = params?.arguments || {};
      if (name === 'get_status') {
        const st = await (await room(env).fetch('https://room/status')).json();
        const b = st.battery;
        const ago = b ? Math.round((Date.now() - b.t) / 60000) : null;
        const parts = [st.online ? 'Display is online.' : 'Display is offline.'];
        if (b) parts.push(`Battery ${b.percent}%${b.usb ? (b.charging ? ', charging' : ', plugged in (full or not charging)') : ', on battery'} (reported ${ago} min ago).`);
        else parts.push('No battery report yet.');
        if (st.focusToday && st.focusToday.count) parts.push(`Focus today: ${st.focusToday.count} session(s), ${st.focusToday.minutes} min.`);
        else parts.push('No focus sessions completed today.');
        const e = st.estimate;
        if (e) parts.push(`On battery for ${e.sinceUnplugH} h, dropping ~${e.perHour}%/h — about ${e.hoursLeft} h left.`);
        return ok({ content: [{ type: 'text', text: parts.join(' ') }] });
      }
      const face = args.face;
      const msgs = [];
      if (name === 'set_face') {
        if (!(face in FACES)) return ok({ content: [{ type: 'text', text: `Unknown face: ${face}` }], isError: true });
        msgs.push(`F${FACES[face]}`);
      } else if (name === 'set_alarm') {
        if (args.cancel_all) msgs.push('L0');
        else {
          const m = /^(\d{1,2}):(\d{2})$/.exec(String(args.time || '').trim());
          if (!m || +m[1] > 23 || +m[2] > 59) return ok({ content: [{ type: 'text', text: 'time must be "HH:MM" (24h)' }], isError: true });
          const mask = { daily: 127, weekdays: 62, weekends: 65 }[args.repeat] || 128;
          msgs.push(`L${+m[1]}:${m[2]}|${mask}`);
        }
      } else if (name === 'set_timer') {
        if (args.cancel_all) msgs.push('W0|');
        else {
          const secs = Math.max(5, Math.min(720 * 60, Math.round((Number(args.minutes) || 0) * 60)));
          const label = String(args.label || '').replace(/[|\n]/g, ' ').trim().slice(0, 24);
          msgs.push(`W${secs}|${label}`);
        }
      } else if (name === 'start_focus') {
        msgs.push(args.stop ? 'M0' : `M${Math.max(1, Math.min(180, parseInt(args.minutes) || 25))}`);
      } else if (name === 'speak_on_display') {
        const text = String(args.text || '').replace(/\s+/g, ' ').trim().slice(0, 300);
        if (!text) return ok({ content: [{ type: 'text', text: 'Empty text' }], isError: true });
        if (face !== undefined && !(face in FACES)) return ok({ content: [{ type: 'text', text: `Unknown face: ${face}` }], isError: true });
        if (!env.ELEVENLABS_API_KEY) return ok({ content: [{ type: 'text', text: 'ELEVENLABS_API_KEY not set on the worker.' }], isError: true });
        const st = await (await room(env).fetch('https://room/status')).json();
        if (!st.online) return ok({ content: [{ type: 'text', text: 'Display is offline — nothing spoken (no credits used).' }], isError: true });
        // 16kHz 单声道 16bit PCM，板子不用解码直接播
        if (!env.VOICE_ID) return ok({ content: [{ type: 'text', text: 'VOICE_ID not set in wrangler.toml.' }], isError: true });
        const tts = await fetch(`https://api.elevenlabs.io/v1/text-to-speech/${env.VOICE_ID}?output_format=pcm_16000`, {
          method: 'POST',
          headers: { 'xi-api-key': env.ELEVENLABS_API_KEY, 'Content-Type': 'application/json' },
          body: JSON.stringify({
            text,
            model_id: 'eleven_flash_v2_5',
            voice_settings: { stability: 0.5, similarity_boost: 0.75, speed: Number(env.VOICE_SPEED || 1) },
          }),
        });
        if (!tts.ok) return ok({ content: [{ type: 'text', text: `TTS error ${tts.status}: ${(await tts.text()).slice(0, 200)}` }], isError: true });
        const pre = [];
        if (face !== undefined) pre.push(`F${FACES[face]}`);
        pre.push(`S${subtitleSafe(text)}`);
        const r = await room(env).fetch('https://room/audio', { method: 'POST', headers: { 'x-pre': JSON.stringify(pre) }, body: await tts.arrayBuffer() });
        const { online } = await r.json();
        return ok({ content: [{ type: 'text', text: online ? 'Spoken on the display' : 'Display went offline — not spoken.' }], isError: !online });
      } else if (name === 'show_subtitle' || name === 'say') {
        const text = String(args.text || '').replace(/\s+/g, ' ').trim().slice(0, 120);
        if (!text) return ok({ content: [{ type: 'text', text: 'Empty text' }], isError: true });
        if (face !== undefined) {
          if (!(face in FACES)) return ok({ content: [{ type: 'text', text: `Unknown face: ${face}` }], isError: true });
          msgs.push(`F${FACES[face]}`);
        }
        msgs.push(`S${subtitleSafe(text)}`);
      } else {
        return ok({ content: [{ type: 'text', text: `Unknown tool: ${name}` }], isError: true });
      }
      const r = await room(env).fetch('https://room/send', { method: 'POST', body: JSON.stringify({ msgs }) });
      const { online } = await r.json();
      const what = name === 'set_alarm' ? (args.cancel_all ? 'All alarms cancelled' : `Alarm set: ${args.time} (${args.repeat || 'once'})`) : name === 'set_timer' ? (args.cancel_all ? 'All timers cancelled' : `Timer set: ${args.minutes} min${args.label ? ` (${args.label})` : ''}`) : name === 'set_face' ? `Face set: ${face}` : name === 'start_focus' ? (args.stop ? 'Focus session stopped' : `Focus session started (${parseInt(args.minutes) || 25} min)`) : 'Subtitle shown on the display';
      const text = online ? what : 'Display is offline (not connected to WiFi right now) — nothing shown.';
      return ok({ content: [{ type: 'text', text }], isError: !online });
    }
    default:
      return { jsonrpc: '2.0', id, error: { code: -32601, message: `Method not found: ${method}` } };
  }
}

function eq(a, b) {
  // 定长比较，避免按时间猜密钥
  if (!a || !b || a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}

export default {
  async scheduled(event, env, ctx) {
    // 每 30 分钟跑一次：到了问候时间就问候
    if (env.GREETINGS === 'off') return;
    ctx.waitUntil(sendGreeting(env, localHHMM(env, event.scheduledTime)));
  },

  async fetch(request, env) {
    const url = new URL(request.url);
    const [, kind, token] = url.pathname.split('/');
    const tokenOk = (kind === 'mcp' && eq(token, env.MCP_TOKEN)) || (kind === 'ws' && eq(token, env.BOARD_TOKEN));

    // 除了板子长连接和看日志本身，所有请求都记一笔（密钥打码），排查连接器握手用
    if (kind !== 'ws' && !url.searchParams.has('log')) {
      const entry = {
        t: new Date().toISOString(),
        ua: (request.headers.get('user-agent') || '').slice(0, 40),
        calls: [`${request.method} /${kind || ''}${token ? (tokenOk ? '/<ok>' : '/<bad>') : ''}${url.pathname.split('/').slice(3).join('/')}`],
      };
      if (request.method === 'POST' && kind === 'mcp' && tokenOk) {
        try {
          const b = await request.clone().json();
          entry.calls = (Array.isArray(b) ? b : [b]).map(
            (m) =>
              m.method +
              (m.params?.name ? `:${m.params.name}` : '') +
              (m.params?.arguments?.face ? `:${m.params.arguments.face}` : '') +
              (m.params?.arguments?.text ? ` "${String(m.params.arguments.text).slice(0, 20)}"` : '')
          );
        } catch {}
      }
      await room(env).fetch('https://room/log', { method: 'POST', body: JSON.stringify(entry) });
    }

    if (kind === 'ws' && eq(token, env.BOARD_TOKEN)) {
      if (request.headers.get('Upgrade') !== 'websocket') return new Response('expected websocket', { status: 426 });
      return room(env).fetch(new Request('https://room/ws', request));
    }

    if (kind === 'mcp' && eq(token, env.MCP_TOKEN)) {
      if (request.method === 'GET' && url.searchParams.has('log')) return room(env).fetch('https://room/log');
      if (request.method === 'GET' && url.searchParams.has('greet')) {  // 测试：手动触发一次定时问候
        await sendGreeting(env, url.searchParams.get('greet'));
        return new Response('ok');
      }
      if (request.method !== 'POST') return new Response(null, { status: 405, headers: { Allow: 'POST' } });
      let body;
      try {
        body = await request.json();
      } catch {
        return Response.json({ jsonrpc: '2.0', id: null, error: { code: -32700, message: 'Parse error' } }, { status: 400 });
      }
      if (Array.isArray(body)) {
        const out = (await Promise.all(body.map((m) => handleRpc(m, env)))).filter(Boolean);
        return out.length ? Response.json(out) : new Response(null, { status: 202 });
      }
      const out = await handleRpc(body, env);
      return out ? Response.json(out) : new Response(null, { status: 202 });
    }

    return new Response('not found', { status: 404 });
  },
};
