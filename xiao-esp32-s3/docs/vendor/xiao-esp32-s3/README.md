# XIAO ESP32-S3 vendor source snapshot

Retrieved 2026-07-16 PDT for the XIAO ESP32-S3 + ReSpeaker XVF3800 firmware audit.

This directory preserves the authoritative inputs used by the canonical Obsidian note `Engineering/Projects/Pipecat House Voice/XIAO ESP32-S3 — Complete Device Reference.md`. Operational conclusions belong in that note; these files are immutable evidence. Verify them with:

```sh
shasum -a 256 -c SHA256SUMS
```

## Version boundaries

- The firmware dependency lock is ESP-IDF `5.5.4`; programming-guide sources are pinned to tag `v5.5.4` (tag object `eebd1d67000d55fcf392711763de30bb3c8ddbcd`, peeled commit `735507283d5b2f9fb363a1901172dbd9e847945d`).
- The Seeed wiki source is pinned to `Seeed-Studio/wiki-documents` commit `8289a990c9e7bfdc8e43a1d85e1805e628a559bb` rather than a moving rendered page.
- The current PDFs identify themselves as ESP32-S3 datasheet v2.2 and TRM v1.8.
- Both base-board schematic v1.2 and v1.3 are retained because the PCB revision installed in the deployed theater and kitchen satellites has not been physically verified.
- The Sense base/expansion schematics are boundary references only. The production target is a XIAO ESP32-S3 paired with a ReSpeaker XVF3800 carrier; no Sense camera, PDM microphone, or SD-card use exists in the audited firmware.

## Source map

| Local file | Upstream source |
|---|---|
| `seeed-xiao-esp32s3-wiki.md` | [Seeed wiki source at pinned commit](https://raw.githubusercontent.com/Seeed-Studio/wiki-documents/8289a990c9e7bfdc8e43a1d85e1805e628a559bb/sites/en/docs/Sensor/SeeedStudio_XIAO/SeeedStudio_XIAO_ESP32S3/XIAO_ESP32S3_Getting_Started.md) |
| `seeed-xiao-esp32s3-pin-multiplexing.md` | [Seeed pin-multiplexing source at pinned commit](https://raw.githubusercontent.com/Seeed-Studio/wiki-documents/8289a990c9e7bfdc8e43a1d85e1805e628a559bb/sites/en/docs/Sensor/SeeedStudio_XIAO/SeeedStudio_XIAO_ESP32S3/XIAO_ESP32S3_Pin_Multiplexing.md) |
| `seeed-xiao-esp32s3-sense-power-consumption.md` | [Seeed Sense power source at pinned commit](https://raw.githubusercontent.com/Seeed-Studio/wiki-documents/8289a990c9e7bfdc8e43a1d85e1805e628a559bb/sites/en/docs/Sensor/SeeedStudio_XIAO/SeeedStudio_XIAO_ESP32S3/XIAO_ESP32S3_Sense_Consumption.md) |
| `seeed-xiao-esp32s3-sense-camera.md` | [Seeed Sense camera source at pinned commit](https://raw.githubusercontent.com/Seeed-Studio/wiki-documents/8289a990c9e7bfdc8e43a1d85e1805e628a559bb/sites/en/docs/Sensor/SeeedStudio_XIAO/SeeedStudio_XIAO_ESP32S3_Sense/XIAO_ESP32S3_Sense_camera.md) |
| `seeed-xiao-esp32s3-sense-microphone.md` | [Seeed Sense microphone source at pinned commit](https://raw.githubusercontent.com/Seeed-Studio/wiki-documents/8289a990c9e7bfdc8e43a1d85e1805e628a559bb/sites/en/docs/Sensor/SeeedStudio_XIAO/SeeedStudio_XIAO_ESP32S3_Sense/XIAO_ESP32S3_Sense_mic.md) |
| `seeed-xiao-esp32s3-sense-sd-filesystem.md` | [Seeed Sense SD/filesystem source at pinned commit](https://raw.githubusercontent.com/Seeed-Studio/wiki-documents/8289a990c9e7bfdc8e43a1d85e1805e628a559bb/sites/en/docs/Sensor/SeeedStudio_XIAO/SeeedStudio_XIAO_ESP32S3_Sense/XIAO_ESP32S3_Sense_tf_and_filesystem.md) |
| `seeed-xiao-esp32s3-schematic-v1.2.pdf` | [Seeed base schematic v1.2](https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/XIAO_ESP32S3_SCH_v1.2.pdf) |
| `seeed-xiao-esp32s3-schematic-v1.3.pdf` | [Seeed base schematic v1.3](https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/XIAO_ESP32S3_V1.3_SCH_260115.pdf) |
| `seeed-xiao-esp32s3-sense-schematic-v1.5.pdf` | [Seeed Sense schematic v1.5](https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/new-res/202003753_XIAO%20ESP32S3%20Sense_v1.5_SCH_260226.pdf.pdf) |
| `seeed-xiao-esp32s3-sense-expansion-schematic-v1.0.pdf` | [Seeed Sense expansion schematic v1.0](https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/XIAO_ESP32S3_ExpBoard_v1.0_SCH.pdf) |
| `espressif-esp32-s3-datasheet.pdf` | [ESP32-S3 series datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf) |
| `espressif-esp32-s3-technical-reference-manual.pdf` | [ESP32-S3 TRM](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf) |
| `esp-idf-v5.5.4-i2s.rst` | [ESP-IDF 5.5.4 I2S guide source](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-reference/peripherals/i2s.rst) |
| `esp-idf-v5.5.4-external-ram.rst` | [ESP-IDF 5.5.4 external RAM guide source](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-guides/external-ram.rst) |
| `esp-idf-v5.5.4-nvs-flash.rst` | [ESP-IDF 5.5.4 NVS guide source](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-reference/storage/nvs_flash.rst) |
| `esp-idf-v5.5.4-freertos-idf.rst` | [ESP-IDF 5.5.4 FreeRTOS guide source](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-reference/system/freertos_idf.rst) |
| `esp-idf-v5.5.4-performance-speed.rst` | [ESP-IDF 5.5.4 scheduling/performance guidance](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-guides/performance/speed.rst) |
| `esp-idf-v5.5.4-wifi.rst` | [ESP-IDF 5.5.4 Wi-Fi guide source](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-guides/wifi.rst) |
| `esp-idf-v5.5.4-esp-wifi-api.rst` | [ESP-IDF 5.5.4 Wi-Fi API source](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-reference/network/esp_wifi.rst) |
| `esp-idf-v5.5.4-rf-coexistence.rst` | [ESP-IDF 5.5.4 RF coexistence guide source](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-guides/coexist.rst) |
| `esp-idf-v5.5.4-sleep-modes.rst` | [ESP-IDF 5.5.4 sleep-mode guide source](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-reference/system/sleep_modes.rst) |
| `esp-idf-v5.5.4-adc-oneshot.rst` | [ESP-IDF 5.5.4 ADC oneshot guide source](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-reference/peripherals/adc_oneshot.rst) |
| `esp-idf-v5.5.4-capacitive-touch.rst` | [ESP-IDF 5.5.4 touch-sensor guide source](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-reference/peripherals/cap_touch_sens.rst) |

The RST files are upstream source snapshots and retain Sphinx substitutions/includes. Use the version-pinned rendered guide at `https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/` when diagrams or expanded target-specific conditionals are needed.
