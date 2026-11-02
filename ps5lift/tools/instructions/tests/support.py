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
