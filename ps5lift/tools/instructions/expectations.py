"""Where a case's expectation comes from: its recording, or an expectation written from AMD's manual.

`expected_result(case, recorded)` gives the result a case is judged against: the recording, when the case has one.
A case whose form `cases.expectation_written` names is never recorded, and no expectations are written yet, so such a
case, like any case without a recording, raises `NoExpectation`."""
from .cases import expectation_written

NO_EXPECTATION = "no recording and no written expectation"


class ExpectationError(Exception):
    pass


class NoExpectation(ExpectationError):
    pass


def expected_result(case, recorded):
    if expectation_written(case.instance.form):
        if recorded is not None:
            raise ExpectationError(f"case {case.spec.index} of {case.instance.form.key} has a recording, but its "
                                   "form's expectations are written from AMD's manual, not recorded")
        raise NoExpectation(NO_EXPECTATION)
    if recorded is None:
        raise NoExpectation(NO_EXPECTATION)
    return recorded
