#include <opus.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "main.h"

#define WEBRTC_SAMPLE_RATE (16000)
#define BOARD_I2S_SAMPLE_RATE (48000)
#define UPSAMPLE_RATIO (BOARD_I2S_SAMPLE_RATE / WEBRTC_SAMPLE_RATE)

#define OPUS_BUFFER_SIZE 1276
#define PCM_SAMPLES_PER_FRAME 320
#define PCM_BUFFER_SIZE (PCM_SAMPLES_PER_FRAME * sizeof(int16_t))
#define BOARD_FRAME_SAMPLES (PCM_SAMPLES_PER_FRAME * UPSAMPLE_RATIO * 2)
#define BOARD_FRAME_BYTES (BOARD_FRAME_SAMPLES * sizeof(int32_t))

#define OPUS_ENCODER_BITRATE 30000
#define OPUS_ENCODER_COMPLEXITY 0
#define OPUS_EXPECTED_PACKET_LOSS_PCT 10

// TRANSPORT-DIRECTION-SYMMETRY:
// | mechanism | server -> satellite | satellite -> server |
// | Opus FEC | server hint + firmware FEC decode | firmware hint + server FEC decode |
// | RFC 2198 | server wrap + firmware unwrap | firmware wrap + server unwrap |
// | seq gaps | firmware PLC | server PLC |
// | counters | /playback/stats | /health.uplink_loss |
#define I2S_WRITE_TIMEOUT_MS 200
#define XVF_CONTROL_TIMEOUT_MS 100
#define XVF_CONTROL_RETRIES 8
#define PLAYBACK_SILENCE_FRAMES 20

static constexpr gpio_num_t PIN_I2S_BCLK = GPIO_NUM_8;
static constexpr gpio_num_t PIN_I2S_WS = GPIO_NUM_7;
static constexpr gpio_num_t PIN_I2S_DOUT = GPIO_NUM_44;
static constexpr gpio_num_t PIN_I2S_DIN = GPIO_NUM_43;
static constexpr gpio_num_t PIN_I2C_SDA = GPIO_NUM_5;
static constexpr gpio_num_t PIN_I2C_SCL = GPIO_NUM_6;
static constexpr uint8_t AIC3104_ADDR = 0x18;
static constexpr uint8_t AIC3104_PAGE_CTRL = 0x00;
static constexpr uint8_t AIC3104_LEFT_DAC_VOLUME = 0x2B;
static constexpr uint8_t AIC3104_RIGHT_DAC_VOLUME = 0x2C;

// AIC3104 DAC digital volume register value (0x00 = 0 dB loudest,
// each increment = -0.5 dB). Overridable at build via -DPIPECAT_DAC_ATTEN=.
// 2026-07-09 crackle hunt: 0x00 (0dB wide open) overdrives the output stage
// on peaks — ESPHome always ran attenuated via the HA volume slider. 0x0C
// (−6 dB) = crackle-free by ear at full program level with ref_gain=1.0.
#ifndef PIPECAT_DAC_ATTEN
#define PIPECAT_DAC_ATTEN 0x0C
#endif

// XVF3800 control port. Pre-flashed via DFU; we just need to confirm it's
// alive on I2C and that it's clocking BCLK/WS as I2S master (otherwise our
// secondary-mode i2s_channel_read / i2s_channel_write will time out forever).
static constexpr uint8_t XVF3800_ADDR = 0x2C;
// Vendor control protocol: write {resid, cmd|READ_BIT, read_len+1}, then read
// read_len+1 bytes (leading status byte + payload) — see xvf_read_bytes().
// Firmware version: DFU controller servicer (resid 240) cmd 88 = GETVERSION,
// 3x uint8 MAJOR MINOR PATCH — the exact transaction the respeaker_xvf3800
// ESPHome component's read_dfu_version() uses, live-proven to return 1.0.7
// (formatBCE/Seeed I2S-master build) on this firmware over I2C.
// (xvf_host.py also documents an app-servicer VERSION at resid 48 cmd 0.)
// NOTE: an earlier mis-framed read here (bare 1-byte 0xB3 write, no cmd/len,
// no status byte) decoded garbage as "v6.34.4"; that number was an artifact,
// not a firmware version.
static constexpr uint8_t XVF_RESID_DFU_CONTROLLER = 240;
static constexpr uint8_t XVF_CMD_DFU_GETVERSION = 88;
static constexpr uint8_t XVF_READ_BIT = 0x80;

static constexpr uint8_t XVF_RESID_PP = 17;
static constexpr uint8_t XVF_RESID_AEC = 33;
static constexpr uint8_t XVF_RESID_AUDIO_MGR = 35;
static constexpr uint8_t XVF_CMD_PP_AGCONOFF = 10;
static constexpr uint8_t XVF_CMD_PP_AGCMAXGAIN = 11;
static constexpr uint8_t XVF_CMD_PP_AGCDESIREDLEVEL = 12;
static constexpr uint8_t XVF_CMD_PP_AGCGAIN = 13;
static constexpr uint8_t XVF_CMD_PP_LIMITONOFF = 19;
static constexpr uint8_t XVF_CMD_PP_MIN_NS = 21;
static constexpr uint8_t XVF_CMD_PP_MIN_NN = 22;
static constexpr uint8_t XVF_CMD_PP_ECHOONOFF = 23;
static constexpr uint8_t XVF_CMD_PP_NLATTENONOFF = 27;
static constexpr uint8_t XVF_CMD_PP_DTSENSITIVE = 31;
static constexpr uint8_t XVF_CMD_PP_ATTNS_MODE = 32;
static constexpr uint8_t XVF_CMD_PP_ATTNS_NOMINAL = 33;
static constexpr uint8_t XVF_CMD_PP_ATTNS_SLOPE = 34;

static constexpr uint8_t XVF_CMD_AEC_HPFONOFF = 1;
static constexpr uint8_t XVF_CMD_AEC_FAR_EXTGAIN = 5;
static constexpr uint8_t XVF_CMD_AEC_ASROUTONOFF = 35;
static constexpr uint8_t XVF_CMD_AEC_FIXEDBEAMSONOFF = 37;
static constexpr uint8_t XVF_CMD_AEC_AZIMUTH_VALUES = 75;
static constexpr uint8_t XVF_CMD_AEC_SPENERGY_VALUES = 80;

// --- LED ring + mute (GPO servicer), ported from ESPHome respeaker_xvf3800 ---
// The 12-LED ring and the mute GPIO are driven by the XVF3800 (XMOS) chip over
// this same I2C control port. The ring takes a 48-byte payload = 12 x [B,G,R,0].
static constexpr uint8_t XVF_RESID_GPO = 20;
static constexpr uint8_t XVF_CMD_GPO_READ_VALUES = 0;
static constexpr uint8_t XVF_CMD_GPO_WRITE_VALUE = 1;
static constexpr uint8_t XVF_CMD_GPO_LED_RING_VALUE = 18;
static constexpr uint8_t XVF_GPO_MUTE_PIN = 30;  // GPIO 30 = mic mute
static constexpr uint8_t XVF_LED_COUNT = 12;

// Master enable + one-shot self-test for the LED ring (both build-overridable).
// LED_SELFTEST lights the whole ring dim-white once at boot to confirm the ring
// is wired to XMOS on this board (the single hardware unknown).
#ifndef PIPECAT_LED_ENABLE
#define PIPECAT_LED_ENABLE 1
#endif
#ifndef PIPECAT_LED_SELFTEST
#define PIPECAT_LED_SELFTEST 0
#endif
#ifndef PIPECAT_LED_BRIGHTNESS
#define PIPECAT_LED_BRIGHTNESS 150  // 0-255 scale applied to each channel
#endif
#ifndef PIPECAT_LED_BOOT_SPLASH
#define PIPECAT_LED_BOOT_SPLASH 1  // flowing-rainbow splash at boot (visible "on")
#endif
// XVF3800 mic input gain. 90 (old default) clipped loud/near speech before the
// limiter -> peak pinned at 32768, degrading wake. 60 gives ~+9.5dB of headroom;
// AGC (maxgain 64) still boosts far-field back toward the wake threshold.
#ifndef PIPECAT_MIC_GAIN
#define PIPECAT_MIC_GAIN 60.0f
#endif

static constexpr uint8_t XVF_CMD_AUDIO_MGR_MIC_GAIN = 0;
static constexpr uint8_t XVF_CMD_AUDIO_MGR_REF_GAIN = 1;
// Packed six-channel mode (XMOS v3.2.1 §3.6.1 / programming guide §4.1.2):
// OP_PACKED enables TDM of three 16 kHz lanes per 48 kHz slot; OP_ALL writes
// all six [category,source] pairs atomically (order: L_PK0..2 then R_PK0..2).
static constexpr uint8_t XVF_CMD_AUDIO_MGR_OP_PACKED = 13;
static constexpr uint8_t XVF_CMD_AUDIO_MGR_OP_UPSAMPLE = 14;
static constexpr uint8_t XVF_CMD_AUDIO_MGR_OP_L = 15;
static constexpr uint8_t XVF_CMD_AUDIO_MGR_OP_R = 19;
static constexpr uint8_t XVF_CMD_AUDIO_MGR_OP_ALL = 23;
static constexpr uint8_t XVF_CMD_AUDIO_MGR_SYS_DELAY = 26;

static constexpr uint8_t XVF_AUDIO_CATEGORY_RAW = 1;
static constexpr uint8_t XVF_AUDIO_CATEGORY_MAX = 12;
static constexpr uint8_t XVF_AUDIO_SOURCE_MAX = 5;
static constexpr uint8_t XVF_AUDIO_CATEGORY_PROCESSED = 6;
static constexpr uint8_t XVF_AUDIO_CATEGORY_ASR = 7;
static constexpr uint8_t XVF_AUDIO_SOURCE_AUTO_SELECT = 3;
static_assert(PIPECAT_DUAL_STREAM_RAW_MIC >= -1 &&
                  PIPECAT_DUAL_STREAM_RAW_MIC <= 3,
              "PIPECAT_DUAL_STREAM_RAW_MIC must be -1 or a mic index 0..3");
static constexpr float PI_F = 3.14159265358979323846f;

enum XvfControlStatus : uint8_t {
  XVF_CTRL_DONE = 0,
  XVF_CTRL_WAIT = 1,
  XVF_SERVICER_COMMAND_RETRY = 0x40,
};

static i2c_master_bus_handle_t i2c_bus = nullptr;
static i2c_master_dev_handle_t aic3104 = nullptr;
static i2c_master_dev_handle_t xvf3800 = nullptr;
static i2s_chan_handle_t tx_handle = nullptr;
static i2s_chan_handle_t rx_handle = nullptr;
static bool xvf3800_present = false;
static bool xvf_beam_telemetry_supported = true;
// XVF application firmware version as read at boot (DFU controller resid 240
// cmd 88). "unknown" until the properly-framed version read succeeds.
static char s_xvf_version[16] = "unknown";

static std::atomic<bool> is_playing = false;
static unsigned int silence_count = 0;

