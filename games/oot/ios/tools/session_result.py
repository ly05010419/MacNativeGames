"""Judge an automated iOS session from evidence, never from its exit time."""
import re

CLOSE_MARKER = 'save idle; closing iOS test session'
FAILURES = re.compile(r'OutOfMemory|Insufficient Memory|\bFATAL\b|GPU.*(?:error|failed)', re.I)


def classify(log: str, *, remaining=(), crash_reports=()):
    errors = [line for line in log.splitlines() if FAILURES.search(line)]
    if errors or crash_reports:
        return {'passed': False, 'outcome': 'crash or GPU failure',
                'errors': errors[-8:], 'crash_reports': list(crash_reports)}
    if remaining:
        return {'passed': False, 'outcome': 'process still running'}
    if CLOSE_MARKER not in log:
        return {'passed': False, 'outcome': 'exit without verified save-safe close'}
    return {'passed': True, 'outcome': 'verified save-safe close'}
