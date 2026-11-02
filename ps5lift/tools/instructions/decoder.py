"""Decode x86-64 encodings with the same Zydis settings as the lifter, through the ps5lift_decode executable."""
import json
import os
import subprocess
from dataclasses import dataclass
from typing import Optional


class DecodeError(Exception):
    pass


class RejectedEncoding(DecodeError):
    def __init__(self, index, encoding, status):
        super().__init__(f"cannot decode {encoding}: status {status}")
        self.index = index


@dataclass(frozen=True)
class DecodedOperand:
    type: str
    visibility: str
    action: str
    size: int
    register: Optional[str] = None
    base: Optional[str] = None
    index: Optional[str] = None
    scale: Optional[int] = None
    displacement: Optional[int] = None
    displacement_size: Optional[int] = None
    immediate: Optional[int] = None


@dataclass(frozen=True)
class Decoded:
    length: int
    mnemonic: str
    text: str
    isa_ext: str
    isa_set: str
    category: str
    operand_width: int
    privileged: bool
    operands: tuple


def _parse(fields):
    operands = tuple(DecodedOperand(**operand) for operand in fields.pop("operands"))
    return Decoded(operands=operands, **fields)


class Decoder:
    def __init__(self, executable):
        self.executable = executable

    def decode(self, encodings):
        """Decodes each encoding; the result lists them in order. An encoding Zydis rejects raises RejectedEncoding."""
        lines = [encoding.hex(" ") for encoding in encodings]
        stdin = "".join(line + "\n" for line in lines)
        completed = subprocess.run([self.executable], input=stdin, capture_output=True, text=True, check=True)
        outputs = completed.stdout.splitlines()
        if len(outputs) != len(lines):
            raise DecodeError(f"{self.executable} answered {len(outputs)} lines for {len(lines)} encodings")
        decoded = []
        for index, (line, output) in enumerate(zip(lines, outputs)):
            fields = json.loads(output)
            if "error" in fields:
                raise RejectedEncoding(index, line, fields["error"])
            decoded.append(_parse(fields))
        return decoded

    def mnemonics(self):
        completed = subprocess.run([self.executable, "--mnemonics"], capture_output=True, text=True, check=True)
        return set(completed.stdout.splitlines())


def from_environment():
    executable = os.environ.get("PS5LIFT_DECODE")
    if executable is None:
        raise DecodeError("PS5LIFT_DECODE is not set: it names the ps5lift_decode executable")
    return Decoder(executable)
