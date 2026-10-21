// 麦克风：ES7210 + 新版 I2S 驱动。只算音量判断"有没有人在说话"，不录音、不上传。
// 喇叭：ES8311 + NS4150B 功放（PA 脚），和麦克风共用同一个 I2S（全双工）。
// 实测（家里，一臂远）：安静 6~12，说话 100~400。
#include "audio.h"
#include "log.h"
#include <Wire.h>
#include "ESP_I2S.h"
#include "es7210.h"
extern "C" {
#include "es8311.h"
}
#include "pin_config.h"

volatile bool micVoice = false;
volatile float micLevel = 0;
volatile float micFloor = 10;
volatile bool audioSpeaking = false;
static volatile bool micOn = true;
static audio_hal_codec_mode_t micMode = AUDIO_HAL_CODEC_MODE_ENCODE;
static volatile uint32_t speakEndedAt = 0;

struct Clip { int16_t *buf; size_t bytes; };
static QueueHandle_t playQ = nullptr;

// 整句录音：开口前留半秒，停顿不超过 1.3 秒算同一句（"喊名字，（停一下）……"要录在一起），最长 15 秒
volatile bool micRecordEnabled = false;
volatile bool micPTT = false;
static bool recPTT = false;  // 这一段是按键录的
static QueueHandle_t utterQ = nullptr;
static const size_t PRE_N = 8000;              // 0.5 秒
static int16_t preRing[PRE_N];
static size_t preHead = 0;
static const size_t REC_MAX = 16000 * 15;      // 样本数
static int16_t *rec = nullptr;
static size_t recN = 0;
static size_t recVoiced = 0;  // 这段里音量过门槛的样本数（真正像说话的部分）

bool audioTakeUtterance(Utterance &u) {
  return utterQ && xQueueReceive(utterQ, &u, 0) == pdTRUE;
}

static I2SClass i2s;

