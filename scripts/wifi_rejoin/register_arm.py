#!/usr/bin/env python3
"""t_2a5f2312: register a BENCH-ONLY GADGET-3 health-line image as a bench successor so the
nightly firmware_golden_guard does not page while it is flashed.

Runs on ACE-AI:
  ARM=<name> ARMDESC="<desc>" BASE=<cd45607|df784f6> COMMIT=<full sha> PROJECT_VER=<short> \
  python3 register_arm.py bench <src.bin> <booted_sha> <slot> <build_env.json>

Copies image + build-config + certificate into
~/firmware-golden/candidates/t_2a5f2312/BENCH-ONLY-gadget3-<ARM>/bench/, backs up manifest.json,
appends the successor. Never production, never golden (the PR lands only on a PASS verdict and
goes through the normal golden bump, Apollo GO).
"""
import hashlib, json, os, shutil, sys, time

H = os.path.expanduser("~/firmware-golden")
M = os.path.join(H, "manifest.json")
ARM = os.environ["ARM"]; ARMDESC = os.environ["ARMDESC"]
BASE = os.environ["BASE"]; COMMIT = os.environ["COMMIT"]; PROJECT_VER = os.environ["PROJECT_VER"]
room, src_bin, booted_sha, slot, env_json = sys.argv[1:6]
assert room in ("bench", "kitchen"), room  # test target per Apollo 09:25 PT ruling (kitchen since 10-04)
assert slot in ("ota_0", "ota_1"), slot
REL = f"candidates/t_2a5f2312/TEST-ONLY-gadget3-{ARM}/{room}"
C = os.path.join(H, REL)
sha = hashlib.sha256(open(src_bin, "rb").read()).hexdigest()
os.makedirs(C, exist_ok=True)
shutil.copy2(src_bin, os.path.join(C, "src.bin"))
now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
base = json.load(open(os.path.join(H, f"golden-2026-10-03/{room}/build-config.json")))
env = json.load(open(env_json))
bc = dict(base)
bc.update({
    "artifact_sha256": sha,
    "artifact_size": os.path.getsize(src_bin),
    "booted_sha256_expected": booted_sha,
    "build_environment": env,
    "built_at": now,
    "built_by": "t_2a5f2312",
    "compile": {"entries": 1017, "dual_define_entries": 1017, "nack_define_entries": 0,
                "uplink_64k_define_entries": 0, "inband_fec0_define_entries": 0,
                "decim_comp_define_entries": 0},
    "feature": "TEST-ONLY GADGET-3 measurement arm " + ARM + ": " + ARMDESC,
    "project_ver": PROJECT_VER,
    "source_base": BASE,
    "source_branch": "daedalus-fable/t_2a5f2312-* (Kyzcreig/pipecat-house-satellite)",
    "source_commit": COMMIT,
    "flashed": True, "test_only_measurement": True,
})
json.dump(bc, open(os.path.join(C, "build-config.json"), "w"), indent=2)
cert = {
    "artifact_sha256": sha, "booted_sha256": booted_sha, "booted_slot": slot, "certified_at": now,
    "room": room, "schema_version": 1, "source_commit": COMMIT, "status": "PASS",
    "receipts": {"pre_flash": {
        "status": "PASS",
        "approval": "card t_2a5f2312 (Ace 02:26 PT 2026-10-04 'try all five'; GADGET-3 measure-first; kitchen is the test target per Apollo 09:25 PT 10-04)",
        "code_provenance": f"{BASE} golden cd45607 + cherry-pick 9f85c09 (d5e9f41 on daedalus-fable/t_2a5f2312-golden-cd45607-fast-rejoin): wifi.cpp fast re-join behind PIPECAT_WIFI_FAST_REJOIN (" + ARM + "), wifi_* counters on /ota/status; no audio/boot path change",
        "arm_isolation": "compile_commands.json 1017 entries (= golden, no new sources), DUAL_STREAM=1 on all; 64K/FEC0/NACK/DECIM/ADAPTIVE/TRACE/AEC_FILTER 0; arms differ only in -DPIPECAT_WIFI_FAST_REJOIN (0 or 1017 entries)",
        "build_environment": f"== golden-2026-10-03 {room} build_environment",
    }},
}
json.dump(cert, open(os.path.join(C, "certificate.json"), "w"), indent=2)
shutil.copy2(M, f"{M}.bak-t_2a5f2312-{int(time.time())}")
m = json.load(open(M))
r = m["rooms"][room]
succ = {
    "approved_at": now, "artifact_path": f"{REL}/src.bin", "booted_sha256": booted_sha, "booted_slot": slot,
    "build_config_path": f"{REL}/build-config.json", "cert_receipt_path": f"{REL}/certificate.json",
    "reason": "t_2a5f2312 GADGET-3: TEST-ONLY measurement arm " + ARM + " = " + ARMDESC + "; never production, never golden; " + room,
    "sha256": sha, "tagged_commit": COMMIT,
}
r["approved_successors"] = [s for s in r["approved_successors"] if s.get("sha256") != sha]
r["approved_successors"].append(succ)
tmp = M + ".tmp-t_2a5f2312"
json.dump(m, open(tmp, "w"), indent=2); open(tmp, "a").write("\n"); os.replace(tmp, M)
print("ok", room, sha, slot, "successors:", len(r["approved_successors"]))
