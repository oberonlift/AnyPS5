# Next steps

## 1. Libraries that need AnyPS5's own CMake build

`tools/survey_libraries.py` compiles sources without AnyPS5's CMake, so these fail for lack of generated or configured headers (see [known gaps](DEBT.md)): libSceVideoOut's `Telemetry.cpp` (`TraceRevision.hpp`), libSceSsl (mbedtls configuration), libSceVorbisDec (ogg `config_types.h`), libSceUsbd (libusb) and libSceVideodec2 (FFmpeg). Build them, and libSceAgcDriver with its shader recompiler, through AnyPS5's CMake with `ANYPS5_GUEST_LIFTED` for aarch64.

## 2. Run relinked PS5 executables

- Seed discovery from `DT_INIT_ARRAY` and `DT_FINI_ARRAY`.
- Handle TLS relocations beyond static TLS.
- Lift the bundled `sce_module` files the relinker converts, and link the bridges for every library a title imports.
- Bridge the exports still left out: records passed or returned by value, complex values, function-pointer returns and C++-linkage exports.

## 3. Breakout demo, then one title

Neither is in hand yet: the demo would be homebrew built with an open PS5 SDK, which the repo can carry, and a title is a dump that cannot be. Then performance work, as the [roadmap](DESIGN.md#roadmap) describes.