static bool aic3104_write(uint8_t reg, uint8_t value) {
  if (aic3104 == nullptr) {
    return false;
  }

  uint8_t payload[2] = {reg, value};
  esp_err_t ret = i2c_master_transmit(aic3104, payload, sizeof(payload),
                                      pdMS_TO_TICKS(100));
  if (ret != ESP_OK) {
    ESP_LOGW(LOG_TAG, "AIC3104 write 0x%02x failed: %s", reg,
             esp_err_to_name(ret));
    return false;
  }
  return true;
}

static esp_err_t xvf_write_bytes(uint8_t resid, uint8_t cmd,
                                 const uint8_t *value, size_t value_len) {
  if (xvf3800 == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (value_len > 29) {
    return ESP_ERR_INVALID_SIZE;
  }

  uint8_t payload[32] = {resid, cmd, static_cast<uint8_t>(value_len)};
  if (value_len > 0) {
    memcpy(&payload[3], value, value_len);
  }
  esp_err_t ret = i2c_master_transmit(xvf3800, payload, value_len + 3,
                                      pdMS_TO_TICKS(XVF_CONTROL_TIMEOUT_MS));
  if (ret != ESP_OK) {
    ESP_LOGW(LOG_TAG, "XVF3800 write resid=%u cmd=%u failed: %s", resid, cmd,
             esp_err_to_name(ret));
  }
  return ret;
}

static esp_err_t xvf_read_bytes(uint8_t resid, uint8_t cmd, uint8_t *out,
                                size_t out_len) {
  if (xvf3800 == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (out_len > 31) {
    return ESP_ERR_INVALID_SIZE;
  }

  uint8_t req[3] = {resid, static_cast<uint8_t>(cmd | XVF_READ_BIT),
                    static_cast<uint8_t>(out_len + 1)};
  uint8_t resp[32] = {};
  for (int attempt = 0; attempt < XVF_CONTROL_RETRIES; attempt++) {
    esp_err_t ret = i2c_master_transmit_receive(
        xvf3800, req, sizeof(req), resp, out_len + 1,
        pdMS_TO_TICKS(XVF_CONTROL_TIMEOUT_MS));
    if (ret != ESP_OK) {
      ESP_LOGW(LOG_TAG, "XVF3800 read resid=%u cmd=%u failed: %s", resid, cmd,
               esp_err_to_name(ret));
      return ret;
    }

    uint8_t status = resp[0];
    if (status == XVF_CTRL_DONE) {
      memcpy(out, &resp[1], out_len);
      return ESP_OK;
    }
    if (status != XVF_CTRL_WAIT && status != XVF_SERVICER_COMMAND_RETRY) {
      ESP_LOGW(LOG_TAG,
               "XVF3800 read resid=%u cmd=%u returned status 0x%02x", resid,
               cmd, status);
      return ESP_ERR_INVALID_RESPONSE;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return ESP_ERR_TIMEOUT;
}

static void store_le32(uint8_t *out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xff);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

static uint32_t load_le32(const uint8_t *in) {
  return static_cast<uint32_t>(in[0]) |
         (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

static esp_err_t xvf_write_int32(uint8_t resid, uint8_t cmd, int32_t value) {
  uint8_t payload[sizeof(value)];
  store_le32(payload, static_cast<uint32_t>(value));
  return xvf_write_bytes(resid, cmd, payload, sizeof(payload));
}

static esp_err_t xvf_write_float(uint8_t resid, uint8_t cmd, float value) {
  uint8_t payload[sizeof(value)];
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  store_le32(payload, bits);
  return xvf_write_bytes(resid, cmd, payload, sizeof(payload));
}

static esp_err_t xvf_write_u8_pair(uint8_t resid, uint8_t cmd, uint8_t first,
                                   uint8_t second) {
  uint8_t payload[2] = {first, second};
  return xvf_write_bytes(resid, cmd, payload, sizeof(payload));
}

static esp_err_t xvf_read_u8_pair(uint8_t resid, uint8_t cmd, uint8_t *first,
                                  uint8_t *second) {
  uint8_t payload[2] = {};
  esp_err_t ret = xvf_read_bytes(resid, cmd, payload, sizeof(payload));
  if (ret == ESP_OK) {
    *first = payload[0];
    *second = payload[1];
  }
  return ret;
}

esp_err_t pipecat_xvf_audio_mux_status(PipecatXvfAudioMuxStatus *status) {
  if (status == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t ret = xvf_read_u8_pair(
      XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_OP_L, &status->op_l_category,
      &status->op_l_source);
  if (ret == ESP_OK) {
    ret = xvf_read_u8_pair(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_OP_R,
                           &status->op_r_category, &status->op_r_source);
  }
  if (ret == ESP_OK) {
    ret = xvf_read_u8_pair(XVF_RESID_AUDIO_MGR,
                           XVF_CMD_AUDIO_MGR_OP_UPSAMPLE,
                           &status->upsample_l, &status->upsample_r);
  }
  return ret;
}

bool pipecat_xvf_set_audio_mux_right(uint8_t category, uint8_t source,
                                     PipecatXvfAudioMuxStatus *status) {
  if (status == nullptr || category > XVF_AUDIO_CATEGORY_MAX ||
      source > XVF_AUDIO_SOURCE_MAX) {
    return false;
  }
  esp_err_t ret = xvf_write_u8_pair(XVF_RESID_AUDIO_MGR,
                                    XVF_CMD_AUDIO_MGR_OP_R, category, source);
  if (ret != ESP_OK || pipecat_xvf_audio_mux_status(status) != ESP_OK) {
    return false;
  }
  return status->op_r_category == category && status->op_r_source == source;
}

// ===== Packed six-channel mode (t_2ccb0829 experiment C) =====
// AUDIO_MGR_OP_ALL (cmd 23, 12 uint8s = six [category,source] pairs, order
// L_PK0,L_PK1,L_PK2,R_PK0,R_PK1,R_PK2) + AUDIO_MGR_OP_PACKED (cmd 13, <L>,<R>).
// With packing ON each 48 kHz slot TDM-carries three 16 kHz lanes with LSB
// packing markers. WARNING: while packed, the normal uplink decimators in
// pipecat_send_audio produce GARBAGE (they average across TDM boundaries) —
// packed mode is bench-capture only; callers must restore OP_PACKED 0 0 and
// the production mux afterwards. Nothing here runs at boot.

esp_err_t pipecat_xvf_packed_status(PipecatXvfPackedStatus *status) {
  if (status == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t ret = xvf_read_u8_pair(XVF_RESID_AUDIO_MGR,
                                   XVF_CMD_AUDIO_MGR_OP_PACKED,
                                   &status->packed_l, &status->packed_r);
  if (ret != ESP_OK) {
    return ret;
  }
  uint8_t payload[12] = {};
  ret = xvf_read_bytes(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_OP_ALL, payload,
                       sizeof(payload));
  if (ret == ESP_OK) {
    memcpy(status->op_all, payload, sizeof(payload));
  }
  return ret;
}

bool pipecat_xvf_set_packed_mode(bool enable, const uint8_t op_all[12],
                                 PipecatXvfPackedStatus *status) {
  if (status == nullptr) {
    return false;
  }
  if (op_all != nullptr) {
    for (int i = 0; i < 6; i++) {
      if (op_all[i * 2] > XVF_AUDIO_CATEGORY_MAX ||
          op_all[i * 2 + 1] > XVF_AUDIO_SOURCE_MAX) {
        return false;
      }
    }
    if (xvf_write_bytes(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_OP_ALL, op_all,
                        12) != ESP_OK) {
      return false;
    }
  }
  uint8_t flag = enable ? 1 : 0;
  if (xvf_write_u8_pair(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_OP_PACKED, flag,
                        flag) != ESP_OK) {
    return false;
  }
  if (pipecat_xvf_packed_status(status) != ESP_OK) {
    return false;
  }
  bool ok = status->packed_l == flag && status->packed_r == flag;
  if (ok && op_all != nullptr) {
    ok = memcmp(status->op_all, op_all, 12) == 0;
  }
  return ok;
}

// ===== Raw I2S bench capture tap (t_2ccb0829 experiments A/B/C) =====
// Streams UNPROCESSED 48 kHz/32-bit stereo I2S frames (LSB markers intact —
// required for packed-mode unpacking and for full-precision cat-3/11
// cross-correlation). The uplink publisher shares rx_handle, so it is paused
// (uplink sends silence) for the duration; g_raw_capture_pause is checked
// once per 20 ms frame, so wait one frame after setting it before reading.
volatile bool g_raw_capture_pause = false;

esp_err_t pipecat_raw_i2s_capture(uint32_t duration_ms,
                                  bool (*sink)(const uint8_t *chunk,
                                               size_t len, void *ctx),
                                  void *ctx) {
  if (sink == nullptr || duration_ms == 0 || duration_ms > 15000) {
    return ESP_ERR_INVALID_ARG;
  }
  if (rx_handle == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  int32_t *chunk = (int32_t *)heap_caps_malloc(BOARD_FRAME_BYTES,
                                               MALLOC_CAP_DMA);
  if (chunk == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  g_raw_capture_pause = true;
  // Let the publisher's in-flight i2s_channel_read (<=200ms timeout) drain.
  vTaskDelay(pdMS_TO_TICKS(250));
  esp_err_t result = ESP_OK;
  // BOARD_FRAME_BYTES = 20ms of 48k/32-bit stereo.
  uint32_t frames = (duration_ms + 19) / 20;
  for (uint32_t i = 0; i < frames; i++) {
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(rx_handle, chunk, BOARD_FRAME_BYTES,
                                     &bytes_read, pdMS_TO_TICKS(200));
    if (ret != ESP_OK || bytes_read == 0) {
      result = ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret;
      break;
    }
    if (!sink((const uint8_t *)chunk, bytes_read, ctx)) {
      result = ESP_FAIL;  // client went away
      break;
    }
  }
  g_raw_capture_pause = false;
  heap_caps_free(chunk);
  return result;
}

static esp_err_t xvf_read_scalar(uint8_t resid, uint8_t cmd, bool is_float,
                                 float *value) {
  uint8_t payload[sizeof(uint32_t)] = {};
  esp_err_t ret = xvf_read_bytes(resid, cmd, payload, sizeof(payload));
  if (ret != ESP_OK) {
    return ret;
  }

  uint32_t bits = load_le32(payload);
  if (is_float) {
    memcpy(value, &bits, sizeof(bits));
  } else {
    int32_t int_value = 0;
    memcpy(&int_value, &bits, sizeof(bits));
    *value = static_cast<float>(int_value);
  }
  return ESP_OK;
}

enum class TuneTarget : uint8_t {
  XVF_FLOAT,
  XVF_INT32,
  DAC_ATTEN,
};

struct TuneEntry {
  const char *name;
  uint8_t resid;
  uint8_t cmd;
  TuneTarget target;
  bool persistent;
  bool has_range;
  float min_value;
  float max_value;
  float default_value;
  bool dtsensitive_range;
};

// Persistent entries are the reconciler-owned DSP surface. Legacy entries stay
// available for explicitly volatile experiments.
static const TuneEntry kTuneEntries[] = {
    {"far_extgain", XVF_RESID_AEC, XVF_CMD_AEC_FAR_EXTGAIN,
     TuneTarget::XVF_FLOAT, false, false, 0.0f, 0.0f,
     PIPECAT_AEC_FAR_EXTGAIN_DB, false},
    // XMOS XVF3800 v3.2.1 documented range: 0..1000 linear gain.
    {"asr_gain", XVF_RESID_AEC, 36, TuneTarget::XVF_FLOAT, true, true,
     0.0f, 1000.0f, 1.0f, false},
    {"ref_gain", XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_REF_GAIN,
     TuneTarget::XVF_FLOAT, false, false, 0.0f, 0.0f, 1.0f, false},
    {"mic_gain", XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_MIC_GAIN,
     TuneTarget::XVF_FLOAT, false, false, 0.0f, 0.0f, PIPECAT_MIC_GAIN,
     false},
    {"sys_delay", XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_SYS_DELAY,
     TuneTarget::XVF_INT32, false, true, -64.0f, 256.0f, 12.0f, false},
    {"echo_onoff", XVF_RESID_PP, XVF_CMD_PP_ECHOONOFF,
     TuneTarget::XVF_INT32, false, true, 0.0f, 1.0f, 1.0f, false},
    {"nlatten_onoff", XVF_RESID_PP, XVF_CMD_PP_NLATTENONOFF,
     TuneTarget::XVF_INT32, false, true, 0.0f, 1.0f, 1.0f, false},
    // XMOS-documented PP ranges/defaults. One table keeps HTTP, persistence,
    // boot replay, and introspection in lockstep.
    {"dtsensitive", XVF_RESID_PP, XVF_CMD_PP_DTSENSITIVE,
     TuneTarget::XVF_INT32, true, true, 0.0f, 15.0f, 15.0f, true},
    {"agc_maxgain", XVF_RESID_PP, XVF_CMD_PP_AGCMAXGAIN,
     TuneTarget::XVF_FLOAT, true, true, 1.0f, 1000.0f, 64.0f, false},
    {"agc_desired", XVF_RESID_PP, XVF_CMD_PP_AGCDESIREDLEVEL,
     TuneTarget::XVF_FLOAT, true, true, 1.0e-8f, 1.0f,
     PIPECAT_AGC_DESIRED_LEVEL, false},
    {"min_nn", XVF_RESID_PP, XVF_CMD_PP_MIN_NN, TuneTarget::XVF_FLOAT,
     true, true, 0.0f, 1.0f, 0.51f, false},
    {"min_ns", XVF_RESID_PP, XVF_CMD_PP_MIN_NS, TuneTarget::XVF_FLOAT,
     true, true, 0.0f, 1.0f, 0.15f, false},
    // AIC3104 attenuation is ack-only: 0=0 dB, 128=mute.
    {"dac_atten", 0, 0, TuneTarget::DAC_ATTEN, true, true, 0.0f, 128.0f,
     static_cast<float>(PIPECAT_DAC_ATTEN), false},
};

static const TuneEntry *find_tune_entry(const char *param) {
  if (param == nullptr) {
    return nullptr;
  }
  for (const auto &entry : kTuneEntries) {
    if (strcmp(param, entry.name) == 0) {
      return &entry;
    }
  }
  return nullptr;
}

static float clamp_dtsensitive(float value) {
  if (value > 5.0f && value < 10.0f) {
    return value < 7.5f ? 5.0f : 10.0f;
  }
  return value;
}

static bool normalize_tune_value(const TuneEntry &entry, float requested,
                                 float *applied_value, bool *clamped) {
  if (!std::isfinite(requested)) {
    return false;
  }
  float value = requested;
  if (entry.has_range) {
    value = fminf(entry.max_value, fmaxf(entry.min_value, value));
  }
  if (entry.dtsensitive_range) {
    value = clamp_dtsensitive(value);
  }
  if (entry.target != TuneTarget::XVF_FLOAT) {
    value = static_cast<float>(static_cast<int32_t>(value));
  }
  *applied_value = value;
  *clamped = fabsf(value - requested) > 0.0001f;
  return true;
}

bool pipecat_xvf_param_persistent(const char *param) {
  const TuneEntry *entry = find_tune_entry(param);
  return entry != nullptr && entry->persistent;
}

size_t pipecat_xvf_persistent_param_count() {
  size_t count = 0;
  for (const auto &entry : kTuneEntries) {
    if (entry.persistent) count++;
  }
  return count;
}

const char *pipecat_xvf_persistent_param_name(size_t index) {
  for (const auto &entry : kTuneEntries) {
    if (entry.persistent && index-- == 0) {
      return entry.name;
    }
  }
  return nullptr;
}

bool pipecat_xvf_param_default(const char *param, float *value) {
  const TuneEntry *entry = find_tune_entry(param);
  if (entry == nullptr || !entry->persistent || value == nullptr) {
    return false;
  }
  *value = entry->default_value;
  return true;
}

// Apply one named tune and verify typed XVF registers by readback. DAC attenuation
// is the sole ack-only target because the codec path has no independent reader.
esp_err_t pipecat_xvf_tune(const char *param, float value,
                           PipecatXvfTuneResult *result) {
  if (result == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  *result = {};
  result->requested = value;

  const TuneEntry *entry = find_tune_entry(param);
  if (entry == nullptr) {
    return ESP_ERR_NOT_FOUND;
  }
  float applied_value = 0.0f;
  if (!normalize_tune_value(*entry, value, &applied_value, &result->clamped)) {
    return ESP_ERR_INVALID_ARG;
  }
  result->applied_value = applied_value;

  if (entry->target == TuneTarget::DAC_ATTEN) {
    int atten = static_cast<int>(applied_value);
    bool ok = aic3104_write(AIC3104_PAGE_CTRL, 0x00) &&
              aic3104_write(AIC3104_LEFT_DAC_VOLUME,
                            static_cast<uint8_t>(atten)) &&
              aic3104_write(AIC3104_RIGHT_DAC_VOLUME,
                            static_cast<uint8_t>(atten));
    result->ack_only = true;
    result->applied = ok;
    ESP_LOGI(LOG_TAG,
             "dac tune: dac_atten requested=%.4f applied=%d (-%.1f dB) "
             "clamped=%d -> %s",
             (double)value, atten, atten * 0.5, result->clamped,
             ok ? "ESP_OK" : "ESP_FAIL");
    return ok ? ESP_OK : ESP_FAIL;
  }

  bool is_float = entry->target == TuneTarget::XVF_FLOAT;
  esp_err_t ret =
      is_float ? xvf_write_float(entry->resid, entry->cmd, applied_value)
               : xvf_write_int32(entry->resid, entry->cmd,
                                 static_cast<int32_t>(applied_value));
  if (ret == ESP_OK) {
    ret = xvf_read_scalar(entry->resid, entry->cmd, is_float,
                          &result->readback);
  }
  if (ret == ESP_OK) {
    result->readback_valid = true;
    result->applied = fabsf(result->readback - applied_value) <= 0.0001f;
    if (!result->applied) {
      ret = ESP_ERR_INVALID_RESPONSE;
    }
  }
  ESP_LOGI(LOG_TAG,
           "xvf tune: %s requested=%.4f applied_value=%.4f readback=%s%.4f "
           "applied=%d clamped=%d (resid=%u cmd=%u) -> %s",
           entry->name, (double)value, (double)applied_value,
           result->readback_valid ? "" : "n/a:", (double)result->readback,
           result->applied, result->clamped, entry->resid, entry->cmd,
           esp_err_to_name(ret));
  return ret;
}

static bool xvf_read_float4(uint8_t resid, uint8_t cmd, float values[4]) {
  uint8_t payload[sizeof(float) * 4] = {};
  esp_err_t ret = xvf_read_bytes(resid, cmd, payload, sizeof(payload));
  if (ret != ESP_OK) {
    return false;
  }
  for (size_t i = 0; i < 4; i++) {
    uint32_t bits = load_le32(&payload[i * sizeof(float)]);
    memcpy(&values[i], &bits, sizeof(bits));
  }
  return true;
}

static int azimuth_to_led(float radians) {
  float degrees = radians * 180.0f / PI_F;
  int led = static_cast<int>(roundf(degrees / 30.0f));
  if (led < 0) {
    led += 12;
  }
  return led % 12;
}

// Write all 12 LEDs in one GPO transaction. rgb[i] is 0x00RRGGBB; the ring wants
// per-LED [B, G, R, 0x00]. This needs a 51-byte I2C write (3 header + 48 payload),
// which exceeds xvf_write_bytes()'s 29-byte cap, so it has its own buffer.
// Ported from ESPHome respeaker_xvf3800::set_led_ring (GPO resid 20, cmd 18).
static esp_err_t xvf_write_led_ring(const uint32_t rgb[XVF_LED_COUNT]) {
  if (xvf3800 == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  uint8_t payload[3 + XVF_LED_COUNT * 4];
  payload[0] = XVF_RESID_GPO;
  payload[1] = XVF_CMD_GPO_LED_RING_VALUE;
  payload[2] = XVF_LED_COUNT * 4;  // 48 data bytes
  for (int i = 0; i < XVF_LED_COUNT; i++) {
    uint32_t c = rgb[i];
    payload[3 + i * 4 + 0] = static_cast<uint8_t>(c & 0xFF);          // B
    payload[3 + i * 4 + 1] = static_cast<uint8_t>((c >> 8) & 0xFF);   // G
    payload[3 + i * 4 + 2] = static_cast<uint8_t>((c >> 16) & 0xFF);  // R
    payload[3 + i * 4 + 3] = 0x00;                                    // W/unused
  }
  esp_err_t ret = i2c_master_transmit(xvf3800, payload, sizeof(payload),
                                      pdMS_TO_TICKS(XVF_CONTROL_TIMEOUT_MS));
  if (ret != ESP_OK) {
    ESP_LOGW(LOG_TAG, "LED ring write failed: %s", esp_err_to_name(ret));
  }
  return ret;
}

// Fill the whole ring with one packed 0x00RRGGBB color.
static esp_err_t xvf_led_fill(uint32_t rgb) {
  uint32_t ring[XVF_LED_COUNT];
  for (int i = 0; i < XVF_LED_COUNT; i++) {
    ring[i] = rgb;
  }
  return xvf_write_led_ring(ring);
}

// --- LED state machine (ported behavior from ESPHome respeaker_xvf3800) --------
// Drives the ring from the device's own state signals. Wake is SERVER-side in
// Track-B, so "listening" here is inferred from mic activity + the server's
// wake-ack sound arriving; "speaking" from is_playing. Colors follow the HA
// Voice PE convention. All scaled by PIPECAT_LED_BRIGHTNESS.
enum LedState {
  LED_OFF = 0,       // no WebRTC peer -> ring dark
  LED_IDLE,          // connected, quiet -> dim cyan breathing
  LED_LISTENING,     // recent mic energy -> solid cyan
  LED_SPEAKING,      // reply audio playing -> green breathing
  LED_THINKING,      // server processing the command -> flowing rainbow
  LED_WAITING,       // wake fired, waiting for command -> purple beam at talker
};

static LedState g_led_state = LED_OFF;

// --- Server-driven LED phase (Stage 2b) --------------------------------------
// The server (ACE-AI) sends the authoritative voice-assistant phase over the
// RTVI data channel (wake fired / thinking / speaking / idle), exactly like the
// old ESPHome firmware used voice_assistant_phase. This is the source of truth;
// mic-energy inference is only a fallback when no server phase has arrived.
// A phase is honored for LED_PHASE_TTL_MS after it's received, then we fall back
// to device-local inference (so a dropped "idle" message can't stick forever).
enum ServerPhase {
  PHASE_NONE = 0,   // no server signal -> device-local inference
  PHASE_IDLE,       // server says idle (waiting for wake) -> rainbow, no flicker
  PHASE_WAITING,    // wake fired -> purple beam at the wake direction
  PHASE_THINKING,   // processing -> flowing rainbow
  PHASE_SPEAKING,   // replying -> green comet
};
static std::atomic<int> g_server_phase{PHASE_NONE};
static std::atomic<uint32_t> g_server_phase_ms{0};  // millis() of last phase msg
// TTL: server sends idle explicitly, so a long TTL is safe; the fallback only
// matters if the data channel dies mid-turn.
static constexpr uint32_t LED_PHASE_TTL_MS = 30000;

// Called from the RTVI data-channel callbacks (rtvi_callbacks.cpp) to set the
// authoritative phase. Thread-safe (atomics); the LED render loop reads it.
extern "C" void pipecat_led_set_phase(int phase) {
  g_server_phase.store(phase);
  g_server_phase_ms.store((uint32_t)(esp_timer_get_time() / 1000));
  ESP_LOGI(LOG_TAG, "LED phase <- server: %d", phase);
}

// HSV->RGB (h in [0,360), s/v in [0,1]) -> packed 0x00RRGGBB. Ported from the
// ESPHome firmware's hsv_to_rgb used by the flowing-rainbow effect.
static uint32_t led_hsv(float h, float s, float v) {
  float c = v * s;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = v - c;
  float r = 0, g = 0, b = 0;
  if (h < 60)       { r = c; g = x; b = 0; }
  else if (h < 120) { r = x; g = c; b = 0; }
  else if (h < 180) { r = 0; g = c; b = x; }
  else if (h < 240) { r = 0; g = x; b = c; }
  else if (h < 300) { r = x; g = 0; b = c; }
  else              { r = c; g = 0; b = x; }
  uint8_t rr = static_cast<uint8_t>((r + m) * 255.0f);
  uint8_t gg = static_cast<uint8_t>((g + m) * 255.0f);
  uint8_t bb = static_cast<uint8_t>((b + m) * 255.0f);
  return (static_cast<uint32_t>(rr) << 16) |
         (static_cast<uint32_t>(gg) << 8) | bb;
}

// Render the current LED state to the ring, using the ESPHome effect set:
//   IDLE      -> flowing rainbow (each LED hue-offset, whole ring rotating)
//   LISTENING -> led_beam (a smooth bright dot pointing at the talker) + cyan base
//   SPEAKING  -> comet_ccw (a bright head with a fading tail, rotating)
//   OFF       -> dark
// beam_led = -1 for none, else the LED index (0..11) at the active talker.
static void led_render(LedState state, int beam_led) {
#if !PIPECAT_LED_ENABLE
  (void)state; (void)beam_led;
  return;
#else
  if (!xvf3800_present) {
    return;
  }
  const float bmax = PIPECAT_LED_BRIGHTNESS / 255.0f;  // 0..1 master brightness
  static float rainbow_hue = 0.0f;  // rotating rainbow offset
  static float comet_pos = 0.0f;    // rotating comet head

  uint32_t ring[XVF_LED_COUNT] = {0};
  switch (state) {
    case LED_OFF:
      // all zero
      break;
    case LED_THINKING:
    case LED_IDLE: {
      // Flowing rainbow: 12 evenly-spaced hues, whole wheel rotating.
      // THINKING spins faster than IDLE for a more "working" feel.
      rainbow_hue += (state == LED_THINKING) ? 9.0f : 3.0f;  // deg per tick
      if (rainbow_hue >= 360.0f) rainbow_hue -= 360.0f;
      const float step = 360.0f / XVF_LED_COUNT;
      float h = rainbow_hue;
      for (int i = 0; i < XVF_LED_COUNT; i++) {
        ring[i] = led_hsv(h, 1.0f, bmax);  // rainbow at full brightness
        h += step;
        if (h >= 360.0f) h -= 360.0f;
      }
      break;
    }
    case LED_WAITING:
    case LED_LISTENING: {
      // Smooth beam dot (led_beam): bright at the talker LED, linear falloff,
      // over a dim purple base so the ring is clearly "awake". The center is
      // EASED toward the target LED (not snapped) so the purple dot floats
      // around the ring the way the ESPHome led_beam effect did, instead of
      // abruptly jumping to the new direction.
      const int fade = 3;
      static float beam_center = -1.0f;  // persists across ticks for easing
      if (beam_led >= 0) {
        float target = (float)((beam_led + 5) % XVF_LED_COUNT);
        if (beam_center < 0.0f) {
          beam_center = target;  // first acquisition: snap (no prior position)
        } else {
          // Ease toward target along the SHORTEST path around the 12-LED ring.
          float d = target - beam_center;
          if (d > XVF_LED_COUNT / 2.0f) d -= XVF_LED_COUNT;
          if (d < -XVF_LED_COUNT / 2.0f) d += XVF_LED_COUNT;
          beam_center += d * 0.25f;  // 25% per tick -> smooth float, ~5 ticks
          if (beam_center < 0.0f) beam_center += XVF_LED_COUNT;
          if (beam_center >= XVF_LED_COUNT) beam_center -= XVF_LED_COUNT;
        }
      }
      float center = (beam_led >= 0) ? beam_center : 0.0f;
      for (int i = 0; i < XVF_LED_COUNT; i++) {
        uint32_t base = led_hsv(275.0f, 1.0f, bmax * 0.25f);  // dim purple base
        if (beam_led >= 0) {
          float dist = fabsf(i - center);
          if (dist > XVF_LED_COUNT / 2.0f) dist = XVF_LED_COUNT - dist;
          float f = 1.0f - (dist / (fade + 1.0f));
          if (f > 0.0f) {
            uint32_t dot = led_hsv(275.0f, 1.0f, bmax * f);  // purple beam at talker
            base = dot;
          }
        }
        ring[i] = base;
      }
      break;
    }
    case LED_SPEAKING: {
      // comet_ccw: a bright green head with a fading tail sweeping the ring.
      comet_pos += 0.6f;  // LEDs per tick
      if (comet_pos >= XVF_LED_COUNT) comet_pos -= XVF_LED_COUNT;
      int head = (int)comet_pos;
      const int tail = 4;
      ring[head % XVF_LED_COUNT] = led_hsv(140.0f, 1.0f, bmax);  // green head
      for (int i = 1; i <= tail; i++) {
        float tf = 1.0f - (float)i / (tail + 1);
        int idx = (head + i) % XVF_LED_COUNT;  // ccw tail
        ring[idx] = led_hsv(140.0f, 1.0f, bmax * tf);
      }
      break;
    }
  }
  xvf_write_led_ring(ring);
#endif
}

static void configure_xvf3800_dsp_profile() {
  if (!xvf3800_present) {
    return;
  }

  uint32_t ok = 0;
  uint32_t total = 0;
  auto record = [&ok, &total](esp_err_t ret) {
    total++;
    if (ret == ESP_OK) {
      ok++;
    }
  };

  // ASR audio routing (ported from ESPHome respeaker_xvf3800 "Phase 2c PERMANENT
  // FIX", proven 2026-06-09 with a 6/6-vs-0/4 word-recall A/B). The ESP32 mono
  // path reads I2S channel 0 (LEFT slot). The XVF3800 has two relevant output
  // categories:
  //   category 6 (PROCESSED / voice-comm) -> runs the conferencing post-processor
  //     (aggressive NS + de-reverb) which SPECTRALLY GUTS speech for ASR. This is
  //     what we shipped before, and it garbled STT ("what time is it" -> "a 20").
  //   category 7 (ASR)  -> the clean post-beamformer autoselect output, no
  //     suppression, which is what Parakeet/Riva is trained on.
  // Put category-7 (ASR) auto-select on the LEFT slot so STT gets the clean beam,
  // and engage AEC ASR-mode. XVF params are volatile (reset on XMOS power-cycle /
  // DFU), so this re-applies on every boot, exactly like the ESPHome component.
#if PIPECAT_DUAL_STREAM
  // Both independent 16 kHz lanes must be upsampled onto the 48 kHz I2S slots.
  record(xvf_write_u8_pair(XVF_RESID_AUDIO_MGR,
                           XVF_CMD_AUDIO_MGR_OP_UPSAMPLE, 1, 1));
#endif
  record(xvf_write_u8_pair(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_OP_L,
                           XVF_AUDIO_CATEGORY_ASR,
                           XVF_AUDIO_SOURCE_AUTO_SELECT));
#if PIPECAT_DUAL_STREAM
#if PIPECAT_DUAL_STREAM_RAW_MIC >= 0
  record(xvf_write_u8_pair(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_OP_R,
                           XVF_AUDIO_CATEGORY_RAW,
                           PIPECAT_DUAL_STREAM_RAW_MIC));
#else
  record(xvf_write_u8_pair(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_OP_R,
                           XVF_AUDIO_CATEGORY_PROCESSED,
                           XVF_AUDIO_SOURCE_AUTO_SELECT));
#endif
#else
  record(xvf_write_u8_pair(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_OP_R,
                           XVF_AUDIO_CATEGORY_ASR,
                           XVF_AUDIO_SOURCE_AUTO_SELECT));
#endif

  // Speaker/far-end reference gain. Seeed's sample default is 8.0 (linear,
  // ≈ +18 dB) which OVERDRIVES the analog output stage — the 2026-07-09
  // crackle hunt proved it by ear + stats (delivery clean, crackle scaled
  // with level, gone at ref_gain=1.0 + DAC −6dB). Keep at 1.0.
  record(xvf_write_float(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_REF_GAIN,
                         1.0f));
  record(xvf_write_float(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_MIC_GAIN,
                         PIPECAT_MIC_GAIN));
  record(xvf_write_int32(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_SYS_DELAY,
                         12));

  // Keep adaptive beamforming/AEC active and align the far-end reference gain
  // with the host playback path. Per-build override is useful when speaker
  // attenuation changes between bench and kitchen enclosures.
  // ASROUTONOFF=1: engage ASR-mode output (bypass the conferencing post-proc),
  // the mate to the category-7 LEFT-slot routing above. Was 0 (OFF) before, which
  // fed suppressed audio to STT and caused the garbling.
  record(xvf_write_int32(XVF_RESID_AEC, XVF_CMD_AEC_ASROUTONOFF, 1));
  record(xvf_write_int32(XVF_RESID_AEC, XVF_CMD_AEC_FIXEDBEAMSONOFF, 0));
  record(xvf_write_int32(XVF_RESID_AEC, XVF_CMD_AEC_HPFONOFF, 2));
  record(xvf_write_float(XVF_RESID_AEC, XVF_CMD_AEC_FAR_EXTGAIN,
                         PIPECAT_AEC_FAR_EXTGAIN_DB));

  // Enable the production post-processor: AGC, limiter, echo suppression,
  // non-linear echo attenuation, and conservative noise floors from XMOS docs.
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_AGCGAIN, 2.0f));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_AGCMAXGAIN, 64.0f));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_AGCDESIREDLEVEL,
                         PIPECAT_AGC_DESIRED_LEVEL));
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_AGCONOFF, 1));
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_LIMITONOFF, 1));
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_ECHOONOFF, 1));
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_NLATTENONOFF, 1));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_MIN_NS, 0.15f));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_MIN_NN, 0.51f));
  // The official XVF3800 range is 0..5 or 10..15. Keep the baked fallback at
  // the highest valid double-talk preference; NVS/reconciler state overlays it.
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_DTSENSITIVE, 15));
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_ATTNS_MODE, 1));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_ATTNS_NOMINAL, 1.0f));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_ATTNS_SLOPE, 1.0f));

