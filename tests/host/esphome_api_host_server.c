/* Loopback ESPHome native API server for the interop test (t_6215e36e).
 *
 * Runs the firmware's protocol code (esphome_api.c + ble_hci.c) behind a
 * POSIX socket so the real Home Assistant client library can talk to it:
 *   esphome_api_host_server            -> prints "PORT <n>", serves 1 client
 * Once the client subscribes to raw advertisements, every 100 ms it parses a
 * canned HCI LE Advertising Report (the bytes a controller would hand VHCI)
 * and sends the batch, exactly like ble_proxy.cpp does on the device.
 * Exits 0 when the client sends DisconnectRequest, 2 on a protocol error, 3
 * on timeout.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ble_hci.h"
#include "esphome_api.h"

static const EspApiDeviceInfo kInfo = {
    .name = "xvf3800-test",
    .friendly_name = "XVF3800 test",
    .mac_address = "dc:b4:d9:38:a6:cc",
    .bt_mac_address = "dc:b4:d9:38:a6:ce",
    .model = "xiao-esp32-s3 XVF3800",
    .manufacturer = "Espressif",
    .project_name = "kyzcreig.pipecat-house-satellite",
    .project_version = "test",
    .esphome_version = "pipecat-test",
    .compilation_time = "",
    .server_info = "pipecat-house-satellite ble_proxy",
};

/* Two reports in one LE Advertising Report event:
 *  C0:11:22:33:44:55 random, ADV_NONCONN_IND, flags + iBeacon-ish mfr data,
 *  rssi -67; 00:00:00:00:00:01 public, empty, rssi -90. */
static const unsigned char kHciEvent[] = {
    0x04, 0x3E, 0x22, 0x02, 0x02,
    /* report 1 */
    0x03, 0x01, 0x55, 0x44, 0x33, 0x22, 0x11, 0xC0, 0x0C, 0x02, 0x01, 0x06,
    0x08, 0xFF, 0x4C, 0x00, 0x02, 0x15, 0xAA, 0xBB, 0xCC, 0xBD,
    /* report 2 */
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA6};

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int send_all(int fd, const unsigned char *p, size_t n) {
  while (n) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0)
      return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 1))
    return 4;
  socklen_t al = sizeof(a);
  getsockname(ls, (struct sockaddr *)&a, &al);
  printf("PORT %u\n", ntohs(a.sin_port));

  int c = accept(ls, NULL, NULL);
  if (c < 0)
    return 4;
  EspApiSession s;
  espapi_session_reset(&s);
  unsigned char out[ESPAPI_TX_MAX * 2];
  unsigned batches = 0;
  const double deadline = now_s() + 60;
  double next_adv = 0;
  for (;;) {
    if (now_s() > deadline)
      return 3;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(c, &rf);
    struct timeval tv = {0, 20000};
    if (select(c + 1, &rf, NULL, NULL, &tv) > 0) {
      unsigned char in[256];
      ssize_t r = recv(c, in, sizeof(in), 0);
      if (r <= 0) {
        fprintf(stderr, "client closed without DisconnectRequest\n");
        return 2;
      }
      size_t out_len = 0;
      int rc = espapi_session_feed(&s, &kInfo, in, (size_t)r, out, sizeof(out),
                                   &out_len);
      if (rc == ESPAPI_ERROR) {
        fprintf(stderr, "protocol error\n");
        return 2;
      }
      if (out_len && send_all(c, out, out_len))
        return 2;
      if (rc == ESPAPI_CLOSE) {
        printf("CLOSED frames_in=%u pings_in=%u unknown_in=%u batches=%u\n",
               s.frames_in, s.pings_in, s.unknown_in, batches);
        close(c);
        return 0;
      }
    }
    if (s.subscribed && now_s() >= next_adv) {
      next_adv = now_s() + 0.1;
      BleAdvReport rep[ESPAPI_ADV_BATCH_MAX];
      int n = ble_hci_parse_adv_report(kHciEvent, sizeof(kHciEvent), rep,
                                       ESPAPI_ADV_BATCH_MAX);
      if (n != 2) {
        fprintf(stderr, "fixture parse failed: %d\n", n);
        return 2;
      }
      size_t len = espapi_encode_raw_adverts(rep, (size_t)n, out, sizeof(out));
      if (!len || send_all(c, out, len))
        return 2;
      batches++;
    }
  }
}
