#!/bin/bash
# 让对话桥开机自动启动、挂了自动重启（macOS launchd）。
# 注意：macOS 不让后台服务读"文稿 / 桌面 / 下载"里的文件，所以这个文件夹要放在别处（比如 ~/desk-companion-bridge）。
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
case "$DIR" in
  "$HOME/Documents"*|"$HOME/Desktop"*|"$HOME/Downloads"*)
    echo "这个文件夹在 文稿/桌面/下载 里，后台服务读不到。先把 bridge 文件夹挪到 ~/desk-companion-bridge 再运行。"; exit 1;;
esac
LABEL=com.desk-companion.bridge
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
PROXY_ENV=""
if [ -n "$HTTPS_PROXY" ]; then  # 需要代理才能连 Claude 的话，运行前先 export HTTPS_PROXY=...
  PROXY_ENV="<key>HTTPS_PROXY</key><string>$HTTPS_PROXY</string><key>HTTP_PROXY</key><string>$HTTPS_PROXY</string><key>NO_PROXY</key><string>127.0.0.1,localhost</string>"
fi
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array><string>/usr/bin/python3</string><string>-u</string><string>$DIR/bridge.py</string></array>
  <key>WorkingDirectory</key><string>$DIR</string>
  <key>EnvironmentVariables</key>
  <dict>
    <key>HOME</key><string>$HOME</string>
    <key>PATH</key><string>$PATH</string>
    $PROXY_ENV
  </dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>15</integer>
  <key>StandardOutPath</key><string>$DIR/bridge.log</string>
  <key>StandardErrorPath</key><string>$DIR/bridge.log</string>
</dict>
</plist>
PL
launchctl bootout gui/$(id -u)/$LABEL 2>/dev/null || true
launchctl bootstrap gui/$(id -u) "$PLIST"
echo "对话桥已在后台运行，日志：$DIR/bridge.log"
echo "停掉：launchctl bootout gui/\$(id -u)/$LABEL"
