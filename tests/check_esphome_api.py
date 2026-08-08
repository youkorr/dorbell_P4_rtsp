"""Check our reading of the ESPHome automation API against the real thing.

`tests/stubs/` is *our reading* of ESPHome's headers, so the C++ lint can only
catch our own inconsistencies -- never a stub that is wrong in the same way the
component is. Two bugs got through exactly that gap and only surfaced on the
user's ESP-IDF build:

  * `void play(Ts... x) override` instead of `const Ts &...x`. It overrides
    nothing, the Action stays abstract, and the error is reported against the
    `new` in the generated main.cpp -- i.e. against a line of YAML.
  * a raw constant passed to a TEMPLATABLE_VALUE setter. Those fields now store
    a bare function pointer (TemplatableFn), so codegen has to wrap constants
    with `cg.templatable(...)`; a raw int fails a static_assert.

Neither is visible without the real headers, so this checks both directly:
the stub against the installed ESPHome, and the codegen against our own header.

    pip install --no-deps esphome
    python3 tests/check_esphome_api.py

Skips cleanly (exit 0) when ESPHome is not installed -- the C++ lint must stay
runnable on a bare machine.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STUB = ROOT / "tests" / "stubs" / "esphome" / "core" / "automation.h"
COMPONENT = ROOT / "components" / "rtsp_server"

failures = []


def check(label, ok, detail=""):
    print(f"{'PASS' if ok else '**FAIL**':10} {label}")
    if not ok:
        if detail:
            print(f"           {detail}")
        failures.append(label)


# ---------------------------------------------------------------------------
# 1. Is our stub still telling the truth about Action::play?
# ---------------------------------------------------------------------------

try:
    import esphome  # noqa: F401

    real = Path(esphome.__file__).parent / "core" / "automation.h"
except ImportError:
    real = None

if real is None or not real.exists():
    print("esphome not installed -- skipping the checks against the real header")
else:
    real_src = real.read_text()
    stub_src = STUB.read_text()

    # The pure virtual every Action subclass must match.
    m = re.search(r"virtual void play\(([^)]*)\)\s*=\s*0;", real_src)
    check("the real header declares Action::play", m is not None)
    if m:
        real_params = " ".join(m.group(1).split())
        stub_m = re.search(r"virtual void play\(([^)]*)\)\s*=\s*0;", stub_src)
        stub_params = " ".join(stub_m.group(1).split()) if stub_m else "<absent>"
        check(
            f"stub Action::play matches upstream ({real_params})",
            stub_params == real_params,
            f"stub says '{stub_params}', ESPHome says '{real_params}'",
        )

    # TemplatableFn is what makes raw constants illegal; if upstream ever goes
    # back to accepting them, the codegen rule below stops being necessary.
    check(
        "upstream still backs TEMPLATABLE_VALUE with TemplatableFn for trivial types",
        "TemplatableStorage" in real_src and "TemplatableFn" in real_src,
        "the storage type changed; re-read the macro before trusting this file",
    )


# ---------------------------------------------------------------------------
# 2. Does our codegen wrap every templatable constant?
# ---------------------------------------------------------------------------

header = (COMPONENT / "automation.h").read_text()
init = (COMPONENT / "__init__.py").read_text()

templatable = set(re.findall(r"TEMPLATABLE_VALUE\(\s*[\w:]+\s*,\s*(\w+)\s*\)", header))
check(f"found the TEMPLATABLE_VALUE fields: {sorted(templatable) or 'none'}", True)

for field in sorted(templatable):
    # Every `var.set_<field>(...)` in the codegen must hand it a cg.templatable
    # result, never a bare Python value.
    for call in re.findall(rf"\.set_{field}\(([^\n]*)\)\)", init):
        check(
            f"codegen wraps '{field}' in cg.templatable()",
            "cg.templatable" in call,
            f"got `.set_{field}({call.strip()})` -- a raw constant fails a "
            f"static_assert in TemplatableFn",
        )

# And the mirror image: a plain (non-templatable) setter must NOT be handed a
# lambda-producing wrapper, which would not convert to its scalar parameter.
plain = set(re.findall(r"void set_(\w+)\((?:uint\d+_t|int|float|bool|size_t) ", header))
for field in sorted(plain - templatable):
    for call in re.findall(rf"\.set_{field}\(([^\n]*)\)\)", init):
        check(
            f"codegen passes '{field}' straight through (plain setter)",
            "cg.templatable" not in call,
            f"got `.set_{field}({call.strip()})` on a non-templatable setter",
        )

print()
if failures:
    print(f"{len(failures)} CHECK(S) FAILED")
    sys.exit(1)
print("esphome API checks passed")
