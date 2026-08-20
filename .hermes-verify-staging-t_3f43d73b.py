#!/usr/bin/env python3
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path("/private/tmp/pipecat-satellite-t_3f43d73b-recovered")
COMMIT = "8e57d8fb4d88b2acd5b4bc2998456efb2e745972"
FILES = [
    "xiao-esp32-s3/src/main.cpp",
    "xiao-esp32-s3/src/media.cpp",
    "xiao-esp32-s3/src/ota.cpp",
    "tests/host/test_xvf_nvs_source.py",
]


def run(argv, cwd=ROOT):
    result = subprocess.run(argv, cwd=cwd, text=True, capture_output=True)
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {' '.join(argv)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout.strip()


assert ROOT.is_dir(), f"missing recovery worktree: {ROOT}"
assert run(["git", "rev-parse", "HEAD"]) == COMMIT
assert run(["git", "status", "--porcelain"]) == ""
run(["git", "diff", "--check", f"{COMMIT}^", COMMIT])
assert run([sys.executable, "tests/host/test_xvf_nvs_source.py"]).endswith("PASS")
assert run([sys.executable, "tests/host/test_rtvi_heartbeat_source.py"]).endswith("PASS")

mutations = {
    "remove_applied_persist_gate": (
        "xiao-esp32-s3/src/ota.cpp",
        "if (ret == ESP_OK && result.applied && persist &&",
        "if (ret == ESP_OK && persist &&",
    ),
    "make_agc_desired_nonpersistent": (
        "xiao-esp32-s3/src/media.cpp",
        "TuneTarget::XVF_FLOAT, true, true, 1.0e-8f, 1.0f,",
        "TuneTarget::XVF_FLOAT, false, true, 1.0e-8f, 1.0f,",
    ),
    "remove_save_commit": (
        "xiao-esp32-s3/src/ota.cpp",
        "ret = nvs_set_blob(nvs, param, &value, sizeof(value));\n  if (ret == ESP_OK) ret = nvs_commit(nvs);",
        "ret = nvs_set_blob(nvs, param, &value, sizeof(value));\n  if (ret == ESP_OK) ret = ESP_OK;",
    ),
    "underallocate_uri_handlers": (
        "xiao-esp32-s3/src/ota.cpp",
        "config.max_uri_handlers = 7;",
        "config.max_uri_handlers = 6;",
    ),
}

for name, (relative, old, new) in mutations.items():
    with tempfile.TemporaryDirectory(prefix="hermes-verify-xvf-mutation-") as temp:
        probe = Path(temp)
        for rel in FILES:
            dest = probe / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / rel, dest)
        target = probe / relative
        text = target.read_text()
        assert old in text, f"{name}: mutation anchor missing"
        target.write_text(text.replace(old, new, 1))
        result = subprocess.run(
            [sys.executable, str(probe / "tests/host/test_xvf_nvs_source.py")],
            cwd=probe,
            text=True,
            capture_output=True,
        )
        assert result.returncode != 0, f"{name}: source contract stayed green"
        print(f"{name}: RED")

print("AD_HOC_VERIFY_PASS: committed source contracts pass; 4/4 load-bearing mutations fail")
