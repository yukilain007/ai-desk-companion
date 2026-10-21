#pragma once
#include <Arduino.h>

// 麦克风（ES7210）在核心 0 上单独读；主循环只看这几个值
extern volatile bool micVoice;     // 现在有人在说话
extern volatile float micLevel;    // 当前音量（RMS）
extern volatile float micFloor;    // 环境噪音底

extern volatile bool audioSpeaking;  // 喇叭正在说话（这时麦克风不判断有人说话）

extern volatile bool micRecordEnabled;  // 对话桥连着且耳朵开着时才录整句
extern volatile bool micPTT;            // 按住 BOOT 键说话：不管耳朵、不靠音量判断，按住就录

struct Utterance { int16_t *buf; size_t bytes; bool ptt; };  // 16kHz 单声道 16bit，ps_malloc 来的
bool audioTakeUtterance(Utterance &u);              // 取一段录好的话（取走的人负责 free）

void audioBegin();
void audioMicEnable(bool on);  // 深睡时关麦克风（ES7210 待机、不读数据），醒来再开
// 播放一段 16kHz 单声道 16bit PCM。buf 必须是 ps_malloc 来的，交给播放任务，播完它会 free。
bool audioPlay(int16_t *buf, size_t bytes);
