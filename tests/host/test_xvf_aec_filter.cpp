// Host tests for the XVF3800 AEC filter-coefficient read sequence
// (t_c1bfa4f6). The fake models the device side of XMOS shf_wrapper.c:
// FAR_MIC_INDEX latches {far, mic}; START_OFFSET latches the page start;
// COEFFS returns 15 floats from that offset (zero past the end).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "../../xiao-esp32-s3/src/xvf_aec_filter.h"

namespace {

struct FakeXvf {
  int32_t num_mics = 4;
  int32_t num_farends = 1;
  int32_t length = 3072;
  int32_t far = -1;
  int32_t mic = -1;
  int32_t offset = -1;
  int64_t clock_us = 0;
  int64_t us_per_op = 10;
  int fail_on_coeff_page = -1;  // 0-based page index whose COEFFS read fails
  int coeff_reads = 0;
  int yields = 0;
  std::vector<uint8_t> writes;  // cmd of every write, in order
  std::vector<int32_t> offsets;

  float coeff(int32_t f, int32_t m, int32_t i) const {
    return static_cast<float>(f * 100000 + m * 10000 + i) * 0.5f;
  }
};

void store_le32(uint8_t *out, uint32_t v) {
  out[0] = v & 0xff;
  out[1] = (v >> 8) & 0xff;
  out[2] = (v >> 16) & 0xff;
  out[3] = (v >> 24) & 0xff;
}

int fake_write(void *ctx, uint8_t cmd, const int32_t *values, size_t count) {
  FakeXvf *x = static_cast<FakeXvf *>(ctx);
  x->clock_us += x->us_per_op;
  x->writes.push_back(cmd);
  if (cmd == XVF_AEC_CMD_FAR_MIC_INDEX) {
    assert(count == 2);
    x->far = values[0];
    x->mic = values[1];
  } else if (cmd == XVF_AEC_CMD_FILTER_COEFF_START_OFFSET) {
    assert(count == 1);
    x->offset = values[0];
    x->offsets.push_back(values[0]);
  } else {
    assert(cmd == XVF_AEC_CMD_FILTER_ABORT && count == 1);
    x->far = x->mic = x->offset = -1;
  }
  return 0;
}

int fake_read(void *ctx, uint8_t cmd, uint8_t *out, size_t len) {
  FakeXvf *x = static_cast<FakeXvf *>(ctx);
  x->clock_us += x->us_per_op;
  if (cmd == XVF_AEC_CMD_NUM_MICS || cmd == XVF_AEC_CMD_NUM_FARENDS ||
      cmd == XVF_AEC_CMD_FILTER_LENGTH) {
    assert(len == 4);
    int32_t v = cmd == XVF_AEC_CMD_NUM_MICS      ? x->num_mics
                : cmd == XVF_AEC_CMD_NUM_FARENDS ? x->num_farends
                                                 : x->length;
    store_le32(out, static_cast<uint32_t>(v));
    return 0;
  }
  assert(cmd == XVF_AEC_CMD_FILTER_COEFFS);
  assert(len == XVF_AEC_COEFFS_PER_PAGE * sizeof(float));
  assert(x->far >= 0 && x->mic >= 0 && x->offset >= 0);
  if (x->coeff_reads++ == x->fail_on_coeff_page) {
    return 0x107;  // ESP_ERR_TIMEOUT-shaped
  }
  for (size_t i = 0; i < XVF_AEC_COEFFS_PER_PAGE; i++) {
    int32_t idx = x->offset + static_cast<int32_t>(i);
    float v = idx < x->length ? x->coeff(x->far, x->mic, idx) : 0.0f;
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    store_le32(&out[i * 4], bits);
  }
  return 0;
}

int64_t fake_now(void *ctx) {
  return static_cast<FakeXvf *>(ctx)->clock_us;
}
void fake_yield(void *ctx) {
  static_cast<FakeXvf *>(ctx)->yields++;
}

XvfAecFilterOps ops_for(FakeXvf *x) {
  return XvfAecFilterOps{x, fake_write, fake_read, fake_now, fake_yield};
}

void test_full_read_every_mic() {
  for (int32_t mic = 0; mic < 4; mic++) {
    FakeXvf x;
    XvfAecFilterOps ops = ops_for(&x);
    std::vector<float> out(XVF_AEC_FILTER_MAX_COEFFS, -1.0f);
    XvfAecFilterResult r =
        xvf_aec_filter_read(&ops, 0, mic, out.data(), out.size(), 5000000);
    assert(r.status == XvfAecFilterStatus::OK);
    assert(r.length == 3072 && r.pages == 205 && !r.aborted);
    assert(r.num_mics == 4 && r.num_farends == 1);
    for (int32_t i = 0; i < 3072; i++) {
      assert(out[i] == x.coeff(0, mic, i));
    }
    // Last page is partial (3072 = 204*15 + 12): nothing past length written.
    assert(out[3072] == -1.0f);
    assert(x.writes.front() == XVF_AEC_CMD_FAR_MIC_INDEX);
    assert(x.offsets.size() == 205 && x.offsets[1] == 15 &&
           x.offsets.back() == 3060);
    assert(x.yields == 205);
  }
}

void test_bad_index_issues_no_write() {
  const int32_t bad[][2] = {{0, 4}, {0, -1}, {1, 0}, {-1, 0}};
  for (const auto &fm : bad) {
    FakeXvf x;
    XvfAecFilterOps ops = ops_for(&x);
    std::vector<float> out(XVF_AEC_FILTER_MAX_COEFFS);
    XvfAecFilterResult r = xvf_aec_filter_read(&ops, fm[0], fm[1], out.data(),
                                               out.size(), 5000000);
    assert(r.status == XvfAecFilterStatus::BAD_INDEX);
    assert(x.writes.empty() && !r.aborted);
  }
}

void test_length_over_capacity_refused_before_any_write() {
  FakeXvf x;
  x.length = 5000;
  XvfAecFilterOps ops = ops_for(&x);
  std::vector<float> out(XVF_AEC_FILTER_MAX_COEFFS);
  XvfAecFilterResult r =
      xvf_aec_filter_read(&ops, 0, 0, out.data(), out.size(), 5000000);
  assert(r.status == XvfAecFilterStatus::BAD_LENGTH && r.length == 5000);
  assert(x.writes.empty());

  FakeXvf zero;
  zero.length = 0;
  ops = ops_for(&zero);
  r = xvf_aec_filter_read(&ops, 0, 0, out.data(), out.size(), 5000000);
  assert(r.status == XvfAecFilterStatus::BAD_LENGTH && zero.writes.empty());
}

void test_io_error_mid_sequence_aborts() {
  FakeXvf x;
  x.fail_on_coeff_page = 17;
  XvfAecFilterOps ops = ops_for(&x);
  std::vector<float> out(XVF_AEC_FILTER_MAX_COEFFS);
  XvfAecFilterResult r =
      xvf_aec_filter_read(&ops, 0, 2, out.data(), out.size(), 5000000);
  assert(r.status == XvfAecFilterStatus::IO_ERROR && r.io_error == 0x107);
  assert(r.aborted && r.pages == 17);
  assert(x.writes.back() == XVF_AEC_CMD_FILTER_ABORT);
  assert(x.far == -1);  // device state machine reset
}

void test_budget_exceeded_aborts() {
  FakeXvf x;
  x.us_per_op = 1000;  // 2 ms/page -> 205 pages would take ~410 ms
  XvfAecFilterOps ops = ops_for(&x);
  std::vector<float> out(XVF_AEC_FILTER_MAX_COEFFS);
  XvfAecFilterResult r =
      xvf_aec_filter_read(&ops, 0, 0, out.data(), out.size(), 100000);
  assert(r.status == XvfAecFilterStatus::TIMEOUT);
  assert(r.aborted && r.pages > 0 && r.pages < 205);
  assert(x.writes.back() == XVF_AEC_CMD_FILTER_ABORT);
}

void test_status_names() {
  assert(strcmp(xvf_aec_filter_status_name(XvfAecFilterStatus::OK), "ok") == 0);
  assert(strcmp(xvf_aec_filter_status_name(XvfAecFilterStatus::TIMEOUT),
                "timeout") == 0);
}

}  // namespace

int main() {
  test_full_read_every_mic();
  test_bad_index_issues_no_write();
  test_length_over_capacity_refused_before_any_write();
  test_io_error_mid_sequence_aborts();
  test_budget_exceeded_aborts();
  test_status_names();
  printf("xvf aec filter: PASS\n");
  return 0;
}