#if PIPECAT_DUAL_STREAM && PIPECAT_DUAL_STREAM_RAW_MIC >= 0
  ESP_LOGI(LOG_TAG,
           "XVF3800 DSP profile: %lu/%lu control writes acked "
           "(right=raw category 1 mic %d, AEC, AGC on, limiter; "
           "far_extgain=%.1fdB agc_desired_level=%.5f)",
           (unsigned long)ok, (unsigned long)total,
           PIPECAT_DUAL_STREAM_RAW_MIC,
           (double)PIPECAT_AEC_FAR_EXTGAIN_DB,
           (double)PIPECAT_AGC_DESIRED_LEVEL);
#else
  ESP_LOGI(LOG_TAG,
           "XVF3800 DSP profile: %lu/%lu control writes acked "
           "(processed auto-beam, AEC, AGC on, limiter, no Wi-Fi PS; "
           "far_extgain=%.1fdB agc_desired_level=%.5f)",
           (unsigned long)ok, (unsigned long)total,
           (double)PIPECAT_AEC_FAR_EXTGAIN_DB,
           (double)PIPECAT_AGC_DESIRED_LEVEL);
#endif

#if PIPECAT_LED_ENABLE && PIPECAT_LED_BOOT_SPLASH
  // Boot rainbow splash: run the flowing-rainbow effect for a few seconds at
  // startup so "turning on" is clearly visible (matches ESPHome boot behavior).
  if (xvf3800_present) {
    const float bmax = PIPECAT_LED_BRIGHTNESS / 255.0f;
    const float step = 360.0f / XVF_LED_COUNT;
    float hue = 0.0f;
    for (int frame = 0; frame < 90; frame++) {  // ~90 * 40ms = 3.6s
      uint32_t ring[XVF_LED_COUNT] = {0};
      float h = hue;
      for (int i = 0; i < XVF_LED_COUNT; i++) {
        ring[i] = led_hsv(h, 1.0f, bmax);
        h += step;
        if (h >= 360.0f) h -= 360.0f;
      }
      xvf_write_led_ring(ring);
      hue += 6.0f;
      if (hue >= 360.0f) hue -= 360.0f;
      vTaskDelay(pdMS_TO_TICKS(40));
    }
    xvf_led_fill(0x000000);
    ESP_LOGI(LOG_TAG, "LED boot splash: rainbow done");
  }
