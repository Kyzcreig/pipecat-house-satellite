#!/usr/bin/env python3
"""Source contract for /xvf/route (t_3565803e): default off, every piece gated, no NVS.

The calibration routing must never reach the fleet image unless PIPECAT_XVF_ROUTE_CAL=1 is in the
build environment, must never persist, and must restore on the TTL, on request and on failure.
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "xiao-esp32-s3" / "src"
fails = []


def need(cond, msg):
    if not cond:
        fails.append(msg)


cfg = (SRC / "pipecat_build_config.h.in").read_text()
need(re.search(r"#define PIPECAT_XVF_ROUTE_CAL 0\b", cfg), "PIPECAT_XVF_ROUTE_CAL must default to 0")

top = (SRC.parent / "CMakeLists.txt").read_text()
need("PIPECAT_XVF_ROUTE_CAL)" in top, "CMake flag loop must carry PIPECAT_XVF_ROUTE_CAL")


def gated_blocks(text):
    # the long blocks end with a labelled #endif (they nest other #if blocks); the short ones do not
    long = re.findall(r"#if PIPECAT_XVF_ROUTE_CAL\n.*?#endif  // PIPECAT_XVF_ROUTE_CAL", text, re.S)
    for b in long:
        text = text.replace(b, "")
    return long + re.findall(r"#if PIPECAT_XVF_ROUTE_CAL\n.*?#endif", text, re.S)


media = (SRC / "media.cpp").read_text()
ota = (SRC / "ota.cpp").read_text()
for name, text in (("media.cpp", media), ("ota.cpp", ota)):
    ungated = text
    for b in gated_blocks(text):
        ungated = ungated.replace(b, "")
    for sym in ("pipecat_xvf_route_set", "pipecat_xvf_route_restore", "xvf_route_post_handler",
                "\"/xvf/route\"", "s_route_timer"):
        need(sym not in ungated, f"{name}: {sym} outside #if PIPECAT_XVF_ROUTE_CAL")

route_impl = media[media.index("#if PIPECAT_XVF_ROUTE_CAL\n// Temporary output routing"):]
route_impl = route_impl[:route_impl.index("#endif  // PIPECAT_XVF_ROUTE_CAL")]
need("nvs_" not in route_impl, "route code must not touch NVS")
need("esp_timer_start_once" in route_impl, "a set must arm the TTL timer")
need('route_restore_locked("ttl")' in route_impl, "the TTL must restore")
need('route_restore_locked("set_failed")' in route_impl, "a failed set must restore")
need("esp_restart()" in route_impl, "an unrestorable route must reboot into the boot profile")
need("xvf_route_cal_valid(l_cat, l_src)" in route_impl and "xvf_route_cal_valid(r_cat, r_src)" in route_impl,
     "both slots must pass the calibration-category policy")
need("config.max_uri_handlers += 2;  // GET/POST /xvf/route" in ota, "httpd needs 2 slots for /xvf/route")

if fails:
    print("\n".join("FAIL " + f for f in fails))
    sys.exit(1)
print("xvf_route_cal source contract: PASS")
