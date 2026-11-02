# ps5lift instruction tests, stages 1 and 2: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every form of every PS5-set instruction gets hardware-recorded cases that run lifted in both lift modes and both floating-point modes, and every instruction outside the set stops as the console would. The work ends with the first full report, which is the work list for stages 3 and 4.

**Architecture:** A Python package, `ps5lift/tools/instructions`, does the generation:
- it reads Zydis's database at the revision that generated the Zydis submodule's tables, and classifies each definition;
- it encodes instances with its own encoder, verified by Zydis through `ps5lift_decode`, a small executable built with the lifter's own decoder settings;
- it schedules seeded inputs and emits one x86-64 guest program per test group.

Two drivers run each case and write state differences:
- a native x86-64 library, which records on Intel hardware;
- a lifted bridge, which observes runtime stops through a new `ps5lift_stop` handler and host memory faults through a signal handler.

A comparator then applies the design's masks, tolerances, written expectations and fast-mode contract.

**Tech Stack:** Python 3 standard library (`json`, `struct`, `lzma`, `hashlib`, `decimal`, `unittest`), C for the native driver, C++20 for the lifted driver, the decoder helper and the lifter and runtime changes. Zydis `a2278f1d`, zydis-db `282fdf6`, LLVM 18 or newer, clang and lld, CMake and ctest.

**Spec:** [docs/dev/PS5LIFT_INSTRUCTION_TESTS.md](../../dev/PS5LIFT_INSTRUCTION_TESTS.md)

