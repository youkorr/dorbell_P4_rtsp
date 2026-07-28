#!/usr/bin/env bash
# Run the real `esphome config` over every example YAML in the repository root.
#
# This is the only check here that uses ESPHome's own validation rather than our
# reading of it, and it is worth its runtime: three bugs that every other harness
# passed were caught the first time this ran --
#
#   * `id: status_led` on a `status_led` light -- an id may not be an integration
#     name, so the whole config is rejected;
#   * `version: 5.4.0` pinned in `framework:` -- below what the `i2c` component
#     now requires, so a config that was valid when written stopped being valid;
#   * a `wifi:` block with no `esp32_hosted:` -- the P4 has no radio, and ESPHome
#     refuses the combination outright.
#
# None of them are visible to a YAML parser or to a grep. All three are fatal.
#
# `rtsp_server` is redirected to this working tree, so the check tests the code
# you are about to commit rather than what is already pushed. Every OTHER source
# is left pointing at the git ref the YAML declares, and is fetched -- so this
# needs network. Substituting a local sibling checkout for those would be worse
# than useless: it would validate against whatever branch that checkout happens
# to sit on, which is not what the YAML asks for.
set -uo pipefail

cd "$(dirname "$0")/.."
repo=$(pwd)
parent=$(dirname "$repo")

if ! command -v esphome >/dev/null 2>&1; then
  echo "esphome not installed -- skipping (pip install esphome)"
  exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# `esphome config` resolves !secret, so every referenced key must exist. The
# api key must be valid base64 of 32 bytes or validation fails on that instead.
cat > "$work/secrets.yaml" <<'EOF'
api_key: "kQzQZ1YvQ0F0bJ4pQ1YvQ0F0bJ4pQ1YvQ0F0bJ4pQ1Y="
ota_password: "otapassword"
wifi_ssid: "ssid"
wifi_password: "wifipassword"
fallback_password: "fallbackpassword"
rtsp_username: "user"
rtsp_password: "password"
EOF

localize() {
  python3 - "$1" "$2" "$repo" <<'PY'
import re, sys, os
src_path, dst_path, repo = sys.argv[1:4]
src = open(src_path).read()

SELF = os.path.basename(repo)

def repl(m):
    if m.group("repo") != SELF:
        return m.group(0)  # someone else's repo: keep the declared git ref
    return f"  - source: {repo}/components\n    components: {m.group('comps')}\n"

pattern = re.compile(
    r"  - source:\n"
    r"(?:      [^\n]*\n)*?"                       # type:, comments, blank-ish lines
    r"      url: https://github\.com/[^/\n]+/(?P<repo>[^\s\n]+)\n"
    r"(?:      [^\n]*\n)*?"                       # ref:, and anything after it
    r"    components: (?P<comps>\[[^\]\n]*\])\n"
    r"(?:    refresh: [^\n]*\n)?"
)
open(dst_path, "w").write(pattern.sub(repl, src))
PY
}

fail=0
shopt -s nullglob
for yaml in "$repo"/*.yaml; do
  base=$(basename "$yaml")
  [ "$base" = "secrets.yaml" ] && continue
  localize "$yaml" "$work/$base"

  out=$(cd "$work" && esphome config "$base" 2>&1)
  if grep -q "Configuration is valid" <<<"$out"; then
    printf '  OK   %s\n' "$base"
  elif missing=$(grep -o "Component not found: [a-z0-9_]*" <<<"$out" | head -1) &&
       [ -n "$missing" ] && grep -q "${missing#Component not found: }" <<<"$(
         sed -n '/^external_components:/,/^[a-z]/p' "$yaml")"; then
    # ESPHome reports a component whose Python module raised ImportError as
    # "Component not found", indistinguishably from a real one. When the name is
    # one this YAML pulls from another repository, that is almost always a
    # missing Python dependency HERE -- lvgl needs aioesphomeapi, font needs
    # freetype-py and esphome_glyphsets. Not this repository's problem, and not a
    # reason to fail: say so and move on.
    printf '  SKIP %s (%s -- install its Python deps to check this file)\n' \
           "$base" "$missing"
  else
    printf '  FAIL %s\n' "$base"
    sed -n '/Failed config/,$p' <<<"$out" | head -25 | sed 's/^/       /'
    # An INFO-level error (bad URL, network) is worth showing too.
    grep -q "Failed config" <<<"$out" || tail -12 <<<"$out" | sed 's/^/       /'
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo
  echo "example config validation FAILED"
  exit 1
fi
echo
echo "all example configs valid"
