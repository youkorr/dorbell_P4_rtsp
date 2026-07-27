# Tests

Three harnesses, all runnable on a plain Linux host without ESP-IDF.

| | What it proves |
|---|---|
| `lint_cpp.sh` | the C++ is internally consistent and compiles |
| `test_config_schema.py` | the ESPHome schema and codegen behave |
| `test_rtp.cpp` | the RTP packetizers produce correct bytes |

```bash
./tests/lint_cpp.sh

pip install --no-deps esphome voluptuous
python3 tests/test_config_schema.py

g++ -std=gnu++17 -fsanitize=address,undefined -I components/rtsp_server \
    -o /tmp/test_rtp tests/test_rtp.cpp components/rtsp_server/rtp.cpp && /tmp/test_rtp
```

## What none of them prove

`tests/stubs/` are *our reading* of the ESP-IDF and ESPHome headers, not the
real ones. So the lint catches our own mistakes — typos, wrong member names,
signature drift between our files, 32-bit template deduction — but it cannot
tell us that `jpeg_encoder_process()` really takes those arguments, or that a
given ESPHome helper still exists. Only a real build does that.

Keep the stubs minimal and honest: a stub that is more permissive than reality
turns a green lint into a false sense of safety.
