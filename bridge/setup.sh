#!/bin/bash
# 对话桥一次性安装：whisper（本地转文字）、Python 依赖、模型文件。需要 Homebrew。
set -e
cd "$(dirname "$0")"
brew install whisper-cpp nowplaying-cli
pip3 install --user websockets numpy sherpa-onnx
mkdir -p models
[ -f models/ggml-small.bin ] || curl -L -o models/ggml-small.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin
# 声纹模型（可选，只有录声纹才用到）
[ -f models/3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx ] || curl -L -o models/3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx \
  https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx
[ -f config.env ] || { cp config.example.env config.env; echo "已生成 config.env，记得打开填一下"; }
[ -f persona/CLAUDE.md ] || { cp persona/CLAUDE.example.md persona/CLAUDE.md; echo "已生成 persona/CLAUDE.md，写上它的人设"; }
echo "装好了。先手动跑一次试试：python3 bridge.py"
