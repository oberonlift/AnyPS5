"""Run a driver over every case of an input file, whatever the cases do to it.

`run` writes the result file's header and starts the driver at the first case. When the driver dies, or writes no result
for `timeout` seconds, the case after its last complete result gets a CRASHED result (with the exit status as detail,
128 plus the signal for a signal) or a TIMED_OUT one; a partly written result is dropped, the end of the driver's stderr
is kept for that case, and the driver starts again at the next case. A driver that ends before it reports readiness
(`records.READY_VARIABLE`), such as one the loader cannot start, or that exits with `records.DRIVER_FAILED`, cannot run
cases at all, and ends the run with its stderr. `run` returns the kept stderr by case index."""
import os
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from . import records

STDERR_KEPT = 4096
SIGNAL_STATUS = 128
_POLL_SECONDS = 0.02


class RunnerError(Exception):
    pass


@dataclass(frozen=True)
class _Ending:
    status: Optional[int]
    ready: bool
    stderr: str


def _exit_status(returncode):
    return returncode if returncode >= 0 else SIGNAL_STATUS - returncode


def _describe(status):
    if status is None:
        return "killed after the timeout without a result"
    if status >= SIGNAL_STATUS:
        return f"signal {status - SIGNAL_STATUS}"
    return f"exit status {status}"


def _reported_ready(descriptor):
    try:
        return os.read(descriptor, 1) != b""
    except BlockingIOError:
        return False


def _attempt(command, env, inputs_path, results_path, first, timeout):
    ready_read, ready_write = os.pipe()
    os.set_blocking(ready_read, False)
    environment = {
        **env,
        records.INPUTS_VARIABLE: str(inputs_path),
        records.RESULTS_VARIABLE: str(results_path),
        records.FIRST_VARIABLE: str(first),
        records.READY_VARIABLE: str(ready_write),
    }
    try:
        with tempfile.TemporaryFile() as stderr:
            try:
                process = subprocess.Popen(command, env=environment, stdin=subprocess.DEVNULL,
                                           stdout=subprocess.DEVNULL, stderr=stderr, pass_fds=(ready_write,))
            finally:
                os.close(ready_write)
            size = results_path.stat().st_size
            progress = time.monotonic()
            status = None
            while True:
                returncode = process.poll()
                if returncode is not None:
                    status = _exit_status(returncode)
                    break
                current = results_path.stat().st_size
                if current != size:
                    size, progress = current, time.monotonic()
                elif time.monotonic() - progress > timeout:
                    process.kill()
                    process.wait()
                    break
                time.sleep(_POLL_SECONDS)
            stderr.seek(max(stderr.seek(0, os.SEEK_END) - STDERR_KEPT, 0))
            return _Ending(status, _reported_ready(ready_read), stderr.read().decode(errors="replace"))
    finally:
        os.close(ready_read)


def _complete_results(results_path, header, cases):
    data = results_path.read_bytes()
    found, results, end = records.read_result_prefix(data)
    if found != header:
        raise RunnerError(f"{results_path}'s header no longer matches the inputs'")
    written = [result.case for result in results]
    if written != cases[:len(written)]:
        raise RunnerError(f"{results_path} holds results for cases {written[:10]}…, not the inputs' {cases[:10]}…")
    if end != len(data):
        with open(results_path, "r+b") as file:
            file.truncate(end)
    return len(results)


def run(command, env, inputs_path, results_path, timeout):
    inputs_path, results_path = Path(inputs_path), Path(results_path)
    header, inputs = records.read_inputs(inputs_path)
    cases = [case.case for case in inputs]
    results_path.write_bytes(records.result_file_header(header, len(cases)))
    stops = {}
    first = 0
    while first < len(cases):
        ending = _attempt(command, env, inputs_path, results_path, first, timeout)
        complete = _complete_results(results_path, header, cases)
        if complete < first:
            raise RunnerError(f"{results_path} lost results: it holds {complete}, and the driver started at {first}")
        if not ending.ready:
            raise RunnerError(f"{command[0]} ended before it was ready to run cases ({_describe(ending.status)}):\n"
                              f"{ending.stderr}")
        if ending.status == records.DRIVER_FAILED:
            raise RunnerError(f"{command[0]} could not run cases:\n{ending.stderr}")
        if ending.status == 0:
            if complete != len(cases):
                raise RunnerError(f"{command[0]} exited 0 after {complete} of {len(cases)} cases:\n{ending.stderr}")
            break
        if complete == len(cases):
            raise RunnerError(f"{command[0]} ended with status {ending.status} after its last case:\n{ending.stderr}")
        case = inputs[complete]
        if ending.status is None:
            outcome, detail = records.Outcome.TIMED_OUT, 0
        else:
            outcome, detail = records.Outcome.CRASHED, ending.status
        expected = () if case.console_defined else None
        with open(results_path, "ab") as file:
            file.write(records.pack_result(records.Result(case.case, outcome, detail, 0, 0, {}, {}, expected)))
        stops[case.case] = ending.stderr
        first = complete + 1
    return stops
