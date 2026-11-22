"""Prints every check of the fast-forward rhythm tests, passed or not, with
their numbers. Run from tests/host after make:
python3 -I ff_review_probe.py [test names, default both]."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import host_tests  # noqa: E402


def check(cond, what):
    host_tests.checks += 1
    if not cond:
        host_tests.failures.append(what)
    print(("  ok   " if cond else "  FAIL ") + what)
    return cond


host_tests.check = check
os.makedirs(host_tests.WORK, exist_ok=True)
names = sys.argv[1:] or ["fast_forward_follows_scene", "fast_forward_even"]
for name in names:
    print(name)
    getattr(host_tests, "test_" + name)()
print("%d checks, %d failed" % (host_tests.checks, len(host_tests.failures)))
sys.exit(1 if host_tests.failures else 0)
