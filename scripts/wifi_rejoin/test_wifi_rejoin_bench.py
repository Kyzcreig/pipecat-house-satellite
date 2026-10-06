"""Unit tests for the pure verdict logic in wifi_rejoin_bench.py (no network)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import wifi_rejoin_bench as b  # noqa: E402

NEAR = "0c:ea:14:8e:eb:b3"
FAR = "0c:ea:14:8e:e5:9b"


def _row(i, ping, dev=None, ap=NEAR, rssi=-55, path=None, rebooted=False, peer=True):
    return {
        "trial": i, "ping_gap_ms": ping, "dev_rejoin_ms": dev, "path": path,
        "rebooted": rebooted, "peer_survived": peer,
        "post_sta": {"ap_mac": ap, "signal": rssi},
    }


def test_pass_on_device_clock_when_every_trial_has_it():
    rows = [_row(i, 1500 + i * 10, dev=900 + i * 5, path="fast") for i in range(20)]
    s = b.summarize(rows, NEAR)
    assert s["basis"] == "device"
    assert s["median_ms"] < 2000 and s["p95_ms"] < 5000
    assert s["near_ap_hits"] == 20 and s["path_fast"] == 20
    assert s["pass"] is True


def test_falls_back_to_ping_clock_when_device_counters_missing():
    rows = [_row(i, 3200 + i * 50) for i in range(20)]
    s = b.summarize(rows, NEAR)
    assert s["basis"] == "ping"
    assert s["median_ms"] > 2000
    assert s["pass_median"] is False and s["pass"] is False


def test_one_far_ap_join_fails_the_near_ap_leg():
    rows = [_row(i, 1200, dev=800) for i in range(19)] + [_row(19, 1200, dev=800, ap=FAR)]
    s = b.summarize(rows, NEAR)
    assert s["near_ap_hits"] == 19
    assert s["pass_near_ap"] is False and s["pass"] is False
    assert s["ap_counts"] == {NEAR: 19, FAR: 1}


def test_p95_outliers_fail_even_with_good_median():
    # 1 outlier in 20 is the 100th percentile (interpolated p95 stays ~1.3 s);
    # 2 of 20 put sorted[18] at the outlier and p95 fails.
    rows = [_row(i, 1000, dev=900) for i in range(18)] + \
           [_row(18, 9000, dev=8800), _row(19, 9000, dev=8800)]
    s = b.summarize(rows, NEAR)
    assert s["pass_median"] is True
    assert s["pass_p95"] is False and s["pass"] is False


def test_timeout_trial_fails_run():
    rows = [_row(i, 1000, dev=900) for i in range(19)] + [_row(19, None, dev=None)]
    s = b.summarize(rows, NEAR)
    assert s["timeouts"] == 1 and s["basis"] == "ping" and s["pass"] is False


def test_near_ap_defaults_to_best_mean_rssi():
    rows = [_row(i, 1000, ap=NEAR, rssi=-50) for i in range(10)] + \
           [_row(i, 1000, ap=FAR, rssi=-70) for i in range(10, 20)]
    s = b.summarize(rows, None)
    assert s["near_ap"] == NEAR and s["near_ap_hits"] == 10
    assert s["rssi_mean_by_ap"] == {NEAR: -50.0, FAR: -70.0}


def test_percentile_edges():
    assert b.percentile([], 0.95) is None
    assert b.percentile([5.0], 0.95) == 5.0
    assert b.percentile([1.0, 2.0, 3.0, 4.0], 0.5) == 2.5


def test_table_row_renders():
    rows = [_row(i, 1500, dev=900, path="fast") for i in range(20)]
    line = b.fmt_table("fast-on", b.summarize(rows, NEAR))
    assert line.startswith("| fast-on | 20 | 900 | 900 |") and line.endswith("| PASS |")
