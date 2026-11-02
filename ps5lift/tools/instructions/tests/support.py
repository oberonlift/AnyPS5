from instructions import decoder, suite


def generate():
    return suite.generate(decoder.from_environment())


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
