"""Write a test group's instances as one x86-64 guest program, read its layout back from the linked ELF, and turn case
specs into case inputs.

`emit` writes GNU assembly in AT&T syntax. Each instance is its bytes as `.byte` directives, its fields as expressions
(a relative field less the address of the instance's end), inside a function that runs it once:
- `ps5lift_instruction_load` saves the registers SysV preserves, loads the x87 and SSE state with `fxrstor` from the
  input slot (`LOADER_FXRSTOR` labels it), then ymm0 to ymm15 and every GPR but rax and rsp; `ps5lift_instruction_store`
  stores those GPRs, DF (the direction a `stosb` at a probe moves), the FXSAVE image and ymm0 to ymm15 to the output
  slot, clears DF and restores the saved registers.
- A straight instance's function saves the host's rsp, calls the loader, sets DF with `std` or `cld`, OF with an `add`
  and the other flags with `sahf`, loads rax, loads each register a fixup names with the target's address (`lea`), loads
  rsp, runs the instance, captures rax, rsp and the flags (`lahf`, `seto`), restores the host's rsp and calls the
  storer. A branch also records in PATH whether it was taken.
- A call's target is a function of its own that sets PATH and captures; a return and an indirect jump run in a stub the
  function calls with rsp 8 above the case's, so the return address lies where the case's rsp points, and an indirect
  jump's target is a function that sets PATH, captures and returns. Lifted code drops the status flags across those
  calls and returns, and keeps DF.
The table that `_start` hands `host_run_instructions` lists the slots, the block, the guard and each instance. The block
is the 4 KiB before a 64 KiB boundary in a 64 KiB-aligned `.bss` section, and the 64 KiB guard follows it.

`link` links the assembly at the image base its caller gives. `read_layout` reads the ELF's symbol table into a
`Layout`. `case_inputs` is the second phase of building cases: block offsets become addresses, and a fixup in the block
becomes a patch that holds its label's address; a register fixup is the program's own `lea`, so the register keeps the
0 its spec gives it. `fingerprints` are the SHA-256 digests of the assembly and of the inputs."""
import hashlib
import re
import struct
import subprocess
from collections import defaultdict, namedtuple
from dataclasses import dataclass
from pathlib import Path

from . import records
from .cases import BLOCK, RETURN_ADDRESS_SIZE, TARGET, BlockOffset, Shape, fixup_register
from .database import test_group

_PREFIX = "ps5lift_instruction"
TABLE = f"{_PREFIX}_table"
INPUT = f"{_PREFIX}_input"
OUTPUT = f"{_PREFIX}_output"
GUARD = f"{_PREFIX}_guard"
LOADER_FXRSTOR = f"{_PREFIX}_loader_fxrstor"
HOST_LIBRARY = "instructionhost"

_LOAD = f"{_PREFIX}_load"
_STORE = f"{_PREFIX}_store"
_HOST_RSP = f"{_PREFIX}_host_rsp"
_SAVED = f"{_PREFIX}_saved"
_PROBE = f"{_PREFIX}_probe"
_INSTANCE = "instance"
_END = "end"
_TARGET_HELPERS = {Shape.BRANCH: "taken", Shape.CALL: "callee", Shape.INDIRECT_JUMP: "target"}
_STUBS = {Shape.RETURN: "return", Shape.INDIRECT_JUMP: "jump"}
_INSTANCE_SYMBOL = re.compile(rf"{_PREFIX}_(\d+)(?:_([a-z]+))?")
_CALLEE_SAVED = ("rbx", "rbp", "r12", "r13", "r14", "r15")
_RAX = records.GPR_NAMES.index("rax")
_RSP = records.GPR_NAMES.index("rsp")
_LOADED = tuple(number for number in range(len(records.GPR_NAMES)) if number not in (_RAX, _RSP))
_FIELD_DIRECTIVES = {1: ".byte", 2: ".short", 4: ".long", 8: ".quad"}
_RECORD_DIRECTIVES = {"B": ".byte", "H": ".short", "I": ".long", "Q": ".quad"}
_ADDRESS_BYTES = 8
_INDENT = "    "