#endif

#if PIPECAT_LED_SELFTEST
  // One-shot (or looping) LED-ring confirmation: cycle R -> G -> B -> dim-white so
  // we can (a) confirm the ring is XMOS-wired on this board and (b) verify the
  // byte->channel mapping visually. Gated OFF by default; -DPIPECAT_LED_SELFTEST=1
  // runs one cycle, =2 loops forever (easy for a human to eyeball), then boots on.
  {
    const uint8_t b = PIPECAT_LED_BRIGHTNESS;
    const uint32_t seq[] = {
        (uint32_t)b << 16,               // red
        (uint32_t)b << 8,                // green
        (uint32_t)b,                     // blue
        ((uint32_t)b << 16) | ((uint32_t)b << 8) | b,  // white
    };
    const char *names[] = {"RED", "GREEN", "BLUE", "WHITE"};
    int loops = (PIPECAT_LED_SELFTEST >= 2) ? 6 : 1;
    for (int rep = 0; rep < loops; rep++) {
      for (int i = 0; i < 4; i++) {
        esp_err_t r = xvf_led_fill(seq[i]);
        ESP_LOGI(LOG_TAG, "LED selftest: fill %s (0x%06lX) -> %s", names[i],
                 (unsigned long)seq[i], esp_err_to_name(r));
        vTaskDelay(pdMS_TO_TICKS(600));
      }
    }
    xvf_led_fill(0x000000);
    ESP_LOGI(LOG_TAG, "LED selftest: done (ring cleared)");
  }
