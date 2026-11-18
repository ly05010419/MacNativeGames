class UiPackError(Exception):
    """A fault the user can fix (a bad key list, a missing file); the CLI prints it and exits with 2."""
