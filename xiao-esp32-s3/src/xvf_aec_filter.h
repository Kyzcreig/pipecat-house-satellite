#pragma once

#include <stddef.h>
#include <stdint.h>

// XVF3800 AEC filter-coefficient read sequence (t_c1bfa4f6), transport-free so
// the host tests can drive it against a fake device.
//
// Sequence is the one in XMOS host_xvf_control src/special_commands/filters.cpp
// (get_one_filter / get_or_set_full_buffer), RESID 33:
//   read  AEC_NUM_MICS (71), AEC_NUM_FARENDS (72), SPECIAL_CMD_AEC_FILTER_LENGTH (93)
//   write SPECIAL_CMD_AEC_FAR_MIC_INDEX (90) = {far, mic}
//   for offset in 0, 15, 30, ... < length:
//     write SPECIAL_CMD_AEC_FILTER_COEFF_START_OFFSET (91) = offset
//     read  SPECIAL_CMD_AEC_FILTER_COEFFS (92) -> 15 floats
// On any failure after the sequence starts, AEC_FILTER_CMD_ABORT (94) is
// written to reset the device's special-command state machine.
// Deliberately NOT done: the reference sets SHF_BYPASS=1 around the read to
// freeze adaptation. That bypasses the AEC on the live audio path, so this
// reader leaves the AEC running (pages may straddle adaptation steps).
// Nothing here is persisted and no coefficient is ever written.

static constexpr uint8_t XVF_AEC_CMD_FAR_MIC_INDEX = 90;
static constexpr uint8_t XVF_AEC_CMD_FILTER_COEFF_START_OFFSET = 91;
static constexpr uint8_t XVF_AEC_CMD_FILTER_COEFFS = 92;
static constexpr uint8_t XVF_AEC_CMD_FILTER_LENGTH = 93;
static constexpr uint8_t XVF_AEC_CMD_FILTER_ABORT = 94;
static constexpr uint8_t XVF_AEC_CMD_NUM_MICS = 71;
static constexpr uint8_t XVF_AEC_CMD_NUM_FARENDS = 72;
static constexpr size_t XVF_AEC_COEFFS_PER_PAGE = 15;
// 3072 live on both rooms (t_7595755b readback); refuse anything larger
// rather than truncate.
static constexpr size_t XVF_AEC_FILTER_MAX_COEFFS = 4096;

struct XvfAecFilterOps {
  void *ctx;
  // Write `count` little-endian int32 values to RESID 33 / cmd. 0 = ok.
  int (*write_i32)(void *ctx, uint8_t cmd, const int32_t *values,
                   size_t count);
  // Read `len` payload bytes from RESID 33 / cmd. 0 = ok.
  int (*read_bytes)(void *ctx, uint8_t cmd, uint8_t *out, size_t len);
  int64_t (*now_us)(void *ctx);
  // Called once per page so the HTTP task does not hog the I2C bus.
  void (*yield)(void *ctx);
};

enum class XvfAecFilterStatus : uint8_t {
  OK = 0,
  BAD_INDEX,   // far/mic outside the device's AEC_NUM_FARENDS/AEC_NUM_MICS
  BAD_LENGTH,  // device length 0 or larger than the caller's buffer
  IO_ERROR,    // a control transaction failed (io_error holds the code)
  TIMEOUT,     // the read exceeded budget_us
};

struct XvfAecFilterResult {
  XvfAecFilterStatus status;
  int io_error;
  int32_t num_mics;
  int32_t num_farends;
  uint32_t length;  // coefficients written to out on OK
  uint32_t pages;   // COEFFS pages read
  bool aborted;     // AEC_FILTER_CMD_ABORT was issued
};

XvfAecFilterResult xvf_aec_filter_read(const XvfAecFilterOps *ops, int32_t far,
                                       int32_t mic, float *out,
                                       size_t out_capacity, int64_t budget_us);

const char *xvf_aec_filter_status_name(XvfAecFilterStatus status);