static void audioTask(void *) {
  static int16_t buf[1024];  // 512 帧 × 2 声道 ≈ 32ms
  int loud = 0;
  uint32_t lastLoud = 0;
  for (;;) {
    if (!micOn) {  // 深睡：不读、不算，micVoice 保持 false
      micVoice = false;
      micLevel = 0;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    size_t got = i2s.readBytes((char *)buf, sizeof(buf));
    if (!got) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    double sum = 0;
    size_t n = got / 2;
    for (size_t i = 0; i < n; i++) sum += (double)buf[i] * buf[i];
    float rms = sqrt(sum / max((size_t)1, n));
    micLevel = rms;

    // 左声道当单声道：录音中就追加，否则塞进开口前的环形缓冲
    size_t frames = n / 2;
    if (rec) {
      for (size_t i = 0; i < frames && recN < REC_MAX; i++) rec[recN++] = buf[2 * i];
    } else {
      for (size_t i = 0; i < frames; i++) { preRing[preHead] = buf[2 * i]; preHead = (preHead + 1) % PRE_N; }
    }

    // 噪音底：往下跟得快，往上跟得很慢（说话不会把它抬上去）
    float f = micFloor;
    f = rms < f ? f + (rms - f) * 0.05f : f + (rms - f) * 0.002f;
    micFloor = constrain(f, 3.0f, 2000.0f);

    float threshold = max(45.0f, micFloor * 5);
    uint32_t now = millis();
    // 自己在说话（以及刚说完 0.5 秒内）：不算有人说话，也不更新噪音底
    if (audioSpeaking || now - speakEndedAt < 500) {
      micVoice = false;
      loud = 0;
      continue;
    }
    if (rms > threshold) {
      if (rec) recVoiced += frames;
      if (++loud >= 2) { micVoice = true; lastLoud = now; }  // 连续两帧才算，避免敲桌子
    } else {
      loud = 0;
      if (micVoice && now - lastLoud > 350) micVoice = false;
    }

    // 按键说话：按下就开录（带开口前半秒），松开才结束
    if (micPTT && rec && !recPTT) recPTT = true;  // 正在被音量触发录着时按了键：转成按键录
    if (micPTT && !rec) {
      rec = (int16_t *)ps_malloc(REC_MAX * 2);
      recN = 0;
      recVoiced = 0;
      recPTT = true;
      if (rec) for (size_t i = 0; i < PRE_N; i++) rec[recN++] = preRing[(preHead + i) % PRE_N];
    }
    if (rec && recPTT) {
      if (!micPTT || recN >= REC_MAX) {
        Utterance u = { rec, recN * 2, true };
        bool keep = recN > PRE_N + 16000 * 4 / 10;  // 按了不到 0.4 秒算误触
        if (!(keep && utterQ && xQueueSend(utterQ, &u, 0) == pdTRUE)) free(rec);
        rec = nullptr;
        recN = 0;
        recPTT = false;
      }
      continue;  // 按键录音期间不走下面的音量判断
    }

    // 开始录：有人说话、对话桥要、手上没在录
    if (micVoice && !rec && micRecordEnabled) {
      rec = (int16_t *)ps_malloc(REC_MAX * 2);
      recN = 0;
      recVoiced = 0;
      if (rec) for (size_t i = 0; i < PRE_N; i++) rec[recN++] = preRing[(preHead + i) % PRE_N];
    }
    // 结束：安静 0.9 秒或录满
    if (rec && ((!micVoice && now - lastLoud > 1300) || recN >= REC_MAX || !micRecordEnabled)) {
      Utterance u = { rec, recN * 2, false };
      // 只发像说话的：正文 ≥0.35 秒、过门槛的部分 ≥0.25 秒且占两成以上。
      // 门槛不能太高：单喊一声名字只有半秒左右（调成 0.5 秒后名字全被丢在板子上）
      size_t body = recN > PRE_N ? recN - PRE_N : 0;
      bool keep = micRecordEnabled && body > 16000 * 35 / 100 && recVoiced >= 4000 && recVoiced * 10 >= body * 2;
      if (!(keep && utterQ && xQueueSend(utterQ, &u, 0) == pdTRUE)) free(rec);
      rec = nullptr;
      recN = 0;
    }
  }
}

static void playTask(void *) {
  static int16_t stereo[512];
  Clip c;
  for (;;) {
    if (xQueueReceive(playQ, &c, portMAX_DELAY) != pdTRUE) continue;
    audioSpeaking = true;
    digitalWrite(PA, HIGH);  // 开功放
    vTaskDelay(pdMS_TO_TICKS(20));
    size_t n = c.bytes / 2;
    for (size_t i = 0; i < n; i += 256) {
      size_t m = min((size_t)256, n - i);
      for (size_t k = 0; k < m; k++) stereo[2 * k] = stereo[2 * k + 1] = c.buf[i + k];
      i2s.write((uint8_t *)stereo, m * 4);
    }
    memset(stereo, 0, sizeof(stereo));
    for (int k = 0; k < 6; k++) i2s.write((uint8_t *)stereo, sizeof(stereo));  // 尾巴补点静音，免得最后一截被吞
    digitalWrite(PA, LOW);  // 关功放，省电也免得底噪
    free(c.buf);
    speakEndedAt = millis();
    audioSpeaking = false;
    logf("spoke %u bytes\n", (unsigned)c.bytes);
  }
}

bool audioPlay(int16_t *buf, size_t bytes) {
  Clip c = { buf, bytes };
  if (!playQ || xQueueSend(playQ, &c, 0) != pdTRUE) {
    free(buf);
    return false;
  }
  return true;
}

static void speakerInit() {
  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);
  es8311_handle_t es = es8311_create(0, ES8311_ADDRRES_0);
  if (!es) { logf("es8311 create failed\n"); return; }
  const es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = 16000 * 256,
    .sample_frequency = 16000,
  };
  esp_err_t r = es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  r |= es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency);
  r |= es8311_microphone_config(es, false);
  r |= es8311_voice_volume_set(es, 75, NULL);
  logf("speaker %s\n", r == ESP_OK ? "ok" : "init failed");
  playQ = xQueueCreate(2, sizeof(Clip));
  utterQ = xQueueCreate(3, sizeof(Utterance));
  xTaskCreatePinnedToCore(playTask, "play", 4096, nullptr, 2, nullptr, 0);
}

void audioMicEnable(bool on) {
  if (on == micOn) return;
  es7210_adc_ctrl_state(micMode, on ? AUDIO_HAL_CTRL_START : AUDIO_HAL_CTRL_STOP);
  micOn = on;
  logf("mic %s\n", on ? "on" : "off (deep sleep)");
}

void audioBegin() {
  i2s.setPins(PIN_ES7210_BCLK, PIN_ES7210_LRCK, PIN_ES8311_DOUT, PIN_ES7210_DIN, PIN_ES7210_MCLK);
  if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    logf("i2s begin failed\n");
    return;
  }
  audio_hal_codec_config_t cfg = {};
  cfg.adc_input = AUDIO_HAL_ADC_INPUT_ALL;
  cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
  cfg.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
  cfg.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
  cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
  cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;
  esp_err_t r = es7210_adc_init(&Wire, &cfg);
  r |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  r |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2), (es7210_gain_value_t)GAIN_30DB);
  r |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4), (es7210_gain_value_t)GAIN_30DB);
  r |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
  logf("mic %s\n", r == ESP_OK ? "ok" : "init failed");
  speakerInit();
  xTaskCreatePinnedToCore(audioTask, "mic", 6144, nullptr, 1, nullptr, 0);
}