_ELF_IDENT = b"\x7fELF\x02\x01"
_ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
_SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
_SYMBOL = struct.Struct("<IBBHQQ")
_SYMBOL_TABLE = 2
_NO_BITS = 8
_ALLOCATED = 0x2
_Header = namedtuple(
    "_Header",
    "ident type machine version entry program_headers section_headers flags size program_header_size "
    "program_header_count section_header_size section_header_count section_names",
)
_Section = namedtuple("_Section", "name type flags address offset size link info alignment entry_size")


class ProgramError(Exception):
    pass


@dataclass(frozen=True)
class InstanceLayout:
    function: int
    address: int
    length: int
    helpers: dict

    def label(self, name):
        if name != TARGET:
            raise ProgramError(f"the instance at {self.address:#x} has no label {name!r}")
        found = [self.helpers[part] for part in _TARGET_HELPERS.values() if part in self.helpers]
        if len(found) != 1:
            raise ProgramError(f"the instance at {self.address:#x} has {len(found)} branch targets, not 1")
        return found[0]


@dataclass(frozen=True)
class Layout:
    table: int
    input: int
    output: int
    block: int
    guard: int
    loader_fxrstor: int
    instances: tuple


def _symbol(index, part=None):
    return f"{_PREFIX}_{index}" if part is None else f"{_PREFIX}_{index}_{part}"


def _input(offset):
    return f"{INPUT}+{offset}(%rip)"


def _output(offset):
    return f"{OUTPUT}+{offset}(%rip)"


def _function(name, body):
    return [f".globl {name}", f".type {name}, @function", f"{name}:", *body, f".size {name}, .-{name}"]


def _target(instance):
    if instance.shape not in _TARGET_HELPERS:
        raise ProgramError(f"{instance.form.key}: a {instance.shape.value} instance has no branch target")
    return _symbol(instance.index, _TARGET_HELPERS[instance.shape])


def _instance(instance):
    end = _symbol(instance.index, _END)
    lines = [f"{_symbol(instance.index, _INSTANCE)}:"]
    for piece in instance.encoded:
        if isinstance(piece, bytes):
            lines.append(".byte " + ", ".join(f"0x{byte:02x}" for byte in piece))
            continue
        if piece.size not in _FIELD_DIRECTIVES:
            raise ProgramError(f"{instance.form.key}: no directive for a {piece.size}-byte field")
        expression = _target(instance) if piece.expression == TARGET else piece.expression
        if piece.relative:
            expression = f"{expression} - {end}"
        lines.append(f"{_FIELD_DIRECTIVES[piece.size]} {expression}")
    lines.append(f"{end}:")
    return lines


def _enter():
    return [f"movq %rsp, {_HOST_RSP}(%rip)", f"call {_LOAD}"]


def _flags():
    return [
        f"cmpb $0, {_input(records.LOAD_DF)}",
        "je 1f",
        "std",
        "jmp 2f",
        "1:",
        "cld",
        "2:",
        f"movb {_input(records.LOAD_OF_LEFT)}, %al",
        f"addb {_input(records.LOAD_OF_RIGHT)}, %al",
        f"movb {_input(records.LOAD_AH)}, %ah",
        "sahf",
    ]


def _registers(instance):
    lines = [f"movq {_input(records.gpr_offset(_RAX))}, %rax"]
    register = fixup_register(instance)
    if register is not None:
        lines.append(f"leaq {_target(instance)}(%rip), %{register}")
    return lines


def _stack():
    return [f"movq {_input(records.gpr_offset(_RSP))}, %rsp"]


def _stub_stack():
    return [f"movq {_input(records.gpr_offset(_RSP))}, %rax", f"leaq {RETURN_ADDRESS_SIZE}(%rax), %rsp"]


def _capture():
    return [
        f"movq %rax, {_output(records.gpr_offset(_RAX))}",
        f"movq %rsp, {_output(records.gpr_offset(_RSP))}",
        "lahf",
        "seto %al",
        f"movw %ax, {_output(records.STORE_AX)}",
    ]


def _path(value):
    return [f"movb ${value}, {_output(records.PATH)}"]


def _finish():
    return [f"movq {_HOST_RSP}(%rip), %rsp", f"call {_STORE}", "ret"]


def _straight(instance):
    body = [*_enter(), *_flags(), *_registers(instance), *_stack(), *_instance(instance), *_capture(), *_finish()]
    return _function(_symbol(instance.index), body)


def _branch(instance):
    body = [
        *_enter(), *_flags(), *_registers(instance), *_stack(), *_instance(instance),
        *_path(0), "jmp 3f", f"{_target(instance)}:", *_path(1), "3:", *_capture(), *_finish(),
    ]
    return _function(_symbol(instance.index), body)


