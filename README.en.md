# Desk Companion

[中文](README.md) | English

A palm-sized round screen with a living face. Keep it on your desk at home, take it out in public, or slip it into your pocket. The Claude you chat with can use it to change expressions, leave a line on the screen, speak out loud, start a focus timer, and wake you up in the morning.

![Seven expressions](docs/faces.png)

> This is the general-purpose version of a project one person built for themselves. The face is generic and the persona is left blank, so you can shape it around your own AI companion.
>
> **Personal, non-commercial use only.** You may build it, modify it, use it, and share it with friends. You **may not use it commercially, modified or not** (including selling finished units, paid builds or flashing, or offering it as a paid service). For commercial use or custom versions, contact the author. See [License](#license).

---

## What it does

**On its own (no network needed)**
- Blinks and glances around; when idle for a while it winks or raises a brow
- Tap to change expression; slide a finger across the screen to pet it and it squints and blushes
- Cover the screen with your palm and it closes its eyes with a soft "mm"; let go and it says "found you"
- Wakes up when picked up, gets annoyed when shaken hard, sleeps when placed face down
- Double-tap for a 25-minute focus session: a ring of dots, one dot per minute, chimes when done
- Plays a soft chime when a message from Claude arrives, so you don't miss it
- Timers (up to 4) and alarms (up to 3, stored on the board, survive power loss)
- Reminds you to go to bed, and nags again if you're still up late
- On battery: screen off (light sleep) after 20 s of inactivity, deep sleep (mic off) after 5 min

**When online**
- Claude on claude.ai (web / mobile app) can use it: change expressions, show subtitles, speak, start focus, set alarms, check battery
- Daily greetings at set times (the morning one includes the weather)
- iPhone calls and WeChat messages show up on screen (only who it's from, never the content)
- Set up WiFi from your phone over Bluetooth

**At home / in public, with your Mac on**
- Say its name and talk to it; it replies with expressions and subtitles (and can speak if you want)
- Two names: a public name gets short, polite, subtitle-only replies (for when others are around); a private name for relaxed conversation
- "Remind me in 15 minutes", "wake me at 7 tomorrow", "focus for 40 minutes", "note this down…" take effect directly
- Optional voiceprint so it only responds to you
- Follows along when music plays on your Mac

> **Note:** voice recognition is currently set up for **Mandarin Chinese** (whisper language = `zh`), and the timer/alarm phrases are parsed in Chinese. Supporting other languages means changing the whisper language and the parsing rules in `bridge/bridge.py`.

---

## What you need

| Item | Notes |
|---|---|
| Board | **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (1.75" round AMOLED with built-in mic, speaker, IMU, battery connector). Other boards need pin and driver changes |
| Li-ion battery | Connector on the back (optional; without it, keep it on USB) |
| Mac | To flash the firmware and run the voice bridge. The bridge is macOS-only for now |
| Claude account | Add a "custom connector" on claude.ai; the voice bridge uses Claude Code (the `claude` command), logged in to the same account |
| Cloudflare account | For the cloud relay; the free tier is enough. Your own domain is recommended (workers.dev is often unreachable in mainland China) |
| ElevenLabs (optional) | Only needed if you want it to speak |

The three parts are independent, so you can use just some of them:
- **Firmware only**: a little desk pet that moves, keeps time, and wakes you up
- **Firmware + cloud**: Claude on claude.ai can use it
- **Firmware + bridge**: talk to it near your Mac
- **All of it**: everything

---

## 1. Flash the firmware

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/) and the ESP32 core (3.1.x):
   ```bash
   brew install arduino-cli universal-ctags
   arduino-cli core install esp32:esp32@3.1.0 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
2. Install the libraries:
   ```bash
   arduino-cli lib install "GFX Library for Arduino" U8g2 SensorLib XPowersLib WebSockets NimBLE-Arduino
   ```
3. Copy `firmware/config.example.h` to `firmware/config.h` and fill in your name, WiFi, and timezone (it compiles without it, just offline)
4. Plug the board into your Mac over USB and run:
   ```bash
   ./tools/flash.sh
   ```
   You're done when you see `flashed`.

**Want a custom face or wake-up voice?** This version has the generic face, and alarms use chimes. Custom expressions (designed after your companion) or waking you up in their voice are available as custom work from the author.

---

## 2. Cloud relay (so claude.ai can use it)

1. Edit `cloud/wrangler.toml`: domain, timezone, weather cities
2. Deploy and set the secrets:
   ```bash
   cd cloud
   npm install
   npx wrangler login
   npx wrangler secret put MCP_TOKEN        # a long random string; it acts as the connector's key
   npx wrangler secret put BOARD_TOKEN      # another random string, same as in firmware/config.h
   npx wrangler secret put ELEVENLABS_API_KEY   # optional
   npx wrangler deploy
   ```
   Generate random strings with `openssl rand -hex 24`.
3. claude.ai → Settings → Connectors → Add custom connector, with the URL:
   ```
   https://your-domain/mcp/your-MCP_TOKEN
   ```
   **This URL is the key. Don't share it.**
4. To have Claude use it on its own initiative, add a paragraph to your claude.ai personal preferences. See [docs/claude-preference.md](docs/claude-preference.md) (in Chinese; easy to adapt)

---

## 3. Voice bridge (talk to it near your Mac)

```bash
cp -r bridge ~/desk-companion-bridge     # keep it outside ~/Documents so the background service can read it
cd ~/desk-companion-bridge
./setup.sh                               # installs whisper, Python deps, downloads models
```
Then:
1. Open `config.env` and fill in `BOARD_TOKEN`, your name, and its names
2. Write `persona/CLAUDE.md`: who it is, who you are, how you two get along (this is its personality)
3. Run `claude` in a terminal once to log in
4. Try it: `python3 bridge.py`, then say its name to the board
5. Once it works, start it at login: `./install_launchd.sh`

**Voiceprint (optional):** `python3 enroll.py`, then say 8 sentences as prompted on screen. After that it only responds to your voice. The voiceprint stays on your Mac.

**Privacy:** the board only records a full sentence when it hears someone speaking, and sends it to whisper running on your own Mac (offline). If its name wasn't said, the transcript is discarded and nothing is logged.

---

## WiFi setup

Three ways:
- Put it in `firmware/config.h` (baked in when flashing)
- Over USB: `python3 tools/add_wifi.py` (password is hidden as you type)
- From your phone: install **LightBlue** from the App Store, connect to the board (iPhone will ask to pair), find the "WiFi设置" (WiFi settings) characteristic and write `SSID|password`

2.4 GHz only. For an iPhone hotspot, turn on "Maximize Compatibility". It remembers up to 6 networks and picks automatically, preferring the one where it can find your Mac.

---

## Serial commands (for debugging)

`python3 tools/send.py "command"`:

| Command | Effect |
|---|---|
| `0`–`6` | Change expression (calm / wink / curious / meh / music / sleep / smile) |
| `SHello` | Show a subtitle |
| `P` | Battery, network, memory status |
| `M25` / `M0` | Start a 25-minute focus session / stop |
| `W600\|check the dough` / `W0` | Reminder in 10 minutes / cancel all |
| `L7:30\|62` / `L0` | Weekday 7:30 alarm (62 = Mon–Fri, 127 = every day, 128 = once) / cancel all |
| `Nssid\|password` / `Nlist` / `Ndel:ssid` | Add / list / delete WiFi |
| `B` | Pretend to be on battery (test sleep while plugged in) |

---

## Known limitations

- **Battery life**: still being measured. With WiFi and mic on and the screen always on, a small battery lasts less than a day or two; two-stage sleep has been added but real numbers aren't in yet
- **Voice bridge is macOS-only**; Windows / Linux need changes (mainly launchd and finding the Mac on the network)
- **Mandarin-only speech recognition** (whisper set to Chinese); English names are caught by re-recognizing the start of each sentence in English
- About 70 KB of free heap (Bluetooth takes a lot); keep an eye on it when adding bigger features
- The metal case blocks a bit of WiFi signal

## Pitfalls we hit (to save you time)

- On Apple Silicon Macs without Rosetta, Arduino's bundled ctags won't run: all code lives in `.cpp` files, the `.ino` is a one-line comment, and Homebrew's ctags is used
- Don't use `grep -q` to check flash success in scripts: it exits early and kills the upload
- The CST9217 touch chip locks up if polled continuously: read only after an interrupt, and reset it after repeated empty reads
- Serial logging blocks the main loop when nobody is reading: all logs go through `logf()`, which checks for buffer space first
- Don't put names in the whisper prompt: when it can't hear clearly it echoes the prompt back, which looks like someone said the name
- macOS background services (launchd) can't read files in Documents / Desktop / Downloads
- The board's Chinese font has no accented Latin letters like é; the cloud strips accents from subtitles before sending

---

## License

- **Code written for this project**: [PolyForm Noncommercial 1.0.0](LICENSE)
  - ✅ Allowed: personal learning, building one for yourself, modifying, sharing with friends, non-profit use
  - ❌ Not allowed: any commercial use, modified or not: selling finished units or kits, paid builds or flashing, turning it into a paid product or service
  - For commercial use or custom versions (custom face, voice, persona), contact the author
  - Keep the LICENSE file and author attribution when sharing
- **Third-party files keep their original licenses**: `firmware/es7210.*`, `es8311.*`, `audio_hal.h` are Espressif drivers; `firmware/pin_config.h` is from Waveshare's official examples (Apache 2.0, see [docs/LICENSE-waveshare-apache2.txt](docs/LICENSE-waveshare-apache2.txt))
- Libraries used (under their own licenses): Arduino_GFX, U8g2 (Unifont CJK font), SensorLib, XPowersLib, arduinoWebSockets, NimBLE-Arduino, whisper.cpp, sherpa-onnx

> Note: because of the non-commercial restriction, this project is strictly "source-available", not "open source" as defined by the OSI.
