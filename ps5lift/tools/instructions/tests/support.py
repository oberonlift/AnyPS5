from collections import defaultdict

from instructions import cases, database, decoder
from instructions.database import Group


def generate():
    forms = [form for form in database.forms(database.load()) if database.classify(form.definition) is Group.SET]
    instances = cases.instances(forms, decoder.from_environment())
    by_group = defaultdict(list)
    for instance in instances:
        by_group[database.test_group(instance.form)].append(instance)
    specs = {group: cases.case_specs(members) for group, members in by_group.items()}
    return forms, dict(by_group), specs


_GENERATED = []


def generated():
    if not _GENERATED:
        _GENERATED.append(generate())
    return _GENERATED[0]


def find_form(test, forms, mnemonic, operand_size=None, **conditions):
    matches = [
        form
        for form in forms
        if form.definition.mnemonic == mnemonic
        and form.operand_size == operand_size
        and all(
            getattr(form.definition, name) == value
            if hasattr(form.definition, name)
            else form.definition.filters.get(name) == value
            for name, value in conditions.items()
        )
    ]
    test.assertEqual(len(matches), 1, f"{mnemonic} o{operand_size} {conditions} matched {len(matches)} forms")
    return matches[0]