def _call(instance):
    body = [*_enter(), *_flags(), *_registers(instance), *_stack(), *_instance(instance), *_finish()]
    return _function(_symbol(instance.index), body) + _function(_target(instance), [*_path(1), *_capture(), "ret"])


def _stubbed(instance, after):
    stub = _symbol(instance.index, _STUBS[instance.shape])
    body = [*_enter(), *_stub_stack(), f"call {stub}", *after, *_finish()]
    stub_body = [*_flags(), *_registers(instance), *_instance(instance)]
    return _function(_symbol(instance.index), body) + _function(stub, stub_body)


def _return(instance):
    return _stubbed(instance, _capture())


def _indirect_jump(instance):
    return _stubbed(instance, []) + _function(_target(instance), [*_path(1), *_capture(), "ret"])


_SHAPES = {
    Shape.STRAIGHT: _straight,
    Shape.BRANCH: _branch,
    Shape.CALL: _call,
    Shape.RETURN: _return,
    Shape.INDIRECT_JUMP: _indirect_jump,
}


def _start():
    return _function("_start", [
        f"leaq {TABLE}(%rip), %rdi",
        "call host_run_instructions@PLT",
        "movl %eax, %edi",
        "call host_exit@PLT",
        "ud2",
    ])


def _load():
    body = [f"movq %{name}, {_SAVED}+{_ADDRESS_BYTES * number}(%rip)" for number, name in enumerate(_CALLEE_SAVED)]
    body += [f"{LOADER_FXRSTOR}:", f"fxrstor {_input(records.FX_OFFSET)}"]
    body += [f"vmovdqu {_input(records.ymm_offset(number))}, %ymm{number}" for number in range(records.YMM_COUNT)]
    body += [f"movq {_input(records.gpr_offset(number))}, %{records.GPR_NAMES[number]}" for number in _LOADED]
    return _function(_LOAD, [*body, "ret"])


def _store():
    body = [f"movq %{records.GPR_NAMES[number]}, {_output(records.gpr_offset(number))}" for number in _LOADED]
    body += [
        f"leaq {_PROBE}(%rip), %rdi",
        "stosb",
        f"leaq {_PROBE}(%rip), %rax",
        "cmpq %rax, %rdi",
        f"setb {_output(records.STORE_DF)}",
        "cld",
        f"fxsave {_output(records.FX_OFFSET)}",
    ]
    body += [f"vmovdqu %ymm{number}, {_output(records.ymm_offset(number))}" for number in range(records.YMM_COUNT)]
    body += [f"movq {_SAVED}+{_ADDRESS_BYTES * number}(%rip), %{name}" for number, name in enumerate(_CALLEE_SAVED)]
    return _function(_STORE, [*body, "ret"])


def _record(record, values):
    named = {field.name for field in record.fields if field.code != "x"}
    if named != set(values):
        raise ProgramError(f"{record.c_name} takes {sorted(named)}, not {sorted(values)}")
    lines = []
    for field in record.fields:
        if field.code == "x":
            lines.append(f".zero {field.count}")
        elif field.code == "s":
            text = values[field.name]
            if len(text) != field.count:
                raise ProgramError(f"{record.c_name}.{field.name} holds {field.count} bytes, not {text!r}")
            lines.append(f'.ascii "{text.decode()}"')
        else:
            lines.append(f"{_RECORD_DIRECTIVES[field.code]} {values[field.name]}")
    return lines


def _table(instances):
    header = {
        "Magic": records.TABLE_MAGIC,
        "Count": len(instances),
        "Input": INPUT,
        "Output": OUTPUT,
        "Block": BLOCK,
        "Guard": GUARD,
    }
    lines = [".section .rodata", ".balign 8", f".globl {TABLE}", f"{TABLE}:", *_record(records.TABLE_HEADER, header)]
    for instance in instances:
        start, end = _symbol(instance.index, _INSTANCE), _symbol(instance.index, _END)
        entry = {"Function": _symbol(instance.index), "Instance": start, "Length": f"{end} - {start}"}
        lines += _record(records.TABLE_ENTRY, entry)
    return lines


