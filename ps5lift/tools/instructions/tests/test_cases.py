import bisect
import dataclasses
import os
import re
import struct
import subprocess
import tempfile
import unittest
from collections import defaultdict
from decimal import Decimal
from fractions import Fraction
from pathlib import Path

from instructions import cases, database, decoder, program, records, suite, values
from instructions.cases import BlockOffset
from instructions.encoding import Mem
from instructions.tests.support import fake_layout, find_form, generate, generated

CPUID_SOURCE = Path(__file__).resolve().parents[3] / "runtime" / "src" / "Cpuid.cpp"
RUNTIME_INCLUDE = Path(__file__).resolve().parents[3] / "runtime" / "include"
BUDGET = 24 * 1000 * 1000
CONSOLE_MXCSR = 0x9FC0
XSAVE_FAMILY = frozenset("xsave xsave64 xsavec xsavec64 xsaveopt xsaveopt64 xrstor xrstor64".split())
CONSOLE_XSAVE_SIZE = 0x340
NO_ACCESS_CATEGORIES = frozenset({"PREFETCH", "WIDENOP", "NOP", "CLDEMOTE", "PREFETCHWT1", "MPX"})
MEMORY_TYPES = frozenset({"mem", "mem_vsibx", "mem_vsiby", "moffs", "implicit_mem"})
X87_ELEMENTS = frozenset({"float80", "bcd80"})
X87_STATE_FLAGS = frozenset({"fpu_state_cr", "fpu_state_cw"})
X87_CONTROL_PAIRS = frozenset((precision, rounding) for precision in (0, 2, 3) for rounding in range(4))
FX_DEFAULT_FCW = 0x037F
MMX_PENDING_FCW = 0x037E
MMX_PENDING_FSW = 0x8081
STACK_MARGIN = 512
IMPLICIT_BASES = {"si": 6, "di": 7, "bx": 3, "bp": 5, "sp": 4}
MASK64 = 2**64 - 1
NEAR_BRANCHES = frozenset(
    "jmp call ret jo jno jb jnb jz jnz jbe jnbe js jns jp jnp jl jnl jle jnle".split()
)
SSE4A_FORMS = 6
AMD_ONLY_GROUPS = ("sse4a", "monitorx", "clzero")
CACHE_LINE = 64
MISALIGNED_SSE_LOAD = CONSOLE_MXCSR | 1 << 17
MXCSR_INITIAL = 0x1F80
COMPACTED = 1 << 63
XSAVE_HEADER = 512
XSAVE_IMAGE = 576
TRIGONOMETRY = ("fsin", "fcos", "fsincos", "fptan")
REDUCTION_MULTIPLES = (1, 2, 3, 4, 5, 6, 7, 8, 1000, 2**20, 2**40, 2**62)
PI = Decimal(
    "3.1415926535897932384626433832795028841971693993751058209749445923078164062862089986280348253421170679"
)
EXTENDED_BIAS = 16383
EXTENDED_TOP_EXPONENT = 0x7FFF
EXTENDED_INTEGER_BIT = 1 << 63
X87_PRECISION_AND_ROUNDING = 0xF00


def _ieee_edges(bits, exponent_bits):
    fraction = bits - 1 - exponent_bits
    sign = 1 << bits - 1
    smallest_normal = 1 << fraction
    largest_normal = ((1 << exponent_bits) - 2) << fraction | (1 << fraction) - 1
    infinity = ((1 << exponent_bits) - 1) << fraction
    one = ((1 << exponent_bits - 1) - 1) << fraction
    magnitudes = (0, 1, smallest_normal - 1, smallest_normal, largest_normal, infinity)
    return {value | negative for value in magnitudes for negative in (0, sign)} | {one}


def _is_nan(value, bits, exponent_bits, quiet):
    fraction = bits - 1 - exponent_bits
    exponent = value >> fraction & (1 << exponent_bits) - 1
    payload = value & (1 << fraction - 1) - 1
    return exponent == (1 << exponent_bits) - 1 and bool(value >> fraction - 1 & 1) == quiet and payload != 0


