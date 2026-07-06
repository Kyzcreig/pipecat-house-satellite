#include <opus.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
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

// XVF3800 control port. Pre-flashed via DFU; we just need to confirm it's
// alive on I2C and that it's clocking BCLK/WS as I2S master (otherwise our
// secondary-mode i2s_channel_read / i2s_channel_write will time out forever).
static constexpr uint8_t XVF3800_ADDR = 0x2C;
// Vendor control protocol: write 1B resource ID then read N bytes.
// Resource 0xB3 returns 3-byte semantic version of the XMOS DFU firmware,
// matching what formatBCE's respeaker_xvf3800 ESPHome component reads.
static constexpr uint8_t XVF3800_RESID_VERSION = 0xB3;
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

static constexpr uint8_t XVF_CMD_AUDIO_MGR_MIC_GAIN = 0;
static constexpr uint8_t XVF_CMD_AUDIO_MGR_REF_GAIN = 1;
static constexpr uint8_t XVF_CMD_AUDIO_MGR_OP_L = 15;
static constexpr uint8_t XVF_CMD_AUDIO_MGR_OP_R = 19;
static constexpr uint8_t XVF_CMD_AUDIO_MGR_SYS_DELAY = 26;

static constexpr uint8_t XVF_AUDIO_CATEGORY_PROCESSED = 6;
static constexpr uint8_t XVF_AUDIO_SOURCE_AUTO_SELECT = 3;
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

  // Route both I2S output channels to the XVF3800's auto-selected processed
  // beam. This gives the ESP32 mono path the same AEC/beamformed/PP signal on
  // left and right, instead of mixing the default communication + ASR channels.
  record(xvf_write_u8_pair(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_OP_L,
                           XVF_AUDIO_CATEGORY_PROCESSED,
                           XVF_AUDIO_SOURCE_AUTO_SELECT));
  record(xvf_write_u8_pair(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_OP_R,
                           XVF_AUDIO_CATEGORY_PROCESSED,
                           XVF_AUDIO_SOURCE_AUTO_SELECT));

  // Seeed's published XVF3800 tuning defaults for this board family.
  record(xvf_write_float(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_REF_GAIN,
                         8.0f));
  record(xvf_write_float(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_MIC_GAIN,
                         90.0f));
  record(xvf_write_int32(XVF_RESID_AUDIO_MGR, XVF_CMD_AUDIO_MGR_SYS_DELAY,
                         12));

  // Keep adaptive beamforming/AEC active and align the far-end reference gain
  // with the host playback path. Per-build override is useful when speaker
  // attenuation changes between bench and kitchen enclosures.
  record(xvf_write_int32(XVF_RESID_AEC, XVF_CMD_AEC_ASROUTONOFF, 0));
  record(xvf_write_int32(XVF_RESID_AEC, XVF_CMD_AEC_FIXEDBEAMSONOFF, 0));
  record(xvf_write_int32(XVF_RESID_AEC, XVF_CMD_AEC_HPFONOFF, 2));
  record(xvf_write_float(XVF_RESID_AEC, XVF_CMD_AEC_FAR_EXTGAIN,
                         PIPECAT_AEC_FAR_EXTGAIN_DB));

  // Enable the production post-processor: AGC, limiter, echo suppression,
  // non-linear echo attenuation, and conservative noise floors from XMOS docs.
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_AGCGAIN, 2.0f));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_AGCMAXGAIN, 64.0f));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_AGCDESIREDLEVEL, 0.0045f));
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_AGCONOFF, 1));
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_LIMITONOFF, 1));
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_ECHOONOFF, 1));
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_NLATTENONOFF, 1));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_MIN_NS, 0.15f));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_MIN_NN, 0.51f));
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_DTSENSITIVE, 10));
  record(xvf_write_int32(XVF_RESID_PP, XVF_CMD_PP_ATTNS_MODE, 1));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_ATTNS_NOMINAL, 1.0f));
  record(xvf_write_float(XVF_RESID_PP, XVF_CMD_PP_ATTNS_SLOPE, 1.0f));

  ESP_LOGI(LOG_TAG,
           "XVF3800 DSP profile: %lu/%lu control writes acked "
           "(processed auto-beam, AEC, AGC, limiter, no Wi-Fi PS; "
           "far_extgain=%.1fdB)",
           (unsigned long)ok, (unsigned long)total,
           (double)PIPECAT_AEC_FAR_EXTGAIN_DB);
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
  aic3104_write(AIC3104_PAGE_CTRL, 0x00);
  aic3104_write(AIC3104_LEFT_DAC_VOLUME, 0x10);
  aic3104_write(AIC3104_RIGHT_DAC_VOLUME, 0x10);

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

  // Read XMOS firmware version (resource 0xB3 -> 3 bytes major.minor.patch).
  uint8_t resid = XVF3800_RESID_VERSION;
  uint8_t ver[3] = {0, 0, 0};
  esp_err_t ver_ret = i2c_master_transmit_receive(
      xvf3800, &resid, 1, ver, sizeof(ver), pdMS_TO_TICKS(100));
  if (ver_ret == ESP_OK) {
    ESP_LOGI(LOG_TAG, "XVF3800 alive at 0x%02x, DFU firmware v%u.%u.%u",
             XVF3800_ADDR, ver[0], ver[1], ver[2]);
  } else {
    ESP_LOGW(LOG_TAG,
             "XVF3800 ack'd at 0x%02x but version read failed: %s. "
             "Continuing -- maybe vendor protocol mismatch.",
             XVF3800_ADDR, esp_err_to_name(ver_ret));
  }

  configure_xvf3800_dsp_profile();
}

