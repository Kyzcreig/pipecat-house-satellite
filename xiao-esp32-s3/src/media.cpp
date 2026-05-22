#include <opus.h>

#include <atomic>
#include <cstring>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
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

static i2c_master_bus_handle_t i2c_bus = nullptr;
static i2c_master_dev_handle_t aic3104 = nullptr;
static i2c_master_dev_handle_t xvf3800 = nullptr;
static i2s_chan_handle_t tx_handle = nullptr;
static i2s_chan_handle_t rx_handle = nullptr;
static bool xvf3800_present = false;

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
}

static void init_i2s() {
  // XVF3800 DFU v6.0.0 on this bench board is the stock XMOS INT-Device build,
  // where the XVF3800 is the I2S SLAVE and expects the host (ESP32-S3) to
  // provide BCLK/WS. Seeed's HA-specific "i2s_master" firmware would flip
  // these roles, but we cannot rely on that being flashed. Drive the bus
  // ourselves so XVF3800 can clock its internal pipeline off our reference.
  i2s_chan_config_t chan_cfg = {
      .id = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
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

  if (silence_count >= 20 && is_playing) {
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
                                    &bytes_written, pdMS_TO_TICKS(40));
  // Throttled diagnostic so we can correlate "i2s write failed" bursts with
  // XVF3800 clock loss vs occasional underruns.
  static uint32_t play_frames = 0;
  static uint32_t play_ok = 0;
  play_frames++;
  if (ret == ESP_OK) {
    play_ok++;
  } else {
    ESP_LOGW(LOG_TAG, "i2s write failed: %s", esp_err_to_name(ret));
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
  if (is_playing) {
    memset(read_buffer, 0, PCM_BUFFER_SIZE);
    vTaskDelay(pdMS_TO_TICKS(20));
  } else {
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
        uint32_t rawmag = raw < 0 ? (uint32_t)(-raw) : (uint32_t)raw;
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
               "mic capture: %lu/%lu ok, %lu zero-byte, last_err=%s, peak |s16|=%ld mono=%ld raw=%lu%s",
               (unsigned long)diag_ok, (unsigned long)diag_frames,
               (unsigned long)diag_zero_bytes,
               esp_err_to_name(diag_last_err),
               (long)diag_peak,
               (long)diag_mono_peak,
               (unsigned long)diag_raw_peak,
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
      diag_frames = 0;
      diag_ok = 0;
      diag_zero_bytes = 0;
      diag_last_err = ESP_OK;
      diag_peak = 0;
      diag_mono_peak = 0;
      diag_raw_peak = 0;
    }
#endif
  }

  auto encoded_size =
      opus_encode(opus_encoder, (const opus_int16 *)read_buffer,
                  PCM_SAMPLES_PER_FRAME, encoder_output_buffer,
                  OPUS_BUFFER_SIZE);
  if (encoded_size > 0) {
    peer_connection_send_audio(peer_connection, encoder_output_buffer,
                               encoded_size);
  }
}
