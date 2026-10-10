#!/usr/bin/env python3
"""Source contract: the satellite image is reproducible (t_1d4f0294, Fleet v2 IMAGE-SHA).

Same commit + same build env must give the same src.bin sha256, so the manifest can pin
firmware.image_sha256 and parity can grade bytes instead of the version string. The CI job
`reproducible` (build.yaml) proves it by building twice; this contract keeps the inputs that
make it true from being dropped silently.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / "xiao-esp32-s3"
DEFAULTS = (FW / "sdkconfig.defaults").read_text()
CMAKE = (FW / "CMakeLists.txt").read_text()
WORKFLOW = (ROOT / ".github" / "workflows" / "build.yaml").read_text()
CHECK = (ROOT / "scripts" / "repro_build_check.sh").read_text()

# IDF drops date/time from esp_app_desc and prefix-maps IDF/project/component paths.
assert re.search(r"^CONFIG_APP_REPRODUCIBLE_BUILD=y$", DEFAULTS, re.M), "reproducible build turned off"

# Our own sources never stamp the build time (the image would differ every build).
for path in sorted((FW / "src").glob("*.[ch]*")) + sorted((FW / "components").rglob("*.[ch]")):
    text = path.read_text(errors="replace")
    for macro in ("__DATE__", "__TIME__", "__TIMESTAMP__"):
        assert macro not in text, f"{path.relative_to(ROOT)} uses {macro}"

# PROJECT_VER is the build sha, not `git describe` (tag-dependent: same commit, three strings).
assert re.search(r'string\(SUBSTRING "\$\{PIPECAT_BUILD_GIT_SHA_VALUE\}" 0 7 PROJECT_VER\)', CMAKE)
assert CMAKE.index("PROJECT_VER)") < CMAKE.index("project(src)"), "PROJECT_VER must be set before project()"
# Components outside the project dir (../esp32-s3-box-3: srtp, libopus, libpeer) get mapped too.
assert "-fmacro-prefix-map=${PIPECAT_REPO_ROOT}=" in CMAKE
assert "-fdebug-prefix-map=${PIPECAT_REPO_ROOT}=" in CMAKE

# CI builds twice from a digest-pinned toolchain and diffs the bytes.
assert "repro_build_check.sh" in WORKFLOW
assert re.search(r"espressif/idf:v5\.5\.4@sha256:[0-9a-f]{64}", CHECK), "toolchain image not digest-pinned"
assert 'if [ "$sha_a" = "$sha_b" ]' in CHECK
print("reproducible build source contract: OK")
