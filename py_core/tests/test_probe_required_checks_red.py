def test_probe_required_checks_red() -> None:
    """Negative control for the master-gate ruleset: this probe PR must never merge."""
    raise AssertionError("deliberate failure: probe PR for the master-gate ruleset, do not merge")