**Scope:** Stages 1 and 2 of the spec. Stage 3 (fix every mismatch, including exact mode's floating-point contract) and stage 4 (translate every missing instruction) get their own plans, written from the report Task 12 produces.

## Global Constraints

- **Code comments.** Code carries no comments except technical-debt markers a human adds, `#endif` ends and namespace ends ([CONVENTIONS.md](../../dev/CONVENTIONS.md)). Python modules may have a docstring, as `tools/ps5lift.py` does.
- **C++ naming.** PascalCase for classes and public methods; camelCase for private members, private methods and arguments.
- **Python.** Standard library only, like `tools/ps5lift.py`; nothing imports from the working directory.
- **Commits.**
  - Conventional Commits.
  - Author and committer `oberonlift <oberonlift@gmail.com>`, dated in UTC (`TZ=UTC`).
  - The message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  - `git add` with explicit paths only.
  - Never push.
- **Nothing private is committed.** No machine names, host names, addresses, user names or home paths. Machines are "an aarch64 Linux host" (the GB10 does this work) and "an x86-64 Linux host" (the Intel N100 records). No title names.
- **Database.** zydis-db `282fdf6`, file `Data/instructions.json`. It is the revision whose generator produced the tables of Zydis `a2278f1d`; both have the same 1,792 mnemonics. At this revision the set is 2,898 definitions under 922 mnemonics, about 4,100 forms.
- **No skips, no expected failures.** A test that cannot run fails and says why.
- **Determinism.** The seed is one constant in `cases.py`. The same seed, database and generator always produce byte-identical assembly and input files.
- **Recording size.** The committed recordings total at most 24 MB.
- **Guest programs.** Each one links at `GUEST_BASE` (`0xFC00000000`) with the fixtures' `GUEST_FLAGS`, plus `-Wl,--build-id=none`.
- **Build host.** Builds and lifted runs happen on an aarch64 Linux host in `ps5lift/build`, configured as [PS5LIFT.md](../../dev/PS5LIFT.md) describes. Recording happens on an x86-64 Linux host that needs only Python 3.

## Review Focus

1. **A lifted program that dies or hangs partway.** This covers `ps5lift_fail`, a host crash and an endless loop. The case it was on is reported as crashed or timed out, with its stderr, and the run resumes at the next case. Covered by Task 8's `test_runner_resumes_after_crash` and `test_runner_records_timeout`.
2. **A recording made from a different guest program.** This happens after a change to the generator, the seed, the database or the toolchain's layout. The recording is refused with a message that says to record again, and is never compared. Covered by Task 9's `test_stale_recording_refused`.
3. **An access that crosses from the block into the guard page.** It is a page fault whose data address is the guard page's first byte, natively and lifted. Task 6's `test_crossing_case_per_memory_form` puts such a case in every form that accesses memory. Task 9's driver reports `si_addr` unchanged, so a host that reports another address shows up as a mismatch, not a pass.
4. **A database definition the encoder or the schedule cannot handle.** Generation stops, naming the definition and the reason; no form leaves the set silently. Covered by Task 4's `test_every_set_form_encodes` and Task 6's `test_every_form_has_cases`.
5. **A recording host that doesn't match the console.**
   - If its CPU lacks an extension, the recorder refuses that group (Task 8's `test_missing_cpu_flag_refused`).
   - If its kernel enables XSAVE components the console lacks, the cases are unaffected: XSAVE-family cases ask for x87, SSE and AVX only (mask 7 or less), so their images match the console's 0x340-byte area (Task 6's `test_xsave_masks_within_console_components`).

## Files

| Path | Responsibility |
|------|----------------|
| `.gitmodules`, `3rdparty/zydis-db` | The database submodule at `282fdf6` |
| `ps5lift/lifter/include/lifter/Decoder.hpp`, `ps5lift/lifter/src/Decoder.cpp` | `Ps5Lift::InitializeDecoder`, the one place the decoder is configured |
| `ps5lift/lifter/include/lifter/InstructionSet.hpp`, `ps5lift/lifter/src/InstructionSet.cpp` | The lifter's classification of decoded instructions (Task 11) |
| `ps5lift/runtime/include/ps5lift/State.h`, `ps5lift/runtime/src/Runtime.cpp` | `ps5lift_stop` and its handler |
| `ps5lift/tests/instructions/Decode.cpp` | The `ps5lift_decode` executable |
| `ps5lift/tests/instructions/NativeDriver.c` | The x86-64 recording driver, built as `libinstructionhost.so` |
| `ps5lift/tests/instructions/LiftedDriver.cpp` | The bridge that runs cases in a lifted program |
| `ps5lift/tests/instructions/record.py` | The recorder, run on the x86-64 host |
| `ps5lift/tests/instructions/recordings/<group>.results.xz` | The committed recordings |
| `ps5lift/tools/instructions/decoder.py` | The Python side of `ps5lift_decode` |
| `ps5lift/tools/instructions/database.py` | Definitions, the PS5 set, groups and forms |
| `ps5lift/tools/instructions/encoding.py` | The encoder and its verification |
| `ps5lift/tools/instructions/records.py` | Slot layout, input and result files, `records.h` |
| `ps5lift/tools/instructions/cases.py` | Instances, shapes and the input schedule |
| `ps5lift/tools/instructions/program.py` | Guest assembly, linking, the layout read back from the ELF |
| `ps5lift/tools/instructions/runner.py` | Running a driver to completion, resuming after crashes |
| `ps5lift/tools/instructions/expectations.py` | Expectations written from the manuals |
| `ps5lift/tools/instructions/tolerance.py` | The approximation and transcendental comparisons |
| `ps5lift/tools/instructions/compare.py` | Comparison rules and mismatch text |
| `ps5lift/tools/instructions/suite.py` | The `instructions`, `instructions-bundle` and `instructions-import` commands |
| `ps5lift/tools/instructions/tests/test_*.py` | Unit tests (`unittest`) |
| `ps5lift/tools/ps5lift.py` | Registers the three commands; `.stop` support for `fixture` |
| `ps5lift/tests/CMakeLists.txt`, `ps5lift/lifter/CMakeLists.txt` | Targets and tests |
| `ps5lift/tests/fixtures/stop.c`, `stop.expected`, `stop.stop` | A fixture that ends at a stop |
| `docs/dev/PS5LIFT.md`, `docs/dev/PS5LIFT_DESIGN.md`, `docs/dev/TechnicalDebt.md` | Documentation |

Unit tests run as `python3 -m unittest discover -s ps5lift/tools/instructions/tests -t ps5lift/tools`. Tests that need `ps5lift_decode` or clang read the environment variables `PS5LIFT_DECODE` and `PS5LIFT_CLANG`, which ctest sets. If one is unset, the test fails with a message naming the variable.

---

### Task 1: The database, the lifter's decoder settings, and `ps5lift_decode`

**Files:**
- Modify: `.gitmodules`; add the submodule `3rdparty/zydis-db` (https://github.com/zyantific/zydis-db) at `282fdf6`.
- Create: `ps5lift/lifter/include/lifter/Decoder.hpp`, `ps5lift/lifter/src/Decoder.cpp`.
- Modify: `ps5lift/lifter/src/Discovery.cpp:695`, `ps5lift/lifter/CMakeLists.txt`.
- Create: `ps5lift/tests/instructions/Decode.cpp`, `ps5lift/tools/instructions/__init__.py`, `ps5lift/tools/instructions/decoder.py`.
- Test: `ps5lift/tools/instructions/tests/test_decoder.py`; modify `ps5lift/tests/CMakeLists.txt`.

**Interfaces:**
- Produces:
  - `void Ps5Lift::InitializeDecoder(ZydisDecoder& decoder)`. It sets up `ZYDIS_MACHINE_MODE_LONG_64` with `ZYDIS_STACK_WIDTH_64` and enables `ZYDIS_DECODER_MODE_AMD_BRANCHES`. On failure it throws `std::runtime_error("Zydis decoder initialization failed")`.
  - A static library target `ps5lift_decoder` (Decoder.cpp plus Zydis), linked by `ps5lift` and `ps5lift_decode`.
  - The `ps5lift_decode` executable. It reads lines of hex bytes from stdin and prints one JSON object per line, flushing after each line:
    - on success: `length`, `mnemonic`, `text` (Intel syntax at runtime address 0), `isa_ext`, `isa_set`, `category`, `operand_width`, `privileged` (`ZYDIS_ATTRIB_IS_PRIVILEGED`), and `operands`;
    - each operand has `type`, `visibility`, `action`, `size`, plus `register`, or `base`/`index`/`scale`/`displacement`, or `immediate`;
    - on failure: `{"error": "<ZyanStatus hex>"}`.

    With `--mnemonics`, it prints every `ZydisMnemonicGetString`, one per line.
  - `decoder.Decoder(executable)`, with `.decode(encodings: list[bytes]) -> list[Decoded]` (one process, batched) and `.mnemonics() -> set[str]`. `decoder.from_environment() -> Decoder` reads `PS5LIFT_DECODE`. `Decoded` and `DecodedOperand` are frozen dataclasses with the JSON fields as attributes.

- [ ] **Step 1: Write the failing tests** in `test_decoder.py`:
  - `test_amd_branch_mode`: `66 e9 10 00` decodes with length 4, mnemonic `jmp`, operand width 16.
  - `test_endbr64`: `f3 0f 1e fa` is `endbr64`, length 4.
  - `test_add`: `48 01 d8` is `add`, operands `rax` and `rbx`, size 64.
  - `test_privileged`: `0f 01 f8` (`swapgs`) has `privileged` true.
  - `test_mnemonic_count`: `len(mnemonics()) == 1792`.
- [ ] **Step 2: Register `instructions_unit` in ctest** (all hosts), with `ENVIRONMENT` set to `PS5LIFT_DECODE=$<TARGET_FILE:ps5lift_decode>;PS5LIFT_CLANG=${CMAKE_C_COMPILER}`. Run `ctest --test-dir ps5lift/build -R instructions_unit`: it fails because `ps5lift_decode` does not exist.
- [ ] **Step 3: Implement** `Decoder.cpp` and `Decode.cpp`, make Discovery call `InitializeDecoder`, add the submodule, and add the targets.
- [ ] **Step 4: Run** `cmake --build ps5lift/build && ctest --test-dir ps5lift/build -R "instructions_unit|fixture_"`: both pass.
- [ ] **Step 5: Commit** as `feat(ps5lift): decode with Zen 2's branch rules and expose the decoder to tests`.

### Task 2: One runtime entry point for every guest stop

**Files:**
- Modify: `ps5lift/runtime/include/ps5lift/State.h:221-226`, `ps5lift/runtime/src/Runtime.cpp:388-412`, `ps5lift/lifter/src/Lifter.cpp` (`Unsupported()` at 940, `Trap()` at 950, the divide error at 1209, `INT` at 3746), and `ps5lift/tools/ps5lift.py` (`fixture_command`, `libc_fixture_command`).
- Create: `ps5lift/tests/fixtures/stop.c`, `stop.expected`, `stop.stop`; add `stop` to `PS5LIFT_FIXTURES`.

**Interfaces:**
- Produces:
  - In `State.h`, the enum `Ps5LiftStopKind`, with these values fixed: `PS5LIFT_STOP_DIVIDE_ERROR = 0`, `PS5LIFT_STOP_BREAKPOINT = 1`, `PS5LIFT_STOP_INVALID_OPCODE = 2`, `PS5LIFT_STOP_GENERAL_PROTECTION = 3`, `PS5LIFT_STOP_CONSOLE_KERNEL = 4`, `PS5LIFT_STOP_MISSING_TRANSLATION = 5`. Task 11 adds `PS5LIFT_STOP_SEGMENT_TABLES = 6`.
  - `[[noreturn]] void ps5lift_stop(Ps5LiftState* state, uint32_t kind, uint64_t address, uint64_t detail, const char* mnemonic)`. `detail` is the vector for `int n` and 0 otherwise.
  - `typedef void (*Ps5LiftStopHandler)(Ps5LiftState*, uint32_t kind, uint64_t address, uint64_t detail, const char* mnemonic)`.
  - `void ps5lift_set_stop_handler(Ps5LiftStopHandler handler)`; `nullptr` restores the default.
  - The default handler calls `ps5lift_fail` with these texts:
    - divide error and missing translation: their current texts;
    - `int n`: the current `ps5lift_interrupt` text, including the `%fs:0x28` word;
    - `breakpoint at 0x…`;
    - `invalid opcode <mnemonic> at 0x…`;
    - `general-protection fault: <mnemonic> at 0x… is privileged on the PS5`.
  - If a handler returns, `ps5lift_fail` reports that it did.
  - `ps5lift_unsupported`, `ps5lift_trap`, `ps5lift_interrupt` and `ps5lift_divide_error` are deleted.
  - The lifter maps `ud2` to an invalid-opcode stop, `int3` to a breakpoint stop and `hlt` to a general-protection stop.
  - `ps5lift.py` gets `stop_matches(result, stop, native) -> bool`, used by both fixture commands:
    - natively, the program must die from a signal (`returncode < 0`);
    - lifted, it must exit with 70 and `stop` must appear in its stderr.

- [ ] **Step 1: Write the fixture.** `stop.c` prints `before stop\n`, then executes `ud2`. `stop.expected` is `before stop\n`; `stop.stop` is `invalid opcode ud2`.
- [ ] **Step 2: Run** `ctest --test-dir ps5lift/build -R fixture_stop`: it fails (exit code 70 with the old text, and `fixture` does not read `.stop`).
- [ ] **Step 3: Implement** `ps5lift_stop`, the handler, the lifter's call sites and `stop_matches`.
- [ ] **Step 4: Run** `ctest --test-dir ps5lift/build -R "fixture_|lift_"`: everything passes.
- [ ] **Step 5: Commit** as `feat(ps5lift): route every guest stop through ps5lift_stop`.

### Task 3: The PS5 set from the database

**Files:**
- Create: `ps5lift/tools/instructions/database.py`.
- Test: `ps5lift/tools/instructions/tests/test_database.py`.

**Interfaces:**
- Consumes: `decoder.from_environment()` (Task 1).
- Produces:
  - `DATABASE` (the submodule's `Data/instructions.json`).
  - `PS5_EXTENSIONS`, a frozenset of the spec's 37 names, verbatim.
  - `Definition`, a frozen dataclass: `index, mnemonic, encoding, opcode_map, opcode: int, filters: dict, operands: tuple, extension, isa_set, category, cpl, undefined_flags: frozenset`. `undefined_flags` lists the flags whose `affected_flags` value is `u`.
  - `load(path=DATABASE) -> list[Definition]`. It drops definitions with `filters.mode == "!64"`, encoding `mvex`, or any `feature_knc` filter.
  - `Group` enum: `SET, NOT_ON_PS5, PRIVILEGED, SEGMENT_TABLES, CONSOLE_KERNEL`.
  - `classify(definition) -> Group`. The first matching rule wins:
    1. **Hint space:** default encoding, map `0f`, opcode `0x0d` or `0x18`–`0x1f`. `SET`, with NOP or prefetch behaviour.
    2. **Not on the PS5:** extension not in `PS5_EXTENSIONS`, or mnemonic `sysenter` (an invalid opcode in long mode on AMD). `NOT_ON_PS5`.
    3. **CPL 0:** `cpl == 0`. `PRIVILEGED`.
    4. **I/O:** mnemonic in `in out insb insw insd outsb outsw outsd cli sti rdpmc`. `PRIVILEGED`.
    5. **Segment tables:** `SEGMENT_TABLES` for any of:
       - mnemonic in `lar lsl verr verw sgdt sidt sldt smsw str iret iretd iretq lfs lgs lss`;
       - `mov` to or from a segment register;
       - `push` or `pop` of `fs` or `gs`;
       - far `ret` (opcodes `ca`, `cb`);
       - far `call` and `jmp` (`ff /3`, `ff /5`).
    6. **Console kernel:** mnemonic in `syscall int int1`. `CONSOLE_KERNEL`.
    7. Otherwise `SET`.
  - `Form`, a frozen dataclass: `definition, operand_size: int | None, key: str`. The key is `f"{mnemonic}.{index}"`, plus `f".o{size}"` for a multi-size definition.
  - `forms(definitions) -> list[Form]`. A definition gets one form per operand size it encodes in 64-bit mode, according to its operand widths, `opsize_map` and its `operand_size` and `rex_w` filters.
  - `test_group(form) -> str`:
    - the extension in lower case for a set form whose extension is in the list;
    - `hint` for a hint-space form whose extension is not;
    - `outside-not-on-ps5`, `outside-privileged`, `outside-segment-tables` or `outside-console-kernel` for the other groups.

- [ ] **Step 1: Write the failing tests:**
  - `test_counts`: set definitions == 2898 and set mnemonics == 922.
  - `test_classify_examples`, as a table of (mnemonic, a filter that picks one row, expected group):
    - `SET`: `add`; `lzcnt`; the SSE4 `popcnt`; `endbr64`; `bndmov`; `rdsspq`; `prefetchwt1`; `int3`; `ud2`.
    - `NOT_ON_PS5`: the EVEX `vaddps`; `monitor`; `clzero`; `rdfsbase`; `sysenter`.
    - `PRIVILEGED`: `hlt`; `wrmsr`; `in`; `rdpmc`; `xsaves`.
    - `SEGMENT_TABLES`: `lar`; `mov` to `ds`; `mov` from `cs`; `push fs`; far `ret`.
    - `CONSOLE_KERNEL`: `syscall`; `int`; `int1`.
  - `test_knc_rows_dropped`: no loaded `popcnt` row has extension `KNC`.
  - `test_undefined_flags`: `bsf` has `{cf, pf, af, sf, of}`.
  - `test_form_sizes`: `add` `01 /r` gives sizes {16, 32, 64}, `push` `50+r` gives {16, 64}, `addps` gives {None}.
  - `test_database_matches_zydis`: the database's mnemonic set equals `Decoder.mnemonics()`.
- [ ] **Step 2: Run** `ctest --test-dir ps5lift/build -R instructions_unit`: the new tests fail.
- [ ] **Step 3: Implement** `database.py`.
- [ ] **Step 4: Run** the same: they pass.
- [ ] **Step 5: Commit** as `feat(ps5lift): read the PS5 instruction set from Zydis's database`.

### Task 4: The encoder

**Files:**
- Create: `ps5lift/tools/instructions/encoding.py`.
- Test: `ps5lift/tools/instructions/tests/test_encoding.py`.

**Interfaces:**
- Consumes: `Form` and `Definition` (Task 3); `Decoder` (Task 1).
- Produces:
  - **Operand choices:** `Reg(name)` uses Zydis's lower-case register names. `Mem(base, index, scale, displacement, displacement_size)` takes `base="rip"` for RIP-relative, and its `displacement` may be a `str` assembler expression. `Imm(value)`. `Target(label: str)` for a relative branch.
  - `Field(size: int, expression: str, relative: bool)`. `Encoded` is a `list[bytes | Field]`, where a relative field is an expression minus the address of the instruction's end.
  - `encode(form, operands, prefixes=()) -> Encoded`. `prefixes` draws from `bnd`, `lock`, `rep`, `repne` where the definition allows them.
  - The encoder handles the default (legacy), `3dnow`, `xop`, `vex` and `evex` encodings. It follows the definition's filters: `modrm_mod`, `modrm_reg`, `modrm_rm`, `mandatory_prefix`, `rex_w`, `vector_length`, `evex_b`, `operand_size`, `address_size`, `force_modrm_*`. It handles the operand encodings the database uses: `modrm_rm`, `modrm_reg`, `ndsndd`, `mask`, `opcode`, `is4`, `uimm8/16/32`, `simm8`, `simm16_32_32`, `simm16_32_64`, `jimm8`, `jimm32`, `jimm16_32_32`, `disp16_32_64`. VEX uses the two-byte form where it can (map `0f`, W0, no X or B).
  - `placeholder(encoded) -> bytes` fills each field with zeros.
  - `default_operands(form) -> list`: the first legal register of each class, and `[rbx]` for memory.
  - `verify(decoder, form, operands, prefixes, encoded)` raises `EncodingError(f"{form.key}: …")` unless the decoded result matches the encoding:
    - the length equals the encoding's;
    - the mnemonic equals the form's;
    - every visible operand matches its choice (register, or base/index/scale/displacement size, or immediate);
    - the operand width equals the form's size.

- [ ] **Step 1: Write the failing tests:**
  - `test_known_bytes`, comparing exact `placeholder` bytes:
    - `add rax, rbx` → `48 01 d8`;
    - `add ax, 1` (`83 /0`) → `66 83 c0 01`;
    - `vaddps ymm1, ymm2, ymm3` → `c5 ec 58 cb`;
    - `vfmadd231ps xmm0, xmm1, [rax]` → `c4 e2 71 b8 00`;
    - `endbr64` → `f3 0f 1e fa`;
    - `movaps xmm8, [r12]` → `45 0f 28 04 24`;
    - `pshufb mm0, mm1` → `0f 38 00 c1`;
    - `pi2fd mm0, mm1` (3DNow!) → `0f 0f c1 0d`.
  - `test_every_set_form_encodes`: every `SET` form's `default_operands` instance verifies.
  - `test_every_outside_definition_encodes`: one instance of every other group's definition verifies.
- [ ] **Step 2: Run** `ctest --test-dir ps5lift/build -R instructions_unit`: they fail.
- [ ] **Step 3: Implement** `encoding.py`. Every database value it has no rule for raises `EncodingError` naming the definition and the value.
- [ ] **Step 4: Run** the same: they pass.
- [ ] **Step 5: Commit** as `feat(ps5lift): encode every database definition and check it with Zydis`.

### Task 5: Records: slot layout, input and result files, `records.h`

**Files:**
- Create: `ps5lift/tools/instructions/records.py`.
- Test: `ps5lift/tools/instructions/tests/test_records.py`.

**Interfaces:**
- Produces, with these values fixed:
  - **The slot.** One layout serves as both the input slot and the output slot; both are guest symbols, 64-byte aligned. `SLOT_SIZE = 1168`.

    | Field | Offset | Size |
    |-------|--------|------|
    | `FX`, an FXSAVE image (FCW 0, FSW 2, FTW 4, MXCSR 24, ST *i* at 32 + 16*i*) | 0 | 512 |
    | `YMM` *i* | 512 + 32*i* | 32 each |
    | `GPR` *i*, in `PS5LIFT_RAX` order | 1024 + 8*i* | 8 each |
    | `LOAD_AH` | 1152 | 1 |
    | `LOAD_OF_LEFT` | 1153 | 1 |
    | `LOAD_OF_RIGHT` | 1154 | 1 |
    | `LOAD_DF` | 1155 | 1 |
    | `STORE_AX` | 1156 | 2 |
    | `STORE_DF` | 1158 | 1 |
    | `PATH` (0xFF until a branch shape writes 0 or 1) | 1159 | 1 |

  - **Derived load bytes.** `load_bytes(flags) -> (ah, of_left, of_right, df)`, where `ah = SF<<7 | ZF<<6 | AF<<4 | PF<<2 | 2 | CF` and OF comes from `(0x7f, 0x01)` or `(0, 0)`. `store_flags(ax, df) -> dict` is the inverse, applied to `lahf; seto al`.
  - **The block.** `BLOCK_SIZE = 4096`. The block ends on a 64 KiB boundary and is followed by a 64 KiB guard, which the drivers make inaccessible.
  - **The input file.** A header: `b"PS5LIN01"`, then a 32-byte program fingerprint, a 32-byte input fingerprint, and a `u32` case count. Then one record per case: `u32 case, u32 instance, u64 fill_seed, u8 console_defined, u8[3] pad`, the slot, `u16 patch_count`, and the patches as `u16 offset, u16 length, bytes`.
  - **The result file.** The same header with magic `b"PS5LRES1"`. Then one record per case: `u32 case, u8 outcome, u8 pad, u16 detail, u16 slot_words, u16 memory_words, u64 address, u64 data`, followed by `slot_words` pairs and then `memory_words` pairs of `(u16 offset, u64 value)`. These are the 8-byte words that differ from the input. A console-defined case adds `u8 count` and `count` `u64` expected values.
  - `Outcome` IntEnum: `COMPLETED 0, DIVIDE_ERROR 1, BREAKPOINT 2, INVALID_OPCODE 3, GENERAL_PROTECTION 4, PAGE_FAULT 5, ALIGNMENT_CHECK 6, X87_FLOATING_POINT 7, SIMD_FLOATING_POINT 8, SEGMENT_TABLES 9, CONSOLE_KERNEL 10, NEEDS_EXACT_FLOAT 11, MISSING_TRANSLATION 12, HOST_SIGNAL 13, CRASHED 14, TIMED_OUT 15`.
  - **Python side.** Dataclasses `CaseInput` and `Result` (word maps as `dict[int, int]`). Functions `write_inputs(path, header, inputs)`, `read_inputs(path)`, `read_results(data: bytes) -> (header, list[Result])`, and `fill_block(seed) -> bytes`. The fill is xorshift64*: `x ^= x >> 12; x ^= x << 25; x ^= x >> 27; word = x * 0x2545F4914F6CDD1D`, eight bytes little-endian per word, with seed 0 replaced by `0x9E3779B97F4A7C15`.
  - `header_text() -> str` emits `records.h`: every offset and size above as `#define PS5LIFT_INSTRUCTION_*`, the outcome values, packed structs for the file records, and `static inline void Ps5LiftFillBlock(uint8_t* block, uint64_t seed)`.

- [ ] **Step 1: Write the failing tests:**
  - `test_layout_offsets`: the table's values.
  - `test_inputs_round_trip` and `test_results_round_trip`, including a console-defined record.
  - `test_load_bytes`: all flags set gives `ah == 0xd7` and OF from `(0x7f, 1)`.
  - `test_fill_matches_c`: compile a program that includes `header_text()` with `PS5LIFT_CLANG` for the host, print `Ps5LiftFillBlock` for seeds 0, 1 and `2**64-1`, and compare with `fill_block`.
- [ ] **Step 2: Run** `instructions_unit`: they fail.
- [ ] **Step 3: Implement** `records.py`.
- [ ] **Step 4: Run** `instructions_unit`: they pass.
- [ ] **Step 5: Commit** as `feat(ps5lift): define the instruction test records`.

### Task 6: Instances and the input schedule

**Files:**
- Create: `ps5lift/tools/instructions/cases.py`.
- Test: `ps5lift/tools/instructions/tests/test_cases.py`.

**Interfaces:**
- Consumes: `forms`, `classify` and `test_group` (Task 3); `encode`, `verify` and the operand classes (Task 4); `records` (Task 5).
- Produces:
  - `SEED`, one 64-bit constant.
  - `Shape` enum: `STRAIGHT`, `BRANCH` (jcc, jmp rel, loop*, jrcxz/jecxz), `CALL` (direct and indirect), `RETURN` (ret, ret imm16), `INDIRECT_JUMP`.
  - `Instance`: `index, form, operands, prefixes, shape, encoded, text`.
  - `CaseSpec`. Values are symbolic: block offsets, not addresses.
    - `index, instance, gprs: list[int | BlockOffset], vectors, flags: dict, mxcsr, x87: bytes (an FXSAVE image less XMM)`;
    - `patches: list[(block offset, bytes)], fixups: list[(location, label)], fill_seed`;
    - `twin: int | None`, the case with identical inputs and the console MXCSR, which fast mode compares FTZ-only and DAZ-only cases against;
    - `loads_mxcsr: int | None`, the MXCSR value an `ldmxcsr`, `vldmxcsr`, `fxrstor` or `xrstor` instance loads;
    - `console_defined: bool`.
  - `instances(form, decoder) -> list[Instance]` and `case_specs(group_instances) -> list[CaseSpec]`, both deterministic from `SEED`.
  - `projected_recording_bytes(specs) -> int`.
- **The schedule.** This is the decision record; the implementer picks the exact bit patterns.
  - **The primary instance** of each form uses the low and high registers mixed (`rax`, `r15`, `xmm0`, `xmm15`/`ymm15`). Its base cases number *E*, the longest edge list among its input operands. In case *i*, operand *k* takes edge value `(i + k) mod n_k`, and vector lane *j* takes `(i + j) mod n`. Four seeded random cases follow.
  - **Edge values** are the spec's list. For integers: 0, 1, all ones, the minimum and maximum of the width, either side of the sign and carry boundaries, and shift or rotate counts of 0, 1, w−1, w, w+1 and 255. For each float width: ±0, ± the smallest and largest denormal, ± the smallest and largest normal, ±∞, a quiet and a signalling NaN with payloads, 1.0, and values either side of a rounding boundary.
  - **Floating-point instances** (any float element type, or an instruction that reads MXCSR's rounding) run their base cases under each of these MXCSR values:
    - `0x9fc0`, the console's;
    - `0x1f80 | rc << 13` for each rounding control;
    - `0x9f80`, FTZ alone;
    - `0x1fc0`, DAZ alone;
    - `0x1f80` with one exception unmasked, for each of the six.

    An FTZ-only or DAZ-only case's `twin` is the console-state case with the same *i*. Every other instance runs under `0x9fc0`.
  - **x87 instances** cycle TOP 0–7, valid and empty tags, precision control 24, 53 and 64 with each rounding control, all masked, each exception unmasked alone, and pending exceptions in the status word.
  - **Variants** have two cases each (base case 0 and one random). There is one for each special register the form's operands allow: `rsp`, `rbp`, `r12`, `r13`, `ah`–`bh`, `spl`–`dil`, the same register as source and destination, `xmm8`–`xmm15` and `ymm`. A memory form also gets one per addressing form: base only; base and index at scales 1, 2, 4 and 8; disp8; disp32; RIP-relative; `rsp`, `rbp`, `r12` and `r13` as base.
  - **Memory cases.** Every form that accesses memory gets, in addition:
    - a misaligned-by-one case where the access is wider than a byte (a form that requires alignment expects a general-protection fault there);
    - a case that ends exactly at the block's end;
    - a case that crosses into the guard by one byte, which expects a page fault at the guard's first byte.
  - **Branches** take both directions across the cases. `loop` and `jrcxz` take `rcx` values 0, 1, 2 and random. The `bnd` prefix is a variant of every near branch.
  - **Input constraints by mnemonic** (a table in `cases.py`):
    - string instructions: `rsi` and `rdi` in the block, `rcx` at most 64 under `rep`;
    - `xlat`: `rbx` in the block;
    - `enter`/`leave`: `rbp` and `rsp` in the block;
    - `push`/`pop`/`call`/`ret`: `rsp` in the block with 512 bytes on either side;
    - `maskmovq`/`maskmovdqu`: `rdi` in the block;
    - gathers: base in the block, indices kept inside it, masks from the edges;
    - `bt*` with a memory operand and a register offset: the bit offset kept inside the block, plus one case past it;
    - `div`/`idiv`: divisors include 0 and a quotient that overflows;
    - `cmpxchg16b`: 16-byte alignment, plus one misaligned case;
    - `cpuid`: only leaves and subleaves [Cpuid.cpp](../../../ps5lift/runtime/src/Cpuid.cpp) defines;
    - `xgetbv`: only the `ecx` values the PS5 CPUID says are defined;
    - XSAVE family: `edx:eax` a subset of 7.
  - **Console-defined cases:** `cpuid` and `xgetbv`.

- [ ] **Step 1: Write the failing tests:**
  - `test_deterministic`: generating twice gives equal specs.
  - `test_every_form_has_cases`: every set form has an instance with at least one case.
  - `test_edge_values_covered`: for `add r64, r/m64`, `addps` and `cvttss2si`, every edge value of every input operand appears.
  - `test_fp_mxcsr_states`: `addps`'s primary instance has all 13 states.
  - `test_fast_twins`: each twin has the same inputs apart from MXCSR, which is `0x9fc0`.
  - `test_crossing_case_per_memory_form`.
  - `test_xsave_masks_within_console_components`.
  - `test_cpuid_leaves_have_ps5_values`.
  - `test_projected_recording_size`: at most 24 MB over all set groups except `sse4a`.
- [ ] **Step 2: Run** `instructions_unit`: they fail.
- [ ] **Step 3: Implement** `cases.py`. Reduce random cases first, then variant cases, to meet the budget; never reduce the primary instance's edge cases.
- [ ] **Step 4: Run** `instructions_unit`: they pass.
- [ ] **Step 5: Commit** as `feat(ps5lift): schedule instruction cases from edge values and a seed`.

### Task 7: The guest program

**Files:**
- Create: `ps5lift/tools/instructions/program.py`.
- Test: `ps5lift/tools/instructions/tests/test_program.py`.

**Interfaces:**
- Consumes: `Instance`, `Shape` and `CaseSpec` (Task 6); the `records` layout (Task 5).
- Produces:
  - `emit(group, instances) -> str`, GNU-syntax assembly. The instance bytes are `.byte` directives, and fields are `.byte`/`.short`/`.long`/`.quad` expressions.
  - `link(clang, source, work, sysroot) -> Path`. It links with `GUEST_FLAGS`, `-no-pie`, `-Wl,--image-base=0xfc00000000`, `-Wl,--build-id=none` and `-L<work> -linstructionhost`.
  - `Layout` and `read_layout(elf) -> Layout`. It parses the ELF symbol table with `struct` and gives the table, input slot, output slot, block, guard and the loader's `fxrstor` address. For each instance it gives the function, the instance address and length, and its helper labels.
  - `case_inputs(layout, specs) -> list[CaseInput]`: phase two, where block offsets, fixups and labels become addresses.
  - `fingerprints(assembly, inputs_bytes) -> (program, inputs)`, two SHA-256 digests.
- **The code shapes** (the decision record):
  - **Shared functions.**
    - `ps5lift_instruction_load` saves `rbx`, `rbp` and `r12`–`r15` to a save area, runs `fxrstor [input+FX]` (this is the loader `fxrstor` address), loads `ymm0`–`ymm15` with `vmovdqu`, and loads every GPR except `rax` and `rsp`.
    - `ps5lift_instruction_store` stores every GPR except `rax` and `rsp`. It captures DF by running `stosb` at a probe and comparing the probe pointer (`setb` into `STORE_DF`), then runs `cld`, `fxsave [output+FX]` and `vmovdqu` from the YMM registers, restores the saved registers, and returns.
    - All slot and probe accesses are RIP-relative.
  - **`STRAIGHT`** (a function `W`). In order:
    1. `mov [host_rsp], rsp`, then memory fixups via `lea rax, label` and `mov [block+off], rax`, then `call load`.
    2. DF: `cmp byte [LOAD_DF], 0`, then `std` or `cld`.
    3. OF: `mov al, [LOAD_OF_LEFT]; add al, [LOAD_OF_RIGHT]`.
    4. The other flags: `mov ah, [LOAD_AH]; sahf`.
    5. `mov rax, [GPR0]`, then register fixups with `lea`, then `mov rsp, [GPR4]`.
    6. The instance.
    7. `mov [out GPR0], rax; mov [out GPR4], rsp; lahf; seto al; mov [STORE_AX], ax`.
    8. `mov rsp, [host_rsp]; call store; ret`.
  - **`BRANCH`:** like `STRAIGHT`, but the instance targets `taken` inside `W`. The fallthrough runs `mov byte [PATH], 0`, then jumps to the capture code; `taken` runs `mov byte [PATH], 1`.
  - **`CALL`:** the instance calls `callee_k`, a separate global function. It runs `mov byte [PATH], 1`, captures `rax`, `rsp` and `ax` as in step 7, and returns. The code after the instance in `W` runs step 8. An indirect call's target is fixed up with `lea`.
  - **`RETURN`:** `W` loads everything, sets `rsp` to the input `rsp` + 8 with `lea`, and runs `call ret_k`. `ret_k` loads the flags and `rax` (steps 2–5 without the `rsp` load) and then runs the instance. The code after the call in `W` runs steps 7 and 8.
  - **`INDIRECT_JUMP`:** like `RETURN`, but the stub `jump_k` executes the instance. Its target, `target_k`, is a separate global function that runs `mov byte [PATH], 1`, captures as in step 7, and returns. This shape is why lifted code sees the stack pointer at `ret` equal to the stack pointer at entry.
  - **Table and sections.** The table header holds the magic `PS5LTAB1`, the instance count, and the slot, block and guard addresses, followed by the per-instance entries. `_start` calls `host_run_instructions(&table)`, then `host_exit(eax)`, then `ud2`. The block region is a 64 KiB-aligned `.bss` section: 60 KiB unused, then the 4 KiB block, then the 64 KiB guard.

- [ ] **Step 1: Write the failing tests:**
  - `test_emit_links`: the `base` group assembles and links with `PS5LIFT_CLANG` (`--target=x86_64-unknown-linux-gnu`) against a stub `libinstructionhost.so`.
  - `test_instance_bytes`: for every instance in the linked `base` program, the bytes at the instance address decode (with `ps5lift_decode`) to the form's mnemonic and the instance's length.
  - `test_layout_alignment`: the block's end is 64 KiB aligned and the slots are 64-byte aligned.
  - `test_case_inputs_resolve`: no `BlockOffset` or label remains after `case_inputs`.
- [ ] **Step 2: Run** `instructions_unit`: they fail.
- [ ] **Step 3: Implement** `program.py`.
- [ ] **Step 4: Run** `instructions_unit`: they pass.
- [ ] **Step 5: Commit** as `feat(ps5lift): emit the guest programs of the instruction tests`.

### Task 8: The native driver, the runner, the bundle and the first recording

**Files:**
- Create: `ps5lift/tests/instructions/NativeDriver.c`, `ps5lift/tests/instructions/record.py`, `ps5lift/tools/instructions/runner.py`, `ps5lift/tools/instructions/suite.py` (`bundle_command`, `import_command`).
- Modify: `ps5lift/tools/ps5lift.py` (adds the `instructions-bundle` and `instructions-import` subcommands).
- Create: `ps5lift/tests/instructions/recordings/*.results.xz`.
- Test: `ps5lift/tools/instructions/tests/test_runner.py`, `test_bundle.py`.

**Interfaces:**
- Consumes: Tasks 5–7.
- Produces:
  - **`NativeDriver.c`**, built as `libinstructionhost.so` for `x86_64-unknown-linux-gnu` (`-fPIC -shared`, libc). It exports `int host_run_instructions(const void* table)` and `void host_exit(int)`, and reads the environment variables `PS5LIFT_INSTRUCTION_INPUTS`, `PS5LIFT_INSTRUCTION_RESULTS` and `PS5LIFT_INSTRUCTION_FIRST`.
    - **Setup:** it makes the guard `PROT_NONE`, installs `sigaltstack`, and handles SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGTRAP.
    - **Faults.** `REG_TRAPNO` gives the outcome: 0 divide error, 3 breakpoint (address `RIP − 1`), 6 invalid opcode, 13 general protection, 14 page fault (data `si_addr`), 16 x87, 17 alignment check, 19 SIMD. Any other trap number is `HOST_SIGNAL` with the signal number as detail.
    - **Per case:** fill the block, apply the patches, copy the slot into both the input and output slots, then `sigsetjmp` and call the instance function. Afterwards set MXCSR to `0x1f80` and write the result record with `write(2)`.
  - **`runner.run(command, env, inputs_path, results_path, case_count, timeout)`.** It starts the program at `PS5LIFT_INSTRUCTION_FIRST`. When the program ends early, it appends a `CRASHED` record (exit code nonzero) or a `TIMED_OUT` record for the case after the last complete one, keeps the last 4 KiB of stderr, and restarts at the next case. It returns `{case: stderr}`.
  - **`ps5lift.py instructions-bundle --output DIR --clang C [--group G]…`.** For every set group except `sse4a`, it writes `DIR/<group>/` holding the guest ELF, its `.inputs` and `libinstructionhost.so`, plus `DIR/record.py`, `DIR/runner.py` and `DIR/groups.json`. `groups.json` lists the required `/proc/cpuinfo` flags for each group.
  - **`record.py [--group G]…`**, standard library only.
    - It refuses a group whose flags `/proc/cpuinfo` lacks, naming the flag.
    - Before the groups, `--self-test` runs a small built-in program (`mov rax, rbx`; `ud2`; `div` by zero; a load crossing into the guard; `int3`) and checks the five outcomes.
    - It writes `DIR/results/<group>.results`.
  - **`ps5lift.py instructions-import DIR`.** It checks every results file:
    - the header's fingerprints match the bundle's;
    - every case is present;
    - no outcome is `CRASHED`, `TIMED_OUT` or `HOST_SIGNAL`.

    Then it writes `ps5lift/tests/instructions/recordings/<group>.results.xz` with `lzma` preset `9 | PRESET_EXTREME` and prints the total size.

- [ ] **Step 1: Write the failing tests:**
  - `test_runner_resumes_after_crash`: a stub program (a Python script that writes cases 0 and 1, then exits with 70) makes the runner write a `CRASHED` record for case 2 and resume at 3.
  - `test_runner_records_timeout`.
  - `test_bundle_contents`: the `base` bundle has the ELF, the inputs, the driver and `groups.json` with `base: []`.
  - `test_missing_cpu_flag_refused`: `record.py` run with a fake cpuinfo that lacks `avx2` refuses `avx2`.
- [ ] **Step 2: Run** `instructions_unit`: they fail.
- [ ] **Step 3: Implement** the driver, the runner, the bundle, `record.py` and the import.
- [ ] **Step 4: Run** `instructions_unit`: it passes.
- [ ] **Step 5: Record.**
  1. Build a bundle on the aarch64 host.
  2. Copy it to the x86-64 host.
  3. Run `python3 record.py --self-test` there, which must pass, then `python3 record.py`.
  4. Copy `results/` back and run `instructions-import`.
  5. The total printed must be at most 24 MB.
- [ ] **Step 6: Commit** as `feat(ps5lift): record instruction cases on x86-64 hardware`. The code and the recordings go in separate commits; the recordings commit is `test(ps5lift): record the instruction cases on an Intel N100`.

### Task 9: The lifted driver, the comparator and `ps5lift.py instructions`

**Files:**
- Create: `ps5lift/tests/instructions/LiftedDriver.cpp`, `ps5lift/tools/instructions/compare.py`, `ps5lift/tools/instructions/tolerance.py`.
- Modify: `ps5lift/tools/instructions/suite.py` (`instructions_command`), `ps5lift/tools/ps5lift.py` (registers `instructions`).
- Test: `ps5lift/tools/instructions/tests/test_compare.py`, `test_tolerance.py`.

**Interfaces:**
- Consumes: `ps5lift_stop` and `ps5lift_set_stop_handler` (Task 2); `Ps5Lift::Bridge::CallGuest<void>`; Tasks 5–8.
- Produces:
  - **`LiftedDriver.cpp`** provides `ps5lift_import_host__run__instructions` and `ps5lift_import_host__exit`, using the same environment variables and records as the native driver.
    - **Per case:** save `state->Gpr[PS5LIFT_RSP]`, `sigsetjmp`, then `CallGuest<void>(function)`.
    - **On a stop:** the handler maps the runtime kind to an `Outcome` (address and detail as given) and `siglongjmp`s. After a stop the driver restores `Gpr[RSP]` and clears `Unwinding`.
    - **Signals:** during a case, SIGSEGV is a `PAGE_FAULT` at the instance address, with data `si_addr`, and SIGBUS is `HOST_SIGNAL` with detail `SIGBUS`, since an aarch64 alignment fault is no x86 exception. Outside a case they go to the previous handler.
    - **Console-defined cases:** the driver appends what `ps5lift_cpuid` and `ps5lift_xgetbv` give for the case's inputs.
  - **`compare.Rules`** (per form, from `rules_for(form)`):
    - `undefined_flags`;
    - `status_flags_dropped`, true for `CALL`, `RETURN` and `INDIRECT_JUMP` shapes;
    - `float_lanes`: written locations with their element types;
    - `tolerance`: one of `none`, `rcp`, `x87_transcendental`, `random_value` or `counter`.
  - **`compare.compare(spec, rules, expected, actual, float_mode) -> list[Mismatch]`.** It always ignores the storer's FOP, FIP, FDP and MXCSR_MASK, the reserved FXSAVE bytes, and the FXSAVE XMM area (the YMM slots compare instead).
  - **Fast mode adds exactly these allowances:**
    - a NaN in a float lane matches any NaN;
    - under DAZ, in `min*` and `max*`, a signed zero matches a denormal of the same sign;
    - MXCSR bits 0–5 are not compared;
    - an FTZ-only or DAZ-only case is compared with its twin's recorded result, apart from MXCSR's control bits;
    - with FTZ or DAZ set, ± the smallest normal matches a zero of the same sign;
    - a case whose initial MXCSR unmasks an exception expects `NEEDS_EXACT_FLOAT` at the loader `fxrstor`, and one whose `loads_mxcsr` unmasks one expects it at the instance.
  - **`tolerance`:**
    - `within_rcp(expected, actual, width)` allows a relative error of `1.5 * 2**-12` and requires exact zeros, infinities and NaNs;
    - `x87_reference(mnemonic, operands, precision, rounding)` computes the true result with `decimal` at 80 digits, rounded to the precision control;
    - `within_ulp(reference, actual, precision)` allows one unit in the last place inside the documented range and requires exact results outside it.
  - **Properties:**
    - `rdrand`/`rdseed`: CF 1, OF/SF/ZF/AF/PF 0, and the destination's untouched bits as recorded;
    - `rdtsc`/`rdtscp`: the counter does not decrease from one such case to the next in the same run, and the high halves are 0;
    - `rdpid` and `rdtscp`'s `ecx`: below the host's CPU count.
  - **`format_mismatch`** prints the Zydis text, the case's inputs, and the expected and actual value of each differing field.
  - **`ps5lift.py instructions --ps5lift P --clang C --clangxx CXX --triple T [--group G]… [--float exact|fast]… [--unoptimized-lift] [--keep]`.** It generates, links, lifts (with `--float` and `--cpu` as `lifter_target`, and `-O0` for `--unoptimized-lift`), links with `LiftedDriver.cpp` and the runtime, runs through `runner`, and compares against the recording.
    - It refuses a recording whose fingerprints differ: `record again: the guest program differs from the recording's` or `… the program's layout differs …`.
    - It prints one line per form (`PASS`, or the counts of mismatched, missing, stopped, crashed and timed-out cases), then the mismatches, at most 5 per form.
    - It exits 0 only when every case passes.

- [ ] **Step 1: Write the failing tests** in `test_compare.py`, on synthetic `Result`s:
  - `test_undefined_flags_ignored`
  - `test_status_flags_ignored_for_call_ret_indirect_jump`
  - `test_df_compared_for_call`
  - `test_fast_nan_any`
  - `test_fast_min_max_daz`
  - `test_fast_exception_flags_ignored`
  - `test_fast_flush_twin`
  - `test_fast_tininess`
  - `test_fast_unmask_stop_at_loader`
  - `test_fast_unmask_stop_at_ldmxcsr`
  - `test_exact_unmasked_is_simd_fault`
  - `test_storer_fields_ignored`
  - `test_rdrand_properties`
  - `test_rdtsc_properties`
  - `test_cpuid_console_values`
  - `test_stale_recording_refused`
  - `test_mismatch_message`: it contains the text, the inputs and both values.

  In `test_tolerance.py`:
  - `test_rcp_bound`, at exactly `1.5 * 2**-12` and just past it;
  - `test_fsin_reference`, for the known values sin(1) and sin(2^62);
  - `test_fsin_out_of_range_exact`.
- [ ] **Step 2: Run** `instructions_unit`: they fail.
- [ ] **Step 3: Implement** the driver, `compare.py`, `tolerance.py` and the command.
- [ ] **Step 4: Run** `instructions_unit`: it passes. Then run `python3 ps5lift/tools/ps5lift.py instructions --ps5lift ps5lift/build/lifter/ps5lift --clang … --clangxx … --group base --float exact`. It must run every case of `base` and print a line for every form; mismatches are expected and are not failures of this task.
- [ ] **Step 5: Commit** as `feat(ps5lift): run the instruction cases lifted and compare them with the recording`.

### Task 10: Expectations written from the manuals

**Files:**
- Create: `ps5lift/tools/instructions/expectations.py`.
- Modify: `compare.py`, which uses `expected_result`; `docs/dev/TechnicalDebt.md`.
- Test: `ps5lift/tools/instructions/tests/test_expectations.py`.

**Interfaces:**
- Consumes: `CaseSpec`, `Layout`, `Result`.
- Produces: `expected_result(spec, layout, recorded: Result | None) -> Result`. It returns the recording unless one of these rules applies:
  - **SSE4A** (`extrq` and `insertq` with immediate and register fields, `movntsd`, `movntss`): a full model from AMD APM volume 4. There is no recording, and `sse4a` is not bundled.
  - **`bsf`/`bsr` with a zero source:** the destination keeps all 64 bits of its input, and ZF is 1.
  - **x87 pointer fields** in `fxsave`, `fnstenv`, `fnsave` and `xsave` images: FIP, FDP and FOP are 0 unless an unmasked x87 exception is pending, in which case they are as recorded.
  - **MXCSR's MM bit (17):** `ldmxcsr`, `vldmxcsr`, `fxrstor` and `xrstor` accept it, so the result is the input state with MXCSR loaded. MXCSR_MASK in an `fxsave` or `xsave` image is `0x0002ffff`.
  - **66-prefixed near branches** (`jmp`, `call` and `jcc` rel16, and `ret`): the target is `(end + rel16) & 0xffff`, or the popped 16-bit value for `ret`. A taken branch is a page fault at that address, with data the same address; a not-taken `jcc` is a completed case with `PATH` 0. There is no recording.
  - **`dpps`/`dppd`:** both sides are canonicalized the way `sse41.c` does.
- [ ] **Step 1: Write the failing tests:**
  - `test_extrq_model` and `test_insertq_model`, with values worked from APM volume 4's definitions in the test;
  - `test_bsf_zero_source_keeps_destination`;
  - `test_fxsave_pointer_fields_zero_without_pending_exception`;
  - `test_ldmxcsr_mm_bit_accepted`;
  - `test_mxcsr_mask_amd`;
  - `test_jmp_rel16_faults_at_truncated_target`;
  - `test_jcc_rel16_not_taken_completes`;
  - `test_dpps_canonical_nan`.
- [ ] **Step 2: Run** `instructions_unit`: they fail.
- [ ] **Step 3: Implement** the rules, and add TechnicalDebt entries: the x87 transcendentals are compared within one ulp and are unverified on Zen 2; `bsf`/`bsr`, the pointer fields and MXCSR_MASK are written from AMD's manual and unmeasured on a PS5.
- [ ] **Step 4: Run** `instructions_unit`: it passes.
- [ ] **Step 5: Commit** as `feat(ps5lift): expect AMD's documented behaviour where it differs from the recording`.

### Task 11: Instructions outside the set (stage 2)

**Files:**
- Create: `ps5lift/lifter/include/lifter/InstructionSet.hpp`, `ps5lift/lifter/src/InstructionSet.cpp`.
- Modify:
  - `ps5lift/lifter/src/Lifter.cpp`: classify before `EmitSemantics`; the hint space; delete the `far branch` `UnsupportedInstruction`.
  - `ps5lift/lifter/include/lifter/Lifter.hpp` (`LiftReport`), `ps5lift/lifter/src/main.cpp`, `ps5lift/lifter/src/Compile.cpp:97`.
  - `ps5lift/runtime/include/ps5lift/State.h`, `ps5lift/runtime/src/Runtime.cpp` (`PS5LIFT_STOP_SEGMENT_TABLES = 6` and its message).
  - `cases.py` and `compare.py` (cases and expectations for the outside groups).
- Test: `test_outside.py`, and the `outside-*` groups through `ps5lift.py instructions`.

**Interfaces:**
- Produces:
  - `enum class InstructionGroup { Set, NotOnPs5, Privileged, SegmentTables, ConsoleKernel }`.
  - `InstructionGroup Ps5Lift::ClassifyInstruction(const ZydisDecodedInstruction&, const ZydisDecodedOperand*)`. It applies Task 3's rules in the same order, but to decoded data: the opcode map and opcode for the hint space, `meta.isa_ext` against the 37 extensions, `ZYDIS_ATTRIB_IS_PRIVILEGED`, the mnemonic lists, segment-register operands, and `ZYDIS_BRANCH_TYPE_FAR`. It is written apart from `database.py` on purpose: the outside groups' cases are the check that the two agree.
  - **The lifter's stops:** `NotOnPs5` is an invalid-opcode stop, `Privileged` general protection, `SegmentTables` its new stop, `ConsoleKernel` the console-kernel stop (with the vector for `int n`). Hint-space set instructions lift as no operation, or as the prefetch they are.
  - **`LiftReport`** keeps `Unsupported` (the missing translations) and gains `NotOnPs5`, `Privileged`, `SegmentTables` and `ConsoleKernel`, each a `std::map<std::string, std::size_t>`.
  - **`main.cpp` prints** `ps5lift: F functions, I instructions, U missing, N not on the PS5 CPU, P privileged, S segment-table, K console-kernel, T jump tables (…)`, then one line per mnemonic, such as `  missing <m>: n` and `  not on the PS5 CPU <m>: n`.
  - **Outside cases:** one instance per definition (`default_operands`) and one case, expecting the group's outcome at the instance address. They are never recorded.

- [ ] **Step 1: Write the failing tests:**
  - `test_outside_expectations`: `vaddps` (EVEX) expects `INVALID_OPCODE`, `wrmsr` `GENERAL_PROTECTION`, `lar` `SEGMENT_TABLES`, `syscall` `CONSOLE_KERNEL`.
  - Then run `python3 ps5lift/tools/ps5lift.py instructions … --group outside-not-on-ps5`: it fails, because the lifter reports these as missing.
- [ ] **Step 2: Implement** the classification, the stops and the report.
- [ ] **Step 3: Run** `ps5lift.py instructions` for all four `outside-*` groups, with `--float exact` and `--float fast`, with and without `--unoptimized-lift`. Every case passes. Then run `ctest --test-dir ps5lift/build -R "fixture_|lift_|instructions_unit"`: it passes.
- [ ] **Step 4: Commit** as `feat(ps5lift): stop as the console does on instructions outside the PS5 set`.

### Task 12: ctest, documentation and the first report

**Files:**
- Modify: `ps5lift/tests/CMakeLists.txt`, `docs/dev/PS5LIFT.md`, `docs/dev/PS5LIFT_DESIGN.md` (the Tests section), `docs/dev/TechnicalDebt.md`.

**Interfaces:**
- Produces:
  - `PS5LIFT_INSTRUCTION_GROUPS`, a CMake list. On Linux hosts, for each group, lift mode (`optimized`, `unoptimized`), float mode (`exact`, `fast`) and test triple, ctest gets a test named `instructions_<group>_<lift>_<float>_<arch>`.
  - The list starts with the four `outside-*` groups and every set group whose report shows every case passing in all four modes.
- [ ] **Step 1: Run the full report** on the aarch64 host: `ps5lift.py instructions` with every group, both float modes, and both lift modes. Save the output outside the repository; it is the work list for the stage 3 and 4 plans.
- [ ] **Step 2: Register** the passing groups. Run `ctest --test-dir ps5lift/build -R instructions_`: every registered test passes.
- [ ] **Step 3: Run the same `ctest`** on an x86-64 Linux host with ps5lift's build requirements (lifting for x86-64). Installing them on a machine is a system change and needs its owner's approval first.
- [ ] **Step 4: Document:**
  - in PS5LIFT.md: running the suite, `--group`, `--float`, and recording with `instructions-bundle`, `record.py` and `instructions-import`;
  - in PS5LIFT_DESIGN.md's Tests section: a link to the instruction-tests design;
  - in TechnicalDebt.md: the groups not yet registered, by name.
- [ ] **Step 5: Commit** as `test(ps5lift): register the instruction test groups that pass`.