#endif
}

static void init_i2c_and_codec() {
  i2c_master_bus_config_t bus_cfg = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = PIN_I2C_SDA,
      .scl_io_num = PIN_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags = {
          .enable_internal_pullup = 1,
      },
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

  i2c_device_config_t codec_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = AIC3104_ADDR,
      .scl_speed_hz = 100 * 1000,
      .scl_wait_us = 0,
      .flags = {
          .disable_ack_check = 0,
      },
  };
  ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &codec_cfg, &aic3104));

  // ESPHome's AIC3104 component uses these DAC volume registers for unmute.
  // The XMOS firmware owns the deeper codec clocking/routing setup.
  // DAC digital volume: 0x00 = 0 dB (loudest), each step = -0.5 dB attenuation.
  // Was 0x10 (-8 dB); 0x00 = 0 dB, ~8 dB louder. Env-tunable so speaker loudness
  // can be adjusted without a code change (PIPECAT_DAC_ATTEN = register value).
  aic3104_write(AIC3104_PAGE_CTRL, 0x00);
  aic3104_write(AIC3104_LEFT_DAC_VOLUME, PIPECAT_DAC_ATTEN);
  aic3104_write(AIC3104_RIGHT_DAC_VOLUME, PIPECAT_DAC_ATTEN);

  // XVF3800 control port. If this probe fails, the XMOS DFU firmware is
  // missing -- no I2S clocks will ever appear and our slave-mode reads/writes
  // will hang at the watchdog timeout. Refusing to advance saves debugging time.
  i2c_device_config_t xvf_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = XVF3800_ADDR,
      .scl_speed_hz = 100 * 1000,
      .scl_wait_us = 0,
      .flags = {
          .disable_ack_check = 0,
      },
  };
  ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &xvf_cfg, &xvf3800));

  esp_err_t probe = i2c_master_probe(i2c_bus, XVF3800_ADDR, pdMS_TO_TICKS(100));
  if (probe != ESP_OK) {
    ESP_LOGE(LOG_TAG,
             "XVF3800 not responding at 0x%02x: %s. "
             "DFU firmware likely not flashed -- I2S clocks will be absent.",
             XVF3800_ADDR, esp_err_to_name(probe));
    xvf3800_present = false;
    return;
  }
  xvf3800_present = true;

  // Read the XVF firmware version: DFU controller servicer resid 240 cmd 88
  // (GETVERSION), 3x uint8 MAJOR MINOR PATCH — the same properly-framed
  // transaction (write {resid, cmd|0x80, len+1}, read status byte + payload)
  // that the respeaker_xvf3800 ESPHome component uses, live-proven to report
  // 1.0.7 (formatBCE/Seeed I2S-master build) on the theater/kitchen boards.
  uint8_t ver[3] = {0, 0, 0};
  esp_err_t ver_ret = xvf_read_bytes(XVF_RESID_DFU_CONTROLLER,
                                     XVF_CMD_DFU_GETVERSION, ver, sizeof(ver));
  if (ver_ret == ESP_OK) {
    snprintf(s_xvf_version, sizeof(s_xvf_version), "%u.%u.%u", ver[0], ver[1],
             ver[2]);
    ESP_LOGI(LOG_TAG, "XVF3800 alive at 0x%02x, app firmware v%s",
             XVF3800_ADDR, s_xvf_version);
  } else {
    ESP_LOGW(LOG_TAG,
             "XVF3800 ack'd at 0x%02x but version read failed: %s. "
             "Continuing -- maybe vendor protocol mismatch.",
             XVF3800_ADDR, esp_err_to_name(ver_ret));
  }

  configure_xvf3800_dsp_profile();
}

