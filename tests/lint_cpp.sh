#!/usr/bin/env bash
#
# Host syntax-check of the component against stub headers.
#
# What this DOES catch: typos, wrong member names, signature mismatches between
# our own files, headers that do not stand on their own, and the 32-bit
# portability traps below.
#
# What it does NOT catch: whether our reading of the ESP-IDF and ESPHome APIs is
# correct — the stubs in tests/stubs/ are that reading, not the real headers.
# Only a real build proves those.
#
#   ./tests/lint_cpp.sh
#
set -uo pipefail
cd "$(dirname "$0")/.."
SRC=components/rtsp_server
STUB=tests/stubs

# The V4L2 header comes from the esp_video component; use the real one when it
# is checked out next to us, otherwise skip the files that need it.
V4L2=""
for candidate in ../esphome_esp-video/components/esp_video/include; do
  [ -f "$candidate/linux/videodev2.h" ] && V4L2="-I$candidate"
done

fail=0

echo "== 32-bit portability =="
# int32_t is `long int` on riscv32-esp-elf, so a bare literal and an int32_t
# deduce to different template parameters and std::min/std::max will not compile.
if grep -nE 'std::(min|max)\([^)]*(-?[0-9]+)' "$SRC"/*.cpp "$SRC"/*.h 2>/dev/null | grep -vE 'std::(min|max)<'; then
  echo "  FAILED: std::min/std::max with a bare literal; use an explicit"
  echo "          template argument or a hand-written clamp."
  fail=1
else
  echo "  OK   no std::min/std::max mixing literals with fixed-width types"
fi

echo
echo "== YAML lambdas =="
# Lambdas in the example configs are compiled into main.cpp, so they can break
# the build just like the component does — but no compiler here ever sees them.
# Guard the ESPHome helpers that have been renamed or removed under us.
removed='network::get_use_address|network::get_ip_address\(|App\.get_compilation_time'
if grep -nE "$removed" ./*.yaml 2>/dev/null; then
  echo "  FAILED: an example lambda calls an ESPHome helper that no longer exists."
  echo "          Prefer a method on the component (e.g. stream_url())."
  fail=1
else
  echo "  OK   no example lambda calls a removed ESPHome helper"
fi

echo
echo "== YAML pins =="
# A GPIO claimed twice in one config is accepted by every check above and only
# shows up as unexplained hardware misbehaviour on the bench: the chime relay
# and the display reset once shared GPIO33 in doorbell-lvgl.yaml.
if python3 - <<'PY'
import collections, glob, re, sys

bad = False
for path in sorted(glob.glob("*.yaml")):
    use = collections.defaultdict(list)
    for n, line in enumerate(open(path), 1):
        if line.lstrip().startswith("#"):
            continue
        for m in re.finditer(r"(\w*_pin|pin|number)\s*:\s*(?:GPIO)?(\d+)\b", line):
            use[int(m.group(2))].append((n, line.strip()))
    for pin, sites in sorted(use.items()):
        if len(sites) > 1:
            bad = True
            print(f"  FAILED: {path} uses GPIO{pin} {len(sites)} times")
            for n, text in sites:
                print(f"            L{n}: {text}")
    if not bad:
        print(f"  OK   {path}: no GPIO claimed twice")
sys.exit(1 if bad else 0)
PY
then :; else fail=1; fi

echo
echo "== compile =="
for f in "$SRC"/*.cpp; do
  name=$(basename "$f")
  [ -z "$V4L2" ] && case "$name" in video_pipeline.cpp|rtsp_server.cpp)
    printf '  %-22s SKIPPED (needs esp_video for linux/videodev2.h)\n' "$name"; continue;; esac
  printf '  %-22s ' "$name"
  if out=$(g++ -std=gnu++17 -fsyntax-only -Wall -Wextra \
        -Wno-unused-parameter -Wno-missing-field-initializers \
        -I"$STUB" $V4L2 -I"$SRC" "$f" 2>&1); then
    echo "OK"
  else
    echo "FAILED"; echo "$out" | head -30; fail=1
  fi
done

echo
echo "== headers stand alone =="
for h in "$SRC"/*.h; do
  name=$(basename "$h")
  [ -z "$V4L2" ] && case "$name" in video_pipeline.h|rtsp_server.h)
    printf '  %-22s SKIPPED\n' "$name"; continue;; esac
  printf '  %-22s ' "$name"
  if out=$(g++ -std=gnu++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter \
        -I"$STUB" $V4L2 -I"$SRC" -x c++ "$h" 2>&1); then
    echo "OK"
  else
    echo "FAILED"; echo "$out" | head -20; fail=1
  fi
done

echo
[ $fail -eq 0 ] && echo "lint passed" || echo "LINT FAILED"
exit $fail
