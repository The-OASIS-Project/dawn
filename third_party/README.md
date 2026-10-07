# Third-party code

Code DAWN carries in the repository but didn't write. Everything here keeps its
own license; each library's notice is in its files or in a `LICENSE` beside them.
Libraries DAWN links against but doesn't carry (libcurl, SQLite, libsodium, ...)
are listed in [DEPENDENCIES.md](../DEPENDENCIES.md).

`format_code.sh` skips this directory, so the files stay as upstream wrote them
(apart from the changes listed below). Each library's directory is on the include
path, so code includes them by bare file name (`#include "toml.h"`).

| Directory | Library | Version | License | Upstream | Changed for DAWN |
|---|---|---|---|---|---|
| `nlohmann/` | nlohmann/json (`json.hpp`) | 3.11.2 | MIT | <https://github.com/nlohmann/json> | No |
| `tomlc99/` | tomlc99 (`toml.c`, `toml.h`) | not recorded | MIT | <https://github.com/cktan/tomlc99> | Include path only. Used by the daemon and the satellite. |
| `tinyexpr/` | tinyexpr (`tinyexpr.c`, `tinyexpr.h`) | not recorded (2020-era) | Zlib | <https://github.com/codeplea/tinyexpr> | `fac()` accumulates in `double`, so factorials past 20! don't overflow; marked in the source as the Zlib license requires. Upstream has since added a recursion-depth limit (`TE_MAX_DEPTH`); DAWN's calculator caps input length before calling it. |
| `utfcpp/` | utf8-cpp (`utf8.h`, `utf8/`) | not recorded | Boost Software License 1.0 | <https://github.com/nemtrif/utfcpp> | No. Used by Piper. |
| `piper/` | Piper TTS (`piper.cpp`, `piper.hpp`, `wavfile.hpp`) | not recorded | MIT (`piper/LICENSE`) | <https://github.com/rhasspy/piper> (`src/cpp/`) | Speech can be interrupted between sentences; 4 ONNX intra-op threads; result timing fields zeroed. Listed in the header of `piper.cpp`. |
| `vosk/` | Vosk API header (`vosk_api.h`) | not recorded | Apache-2.0 | <https://github.com/alphacep/vosk-api> | No. Only for builds with Vosk (`-DENABLE_VOSK=ON`). |
| `unity/` | Unity test framework | 2.6.3 | MIT | <https://github.com/ThrowTheSwitch/Unity> | No. Tests only. |

## Elsewhere in the repository

Two kinds of third-party code live outside this directory, because their build or
serving needs them where they are:

| Path | Library | Version | License |
|---|---|---|---|
| `www/js/vendor/marked.min.js` | marked | 15.0.12 | MIT |
| `www/js/vendor/purify.min.js` | DOMPurify | 3.3.1 | Apache-2.0 / MPL-2.0 |
| `www/js/vendor/chart.umd.js` | Chart.js | 4.4.1 | MIT |
| `www/js/vendor/qrcode-generator-v2.0.4.js` | qrcode-generator | 2.0.4 | MIT |
| `dawn_satellite_arduino/tweetnacl.{c,h}` | TweetNaCl (the Arduino sketch must hold its sources) | upstream release | Public domain |

The `whisper.cpp` and `webrtc-audio-processing` git submodules are separate
repositories with their own licenses.

## Updating a library

Replace the files with the upstream release, reapply the changes in the table
above, record the version here, then build and run the tests (`make -C build-debug
tests-ci && ctest --test-dir build-debug -L ci`). For Piper, also build the
satellite, which compiles it through `common/`.
