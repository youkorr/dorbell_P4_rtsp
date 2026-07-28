# Tests

Six harnesses, all runnable on a plain Linux host without ESP-IDF.

| | What it proves |
|---|---|
| `lint_cpp.sh` | the C++ is internally consistent, compiles, and the actions instantiate |
| `check_example_configs.sh` | ESPHome itself accepts every example YAML in the repository root |
| `check_esphome_api.py` | our stubs and codegen still match the installed ESPHome |
| `test_config_schema.py` | the ESPHome schema and codegen behave |
| `test_rtp.cpp` | G.711 packetizer and companding |
| `test_mjpeg.cpp` | RFC 2435 against real JPEGs, reassembled byte-exact |

```bash
./tests/lint_cpp.sh

pip install --no-deps esphome voluptuous
python3 tests/test_config_schema.py

python3 tests/check_esphome_api.py

pip install esphome            # the real thing, not --no-deps
./tests/check_example_configs.sh

g++ -std=gnu++17 -fsanitize=address,undefined -I components/rtsp_server \
    -o /tmp/test_rtp tests/test_rtp.cpp components/rtsp_server/rtp.cpp && /tmp/test_rtp

pip install Pillow
python3 tests/make_jpeg_fixtures.py /tmp/fx
g++ -std=gnu++17 -fsanitize=address,undefined -I components/rtsp_server \
    -o /tmp/test_mjpeg tests/test_mjpeg.cpp components/rtsp_server/rtp.cpp && /tmp/test_mjpeg /tmp/fx
```

## Why `check_example_configs.sh` exists

`lint_cpp.sh` checks the example YAML the only ways a shell script can: it parses
it, resolves every `id(...).method()` in a lambda against the real headers, and
looks for a GPIO claimed twice. That is worth having, but it is our reading of
ESPHome's rules, and the first run of the real validator found three fatal bugs
it could not see:

  * `id: status_led` on a `status_led` light. An id may not be an integration
    name; ESPHome rejects the entire config.
  * `version: 5.4.0` pinned under `framework:`, below what the `i2c` component
    now requires. It was valid when written — a pinned version rots on its own.
  * a `wifi:` block with no `esp32_hosted:`. The P4 has no radio; ESPHome refuses
    the combination.

The script substitutes this working tree for the `dorbell_P4_rtsp` external
component, so it validates what you are about to commit. Every other source keeps
the git ref its YAML declares and is fetched, so **it needs network**.

A file is reported `SKIP` when a component from another repository fails to
import — ESPHome surfaces a swallowed `ImportError` as "Component not found",
so an absent `aioesphomeapi` or `freetype-py` is indistinguishable from a broken
config. Install those to actually check `doorbell-lvgl.yaml`.

## What none of them prove

`tests/stubs/` are *our reading* of the ESP-IDF and ESPHome headers, not the
real ones. So the lint catches our own mistakes — typos, wrong member names,
signature drift between our files, 32-bit template deduction — but it cannot
tell us that `jpeg_encoder_process()` really takes those arguments, or that a
given ESPHome helper still exists. Only a real build does that.

Keep the stubs minimal and honest: a stub that is more permissive than reality
turns a green lint into a false sense of safety. That is not hypothetical — two
bugs shipped through exactly that gap, and both are now covered:

  * `void play(Ts... x) override` where upstream declares `play(const Ts &...x)`.
    It overrides nothing, the Action stays abstract, and the error surfaces on
    the `new` in the generated main.cpp — pointing at a line of YAML. The stub
    had copied the wrong signature, so the lint agreed with the mistake.
    Now: the stub carries the real signature, `check_esphome_api.py` re-reads it
    from the installed ESPHome so it cannot drift again, and
    `test_automation.cpp` instantiates the actions — templates only get their
    `override` checked when something instantiates them.

  * a raw constant handed to a TEMPLATABLE_VALUE setter. Those fields store a
    bare function pointer now, so codegen must wrap constants with
    `cg.templatable(...)`. This one is invisible to any C++ check: it is a
    Python codegen bug, and `check_esphome_api.py` reads the setters out of
    `automation.h` and checks how `__init__.py` calls each one.
