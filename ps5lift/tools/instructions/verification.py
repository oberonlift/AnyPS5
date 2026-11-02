"""Check encodings by decoding them with the lifter's Zydis settings.

`verify_all` decodes a batch of (form, operands, prefixes, encoded) instances with one `ps5lift_decode` call. It raises
`EncodingError` naming each form whose bytes decode to anything but that form with those operands and prefixes:
- the length, mnemonic and operand width;
- every visible operand: a register, an immediate (0 for a branch field), or memory with its base, index, scale,
  displacement (0 for a field) and displacement size;
- the prefixes Zydis's formatter prints, from the last f2/f3 byte and the definition's prefix flags.
`verify` checks one instance the same way."""
from dataclasses import fields

from .decoder import RejectedEncoding
from .encoding import (
    EncodingError,
    Imm,
    Mem,
    Reg,
    carried_prefixes,
    effective_sizes,
    implicit_register,
    placeholder,
)

_GROUP1_WORDS = {
    0xF2: (
        ("accepts_repnerepnz", "repne", False),
        ("accepts_xacquire", "xacquire", True),
        ("accepts_bound", "bnd", False),
    ),
    0xF3: (
        ("accepts_rep", "rep", False),
        ("accepts_reperepz", "repe", False),
        ("accepts_xrelease", "xrelease", True),
    ),
}
_PRINTED_PREFIXES = frozenset({"xacquire", "xrelease", "lock", "bnd", "notrack", "rep", "repe", "repne"})


def _implicit_choice(form, operand, operand_size, address_size):
    operand_type = operand["operand_type"]
    if operand_type == "implicit_imm1":
        return Imm(1)
    if operand_type != "implicit_reg":
        raise EncodingError(f"{form.key}: no rule for a visible {operand_type} operand")
    return Reg(implicit_register(form, operand["register"], operand_size, address_size))


def _expected_operands(form, operands):
    operand_size, address_size = effective_sizes(form, operands)
    choices = iter(operands)
    expected = []
    for operand in form.definition.operands:
        if "encoding" in operand:
            expected.append(next(choices))
        elif operand.get("visible", True):
            expected.append(_implicit_choice(form, operand, operand_size, address_size))
    return expected


def _memory_differences(expected, decoded):
    wanted = {
        "base": expected.base or "none",
        "index": expected.index or "none",
        "scale": expected.scale if expected.index else 0,
        "displacement": expected.displacement if isinstance(expected.displacement, int) else 0,
        "displacement size": expected.displacement_size,
    }
    got = {
        "base": decoded.base,
        "index": decoded.index,
        "scale": decoded.scale,
        "displacement": decoded.displacement,
        "displacement size": decoded.displacement_size,
    }
    return [f"{name} {got[name]}, not {value}" for name, value in wanted.items() if got[name] != value]


def _described(decoded):
    present = {item.name: getattr(decoded, item.name) for item in fields(decoded)}
    return ", ".join(f"{name}={value}" for name, value in present.items() if value is not None)


def _operand_differences(expected, decoded):
    if isinstance(expected, Mem):
        if decoded.type != "memory":
            return [f"is {_described(decoded)}, not {expected}"]
        return _memory_differences(expected, decoded)
    if isinstance(expected, Reg):
        matches = decoded.type == "register" and decoded.register == expected.name
    elif isinstance(expected, Imm):
        matches = decoded.type == "immediate" and decoded.immediate == expected.value
    else:
        matches = decoded.type == "immediate" and decoded.immediate == 0
    return [] if matches else [f"is {_described(decoded)}, not {expected}"]


def _expected_prefixes(form, prefixes):
    definition = form.definition
    lock, group1 = carried_prefixes(form, prefixes)
    words = {"lock"} if lock else set()
    hle = lock or "accepts_lockless_hle" in definition.prefix_flags
    for flag, word, needs_hle in _GROUP1_WORDS.get(group1, ()):
        if flag in definition.prefix_flags and (hle or not needs_hle):
            words.add(word)
            break
    return words


def _printed_prefixes(text):
    words = set()
    for word in text.split():
        if word not in _PRINTED_PREFIXES:
            break
        words.add(word)
    return words


def _mismatch(form, operands, prefixes, encoding, decoded):
    problems = []
    if decoded.length != len(encoding):
        problems.append(f"length {decoded.length}, not {len(encoding)}")
    if decoded.mnemonic != form.definition.mnemonic:
        problems.append(f"mnemonic {decoded.mnemonic}, not {form.definition.mnemonic}")
    expected = _expected_operands(form, operands)
    visible = [operand for operand in decoded.operands if operand.visibility != "hidden"]
    if len(visible) != len(expected):
        problems.append(f"{len(visible)} visible operands, not {len(expected)}")
    else:
        for position, (want, got) in enumerate(zip(expected, visible)):
            problems.extend(f"operand {position} {difference}" for difference in _operand_differences(want, got))
    if form.operand_size is not None and decoded.operand_width != form.operand_size:
        problems.append(f"operand width {decoded.operand_width}, not {form.operand_size}")
    printed, wanted = _printed_prefixes(decoded.text), _expected_prefixes(form, prefixes)
    if printed != wanted:
        problems.append(f"prefixes {sorted(printed)}, not {sorted(wanted)}")
    if not problems:
        return None
    return f"{form.key}: {encoding.hex(' ')} decodes as '{decoded.text}': {'; '.join(problems)}"


def verify_all(decoder, instances):
    instances = list(instances)
    encodings = [placeholder(encoded) for _, _, _, encoded in instances]
    try:
        results = decoder.decode(encodings)
    except RejectedEncoding as error:
        raise EncodingError(f"{instances[error.index][0].key}: {error}") from error
    mismatches = [
        _mismatch(form, operands, prefixes, encoding, decoded)
        for (form, operands, prefixes, _), encoding, decoded in zip(instances, encodings, results)
    ]
    mismatches = [mismatch for mismatch in mismatches if mismatch is not None]
    if mismatches:
        raise EncodingError("\n".join(mismatches))


def verify(decoder, form, operands, prefixes, encoded):
    verify_all(decoder, [(form, operands, prefixes, encoded)])
