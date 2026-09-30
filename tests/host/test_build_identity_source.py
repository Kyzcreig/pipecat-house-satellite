#!/usr/bin/env python3
"""Source contract: firmware build identity on GET /xvf/params (t_9d8fad45).

"Which firmware is live" must be a device fact: the full source commit, the
dirty flag, compile time and the boot-guard/network-watchdog state, read-only.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / "xiao-esp32-s3"
CMAKE = (FW / "CMakeLists.txt").read_text()
TEMPLATE = (FW / "src" / "pipecat_build_config.h.in").read_text()
OTA = (FW / "src" / "ota.cpp").read_text()
MAIN = (FW / "src" / "main.cpp").read_text()

# CMake bakes the full sha (worktree-safe git -C), with an env override and an
# explicit "unknown" fallback instead of a guess.
assert re.search(r'git -C "\$\{CMAKE_SOURCE_DIR\}" rev-parse HEAD', CMAKE)
assert "$ENV{PIPECAT_BUILD_GIT_SHA}" in CMAKE
assert 'set(PIPECAT_BUILD_GIT_SHA_VALUE "unknown")' in CMAKE
assert "--untracked-files=no" in CMAKE, "dirty flag must ignore build outputs"
assert '#define PIPECAT_BUILD_GIT_SHA "@PIPECAT_BUILD_GIT_SHA_VALUE@"' in TEMPLATE
assert "#define PIPECAT_BUILD_GIT_DIRTY @PIPECAT_BUILD_GIT_DIRTY_VALUE@" in TEMPLATE

start = OTA.index("static esp_err_t xvf_params_handler")
end = OTA.index("// GET /xvf/read?param=", start)
handler = OTA[start:end]

for key in ("build", "git_sha", "dirty", "version", "built", "idf",
            "boot_guard", "reset_reason", "fault_boots", "boots_since_poweron",
            "netwdt_restarts", "net_watchdog_s", "uptime_s"):
    assert f'\\"{key}\\":' in handler, f"/xvf/params lost {key}"
for arg in ("PIPECAT_BUILD_GIT_SHA", "PIPECAT_BUILD_GIT_DIRTY", "app->date",
            "app->time", "pipecat_netwdt_restarts()",
            "pipecat_boots_since_poweron()", "pipecat_boot_fault_count()"):
    assert arg in handler, f"/xvf/params no longer reports {arg}"

# Response buffer lives on the heap (4 KB httpd stack) and every exit frees it.
assert re.search(r"char body\[\d+\]", handler) is None
assert "calloc(1, kParamsBodyCapacity)" in handler
after_alloc = handler[handler.index("calloc(1, kParamsBodyCapacity)"):]
after_alloc = after_alloc[after_alloc.index("}") + 1:]  # past the alloc-fail branch
returns = after_alloc.count("return ")
frees = after_alloc.count("free(body);")
assert returns == frees, f"{returns} returns but {frees} free(body) after alloc"
# The last write is bounds-checked like the per-param writes.
assert "static_cast<size_t>(tail) >= kParamsBodyCapacity - used" in handler

assert "return s_boot_guard.netwdt_restarts;" in MAIN
print("build identity source contract: OK")