def _slots():
    lines = [".bss"]
    for name in (INPUT, OUTPUT):
        lines += [f".balign {records.SLOT_ALIGNMENT}", f".globl {name}", f"{name}:", f".zero {records.SLOT_SIZE}"]
    lines += [f".balign {_ADDRESS_BYTES}", f"{_HOST_RSP}:", f".zero {_ADDRESS_BYTES}"]
    lines += [f"{_SAVED}:", f".zero {_ADDRESS_BYTES * len(_CALLEE_SAVED)}", f"{_PROBE}:", ".zero 1"]
    return lines


def _block_region():
    return [
        f'.section .bss.{BLOCK},"aw",@nobits',
        f".balign {records.BLOCK_END_ALIGNMENT}",
        f".zero {records.BLOCK_END_ALIGNMENT - records.BLOCK_SIZE}",
        f".globl {BLOCK}",
        f"{BLOCK}:",
        f".zero {records.BLOCK_SIZE}",
        f".globl {GUARD}",
        f"{GUARD}:",
        f".zero {records.GUARD_SIZE}",
    ]


def _render(lines):
    return "".join(line + "\n" if line.endswith(":") else _INDENT + line + "\n" for line in lines)


def emit(group, instances):
    for position, instance in enumerate(instances):
        if test_group(instance.form) != group:
            raise ProgramError(f"{instance.form.key} belongs to {test_group(instance.form)}, not {group}")
        if instance.index != position:
            raise ProgramError(f"instance {instance.index} of {instance.form.key} is at position {position}")
    lines = [".text", *_start(), *_load(), *_store()]
    for instance in instances:
        lines += _SHAPES[instance.shape](instance)
    return _render(lines + _table(instances) + _slots() + _block_region())


def link(clang, source, work, sysroot, flags, image_base):
    cross = [f"--sysroot={sysroot}"] if sysroot else []
    elf = Path(work) / Path(source).stem
    command = [
        str(clang), *flags, *cross, "-no-pie", f"-Wl,--image-base={image_base:#x}", "-Wl,--build-id=none", str(source),
        f"-L{work}", f"-l{HOST_LIBRARY}", "-o", str(elf),
    ]
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise ProgramError(f"linking {source} failed with exit code {completed.returncode}:\n$ {' '.join(command)}\n"
                           f"{completed.stdout}{completed.stderr}")
    return elf


class Elf:
    def __init__(self, path):
        self.path = path
        self._data = Path(path).read_bytes()
        header = _Header(*_ELF_HEADER.unpack_from(self._data))
        if not header.ident.startswith(_ELF_IDENT):
            raise ProgramError(f"{path} is not a little-endian ELF64 file")
        size = header.section_header_size
        if size != _SECTION_HEADER.size:
            raise ProgramError(f"{path}: section headers are {size} bytes, not {_SECTION_HEADER.size}")
        self.sections = [
            _Section(*_SECTION_HEADER.unpack_from(self._data, header.section_headers + number * size))
            for number in range(header.section_header_count)
        ]
        self.symbols = self._symbols()

    def _symbols(self):
        tables = [section for section in self.sections if section.type == _SYMBOL_TABLE]
        if len(tables) != 1:
            raise ProgramError(f"{self.path} has {len(tables)} symbol tables, not 1")
        (table,) = tables
        if table.entry_size != _SYMBOL.size:
            raise ProgramError(f"{self.path}: symbols are {table.entry_size} bytes, not {_SYMBOL.size}")
        strings = self.sections[table.link]
        symbols = {}
        for offset in range(table.offset, table.offset + table.size, table.entry_size):
            name_offset, _, _, _, value, _ = _SYMBOL.unpack_from(self._data, offset)
            start = strings.offset + name_offset
            name = self._data[start:self._data.index(b"\0", start)].decode()
            if not name:
                continue
            if symbols.get(name, value) != value:
                raise ProgramError(f"{self.path} defines {name} at {symbols[name]:#x} and {value:#x}")
            symbols[name] = value
        return symbols

    def read(self, address, size):
        for section in self.sections:
            if section.type == _NO_BITS or not section.flags & _ALLOCATED:
                continue
            if section.address <= address and address + size <= section.address + section.size:
                start = section.offset + address - section.address
                return self._data[start:start + size]
        raise ProgramError(f"{self.path}: no section holds [{address:#x}, {address + size:#x})")


def _required(found, name, where):
    if name not in found:
        raise ProgramError(f"{where} has no symbol {name}")
    return found[name]


