# Next steps

## 1. Run EVO Player

[EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5), a GPL-3.0 media player for the PS5 built with the open [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk), is the first real program to run. It draws through libSceAgc and libSceVideoOut, plays audio, reads the pad and runs threads, from a 47 MB executable. Its release package ships a `libc.prx` that is not ours to redistribute, so tests download the release rather than keep it in the repo.

AnyPS5's `tools/audit_dump.py` accepts v0.13.0: the relinker converts its `eboot.bin` and the `libc.prx` it ships, and the libraries implement 523 of its 532 imports. Six are partial (`scePadOpen`, `sceSystemServiceLoadExec`, `sceSystemServiceKillApp`, `sceSystemServiceGetAppId`, `__tls_get_addr`), `getcontext` is a stub and two libSceLibcInternal NIDs (`gNQ1V2vfXDE`, `lKEN2IebgJ0`) are absent.

Its relinked executable lifts: 55518 functions, 5.1 million instructions and 1331 jump tables, 197 of them sized by layout. In order:

- Lift in parallel. One module takes 17.5 minutes and 35 GB on one core, about 12 minutes of it optimization and 5 code generation, and at `-O0` the code outgrows the ±128 MB reach of a direct call between functions of one object. Lifting, optimizing and compiling parts of the program on separate threads, each its own object, fixes both.
- Answer `cpuid` and `xgetbv` as Zen 2 does. Most of the 144246 unsupported instructions are AVX-512, VAES and VPCLMULQDQ on paths that code choosing an implementation by CPU feature never takes on the console.
- Lift the Zen 2 instructions it uses that the lifter does not: `movbe`, `bswap`, `shld`, `shrd`, `adcx`, `adox`, `lea` with 32-bit addressing, AES-NI, SHA, SSE4a's `extrq` and `insertq`, and MMX, which FFmpeg's assembly uses.
- Seed discovery from `DT_INIT_ARRAY` and `DT_FINI_ARRAY`.
- Handle TLS relocations beyond static TLS.
- Lift the bundled `sce_module` files the relinker converts, and link the bridges for every library a program imports.
- Bridge the exports still left out: records passed or returned by value, complex values, function-pointer returns and C++-linkage exports.

## 2. Breakout demo, then one title

A small game built with the same SDK, which the repo can carry, then a title, which is a dump that cannot be. Then performance work, as the [roadmap](DESIGN.md#roadmap) describes.
