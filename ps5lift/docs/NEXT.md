# Next steps

## 1. Run EVO Player

[EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5), a GPL-3.0 media player for the PS5 built with the open [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk), is the first real program to run. It draws through libSceAgc and libSceVideoOut, plays audio, reads the pad and runs threads, from a 47 MB executable. Its release package ships a `libc.prx` that is not ours to redistribute, so tests download the release rather than keep it in the repo.

AnyPS5's `tools/audit_dump.py` accepts v0.13.0: the relinker converts its `eboot.bin` and the `libc.prx` it ships, and the libraries implement 523 of its 532 imports. Six are partial (`scePadOpen`, `sceSystemServiceLoadExec`, `sceSystemServiceKillApp`, `sceSystemServiceGetAppId`, `__tls_get_addr`), `getcontext` is a stub and two libSceLibcInternal NIDs (`gNQ1V2vfXDE`, `lKEN2IebgJ0`) are absent.

Its relinked executable lifts in under two minutes on the GB10's 20 cores: 55518 functions, 5.1 million instructions and 1331 jump tables, 197 of them sized by layout.

The relinker writes it as `app.elf`, whose first `DT_NEEDED` is the game's own libc, converted to `app0/sce_module/libc.prx.guest.prx`: a position-independent object with `DT_INIT`, a TLS segment and 2566 exports named `<NID>#guest`. The suffix keeps them from binding the executable's imports, which all go to AnyPS5's libraries; AnyPS5 reaches them only by name, through its `dlsym` and the application heap's allocator lookup, and calls them from native code. In order:

1. Run an image's initializers: seed discovery from `DT_INIT`, `DT_INIT_ARRAY`, `DT_FINI` and `DT_FINI_ARRAY`, and have the runtime call them in `ld.so`'s order before the entry point.
2. Lift several images as one program: load a position-independent module at a base `ps5lift` chooses and apply its relocations there, give the runtime one table of images to map and dispatch into, and initialize the modules in dependency order before the executable.
3. Give each image its TLS block: static TLS for every image loaded at start, the `R_X86_64_DTPMOD64`, `DTPOFF64` and `TPOFF64` relocations, and `__tls_get_addr`.
4. Reach `#guest` exports: the runtime looks one up and returns its guest address, and AnyPS5's lifted `dlsym` and allocator lookup use that, calling guest code through the runtime the way `GuestCpuCallInterrupted` does, rather than through a native pointer.
5. Run EVO Player, then the instructions it reaches that the lifter lacks: AES-NI, SHA and `pclmulqdq` for TLS, `vmaskmov`, the variable `vpermilps`, SSE4a's `extrq` and `insertq`, and exports still without a bridge (records by value, complex values, function-pointer returns, C++ linkage).

## 2. Breakout demo, then one title

A small game built with the same SDK, which the repo can carry, then a title, which is a dump that cannot be. Then performance work, as the [roadmap](DESIGN.md#roadmap) describes.