static void init_i2s() {
  // The XVF3800 firmware (formatBCE/Seeed I2S-master v1.0.7) on the
  // theater/kitchen boards is the XMOS build that
  // acts as the I2S **MASTER** (it drives BCLK/WS off its own audio pipeline) —
  // exactly what the working respeaker_xvf3800 ESPHome component assumes
  // (`i2s_mode: secondary`, i.e. the ESP32 is the I2S secondary/slave). If the
  // ESP32 also drives the clock (ROLE_MASTER) both ends fight the bus and the
  // captured words come back bit-smeared/rail-pinned (28 shared bits between
  // consecutive samples, constant peak=0x8000) — NOT real mic audio. So the
  // ESP32 must be the I2S SLAVE and clock its reads off the XVF's BCLK/WS.
  i2s_chan_config_t chan_cfg = {
      .id = I2S_NUM_0,
      .role = I2S_ROLE_SLAVE,
      // Jitter headroom (2026-07-08): 12 descriptors x 511 frames ≈ 128ms of
      // DMA at 48k (was 8x240 ≈ 40ms). WebRTC delivers 20ms opus packets with
      // real network jitter; a 40ms ring ran dry on every late packet -> gaps
      // -> the "raspy streaming" Ace kept hearing. ESPHome ran buffer_duration
      // 100ms on this exact path. Paired with the silence pre-roll in
      // pipecat_audio_decode() which fills this ring before speech starts.
      .dma_desc_num = 12,
      .dma_frame_num = 511,
      .auto_clear_after_cb = true,
      .auto_clear_before_cb = false,
      .intr_priority = 0,
  };
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

  i2s_std_config_t std_cfg = {
      .clk_cfg =
          {
              .sample_rate_hz = BOARD_I2S_SAMPLE_RATE,
              .clk_src = I2S_CLK_SRC_DEFAULT,
              .ext_clk_freq_hz = 0,
              .mclk_multiple = I2S_MCLK_MULTIPLE_256,
          },
      .slot_cfg =
          {
              .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
              .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
              .slot_mode = I2S_SLOT_MODE_STEREO,
              .slot_mask = I2S_STD_SLOT_BOTH,
              .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
              .ws_pol = false,
              .bit_shift = true,
              .left_align = true,
              .big_endian = false,
              .bit_order_lsb = false,
          },
      .gpio_cfg =
          {
              .mclk = GPIO_NUM_NC,
              .bclk = PIN_I2S_BCLK,
              .ws = PIN_I2S_WS,
              .dout = PIN_I2S_DOUT,
              .din = PIN_I2S_DIN,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

void pipecat_init_audio_capture() {
  init_i2c_and_codec();
  init_i2s();
}

bool pipecat_xvf3800_present() { return xvf3800_present; }

const char *pipecat_xvf3800_version() { return s_xvf_version; }

static void update_is_playing(int16_t *in_buf, size_t in_samples) {
  bool any_set = false;
  for (size_t i = 0; i < in_samples; i++) {
    if (in_buf[i] != -1 && in_buf[i] != 0 && in_buf[i] != 1) {
      any_set = true;
      break;
    }
  }

  if (any_set) {
    silence_count = 0;
  } else {
    silence_count++;
  }

  if (silence_count >= PLAYBACK_SILENCE_FRAMES && is_playing) {
    is_playing = false;
  } else if (any_set && !is_playing) {
    is_playing = true;
  }
}

// --- Polyphase windowed-sinc 16k->48k interpolator (2026-07-08) -------------
// 24-tap Hamming-windowed sinc, fc=7kHz, polyphase 3x8. HEADROOM-SCALED
// (2026-07-09): the unity-gain taps had worst-case |sum| 49022 -> intersample
// peaks up to 36052 on 32767 input = hard CLIPPING on loud audio (the wake
// beep rasped worse after the FIR landed). Taps scaled by 0.668 (-3.5dB):
// worst-case now 32763 = mathematically clip-free. The -3.5dB is made up at
// the DAC (we run 0dB atten with headroom to spare).
static const int16_t kInterpFir[24] = {
    -130, -38, 213, 555, 509, -462, -2057, -2682, -315, 5640, 13153, 18419,
    18419, 13153, 5640, -315, -2682, -2057, -462, 509, 555, 213, -38, -130};

static void mono_16k_to_stereo_48k_32bit(int16_t *src, size_t src_samples,
                                         int32_t *dst) {
  // 8-sample input history carried across chunks so streaming boundaries are
  // seamless. hist[0] = oldest, hist[7] = newest.
  static int16_t hist[8] = {0};
  size_t out = 0;
  for (size_t i = 0; i < src_samples; i++) {
    // shift in the new sample
    for (int k = 0; k < 7; k++) hist[k] = hist[k + 1];
    hist[7] = src[i];
    for (int p = 0; p < UPSAMPLE_RATIO; p++) {
      int32_t acc = 0;
      // t indexes backwards in time: x[i-t] = hist[7-t]
      for (int t = 0; t < 8; t++) {
        acc += (int32_t)kInterpFir[3 * t + p] * (int32_t)hist[7 - t];
      }
      int32_t sample16 = acc >> 15;
      if (sample16 > 32767) sample16 = 32767;
      if (sample16 < -32768) sample16 = -32768;
      int32_t sample = sample16 << 16;
      dst[out++] = sample;
      dst[out++] = sample;
    }
  }
}

#if PIPECAT_DUAL_STREAM
static void stereo_48k_32bit_to_stereo_16k(int32_t *src,
                                           size_t src_samples, int16_t *dst) {
  size_t out = 0;
  for (size_t i = 0; i + 5 < src_samples &&
                     out < PCM_SAMPLES_PER_FRAME * 2;
       i += UPSAMPLE_RATIO * 2) {
    int32_t left = (src[i + 0] >> 16) + (src[i + 2] >> 16) +
                   (src[i + 4] >> 16);
    int32_t right = (src[i + 1] >> 16) + (src[i + 3] >> 16) +
                    (src[i + 5] >> 16);
    left /= UPSAMPLE_RATIO;
    right /= UPSAMPLE_RATIO;
    if (left > 32767) left = 32767;
    if (left < -32768) left = -32768;
    if (right > 32767) right = 32767;
    if (right < -32768) right = -32768;
    dst[out++] = static_cast<int16_t>(left);
    dst[out++] = static_cast<int16_t>(right);
  }
  while (out < PCM_SAMPLES_PER_FRAME * 2) {
    dst[out++] = 0;
  }
}
#endif

static void stereo_48k_32bit_to_mono_16k(int32_t *src, size_t src_frames,
                                         int16_t *dst) {
  // ANTI-ALIASED decimator (2026-07-08). The previous max-of-6 peak-picker
  // was a nonlinear envelope follower with ZERO anti-aliasing — it folded HF
  // energy into the voice band and smeared formants (far-field STT garble:
  // 'Egg clanker', 'out from one to 30'). Proper decimation: average the two
  // stereo slots per frame (the XVF ASR beam is on both), then a 6-tap boxcar
  // across the 3 input frames per output sample — a crude but LINEAR low-pass
  // at ~8kHz. 24-bit headroom: XVF samples are 32-bit with audio in the top
  // 16, so >>16 per sample then sum 6 and >>im into range.
  size_t out = 0;
  for (size_t i = 0; i + (UPSAMPLE_RATIO * 2 - 1) < src_frames &&
                     out < PCM_SAMPLES_PER_FRAME;
       i += UPSAMPLE_RATIO * 2) {
    int32_t acc = 0;
    for (size_t j = 0; j < UPSAMPLE_RATIO * 2; j++) {
      acc += src[i + j] >> 16;
    }
    int32_t avg = acc / (int32_t)(UPSAMPLE_RATIO * 2);
    if (avg > 32767) avg = 32767;
    if (avg < -32768) avg = -32768;
    dst[out++] = (int16_t)avg;
  }
  while (out < PCM_SAMPLES_PER_FRAME) {
    dst[out++] = 0;
  }
}

static void fill_bench_tone(int16_t *dst, size_t samples) {
  static uint32_t phase = 0;
  for (size_t i = 0; i < samples; i++) {
    dst[i] = (phase < 18) ? 6000 : -6000;
    phase = (phase + 1) % 36;
  }
}

#if PIPECAT_DUAL_STREAM
static void fill_bench_stereo_tone(int16_t *dst, size_t frames) {
  static uint32_t phase = 0;
  for (size_t i = 0; i < frames; i++) {
    int16_t sample = (phase < 18) ? 6000 : -6000;
    dst[i * 2] = sample;
    dst[i * 2 + 1] = sample;
    phase = (phase + 1) % 36;
  }
}
#endif

static int16_t *decoder_buffer = nullptr;
static int32_t *i2s_play_buffer = nullptr;
static OpusDecoder *opus_decoder = nullptr;

// ===== Ring-buffered playback (2026-07-09) =====
// ROOT CAUSE of the "raspy streaming" saga: pipecat_audio_decode() ran on the
// webrtc loop thread and wrote straight into i2s_channel_write — every I2S
// write's timing was slaved to network packet arrival + whatever else that
// loop was doing (keepalive, RTVI, ICE). Any hiccup = DMA starve = micro-gap
// = rasp on real speech. A perfectly-paced /test-tone measured clean (THD
// -46dBc) while streamed TTS rasped: static-clean, dynamic-dirty = timing.
// ESPHome (A/B "super clean", same hardware 2026-07-09) decouples via a
// ~100ms buffered speaker component with its own task. This is that port:
//   producer (webrtc thread): opus decode -> push 16k mono PCM into ring
//   consumer (playback_task, core 1 prio 6): PREBUFFER 100ms -> pop 20ms
//     frames -> sinc upsample -> blocking i2s write (DMA paces the task)
// Prebuffer keys off RING OCCUPANCY, never is_playing edges — the mid-speech
// pre-roll trap (see NOTE below) structurally can't recur: an intra-utterance
// pause just drains the ring and re-arms PREBUFFER.
#define PLAY_RING_SAMPLES 32768  // power of two; ~2.05s @16k mono; 64KB PSRAM
#define PLAY_RING_MASK (PLAY_RING_SAMPLES - 1)
static int16_t *play_ring = nullptr;
static volatile uint32_t play_ring_head = 0;  // free-running; producer-owned
static volatile uint32_t play_ring_tail = 0;  // free-running; consumer-owned
static volatile uint32_t play_ring_drops = 0;

static inline uint32_t play_ring_count() {
  return play_ring_head - play_ring_tail;  // free-running counters
}

// Cumulative playback stats, queryable via GET /playback/stats (ota.cpp).
// Theater has no USB serial, so crackle diagnosis needs these over HTTP:
// crackle + underruns>0 = delivery timing (raise prebuffer); crackle with
// clean counters = corruption below the ring (XVF/codec register layer).
volatile uint32_t g_play_stat_frames = 0;
volatile uint32_t g_play_stat_write_fail = 0;
volatile uint32_t g_play_stat_underruns = 0;
volatile uint32_t g_play_stat_plc = 0;  // opus PLC frames (lost RTP packets concealed)

// Underrun BLIND-SPOT fix (2026-07-11, ladder instrumentation): a FULL ring
// drain re-arms the prebuffer as "normal end of utterance" and was NOT
// counted — only partial-frame strands incremented underruns. Ace HEARD
// mid-speech syllable gaps while underruns=0. When the ring fully drains and
// NEW audio arrives within the resume window (default 750ms, build env
// PIPECAT_GAP_RESUME_MS), that IS a mid-speech gap -> gap_resumes. A genuine
// end of utterance won't refill within the window (TTS turn gaps are
// seconds). Pure counting — zero behavior change.
volatile uint32_t g_play_stat_gap_resumes = 0;

// Phase 6 adaptive prebuffer (NetEQ-lite) observability: current effective
// prebuffer + cumulative step transitions. With PIPECAT_ADAPTIVE_PREBUFFER
// off (default), effective == prebuffer_ms and steps stays 0.
volatile uint32_t g_play_prebuffer_effective_ms = 0;
volatile uint32_t g_play_prebuffer_steps = 0;

// Gap accounting + adaptive prebuffer controller (host-testable pure C —
// tests/host/test_prebuffer_ctl.c).
#include "prebuffer_ctl.h"
#ifndef PIPECAT_ADAPTIVE_PREBUFFER
#define PIPECAT_ADAPTIVE_PREBUFFER 0  // Phase 6 dark by default
#endif
#ifndef PIPECAT_GAP_RESUME_MS
#define PIPECAT_GAP_RESUME_MS 750  // full-drain -> refill window = mid-speech gap
#endif
// Recovery-event inputs to the adaptive controller. g_play_stat_fec is
// defined further down this file; g_red_recovered lives in the vendored
// components/peer/rtp.c.
extern volatile uint32_t g_play_stat_fec;
extern "C" {
extern volatile uint32_t g_red_recovered;
}

// Prebuffer depth: runtime-adjustable via /playback/stats?prebuffer_ms=N so
// the experiment needs no rebuild. Default 80ms (2026-07-11 live finding:
// 40ms cannot cover RED N-2 recovery latency — two 20ms redundant frames
// arrive up to 40ms late, leaving zero margin; 80ms verified gap-free by
// Ace's ear). History: 100ms was ESPHome's default; 40ms was the 2026-07-10
// wake-ack-latency experiment.
volatile uint32_t g_play_prebuffer_samples = 1280;

// Flash-embedded selftest clip (16k mono s16le, -6dB headroom). Playing it
// via pipecat_play_selftest_clip() exercises ring->FIR->I2S->DAC->speaker
// with NO opus and NO network — the definitive opus-vs-analog splitter for
// crackle triage: embedded clean + streamed crackly => opus/transport;
// embedded crackly too => DAC/analog/speaker.
extern const uint8_t selftest_clip_start[] asm("_binary_selftest_clip_pcm_start");
extern const uint8_t selftest_clip_end[] asm("_binary_selftest_clip_pcm_end");

void pipecat_play_selftest_clip() {
  const int16_t *pcm = (const int16_t *)selftest_clip_start;
  size_t total = (selftest_clip_end - selftest_clip_start) / sizeof(int16_t);
  size_t pushed = 0;
  while (pushed < total) {
    uint32_t free_space = PLAY_RING_SAMPLES - play_ring_count();
    if (free_space == 0) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    size_t n = total - pushed;
    if (n > free_space) n = free_space;
    for (size_t i = 0; i < n; i++) {
      play_ring[(play_ring_head + i) & PLAY_RING_MASK] = pcm[pushed + i];
    }
    __sync_synchronize();
    play_ring_head += n;
    pushed += n;
  }
  ESP_LOGI(LOG_TAG, "selftest clip queued: %u samples (no opus, no network)",
           (unsigned)total);
}

static void pipecat_playback_task(void *arg) {
  (void)arg;
  static int16_t pop_buf[PCM_SAMPLES_PER_FRAME];
  bool prebuffering = true;
  uint32_t frames = 0, ok = 0, underruns = 0;

  // Gap accounting + Phase 6 adaptive prebuffer (NetEQ-lite). Both env-gated
  // at BUILD time; adaptive defaults OFF (dark), gap_resumes counting is
  // always on (pure instrumentation, no behavior change).
  prebuffer_ctl pbc;
  pbc_init(&pbc, PIPECAT_ADAPTIVE_PREBUFFER, PIPECAT_GAP_RESUME_MS);
  g_play_prebuffer_effective_ms =
      pbc_effective_ms(&pbc, g_play_prebuffer_samples / 16);

  for (;;) {
    uint32_t avail = play_ring_count();
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // Feed cumulative recovery events (PLC + FEC + RED) to the adaptive
    // controller; a no-op unless PIPECAT_ADAPTIVE_PREBUFFER=1 was baked in.
    pbc_track_recoveries(
        &pbc, g_play_stat_plc + g_play_stat_fec + g_red_recovered, now_ms);
    uint32_t effective_ms = pbc_effective_ms(&pbc, g_play_prebuffer_samples / 16);
    g_play_prebuffer_effective_ms = effective_ms;
    g_play_prebuffer_steps = pbc.transitions;

    if (prebuffering) {
      if (avail < effective_ms * 16) {  // 16 samples/ms @16k
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      // Ring refilled: if a FULL drain happened within the resume window,
      // that drain was a mid-speech gap, not end of utterance.
      if (pbc_on_refill(&pbc, now_ms)) {
        g_play_stat_gap_resumes = pbc.gap_resumes;
        ESP_LOGW(LOG_TAG, "gap resume: ring refilled %lums after full drain "
                 "(mid-speech gap, total %lu)",
                 (unsigned long)(now_ms - pbc.drain_at_ms),
                 (unsigned long)pbc.gap_resumes);
      }
      prebuffering = false;
    }
    if (avail < PCM_SAMPLES_PER_FRAME) {
      // Ring drained: end of utterance (normal) or a network gap. A partial
      // frame stranded = definite mid-speech gap (counted immediately). A
      // FULL drain is ambiguous — arm the resume window; if new audio
      // arrives within it, pbc_on_refill counts it as gap_resumes.
      if (avail > 0) {
        underruns++;  // partial frame stranded = mid-speech gap
        g_play_stat_underruns++;
      } else {
        pbc_on_full_drain(&pbc, now_ms);
      }
      prebuffering = true;
      continue;
    }
    for (size_t i = 0; i < PCM_SAMPLES_PER_FRAME; i++) {
      pop_buf[i] = play_ring[(play_ring_tail + i) & PLAY_RING_MASK];
    }
    __sync_synchronize();
    play_ring_tail += PCM_SAMPLES_PER_FRAME;

    mono_16k_to_stereo_48k_32bit(pop_buf, PCM_SAMPLES_PER_FRAME,
                                 i2s_play_buffer);
    size_t bytes_written = 0;
    size_t bytes_to_write =
        PCM_SAMPLES_PER_FRAME * UPSAMPLE_RATIO * 2 * sizeof(int32_t);
    esp_err_t ret = i2s_channel_write(tx_handle, i2s_play_buffer,
                                      bytes_to_write, &bytes_written,
                                      pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));
    frames++;
    g_play_stat_frames++;
    if (ret == ESP_OK) {
      ok++;
    } else {
      g_play_stat_write_fail++;
      ESP_LOGW(LOG_TAG, "i2s write failed: %s (%lu/%lu bytes)",
               esp_err_to_name(ret), (unsigned long)bytes_written,
               (unsigned long)bytes_to_write);
    }
    if (frames >= 50) {
      ESP_LOGI(LOG_TAG,
               "i2s playback: %lu/%lu frames ok, %lu underruns, %lu drops%s",
               (unsigned long)ok, (unsigned long)frames,
               (unsigned long)underruns, (unsigned long)play_ring_drops,
               xvf3800_present ? "" : " [XVF3800 ABSENT]");
      frames = 0;
      ok = 0;
      underruns = 0;
    }
  }
}

void pipecat_init_audio_decoder() {
  int decoder_error = 0;
  opus_decoder = opus_decoder_create(WEBRTC_SAMPLE_RATE, 1, &decoder_error);
  if (decoder_error != OPUS_OK) {
    ESP_LOGE(LOG_TAG, "Failed to create OPUS decoder: %d", decoder_error);
    return;
  }

  decoder_buffer = (int16_t *)heap_caps_malloc(PCM_BUFFER_SIZE, MALLOC_CAP_8BIT);
  i2s_play_buffer =
      (int32_t *)heap_caps_malloc(BOARD_FRAME_BYTES, MALLOC_CAP_DMA);
  play_ring = (int16_t *)heap_caps_malloc(
      PLAY_RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (play_ring == nullptr) {  // PSRAM unavailable? fall back to internal
    play_ring = (int16_t *)heap_caps_malloc(PLAY_RING_SAMPLES * sizeof(int16_t),
                                            MALLOC_CAP_8BIT);
  }
  if (decoder_buffer == nullptr || i2s_play_buffer == nullptr ||
      play_ring == nullptr) {
    ESP_LOGE(LOG_TAG, "Failed to allocate playback buffers");
    return;
  }
  // Consumer task: core 1 (away from audio_publisher on core 0 prio 7),
  // prio 6 (above led_ring prio 2). Blocking i2s_channel_write paces it.
  xTaskCreatePinnedToCore(pipecat_playback_task, "playback", 4096, NULL, 6,
                          NULL, 1);
}

// Shared producer push: gate on is_playing and push PCM into the play ring.
// Factored out (2026-07-09b) so the FEC/PLC gap-fill path uses the identical
// gating + ring semantics as the normal decode path.
static void push_decoded_to_ring(int16_t *pcm, int nsamples) {
  update_is_playing(pcm, nsamples);
  if (!is_playing) {
    return;
  }
  uint32_t free_space = PLAY_RING_SAMPLES - play_ring_count();
  if (free_space < (uint32_t)nsamples) {
    play_ring_tail += (uint32_t)nsamples - free_space;
    play_ring_drops++;
  }
  for (int i = 0; i < nsamples; i++) {
    play_ring[(play_ring_head + i) & PLAY_RING_MASK] = pcm[i];
  }
  __sync_synchronize();
  play_ring_head += nsamples;
}

// Reset the opus decoder to a clean state at utterance start (escalation §2,
// 2026-07-09): after PLC-heavy idle (concealing background packet loss for
// minutes), the decoder's predictive state is polluted with synthetic frames —
// suspected cause of the "first syllable of a reply crackles" report. RTVI
// bot-started-speaking (rtvi_callbacks.cpp) calls this so each reply decodes
// from a fresh state. Cheap (CTL reset, no realloc), safe on the webrtc thread.
void pipecat_reset_audio_decoder() {
  // GATED (2026-07-09b): the RTVI bot-started-speaking message rides the data
  // channel and can arrive AFTER the first audio packets — resetting the
  // decoder mid-stream discards live predictive state and STUTTERS the first
  // syllable (observed). Only reset when playback is idle (ring empty), i.e.
  // the reset genuinely precedes the utterance's audio.
  if (opus_decoder != nullptr && play_ring_count() == 0 && !is_playing) {
    opus_decoder_ctl(opus_decoder, OPUS_RESET_STATE);
    ESP_LOGI(LOG_TAG, "opus decoder state reset (utterance start, idle)");
  }
}

static uint32_t s_pending_gap = 0;   // gaps signaled but not yet filled
volatile uint32_t g_play_stat_fec = 0;  // gaps recovered via in-band FEC (real audio)

void pipecat_audio_decode(uint8_t *data, size_t size) {
  // NULL data = gap signal from the vendored libpeer rtp.c: one RTP packet
  // (20ms opus frame) was lost in transit. Recovery ladder (2026-07-09b):
  //   1. single gap + FEC-enabled encoder -> decode the redundant copy
  //      embedded in the NEXT packet (opus in-band FEC): REAL audio.
  //   2. multi-gap / no next packet -> opus_decode(NULL) PLC synthesis.
  // The server encoder ships fec=1,packet_loss=10 (aiortc monkey-patch in
  // webrtc_server.py) — 94% of measured gap events are single-packet, the
  // exact profile in-band FEC exists for.
  if (data == NULL) {
    s_pending_gap++;
    return;  // defer: the NEXT packet decides FEC vs PLC
  }

  if (s_pending_gap > 0) {
    // Fill the most recent missing frame from THIS packet's FEC payload if
    // it was a single gap; synthesize PLC for anything deeper.
    while (s_pending_gap > 1) {
      int plc_size = opus_decode(opus_decoder, NULL, 0, decoder_buffer,
                                 PCM_SAMPLES_PER_FRAME, 0);
      g_play_stat_plc++;
      s_pending_gap--;
      if (plc_size > 0) {
        push_decoded_to_ring(decoder_buffer, plc_size);
      }
    }
    int fec_size = opus_decode(opus_decoder, data, size, decoder_buffer,
                               PCM_SAMPLES_PER_FRAME, 1 /* decode_fec */);
    s_pending_gap = 0;
    if (fec_size > 0) {
      g_play_stat_fec++;
      push_decoded_to_ring(decoder_buffer, fec_size);
    } else {
      // Encoder had no FEC data — fall back to PLC for the lost frame.
      int plc_size = opus_decode(opus_decoder, NULL, 0, decoder_buffer,
                                 PCM_SAMPLES_PER_FRAME, 0);
      g_play_stat_plc++;
      if (plc_size > 0) {
        push_decoded_to_ring(decoder_buffer, plc_size);
      }
    }
  }

  int decoded_size = opus_decode(opus_decoder, data, size, decoder_buffer,
                                 PCM_SAMPLES_PER_FRAME, 0);

  if (decoded_size <= 0) {
    return;
  }

  // NOTE (2026-07-08): pre-roll on is_playing edges was tried and REVERTED
  // (injected silence mid-speech). PRODUCER ONLY: all I2S writes live in the
  // playback task; this thread only decodes and pushes to the ring.
  push_decoded_to_ring(decoder_buffer, decoded_size);
}

static OpusEncoder *opus_encoder = nullptr;
static uint8_t *encoder_output_buffer = nullptr;
static int16_t *read_buffer = nullptr;
static int32_t *i2s_capture_buffer = nullptr;

void pipecat_init_audio_encoder() {
  int encoder_error;
#if PIPECAT_DUAL_STREAM
  opus_encoder = opus_encoder_create(WEBRTC_SAMPLE_RATE, 2,
                                     OPUS_APPLICATION_VOIP, &encoder_error);
#else
  opus_encoder = opus_encoder_create(WEBRTC_SAMPLE_RATE, 1,
                                     OPUS_APPLICATION_VOIP, &encoder_error);
#endif
  if (encoder_error != OPUS_OK) {
    ESP_LOGE(LOG_TAG, "Failed to create OPUS encoder: %d", encoder_error);
    return;
  }

#if PIPECAT_DUAL_STREAM
  opus_encoder_ctl(opus_encoder,
                   OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE * 2));
#else
  opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE));
#endif
  opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
  opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(opus_encoder, OPUS_SET_INBAND_FEC(1));
  opus_encoder_ctl(
      opus_encoder,
      OPUS_SET_PACKET_LOSS_PERC(OPUS_EXPECTED_PACKET_LOSS_PCT));

#if PIPECAT_DUAL_STREAM
  read_buffer =
      (int16_t *)heap_caps_malloc(PCM_BUFFER_SIZE * 2, MALLOC_CAP_8BIT);
#else
  read_buffer = (int16_t *)heap_caps_malloc(PCM_BUFFER_SIZE, MALLOC_CAP_8BIT);
#endif
  i2s_capture_buffer =
      (int32_t *)heap_caps_malloc(BOARD_FRAME_BYTES, MALLOC_CAP_DMA);
  encoder_output_buffer = (uint8_t *)malloc(OPUS_BUFFER_SIZE);
  if (read_buffer == nullptr || i2s_capture_buffer == nullptr ||
      encoder_output_buffer == nullptr) {
    ESP_LOGE(LOG_TAG, "Failed to allocate capture buffers");
  }
}

void pipecat_send_audio(PeerConnection *peer_connection) {
#ifdef PIPECAT_BENCH_SEND_TONE
#if PIPECAT_DUAL_STREAM
  fill_bench_stereo_tone(read_buffer, PCM_SAMPLES_PER_FRAME);
#else
  fill_bench_tone(read_buffer, PCM_SAMPLES_PER_FRAME);
#endif
#else
  // Raw bench-capture tap owns rx_handle for its duration; publish silence so
  // the RTP cadence (and server-side liveness) is preserved. No extra sleep:
  // pipecat_send_audio_task paces with vTaskDelayUntil at absolute 20 ms
  // deadlines, so returning fast keeps exactly 50 fps.
  if (g_raw_capture_pause) {
#if PIPECAT_DUAL_STREAM
    memset(read_buffer, 0, PCM_BUFFER_SIZE * 2);
#else
    memset(read_buffer, 0, PCM_BUFFER_SIZE);
#endif
    auto paused_size =
        opus_encode(opus_encoder, (const opus_int16 *)read_buffer,
                    PCM_SAMPLES_PER_FRAME, encoder_output_buffer,
                    OPUS_BUFFER_SIZE);
    if (paused_size > 0) {
      peer_connection_send_audio(peer_connection, encoder_output_buffer,
                                 paused_size);
    }
    return;
  }
  size_t bytes_read = 0;
  esp_err_t ret = i2s_channel_read(rx_handle, i2s_capture_buffer,
                                   BOARD_FRAME_BYTES, &bytes_read,
                                   pdMS_TO_TICKS(200));
  // Throttled diagnostic: report mic capture health once per second.
  // Helps the bench operator see whether XVF3800 is actually clocking I2S
  // and what audio level the mic array is delivering.
  static uint32_t diag_frames = 0;
  static uint32_t diag_ok = 0;
  static uint32_t diag_zero_bytes = 0;
  static esp_err_t diag_last_err = ESP_OK;
  static int32_t diag_peak = 0;
  static int32_t diag_mono_peak = 0;
  diag_frames++;
  if (ret != ESP_OK) {
    diag_last_err = ret;
  } else if (bytes_read == 0) {
    diag_zero_bytes++;
  }
  static uint32_t diag_raw_peak = 0;
  if (ret == ESP_OK && bytes_read > 0) {
    diag_ok++;
    size_t samples = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < samples; i++) {
      int32_t raw = i2s_capture_buffer[i];
      uint32_t rawmag =
          raw < 0 ? static_cast<uint32_t>(-(int64_t)raw)
                  : static_cast<uint32_t>(raw);
      if (rawmag > diag_raw_peak) diag_raw_peak = rawmag;
      int32_t s = raw >> 16;
      if (s < 0) s = -s;
      if (s > diag_peak) diag_peak = s;
    }
#if PIPECAT_DUAL_STREAM
    stereo_48k_32bit_to_stereo_16k(i2s_capture_buffer,
                                   bytes_read / sizeof(int32_t), read_buffer);
#else
    stereo_48k_32bit_to_mono_16k(i2s_capture_buffer,
                                 bytes_read / sizeof(int32_t), read_buffer);
#endif
#if PIPECAT_DUAL_STREAM
    for (size_t i = 0; i < PCM_SAMPLES_PER_FRAME * 2; i++) {
#else
    for (size_t i = 0; i < PCM_SAMPLES_PER_FRAME; i++) {
#endif
      int32_t s = read_buffer[i];
      if (s < 0) s = -s;
      if (s > diag_mono_peak) diag_mono_peak = s;
    }
  } else {
#if PIPECAT_DUAL_STREAM
    memset(read_buffer, 0, PCM_BUFFER_SIZE * 2);
#else
    memset(read_buffer, 0, PCM_BUFFER_SIZE);
#endif
  }
  // PCM frames are 20ms => 50 per second.
  if (diag_frames >= 50) {
    // Dump first 8 raw int32 samples each second to confirm we're seeing
    // real I2S data and not all-zero garbage. Helps diagnose silent stream
    // vs unsynced framing.
    ESP_LOGI(LOG_TAG,
             "mic capture: %lu/%lu ok, %lu zero-byte, last_err=%s, peak |s16|=%ld mono=%ld raw=%lu full_duplex=%d%s",
             (unsigned long)diag_ok, (unsigned long)diag_frames,
             (unsigned long)diag_zero_bytes, esp_err_to_name(diag_last_err),
             (long)diag_peak, (long)diag_mono_peak,
             (unsigned long)diag_raw_peak, is_playing ? 1 : 0,
             xvf3800_present ? "" : " [XVF3800 ABSENT]");
    if (diag_ok > 0) {
      ESP_LOGI(LOG_TAG,
               "raw: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
               (unsigned long)i2s_capture_buffer[0],
               (unsigned long)i2s_capture_buffer[1],
               (unsigned long)i2s_capture_buffer[2],
               (unsigned long)i2s_capture_buffer[3],
               (unsigned long)i2s_capture_buffer[4],
               (unsigned long)i2s_capture_buffer[5],
               (unsigned long)i2s_capture_buffer[6],
               (unsigned long)i2s_capture_buffer[7]);
    }
    // NOTE: beam-telemetry I2C reads (azimuth/spenergy) used to run here on the
    // audio-send task. They can block up to ~800ms (8 retries x 100ms) and were
    // starving the RTP publisher -> "no audio frame" churn. Beam direction is
    // now polled in pipecat_led_task() (core 1, low prio) instead.
    diag_frames = 0;
    diag_ok = 0;
    diag_zero_bytes = 0;
    diag_last_err = ESP_OK;
    diag_peak = 0;
    diag_mono_peak = 0;
    diag_raw_peak = 0;
  }
#endif

  auto encoded_size =
      opus_encode(opus_encoder, (const opus_int16 *)read_buffer,
                  PCM_SAMPLES_PER_FRAME, encoder_output_buffer,
                  OPUS_BUFFER_SIZE);
  if (encoded_size > 0) {
    peer_connection_send_audio(peer_connection, encoder_output_buffer,
                               encoded_size);
  }
  // NOTE: LED rendering + beam telemetry moved OFF this task into led_task()
  // (see below). Those do XVF control-I2C reads/writes that can block up to
  // ~800ms (8 retries x 100ms), which was starving this RTP publisher and
  // causing "No audio frame" server-side timeouts -> peer churn. The audio
  // publisher now does ONLY i2s_read + opus_encode + RTP send.
}

// ---------------------------------------------------------------------------
// LED task: owns ALL XVF control-I2C for the ring (state decision, beam
// telemetry read, and the 48-byte ring write). Runs on its own low-priority
// task so a slow/contended XVF control transaction can never stall the audio
// publisher. ~20Hz tick; LED effects animate here, beam direction is polled
// at ~4Hz and eased toward the target for a smooth "float", matching ESPHome.
// ---------------------------------------------------------------------------
static void pipecat_led_step() {
#if !PIPECAT_LED_ENABLE
  return;
#else
  static int led_beam = -1;            // target beam LED (-1 = none)
  static uint32_t beam_poll_tick = 0;  // throttles the (blocking) az read
  LedState st;

  int phase = g_server_phase.load();
  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
  bool phase_fresh =
      (phase != PHASE_NONE) &&
      ((now_ms - g_server_phase_ms.load()) < LED_PHASE_TTL_MS);

  if (!pipecat_webrtc_connected) {
    st = LED_OFF;
    led_beam = -1;
  } else if (phase_fresh) {
    switch (phase) {
      case PHASE_SPEAKING: st = LED_SPEAKING; led_beam = -1; break;
      case PHASE_THINKING: st = LED_THINKING; led_beam = -1; break;
      case PHASE_WAITING:
        st = LED_WAITING;
        // Point the purple beam at whoever triggered the wake. Poll ~4Hz
        // (every 5th 20ms tick) so the blocking az read is infrequent, and
        // only while we're actually in WAITING.
        if (xvf3800_present && xvf_beam_telemetry_supported &&
            (++beam_poll_tick % 5 == 0)) {
          float az[4] = {};
          if (xvf_read_float4(XVF_RESID_AEC, XVF_CMD_AEC_AZIMUTH_VALUES, az)) {
            led_beam = azimuth_to_led(az[3]);
          }
        }
        break;
      case PHASE_IDLE:
      default:
        st = LED_IDLE; led_beam = -1; break;
    }
  } else if (is_playing) {
    st = LED_SPEAKING;
    led_beam = -1;
  } else {
    st = LED_IDLE;
    led_beam = -1;
  }
  g_led_state = st;
  led_render(st, led_beam);
#endif
}

#ifndef LINUX_BUILD
void pipecat_led_task(void *arg) {
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(50);  // ~20Hz
  while (1) {
    pipecat_led_step();
    vTaskDelay(period);
  }
}
#endif
