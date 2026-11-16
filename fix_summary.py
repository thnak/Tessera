"""Accept both CTest summary formats.

CMake emits `100% tests passed, 0 tests failed out of 595` when some tests fail, but
`100% tests passed out of 595` when none do -- the failure clause is omitted. The regex only
matched the first form, so a fully green run reported "the CTest log has no summary line" and the
release lane failed on a passing suite. Accept both.
"""

import pathlib

p = pathlib.Path('scripts/check_release_suites.py')
t = p.read_text(encoding='utf-8')

old = 'SUMMARY_RE = re.compile(r"(\\d+)% tests passed, (\\d+) tests failed out of (\\d+)")'
new = (
    '# CTest prints the failure clause only when something failed: "100% tests passed, 0 tests\n'
    '# failed out of 595" versus "100% tests passed out of 595". Both are a completed run.\n'
    'SUMMARY_RE = re.compile(r"\\d+% tests passed(?:, \\d+ tests? failed)? out of \\d+")'
)
assert old in t, 'SUMMARY_RE not found'
p.write_text(t.replace(old, new, 1), encoding='utf-8', newline='')
print('SUMMARY_RE accepts both CTest formats')