def _integer_edges(bits):
    required = {0, 1, (1 << bits) - 1, 1 << bits - 1, (1 << bits - 1) - 1}
    for narrower in (8, 16, 32):
        if narrower < bits:
            required |= {(1 << narrower) - 1, 1 << narrower - 1, (1 << narrower - 1) - 1}
    return required


def _patched(spec, offset, size):
    image = bytearray(size)
    covered = set()
    for start, data in spec.patches:
        for position in range(max(start, offset), min(start + len(data), offset + size)):
            image[position - offset] = data[position - start]
            covered.add(position)
    if len(covered) != size:
        raise AssertionError(f"case {spec.index}'s patches leave {size - len(covered)} bytes at {offset} unset")
    return bytes(image)


def _lane(vector, lane, bits):
    return int.from_bytes(vector, "little") >> lane * bits & (1 << bits) - 1


def _resolve(value):
    return value.offset if isinstance(value, BlockOffset) else value


def _displacement(displacement):
    if isinstance(displacement, int):
        return displacement
    match = re.fullmatch(rf"{cases.BLOCK}\s*\+\s*(\d+)", displacement)
    if match is None:
        raise AssertionError(f"no block offset in {displacement!r}")
    return int(match.group(1))


def _accesses_memory(form):
    definition = form.definition
    return definition.category not in NO_ACCESS_CATEGORIES and any(
        operand["operand_type"] in MEMORY_TYPES for operand in definition.operands
    )


def _is_mmx(form):
    definition = form.definition
    return definition.extension == "MMX" or any(operand["operand_type"] == "mmx" for operand in definition.operands)


def _is_x87(form):
    if _is_mmx(form):
        return False
    definition = form.definition
    elements = {operand.get("element_type") for operand in definition.operands}
    return definition.extension == "X87" or bool(elements & X87_ELEMENTS) or bool(definition.flags & X87_STATE_FLAGS)


def _precision_and_rounding(spec):
    return struct.unpack_from("<H", spec.x87, records.FX_FCW)[0] & X87_PRECISION_AND_ROUNDING


def _st0(spec):
    offset = records.fx_st_offset(0)
    return int.from_bytes(spec.x87[offset:offset + 10], "little")


def _positive_normal_extended(bits):
    exponent = bits >> 64
    significand = bits & MASK64
    if not 0 < exponent < EXTENDED_TOP_EXPONENT or not significand & EXTENDED_INTEGER_BIT:
        return None
    return Fraction(significand) * Fraction(2) ** (exponent - EXTENDED_BIAS - 63)


def _extended_ordinal(bits):
    return bits >> 64 << 63 | bits & EXTENDED_INTEGER_BIT - 1


def _form_schedules(instances, group, group_specs):
    schedules = defaultdict(list)
    for spec in group_specs:
        schedules[instances[group][spec.instance].form.key].append(spec)
    return schedules


def _set_groups(instances, specs):
    return [(group, group_specs) for group, group_specs in specs.items()
            if database.classify(instances[group][0].form.definition) is database.Group.SET]


def _operand_size(instance):
    return instance.form.operand_size or 64


def _access_ranges(instance, spec):
    definition = instance.form.definition
    gprs = spec.gprs
    explicit = iter(instance.operands)
    ranges = []
    for operand in definition.operands:
        choice = next(explicit) if "encoding" in operand else None
        operand_type = operand["operand_type"]
        width = operand.get(f"width{_operand_size(instance)}")
        if definition.mnemonic in XSAVE_FAMILY:
            width = CONSOLE_XSAVE_SIZE
        if operand_type in ("mem", "mem_vsibx", "mem_vsiby", "moffs") and isinstance(choice, Mem):
            if choice.base == "rip" or choice.base is None:
                start = _displacement(choice.displacement)
            else:
                start = _resolve(gprs[records.GPR_NAMES.index(choice.base)]) + _displacement(choice.displacement)
            if choice.index is not None and operand_type == "mem":
                start += gprs[records.GPR_NAMES.index(choice.index)] * choice.scale
            if operand_type != "mem":
                if choice.index is not None:
                    lane_bits = 32 if definition.mnemonic[len("v"):].startswith(("gatherd", "pgatherd")) else 64
                    index = _lane(spec.vectors[int(choice.index[3:])], 0, lane_bits)
                    start += (index - (1 << lane_bits) if index >> lane_bits - 1 else index) * choice.scale
            ranges.append((start & MASK64, (start + width) & MASK64, True))
        elif operand_type == "implicit_mem":
            base = operand["mem_base"]
            address = _resolve(gprs[IMPLICIT_BASES[base]])
            if base == "bx":
                address += gprs[0] & 0xFF
            if base == "sp" and "write" in operand.get("action", ""):
                address -= width
            ranges.append((address, address + width, False))
    return ranges


