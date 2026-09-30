#include "xvf_aec_filter.h"

#include <string.h>

static uint32_t load_le32(const uint8_t *in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

static int read_i32(const XvfAecFilterOps *ops, uint8_t cmd, int32_t *value) {
  uint8_t payload[4] = {};
  int ret = ops->read_bytes(ops->ctx, cmd, payload, sizeof(payload));
  if (ret == 0) {
    *value = static_cast<int32_t>(load_le32(payload));
  }
  return ret;
}

static void abort_sequence(const XvfAecFilterOps *ops,
                           XvfAecFilterResult *result) {
  const int32_t zero = 0;
  ops->write_i32(ops->ctx, XVF_AEC_CMD_FILTER_ABORT, &zero, 1);
  result->aborted = true;
}

XvfAecFilterResult xvf_aec_filter_read(const XvfAecFilterOps *ops, int32_t far,
                                       int32_t mic, float *out,
                                       size_t out_capacity, int64_t budget_us) {
  XvfAecFilterResult result = {};
  result.status = XvfAecFilterStatus::IO_ERROR;
  const int64_t start_us = ops->now_us(ops->ctx);

  // Pre-flight reads only: nothing is started until every bound checks out.
  int32_t length = 0;
  int ret = read_i32(ops, XVF_AEC_CMD_NUM_MICS, &result.num_mics);
  if (ret == 0) {
    ret = read_i32(ops, XVF_AEC_CMD_NUM_FARENDS, &result.num_farends);
  }
  if (ret == 0) {
    ret = read_i32(ops, XVF_AEC_CMD_FILTER_LENGTH, &length);
  }
  if (ret != 0) {
    result.io_error = ret;
    return result;
  }
  if (far < 0 || far >= result.num_farends || mic < 0 ||
      mic >= result.num_mics) {
    result.status = XvfAecFilterStatus::BAD_INDEX;
    return result;
  }
  if (length <= 0 || static_cast<size_t>(length) > out_capacity) {
    result.status = XvfAecFilterStatus::BAD_LENGTH;
    result.length = length > 0 ? static_cast<uint32_t>(length) : 0;
    return result;
  }

  const int32_t far_mic[2] = {far, mic};
  ret = ops->write_i32(ops->ctx, XVF_AEC_CMD_FAR_MIC_INDEX, far_mic, 2);
  if (ret != 0) {
    result.io_error = ret;
    abort_sequence(ops, &result);
    return result;
  }

  uint8_t page[XVF_AEC_COEFFS_PER_PAGE * sizeof(float)];
  for (int32_t offset = 0; offset < length;
       offset += static_cast<int32_t>(XVF_AEC_COEFFS_PER_PAGE)) {
    if (ops->now_us(ops->ctx) - start_us > budget_us) {
      result.status = XvfAecFilterStatus::TIMEOUT;
      abort_sequence(ops, &result);
      return result;
    }
    ret = ops->write_i32(ops->ctx, XVF_AEC_CMD_FILTER_COEFF_START_OFFSET,
                         &offset, 1);
    if (ret == 0) {
      ret = ops->read_bytes(ops->ctx, XVF_AEC_CMD_FILTER_COEFFS, page,
                            sizeof(page));
    }
    if (ret != 0) {
      result.io_error = ret;
      abort_sequence(ops, &result);
      return result;
    }
    result.pages++;
    size_t take = static_cast<size_t>(length - offset);
    if (take > XVF_AEC_COEFFS_PER_PAGE) {
      take = XVF_AEC_COEFFS_PER_PAGE;
    }
    for (size_t i = 0; i < take; i++) {
      uint32_t bits = load_le32(&page[i * sizeof(float)]);
      memcpy(&out[offset + static_cast<int32_t>(i)], &bits, sizeof(bits));
    }
    ops->yield(ops->ctx);
  }

  result.status = XvfAecFilterStatus::OK;
  result.length = static_cast<uint32_t>(length);
  return result;
}

const char *xvf_aec_filter_status_name(XvfAecFilterStatus status) {
  switch (status) {
    case XvfAecFilterStatus::OK:
      return "ok";
    case XvfAecFilterStatus::BAD_INDEX:
      return "bad_index";
    case XvfAecFilterStatus::BAD_LENGTH:
      return "bad_length";
    case XvfAecFilterStatus::IO_ERROR:
      return "io_error";
    case XvfAecFilterStatus::TIMEOUT:
      return "timeout";
  }
  return "unknown";
}
