# Tests

Five harnesses, all runnable on a plain Linux host without ESP-IDF.

| | What it proves |
|---|---|
| `lint_cpp.sh` | the C++ is internally consistent, compiles, and the actions instantiate |
| `check_esphome_api.py` | our stubs and codegen still match the installed ESPHome |
| `test_config_schema.py` | the ESPHome schema and codegen behave |
| `test_rtp.cpp` | G.711 packetizer and companding |
| `test_mjpeg.cpp` | RFC 2435 against real JPEGs, reassembled byte-exact |

```bash
./tests/lint_cpp.sh

pip install --no-deps esphome voluptuous
python3 tests/test_config_schema.py

python3 tests/check_esphome_api.py

g++ -std=gnu++17 -fsanitize=address,undefined -I components/rtsp_server \
    -o /tmp/test_rtp tests/test_rtp.cpp components/rtsp_server/rtp.cpp && /tmp/test_rtp

pip install Pillow
python3 tests/make_jpeg_fixtures.py /tmp/fx
g++ -std=gnu++17 -fsanitize=address,undefined -I components/rtsp_server \
    -o /tmp/test_mjpeg tests/test_mjpeg.cpp components/rtsp_server/rtp.cpp && /tmp/test_mjpeg /tmp/fx
```

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