class CasesTest(unittest.TestCase):
    def form(self, mnemonic, operand_size=None, **conditions):
        return find_form(self, generated()[0], mnemonic, operand_size, **conditions)

    def instances_of(self, form):
        group = database.test_group(form)
        return [instance for instance in generated()[1][group] if instance.form == form]

    def specs_of(self, form):
        indexes = {instance.index for instance in self.instances_of(form)}
        return [spec for spec in generated()[2][database.test_group(form)] if spec.instance in indexes]

    def primary_specs(self, form):
        primaries = {instance.index for instance in self.instances_of(form) if instance.role == "primary"}
        return [spec for spec in self.specs_of(form) if spec.instance in primaries]

    def test_deterministic(self):
        forms, first_instances, first_specs = generated()
        _, second_instances, second_specs = generate()
        self.assertEqual(first_instances, second_instances)
        self.assertEqual(first_specs, second_specs)

    def test_selected_groups_generate_as_in_the_whole_suite(self):
        _, every_instance, every_spec = generated()
        selected = ("base", "x87", "hint", "outside-privileged")
        _, instances, specs = suite.generate(decoder.from_environment(), selected)
        self.assertEqual(sorted(instances), sorted(selected))
        self.assertEqual(sorted(specs), sorted(selected))
        for group in selected:
            with self.subTest(group=group):
                self.assertEqual(program.emit(group, instances[group]), program.emit(group, every_instance[group]))
                layout = fake_layout(every_instance[group])
                self.assertEqual(records.pack_input_records(program.case_inputs(layout, specs[group])),
                                 records.pack_input_records(program.case_inputs(layout, every_spec[group])))

    def test_every_form_has_cases(self):
        forms, instances, specs = generated()
        with_cases = set()
        for group, group_specs in specs.items():
            for spec in group_specs:
                with_cases.add(instances[group][spec.instance].form.key)
        missing = [form.key for form in forms if form.key not in with_cases]
        self.assertEqual(missing, [])

    def assert_covers(self, seen, required, what):
        self.assertEqual(sorted(hex(value) for value in required - seen), [], f"{what} misses edge values")

    def test_edge_values_covered(self):
        add = self.form("add", 64, opcode=0x03, modrm_mod="3")
        add_specs = self.primary_specs(add)
        (instance,) = [instance for instance in self.instances_of(add) if instance.role == "primary"]
        for operand in instance.operands:
            seen = {spec.gprs[records.GPR_NAMES.index(operand.name)] for spec in add_specs}
            self.assert_covers(seen, _integer_edges(64), f"add {operand.name}")
            self.assert_covers(seen, set(values.integer_edges(64)), f"add {operand.name}")

        addps = self.form("addps", encoding=None, modrm_mod="3")
        (instance,) = [instance for instance in self.instances_of(addps) if instance.role == "primary"]
        addps_specs = self.primary_specs(addps)
        for operand in instance.operands:
            number = int(operand.name[len("xmm"):])
            for lane in range(4):
                seen = {_lane(spec.vectors[number], lane, 32) for spec in addps_specs}
                self.assert_covers(seen, _ieee_edges(32, 8), f"addps {operand.name} lane {lane}")
                self.assert_covers(seen, set(values.float_edges(32)), f"addps {operand.name} lane {lane}")
                self.assertTrue(any(_is_nan(value, 32, 8, True) for value in seen))
                self.assertTrue(any(_is_nan(value, 32, 8, False) for value in seen))

        cmpxchg = self.form("cmpxchg", 64, opcode=0xB1, modrm_mod="3")
        (instance,) = [instance for instance in self.instances_of(cmpxchg) if instance.role == "primary"]
        cmpxchg_specs = self.primary_specs(cmpxchg)
        for name in [operand.name for operand in instance.operands] + ["rax"]:
            seen = {spec.gprs[records.GPR_NAMES.index(name)] for spec in cmpxchg_specs}
            self.assert_covers(seen, _integer_edges(64), f"cmpxchg {name}")
            self.assert_covers(seen, set(values.integer_edges(64)), f"cmpxchg {name}")
        destination = records.GPR_NAMES.index(instance.operands[0].name)
        self.assertTrue(any(spec.gprs[0] == spec.gprs[destination] for spec in cmpxchg_specs), "no equal compare")

        cvttss2si = self.form("cvttss2si", modrm_mod="3", rex_w="0")
        (instance,) = [instance for instance in self.instances_of(cvttss2si) if instance.role == "primary"]
        source = int(instance.operands[1].name[len("xmm"):])
        seen = {_lane(spec.vectors[source], 0, 32) for spec in self.primary_specs(cvttss2si)}
        self.assert_covers(seen, _ieee_edges(32, 8), "cvttss2si source")
        self.assert_covers(seen, set(values.float_edges(32)), "cvttss2si source")
        self.assertIn(0x4F000000, seen, "cvttss2si misses 2^31, the first float out of int32's range")
        self.assertIn(0x4EFFFFFF, seen, "cvttss2si misses the largest float inside int32's range")

    def test_fp_mxcsr_states(self):
        addps = self.form("addps", encoding=None, modrm_mod="3")
        states = {spec.mxcsr for spec in self.primary_specs(addps)}
        expected = {0x9FC0, 0x1F80, 0x3F80, 0x5F80, 0x7F80, 0x9F80, 0x1FC0}
        expected |= {0x1F80 & ~(1 << bit) for bit in range(7, 13)}
        self.assertEqual(len(expected), 13)
        self.assertEqual(states, expected)
        for mnemonic in ("stmxcsr", "vstmxcsr"):
            reads_mxcsr = self.form(mnemonic)
            self.assertEqual({spec.mxcsr for spec in self.primary_specs(reads_mxcsr)}, expected, mnemonic)
        add = self.form("add", 64, opcode=0x03, modrm_mod="3")
        self.assertEqual({spec.mxcsr for spec in self.specs_of(add)}, {CONSOLE_MXCSR})

    def test_fast_twins(self):
        _, _, specs = generated()
        twins = 0
        for group_specs in specs.values():
            for spec in group_specs:
                flush_alone = spec.mxcsr in (0x9F80, 0x1FC0)
                self.assertEqual(spec.twin is not None, flush_alone, f"case {spec.index} under {spec.mxcsr:#x}")
                if spec.twin is None:
                    continue
                twin = group_specs[spec.twin]
                twins += 1
                self.assertEqual(twin.mxcsr, CONSOLE_MXCSR)
                for name in ("instance", "gprs", "vectors", "flags", "x87", "patches", "fixups", "fill_seed"):
                    self.assertEqual(getattr(twin, name), getattr(spec, name), f"case {spec.index}'s twin {name}")
        self.assertGreater(twins, 0)

    def test_crossing_case_per_memory_form(self):
        forms, instances, specs = generated()
        crossing, straddling = set(), set()
        for group, group_specs in _set_groups(instances, specs):
            for spec in group_specs:
                instance = instances[group][spec.instance]
                aligned = "REQUIRES_ALIGNMENT" in instance.form.definition.xed_attributes
                for start, end, explicit in _access_ranges(instance, spec):
                    if aligned and explicit:
                        if start == records.BLOCK_SIZE:
                            crossing.add(instance.form.key)
                        if start < records.BLOCK_SIZE < end:
                            straddling.add(instance.form.key)
                    elif end == records.BLOCK_SIZE + 1:
                        crossing.add(instance.form.key)
        returns = {form.key for form in forms if form.definition.category == "RET"}
        missing = [form.key for form in forms if _accesses_memory(form) and form.key not in crossing | returns]
        self.assertEqual(missing, [])
        self.assertEqual(sorted(straddling), [], "an access that must be aligned cannot cross a page")
        self.assertGreater(len({key for key in crossing if "REQUIRES_ALIGNMENT" in self.xed(key)}), 0)

    def xed(self, key):
        return next(form for form in generated()[0] if form.key == key).definition.xed_attributes

    def test_return_has_no_memory_cases(self):
        _, instances, specs = generated()
        checked = 0
        for group, group_specs in specs.items():
            for spec in group_specs:
                instance = instances[group][spec.instance]
                if instance.shape is not cases.Shape.RETURN:
                    continue
                for start, end, _ in _access_ranges(instance, spec):
                    checked += 1
                    within = STACK_MARGIN <= start and end <= records.BLOCK_SIZE - STACK_MARGIN and start % 8 == 0
                    self.assertTrue(within, f"case {spec.index} of {instance.form.key} reads [{start}, {end})")
        self.assertGreater(checked, 0)

    def test_indirect_jump_operands_clear_the_return_slot(self):
        _, instances, specs = generated()
        checked = set()
        for group, group_specs in specs.items():
            for spec in group_specs:
                instance = instances[group][spec.instance]
                if instance.shape is not cases.Shape.INDIRECT_JUMP:
                    continue
                return_slot = _resolve(spec.gprs[records.GPR_NAMES.index("rsp")])
                for start, end, _ in _access_ranges(instance, spec):
                    self.assertFalse(
                        start < return_slot + 8 and return_slot < end,
                        f"case {spec.index} of {instance.text} reads [{start}, {end}), "
                        f"over the return address at {return_slot}",
                    )
                    checked.add(instance.operands[0].base)
        self.assertIn("rsp", checked)

    def test_x87_base_cases_cover_the_cycle(self):
        forms, instances, specs = generated()
        checked = 0
        for group, group_specs in _set_groups(instances, specs):
            for key, schedule in _form_schedules(instances, group, group_specs).items():
                if not _is_x87(instances[group][schedule[0].instance].form):
                    continue
                checked += 1
                states = [struct.unpack_from("<HH", spec.x87, 0) for spec in schedule[:len(X87_CONTROL_PAIRS)]]
                self.assertEqual({status >> 11 & 7 for _, status in states}, set(range(8)), key)
                controls = {(control >> 8 & 3, control >> 10 & 3) for control, _ in states}
                self.assertEqual(controls, X87_CONTROL_PAIRS, key)
        self.assertGreater(checked, 0)

    def test_mmx_forms_run_one_pending_case(self):
        forms, instances, specs = generated()
        checked = 0
        for group, group_specs in _set_groups(instances, specs):
            for key, schedule in _form_schedules(instances, group, group_specs).items():
                if not _is_mmx(instances[group][schedule[0].instance].form):
                    continue
                checked += 1
                states = [struct.unpack_from("<HH", spec.x87, records.FX_FCW) for spec in schedule]
                others = [state for state in states if state != (FX_DEFAULT_FCW, 0)]
                self.assertEqual(others, [(MMX_PENDING_FCW, MMX_PENDING_FSW)], key)
        self.assertGreater(checked, 0)

    def test_trigonometry_brackets_multiples_of_half_pi(self):
        for mnemonic in TRIGONOMETRY:
            schedule = self.specs_of(self.form(mnemonic))
            first = _precision_and_rounding(schedule[0])
            operands = {}
            for spec in schedule:
                bits = _st0(spec)
                value = _positive_normal_extended(bits)
                if value is not None and _precision_and_rounding(spec) == first:
                    operands[value] = bits
            ordered = sorted(operands)
            for multiple in REDUCTION_MULTIPLES:
                position = bisect.bisect(ordered, Fraction(PI) * multiple / 2)
                with self.subTest(mnemonic=mnemonic, multiple=multiple):
                    self.assertTrue(0 < position < len(ordered), "no operands either side")
                    below, above = operands[ordered[position - 1]], operands[ordered[position]]
                    self.assertEqual(_extended_ordinal(above) - _extended_ordinal(below), 1,
                                     f"{below:#x} and {above:#x} are not adjacent")

    def test_unknown_implicit_register_names_the_form(self):
        cpuid = self.form("cpuid")
        unknown = {"operand_type": "implicit_reg", "register": "xyz", "visible": False}
        definition = dataclasses.replace(cpuid.definition, operands=cpuid.definition.operands + (unknown,))
        form = database.Form(definition, cpuid.operand_size, cpuid.key)
        with self.assertRaisesRegex(cases.CaseError, rf"^{re.escape(cpuid.key)}: .*'xyz'"):
            cases.instances([form], decoder.from_environment())

    def test_xsave_masks_within_console_components(self):
        forms, instances, specs = generated()
        checked = 0
        for group, group_specs in specs.items():
            for spec in group_specs:
                if instances[group][spec.instance].form.definition.mnemonic in XSAVE_FAMILY:
                    mask = (spec.gprs[2] & 0xFFFFFFFF) << 32 | spec.gprs[0] & 0xFFFFFFFF
                    self.assertEqual(mask & ~7, 0, f"case {spec.index} saves components {mask:#x}")
                    checked += 1
        self.assertGreater(checked, 0)

    def test_cpuid_leaves_have_ps5_values(self):
        compiler = os.environ.get("PS5LIFT_CLANG")
        self.assertIsNotNone(compiler, "PS5LIFT_CLANG is not set: it names the clang executable")
        forms, instances, specs = generated()
        queries = []
        for group, group_specs in specs.items():
            for spec in group_specs:
                mnemonic = instances[group][spec.instance].form.definition.mnemonic
                self.assertEqual(spec.console_defined, mnemonic in ("cpuid", "xgetbv"), f"case {spec.index}")
                if mnemonic == "cpuid":
                    queries.append(f"c {spec.gprs[0] & 0xFFFFFFFF} {spec.gprs[1] & 0xFFFFFFFF}")
                elif mnemonic == "xgetbv":
                    queries.append(f"x {spec.gprs[1] & 0xFFFFFFFF} 0")
        self.assertTrue(any(query.startswith("c ") for query in queries))
        self.assertTrue(any(query.startswith("x ") for query in queries))
        driver = (
            "#include <ps5lift/State.h>\n#include <cstdarg>\n#include <cstdio>\n#include <cstdlib>\n"
            "extern \"C\" void ps5lift_fail(const char* format, ...) {\n"
            "    va_list arguments;\n    va_start(arguments, format);\n    std::vfprintf(stderr, format, arguments);\n"
            "    va_end(arguments);\n    std::fputc('\\n', stderr);\n    std::exit(70);\n}\n"
            "int main() {\n    char kind;\n    unsigned first, second;\n"
            "    while (std::scanf(\" %c %u %u\", &kind, &first, &second) == 3) {\n"
            "        if (kind == 'c') {\n            Ps5LiftState state{};\n"
            "            state.Gpr[PS5LIFT_RAX] = first;\n            state.Gpr[PS5LIFT_RCX] = second;\n"
            "            ps5lift_cpuid(&state, 0);\n"
            "        } else {\n            ps5lift_xgetbv(0, first);\n        }\n"
            "    }\n    return 0;\n}\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "queries.cpp"
            source.write_text(driver)
            executable = Path(directory) / "queries"
            subprocess.run(
                [compiler, "-x", "c++", "-std=c++20", "-fno-exceptions", "-I", RUNTIME_INCLUDE, "-o", executable,
                 source, CPUID_SOURCE],
                check=True,
            )
            completed = subprocess.run([executable], input="\n".join(queries) + "\n", capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_projected_recording_size(self):
        _, instances, specs = generated()
        recorded = [
            spec for group, group_specs in specs.items() for spec in group_specs
            if not cases.expectation_written(instances[group][spec.instance].form)
        ]
        self.assertLessEqual(cases.projected_recording_bytes(recorded), BUDGET)

    def test_written_expectations(self):
        forms = generated()[0]
        written = {form.key for form in forms if cases.expectation_written(form)}
        sse4a = {form.key for form in forms if database.test_group(form) == "sse4a"}
        amd_only = {form.key for form in forms if database.test_group(form) in AMD_ONLY_GROUPS}
        branches = {
            form.key for form in forms
            if form.definition.mnemonic in NEAR_BRANCHES and form.operand_size == 16
        }
        self.assertEqual(written, amd_only | branches)
        self.assertEqual(len(sse4a), SSE4A_FORMS)
        self.assertEqual({key.split(".")[0] for key in amd_only - sse4a}, {"monitorx", "mwaitx", "clzero"})
        self.assertTrue({"jmp", "call", "jz"} <= {key.split(".")[0] for key in branches}, sorted(branches))
        self.assertIn(self.form("jmp", 16, feature_amd="1").key, written)
        self.assertNotIn(self.form("jmp", 64, feature_amd="1").key, written)
        self.assertNotIn(self.form("ret", 64, opcode=0xC3).key, written)

    def test_every_outside_form_is_written(self):
        every = database.forms(database.load())
        outside = [form for form in every if database.classify(form.definition) is not database.Group.SET]
        self.assertLessEqual({"vaddps", "wrmsr", "lar", "syscall"}, {form.definition.mnemonic for form in outside})
        self.assertEqual([form.key for form in outside if not cases.expectation_written(form)], [])

    def test_clzero_points_rax_into_the_block(self):
        specs = self.specs_of(self.form("clzero"))
        offsets = [spec.gprs[records.GPR_NAMES.index("rax")] for spec in specs]
        self.assertTrue(all(isinstance(offset, BlockOffset) for offset in offsets), offsets)
        inside = [offset.offset for offset in offsets if offset.offset < records.BLOCK_SIZE]
        self.assertEqual([offset.offset for offset in offsets if offset.offset >= records.BLOCK_SIZE],
                         [records.BLOCK_SIZE])
        self.assertTrue(all(0 <= offset for offset in inside))
        self.assertTrue(any(offset % CACHE_LINE for offset in inside))
        self.assertIn(records.BLOCK_SIZE - CACHE_LINE, {offset - offset % CACHE_LINE for offset in inside})

    def _xrstor_images(self):
        _, instances, specs = generated()
        for spec in specs["xsave"]:
            instance = instances["xsave"][spec.instance]
            if instance.form.definition.mnemonic not in ("xrstor", "xrstor64"):
                continue
            start = _access_ranges(instance, spec)[0][0]
            if start + XSAVE_IMAGE <= records.BLOCK_SIZE:
                yield instance, spec, _patched(spec, start, XSAVE_IMAGE)

    def test_xrstor_loads_misaligned_sse_in_the_standard_format(self):
        loads = defaultdict(list)
        for instance, spec, image in self._xrstor_images():
            if spec.loads_mxcsr == MISALIGNED_SSE_LOAD:
                loads[instance.form.key].append((spec, image))
        self.assertEqual(len(loads), 2, sorted(loads))
        for key, found in loads.items():
            self.assertEqual(len(found), 1, key)
            ((spec, image),) = found
            self.assertEqual(spec.gprs[0] & 0xFFFFFFFF, 7, key)
            self.assertEqual(spec.gprs[2] & 0xFFFFFFFF, 0, key)
            self.assertEqual(struct.unpack_from("<I", image, records.FX_MXCSR)[0], MISALIGNED_SSE_LOAD, key)
            self.assertEqual(struct.unpack_from("<QQ", image, XSAVE_HEADER), (7, 0), key)

    def test_xrstor_loads_mxcsr_by_format(self):
        seen = set()
        for _, spec, image in self._xrstor_images():
            requested = spec.gprs[0] & 7
            present, compaction = struct.unpack_from("<QQ", image, XSAVE_HEADER)
            mxcsr = struct.unpack_from("<I", image, records.FX_MXCSR)[0]
            if not compaction & COMPACTED:
                expected = mxcsr if requested & 6 else None
            elif requested & 2:
                expected = mxcsr if present & 2 else MXCSR_INITIAL
            else:
                expected = None
            self.assertEqual(spec.loads_mxcsr, expected, f"case {spec.index}")
            seen.add((bool(compaction & COMPACTED), expected is None, expected == MXCSR_INITIAL))
        self.assertLessEqual({(False, False, False), (False, True, False), (True, True, False), (True, False, True),
                              (True, False, False)}, seen)


if __name__ == "__main__":
    unittest.main()
