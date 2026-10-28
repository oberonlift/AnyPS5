# Next steps

## 1. Finish moving AnyPS5 onto upstream's current main

`aarch64-host` on [oberonlift/AnyPS5](https://github.com/oberonlift/AnyPS5) is the tested branch the submodule points at. `aarch64-host-rebase` holds the same commits rebased onto the fork's `main`, 987 upstream commits newer, and has not been built or tested.

1. Check out `aarch64-host-rebase` in `3rdparty/anyps5`, run `git -C 3rdparty/anyps5 submodule update --init 3rdparty/ZArchive`, build and run `ctest`.
2. Run `tools/survey_libraries.py --json survey.json` and port any library that no longer compiles for aarch64: new upstream code may use x86 intrinsics or assembly.
3. Reword `feat(libkernel): build for a lifted guest`: upstream made the same `ParamJsonParser` fix, so the commit no longer touches that file but its message still says it does.
4. Force-push the result over `aarch64-host`, then commit the new submodule pointer here.

## 2. Libraries that need AnyPS5's own CMake build

`tools/survey_libraries.py` compiles sources without AnyPS5's CMake, so these fail for lack of generated or configured headers (see [known gaps](DEBT.md)): libSceVideoOut's `Telemetry.cpp` (`TraceRevision.hpp`), libSceSsl (mbedtls configuration), libSceVorbisDec (ogg `config_types.h`), libSceUsbd (libusb) and libSceVideodec2 (FFmpeg). Build them, and libSceAgcDriver with its shader recompiler, through AnyPS5's CMake with `ANYPS5_GUEST_LIFTED` for aarch64.

## 3. Run relinked PS5 executables

- Seed discovery from `DT_INIT_ARRAY` and `DT_FINI_ARRAY`.
- Handle TLS relocations beyond static TLS.
- Lift the bundled `sce_module` files the relinker converts, and link the bridges for every library a title imports.
- Bridge the exports still left out: records passed or returned by value, complex values, function-pointer returns and C++-linkage exports.

## 4. Breakout demo, then one title

Then performance work, as the [roadmap](DESIGN.md#roadmap) describes.