def read_layout(path):
    symbols = Elf(path).symbols
    parts = defaultdict(dict)
    for name, value in symbols.items():
        match = _INSTANCE_SYMBOL.fullmatch(name)
        if match is not None:
            parts[int(match.group(1))][match.group(2)] = value
    if sorted(parts) != list(range(len(parts))):
        raise ProgramError(f"{path} numbers its instances {sorted(parts)}, not 0 to {len(parts) - 1}")
    instances = []
    for index in range(len(parts)):
        helpers = dict(parts[index])
        where = f"{path}'s instance {index}"
        function = _required(helpers, None, where)
        start = _required(helpers, _INSTANCE, where)
        end = _required(helpers, _END, where)
        for part in (None, _INSTANCE, _END):
            del helpers[part]
        instances.append(InstanceLayout(function, start, end - start, helpers))
    return Layout(
        table=_required(symbols, TABLE, path),
        input=_required(symbols, INPUT, path),
        output=_required(symbols, OUTPUT, path),
        block=_required(symbols, BLOCK, path),
        guard=_required(symbols, GUARD, path),
        loader_fxrstor=_required(symbols, LOADER_FXRSTOR, path),
        instances=tuple(instances),
    )


def _address(layout, value):
    if isinstance(value, BlockOffset):
        return layout.block + value.offset
    if isinstance(value, int):
        return value
    raise ProgramError(f"no rule for a register value {value!r}")


def _put(slot, offset, data, size, what):
    if len(data) != size:
        raise ProgramError(f"{what} is {len(data)} bytes, not {size}")
    slot[offset:offset + size] = data


def _slot(layout, spec):
    what = f"case {spec.index}"
    if len(spec.vectors) != records.YMM_COUNT or len(spec.gprs) != len(records.GPR_NAMES):
        raise ProgramError(f"{what} gives {len(spec.vectors)} vectors and {len(spec.gprs)} GPRs")
    slot = bytearray(records.SLOT_SIZE)
    _put(slot, records.FX_OFFSET, spec.x87, records.FX_XMM, f"{what}'s x87 image")
    struct.pack_into("<I", slot, records.FX_OFFSET + records.FX_MXCSR, spec.mxcsr)
    for number, vector in enumerate(spec.vectors):
        _put(slot, records.ymm_offset(number), vector, records.YMM_STRIDE, f"{what}'s ymm{number}")
        _put(slot, records.fx_xmm_offset(number), vector[:records.FX_XMM_STRIDE], records.FX_XMM_STRIDE,
             f"{what}'s xmm{number}")
    for number, value in enumerate(spec.gprs):
        struct.pack_into("<Q", slot, records.gpr_offset(number), _address(layout, value))
    loads = (records.LOAD_AH, records.LOAD_OF_LEFT, records.LOAD_OF_RIGHT, records.LOAD_DF)
    for offset, value in zip(loads, records.load_bytes(spec.flags)):
        slot[offset] = value
    slot[records.PATH] = records.PATH_UNSET
    return bytes(slot)


def _patches(entry, spec):
    patches = list(spec.patches)
    for location, label in spec.fixups:
        address = entry.label(label)
        if isinstance(location, BlockOffset):
            patches.append((location.offset, address.to_bytes(_ADDRESS_BYTES, "little")))
        elif not isinstance(location, str):
            raise ProgramError(f"case {spec.index}: no rule for a fixup at {location!r}")
    patches.sort()
    end = 0
    for offset, data in patches:
        if offset < end or offset + len(data) > records.BLOCK_SIZE:
            raise ProgramError(f"case {spec.index}: a patch of {len(data)} bytes at block offset {offset} overlaps "
                               "another or leaves the block")
        end = offset + len(data)
    return tuple(patches)


def case_inputs(layout, specs):
    inputs = []
    for spec in specs:
        if not 0 <= spec.instance < len(layout.instances):
            raise ProgramError(f"case {spec.index} runs instance {spec.instance}, which the program does not have")
        entry = layout.instances[spec.instance]
        inputs.append(records.CaseInput(
            spec.index, spec.instance, spec.fill_seed, spec.console_defined, _slot(layout, spec), _patches(entry, spec),
        ))
    return inputs


def fingerprints(assembly, inputs_bytes):
    return hashlib.sha256(assembly.encode()).digest(), hashlib.sha256(inputs_bytes).digest()
