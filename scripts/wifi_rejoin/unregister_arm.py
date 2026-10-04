#!/usr/bin/env python3
"""t_2a5f2312: remove a TEST-ONLY GADGET-3 successor (by sha256) from ~/firmware-golden/manifest.json
once the room is back on golden. Runs on ACE-AI: python3 unregister_arm.py <room> <sha256>.
Backs up the manifest first. Candidate files under candidates/t_2a5f2312/ are left as evidence.
"""
import json, os, shutil, sys, time

H = os.path.expanduser("~/firmware-golden")
M = os.path.join(H, "manifest.json")
room, sha = sys.argv[1:3]
assert room in ("bench", "kitchen"), room
assert len(sha) == 64, sha
shutil.copy2(M, f"{M}.bak-t_2a5f2312-unregister-{int(time.time())}")
m = json.load(open(M))
r = m["rooms"][room]
before = len(r["approved_successors"])
r["approved_successors"] = [s for s in r["approved_successors"] if s.get("sha256") != sha]
after = len(r["approved_successors"])
tmp = M + ".tmp-t_2a5f2312"
json.dump(m, open(tmp, "w"), indent=2); open(tmp, "a").write("\n"); os.replace(tmp, M)
print("unregistered", room, sha[:16], "successors:", before, "->", after)