static void init_i2s() {
  // XVF3800 DFU v6.34.4 on the theater/kitchen boards is the XMOS build that
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
      .dma_desc_num = 8,
      .dma_frame_num = 240,
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

static void mono_16k_to_stereo_48k_32bit(int16_t *src, size_t src_samples,
                                         int32_t *dst) {
  size_t out = 0;
  for (size_t i = 0; i < src_samples; i++) {
    int32_t sample = ((int32_t)src[i]) << 16;
    for (int j = 0; j < UPSAMPLE_RATIO; j++) {
      dst[out++] = sample;
      dst[out++] = sample;
    }
  }
}

static void stereo_48k_32bit_to_mono_16k(int32_t *src, size_t src_frames,
                                         int16_t *dst) {
  size_t out = 0;
  for (size_t i = 0; i + (UPSAMPLE_RATIO * 2 - 1) < src_frames &&
                     out < PCM_SAMPLES_PER_FRAME;
       i += UPSAMPLE_RATIO * 2) {
    int32_t best = 0;
    int32_t best_abs = 0;
    for (size_t j = 0; j < UPSAMPLE_RATIO * 2; j++) {
      int32_t sample = src[i + j] >> 16;
      int32_t sample_abs = sample < 0 ? -sample : sample;
      if (sample_abs > best_abs) {
        best = sample;
        best_abs = sample_abs;
      }
    }
    dst[out++] = (int16_t)best;
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

static int16_t *decoder_buffer = nullptr;
static int32_t *i2s_play_buffer = nullptr;
static OpusDecoder *opus_decoder = nullptr;

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
  if (decoder_buffer == nullptr || i2s_play_buffer == nullptr) {
    ESP_LOGE(LOG_TAG, "Failed to allocate playback buffers");
  }
}

void pipecat_audio_decode(uint8_t *data, size_t size) {
  int decoded_size =
      opus_decode(opus_decoder, data, size, decoder_buffer,
                  PCM_SAMPLES_PER_FRAME, 0);

  if (decoded_size <= 0) {
    return;
  }

  update_is_playing(decoder_buffer, decoded_size);
  if (!is_playing) {
    return;
  }

  mono_16k_to_stereo_48k_32bit(decoder_buffer, decoded_size, i2s_play_buffer);

  size_t bytes_written = 0;
  size_t bytes_to_write =
      decoded_size * UPSAMPLE_RATIO * 2 * sizeof(int32_t);
  esp_err_t ret = i2s_channel_write(tx_handle, i2s_play_buffer, bytes_to_write,
                                    &bytes_written,
                                    pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS));
  // Throttled diagnostic so we can correlate "i2s write failed" bursts with
  // XVF3800 clock loss vs occasional underruns.
  static uint32_t play_frames = 0;
  static uint32_t play_ok = 0;
  play_frames++;
  if (ret == ESP_OK) {
    play_ok++;
  } else {
    ESP_LOGW(LOG_TAG, "i2s write failed: %s (%lu/%lu bytes)",
             esp_err_to_name(ret), (unsigned long)bytes_written,
             (unsigned long)bytes_to_write);
  }
  if (play_frames >= 50) {
    ESP_LOGI(LOG_TAG, "i2s playback: %lu/%lu frames ok%s",
             (unsigned long)play_ok, (unsigned long)play_frames,
             xvf3800_present ? "" : " [XVF3800 ABSENT]");
    play_frames = 0;
    play_ok = 0;
  }
}

static OpusEncoder *opus_encoder = nullptr;
static uint8_t *encoder_output_buffer = nullptr;
static int16_t *read_buffer = nullptr;
static int32_t *i2s_capture_buffer = nullptr;

void pipecat_init_audio_encoder() {
  int encoder_error;
  opus_encoder = opus_encoder_create(WEBRTC_SAMPLE_RATE, 1,
                                     OPUS_APPLICATION_VOIP, &encoder_error);
  if (encoder_error != OPUS_OK) {
    ESP_LOGE(LOG_TAG, "Failed to create OPUS encoder: %d", encoder_error);
    return;
  }

  opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE));
  opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
  opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

  read_buffer = (int16_t *)heap_caps_malloc(PCM_BUFFER_SIZE, MALLOC_CAP_8BIT);
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
  fill_bench_tone(read_buffer, PCM_SAMPLES_PER_FRAME);
#else
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
    stereo_48k_32bit_to_mono_16k(i2s_capture_buffer,
                                 bytes_read / sizeof(int32_t), read_buffer);
    for (size_t i = 0; i < PCM_SAMPLES_PER_FRAME; i++) {
      int32_t s = read_buffer[i];
      if (s < 0) s = -s;
      if (s > diag_mono_peak) diag_mono_peak = s;
    }
  } else {
    memset(read_buffer, 0, PCM_BUFFER_SIZE);
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
    if (xvf3800_present && xvf_beam_telemetry_supported) {
      float azimuth[4] = {};
      float energy[4] = {};
      bool az_ok =
          xvf_read_float4(XVF_RESID_AEC, XVF_CMD_AEC_AZIMUTH_VALUES, azimuth);
      bool energy_ok =
          xvf_read_float4(XVF_RESID_AEC, XVF_CMD_AEC_SPENERGY_VALUES, energy);
      if (az_ok || energy_ok) {
        int led = az_ok ? azimuth_to_led(azimuth[3]) : -1;
        ESP_LOGI(LOG_TAG,
                 "xvf beam: az_ok=%d auto=%.3frad led=%d spenergy=[%.0f %.0f %.0f %.0f]",
                 az_ok ? 1 : 0, (double)azimuth[3], led,
                 (double)energy[0], (double)energy[1], (double)energy[2],
                 (double)energy[3]);
      } else {
        xvf_beam_telemetry_supported = false;
        ESP_LOGW(LOG_TAG,
                 "XVF3800 beam telemetry unavailable; disabling beam polls");
      }
    }
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
}
