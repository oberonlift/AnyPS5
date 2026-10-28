# Next steps

## 1. EVO Player

[EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5), a GPL-3.0 media player for the PS5 built with the open [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk), is the first real program to run. It draws through libSceAgc and libSceVideoOut, plays audio, reads the pad and runs threads, from a 47 MB executable. Its release package ships a `libc.prx` that is not ours to redistribute, so tests download the release rather than keep it in the repo.

AnyPS5's `tools/audit_dump.py` accepts v0.13.0: the relinker converts its `eboot.bin` and the `libc.prx` it ships, and the libraries implement 523 of its 532 imports. Six are partial (`scePadOpen`, `sceSystemServiceLoadExec`, `sceSystemServiceKillApp`, `sceSystemServiceGetAppId`, `__tls_get_addr`), `getcontext` is a stub and two libSceLibcInternal NIDs (`gNQ1V2vfXDE`, `lKEN2IebgJ0`) are absent.

The relinker writes it as `app.elf`, whose first `DT_NEEDED` is the game's own libc, converted to `app0/sce_module/libc.prx.guest.prx`: a position-independent object with `DT_INIT`, a TLS segment and 2566 exports named `<NID>#guest`. The program lifts in about two and a half minutes on the GB10's 20 cores: 59046 functions, 5.5 million instructions and 1618 jump tables, 303 of them sized by layout.

Lifted for the GB10 and run under Xvfb, it boots to its home screen and renders it at 60 frames a second: AnyPS5 builds its AGC pipelines on the GB10's Vulkan device, and the UI, its threads, file access and DNS run. In order:

1. Its update check fails with "the secure (https) connection failed" after resolving `api.github.com`. Find where: the TLS code in the executable (OpenSSL's, with AES-NI, `pclmulqdq` and SHA paths that the fixtures' known answers cover only in part: AES-GCM and SHA-512 are not), AnyPS5's sockets, or certificate verification.
2. The home screen shows vertical colour fringes over its background and icons. Capture the same screen from AnyPS5 running EVO Player natively on an x86-64 host to tell a lifting error from the renderer's.
3. Drive it: pad input under Xvfb, browsing, and playing a file, which reaches FFmpeg's decoders and their SIMD code (much of it assembly found through `lea`), and with it the instructions the lifter still lacks, such as SSE4a's `extrq` and `insertq`, `vmaskmov` and the variable `vpermilps`, and exports still without a bridge (records by value, complex values, function-pointer returns, C++ linkage).
4. Reach `#guest` exports when AnyPS5 needs them: the runtime looks one up and returns its guest address, and AnyPS5's lifted `dlsym` and allocator lookup use that, calling guest code through the runtime the way `GuestCpuCallInterrupted` does, rather than through a native pointer. Nothing has stopped on one yet.
5. Lifting has grown from 100 to 140 seconds with the alignment checks and the control-flow walks of jump-table recovery; profile discovery, which rebuilds a function's predecessors every round.

## 2. Breakout demo, then one title

A small game built with the same SDK, which the repo can carry, then a title, which is a dump that cannot be. Then performance work, as the [roadmap](DESIGN.md#roadmap) describes.
