#!/usr/bin/env python3
"""Runs the PC tests side by side, N at a time, each suite's output printed
whole and in order, as a run one after another prints it:

    par.py [--jobs N] SUITE.py [ARGS...]     one suite: host_tests.py [NAME...],
                                             ui_wired.py OUTDIR [SCENE...]...
    par.py [--jobs N] --plan FILE            run.sh's steps: a command a line,
                                             its words ending in a tab each

N: --jobs, or $GPSP_TEST_JOBS, or half the CPUs. A suite with a TESTS (or
SCENES) list and a main() (host_tests.py and those built on it) is split:
each of its tests runs in a process of its own, through the suite's own
main() with that test's name. Then main() runs once more here, with no tests
and every test's checks, failures and what the end-of-run checks look at
(host_tests.READOUTS) put together: the same last lines, and the same checks
at the end, as the whole suite in one process. Anything else runs as it is.
Every test's run folders (and link sockets) must be its own: one used by two
tests fails the run (they'd collide side by side), as does a module global
that a test changed and that isn't merged here (STATE): run that one with
GPSP_TEST_JOBS=1. The longest jobs start first (times of the last run, in
build/par_times.json, or par_times.json here; none known: first)."""

import concurrent.futures
import contextlib
import copy
import functools
import importlib
import inspect
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
import types

HERE = os.path.dirname(os.path.abspath(__file__))
TIMES = os.path.join(HERE, os.environ.get("GPSP_TEST_BUILD", "build"), "par_times.json")
# A fresh tree's: the times measured when par.py came (2026-11-17, 6 at once).
SEED = os.path.join(HERE, "par_times.json")
# What tests add to as they go, in host_tests and the suites, and how one
# test's share is put together with the others'.
STATE = {"checks": "sum", "failures": "list", "READOUTS": "set", "made": "list"}
SUMMARY = re.compile(r"^[^\n]*\d+ checks, \d+ failed\n?$")   # (a label before it: drive_save_tests.py's)


def jobs_default():
    try:
        return max(1, int(os.environ["GPSP_TEST_JOBS"]))
    except (KeyError, ValueError):
        return max(1, len(os.sched_getaffinity(0)) // 2)


def list_name(path):
    """TESTS or SCENES when the script is a suite par.py can split, else None
    (read, not imported: a plain script runs its tests as it is imported)."""
    try:
        src = open(path, encoding="utf-8").read()
    except OSError:
        return None
    m = re.search(r"^(TESTS|SCENES) = \[", src, re.M)
    if m and re.search(r"^def main\(", src, re.M) and re.search(r"^if __name__ == .__main__.:", src, re.M):
        return m.group(1)
    return None


def load(path):
    sys.path.insert(0, os.path.dirname(path))
    return importlib.import_module(os.path.splitext(os.path.basename(path))[0])


def test_name(fn):
    return fn.__name__.split("_", 1)[1]


def call_main(mod, argv):
    """mod.main() with that sys.argv: its exit status."""
    sys.argv = argv
    try:
        status = mod.main()
    except SystemExit as e:
        status = e.code
    if status is None or status is False:
        return 0
    return status if isinstance(status, int) else 1


def modules(mod):
    """The suite's module and host_tests (where the checks are counted)."""
    host = sys.modules.get("host_tests")
    return [mod] + ([host] if host is not None and host is not mod else [])


def snapshot(mods):
    return {(m.__name__, k): repr(v) for m in mods for k, v in vars(m).items()
            if not k.startswith("__") and not callable(v) and not isinstance(v, types.ModuleType)}


# ---- One test, in a process of its own ----

def worker(path, result, args):
    mod = load(path)
    lst = list_name(path)
    tests = getattr(mod, lst)
    name = args[-1]
    mods = modules(mod)
    # main()'s own doings first (ui_wired.py's OUT), nothing printed
    setattr(mod, lst, [])
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        call_main(mod, [path] + args)
    setattr(mod, lst, tests)
    before = snapshot(mods)
    runs, links = [], []
    host = sys.modules.get("host_tests")
    if host is not None:
        init, start = host.Run.__init__, host.Run.start

        def run_init(self, name, *a, **k):
            runs.append(name)
            init(self, name, *a, **k)

        def run_start(self, *a, **k):
            options = inspect.signature(start).bind(self, *a, **k).arguments.get("options") or ()
            options = list(options)
            if "--link" in options and options.index("--link") + 1 < len(options):
                links.append(options[options.index("--link") + 1])
            return start(self, *a, **k)

        host.Run.__init__, host.Run.start = run_init, run_start
    done = {"ran": False, "offset": None}

    def ending(fn):
        @functools.wraps(fn)
        def wrapped(*a, **k):
            r = fn(*a, **k)
            # what main() prints after the tests is printed once, by the parent
            sys.stdout.flush()
            sys.stderr.flush()
            done["offset"] = os.lseek(1, 0, os.SEEK_CUR)
            done["ran"] = True
            return r
        return wrapped

    setattr(mod, lst, [ending(t) if test_name(t) == name else t for t in tests])
    try:
        status = call_main(mod, [path] + args)
    except BaseException:
        traceback.print_exc()
        status = 1
    sys.stdout.flush()
    sys.stderr.flush()
    setattr(mod, lst, tests)
    after = snapshot(mods)
    state = {}
    for m in mods:
        for k in STATE:
            if hasattr(m, k):
                v = getattr(m, k)
                state[m.__name__ + "." + k] = sorted(v) if isinstance(v, set) else v
    unknown = sorted("%s.%s" % key for key in after if after[key] != before.get(key) and key[1] not in STATE)
    with open(result, "w") as f:
        json.dump({"ran": done["ran"], "offset": done["offset"], "status": status, "state": state,
                   "runs": runs, "links": links, "unknown": unknown}, f)
    return 0 if done["ran"] else 1


# ---- The parent ----

class Job:
    def __init__(self, key, argv, cwd):
        self.key, self.argv, self.cwd = key, argv, cwd
        self.result = None    # the worker's, for a suite's test
        self.seconds = None
        self.status = None
        self.out = self.err = b""

    def run(self, tmp):
        base = os.path.join(tmp, "%d" % id(self))
        with open(base + ".out", "wb") as out, open(base + ".err", "wb") as err:
            t = time.monotonic()
            argv = [a.replace("{result}", base + ".json") for a in self.argv]
            try:
                self.status = subprocess.run(argv, cwd=self.cwd, stdout=out, stderr=err,
                                             stdin=subprocess.DEVNULL).returncode
            except OSError as e:
                err.write(("par: %s: %s\n" % (argv[0], e)).encode())
                self.status = 127
            self.seconds = time.monotonic() - t
        self.out = open(base + ".out", "rb").read()
        self.err = open(base + ".err", "rb").read()
        if os.path.exists(base + ".json"):
            self.result = json.load(open(base + ".json"))
        return self


def emit(out, err):
    if out:
        sys.stdout.buffer.write(out)
    sys.stdout.flush()
    if err:
        sys.stderr.buffer.write(err)
    sys.stderr.flush()


class Step:
    """A command, run as it is."""

    def __init__(self, argv, cwd):
        self.argv = argv
        self.jobs = [Job(" ".join(argv), argv, cwd)]

    def finish(self, problems):
        job = self.jobs[0]
        emit(job.out, job.err)
        if job.status:
            problems.append("%s: exit %d" % (job.key, job.status))
        return job.status == 0


class Suite:
    """A suite split into its tests (list_name())."""

    def __init__(self, path, args, cwd, python):
        self.path, self.args = path, args
        self.label = os.path.basename(path)
        self.mod = load(path)
        self.lst = list_name(path)
        self.tests = getattr(self.mod, self.lst)
        names = [test_name(t) for t in self.tests]
        self.fixed = [a for a in args if a not in names]
        self.initial = {(m, k): copy.deepcopy(getattr(m, k)) for m in modules(self.mod) for k in STATE if hasattr(m, k)}
        # the tests main() runs with these arguments: stand-ins that say so
        called = []

        def stand_in(t):
            def run():
                called.append(test_name(t))
            run.__name__ = t.__name__
            return run

        self.dry([stand_in(t) for t in self.tests], [path] + args)
        worker_argv = [python, "-I", os.path.abspath(__file__), "--worker", path, "{result}", "--"] + self.fixed
        self.jobs = [Job("%s:%s" % (self.label, n), worker_argv + [n], cwd) for n in called]

    def dry(self, tests, argv):
        """main() with these tests and arguments, here: what it printed
        (None: it raised); the counts as they were before."""
        out = io.StringIO()
        setattr(self.mod, self.lst, tests)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                call_main(self.mod, argv)
        except BaseException:
            return None
        finally:
            setattr(self.mod, self.lst, self.tests)
            self.reset()
        return out.getvalue()

    def reset(self):
        for (m, k), v in self.initial.items():
            setattr(m, k, copy.deepcopy(v))

    def splits(self):
        """Whether main() does nothing but the tests and its last lines: run
        with no tests (and a name that isn't one: no end-of-run checks), it
        prints at most "0 checks, 0 failed" (a suite that skips itself,
        pad_tests.py without nspire-usbpad, says why instead: it runs as it
        is, as does one that runs no test with these arguments)."""
        if not self.jobs:
            return False
        out = self.dry([], [self.path] + self.fixed + ["\0par.py"])
        return out is not None and (out == "" or SUMMARY.match(out) is not None)

    def finish(self, problems):
        ok = True
        merged = {}
        for job in self.jobs:
            r = job.result
            if r is None or not r["ran"]:
                # it stopped (an exception, a crash): all it said
                emit(job.out, job.err)
                problems.append("%s: stopped before its end (exit %s)" % (job.key, job.status))
                ok = False
                continue
            emit(job.out[:r["offset"]], job.err)
            for key, v in r["state"].items():
                how = STATE[key.rsplit(".", 1)[1]]
                if how == "sum":
                    merged[key] = merged.get(key, 0) + v
                elif how == "set":
                    merged[key] = merged.get(key, set()) | set(v)
                else:
                    merged[key] = merged.get(key, []) + v
            for name in r["unknown"]:
                problems.append("%s changed %s, which par.py doesn't put together across tests: "
                                "run it with GPSP_TEST_JOBS=1 (or add it to par.py's STATE)" % (job.key, name))
                ok = False
        if not ok:
            return False
        # main() once more: no tests, everyone's checks
        self.reset()
        for key, v in merged.items():
            mname, k = key.rsplit(".", 1)
            setattr(sys.modules[mname], k, v)
        out, err = io.StringIO(), io.StringIO()
        setattr(self.mod, self.lst, [])
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                status = call_main(self.mod, [self.path] + self.args)
        finally:
            setattr(self.mod, self.lst, self.tests)
            self.reset()
        emit(out.getvalue().encode(), err.getvalue().encode())
        if status:
            problems.append("%s: exit %d" % (self.label, status))
        return status == 0


def make_step(argv, cwd, python):
    """A suite par.py can split (python3 [-I] SUITE.py ARGS), or a Step."""
    i = 1
    while i < len(argv) and argv[i] in ("-I", "-u", "-B"):
        i += 1
    if (os.path.basename(argv[0]).startswith("python3") and i < len(argv) and argv[i].endswith(".py")):
        path = os.path.abspath(os.path.join(cwd, argv[i]))
        if list_name(path):
            suite = Suite(path, argv[i + 1:], cwd, python)
            if suite.splits():
                return suite
    return Step(argv, cwd)


def check_shared(steps, problems):
    """Each run folder and link socket in one test only."""
    claimed = {}
    for step in steps:
        for job in step.jobs:
            r = job.result
            if not r:
                continue
            for what, names in (("run folder", r.get("runs", [])), ("link socket", r.get("links", []))):
                for n in set(names):
                    other = claimed.setdefault((what, n), job.key)
                    if other != job.key:
                        problems.append("the %s %s is %s's and %s's: they collide side by side" %
                                        (what, n, other, job.key))


def run(steps, jobs):
    known = {}
    for path in (SEED, TIMES):
        try:
            known.update(json.load(open(path)))
        except (OSError, ValueError):
            pass
    every = [job for step in steps for job in step.jobs]
    # longest first; unknown ones first of all, in order
    order = sorted(every, key=lambda j: -known[j.key] if j.key in known else -1e9)
    problems = []
    failed = 0
    tmp = tempfile.mkdtemp(prefix="gpsp-par-")
    pool = concurrent.futures.ThreadPoolExecutor(max(1, jobs))
    # GPSP_PAR_QUIET=KEY,KEY...: these jobs (tests that measure the clock: the links' frames a second) run first,
    # by themselves (GPSP_PAR_QUIET_JOBS at a time, default 3), and when they are over the file
    # $GPSP_PAR_QUIET_DONE is made (tests/gate.sh holds the other parts back until then); all the others after.
    quiet = set(k for k in os.environ.get("GPSP_PAR_QUIET", "").split(",") if k)
    first = [job for job in order if job.key in quiet]
    order = [job for job in order if job.key not in quiet]
    try:
        futures = {}
        if first:
            qpool = concurrent.futures.ThreadPoolExecutor(max(1, int(os.environ.get("GPSP_PAR_QUIET_JOBS", "3"))))
            try:
                futures.update({job: qpool.submit(job.run, tmp) for job in first})
            finally:
                qpool.shutdown(wait=True)
        done = os.environ.get("GPSP_PAR_QUIET_DONE")
        if done:
            open(done, "w").close()
        futures.update({job: pool.submit(job.run, tmp) for job in order})
        for step in steps:
            for job in step.jobs:
                futures[job].result()
            if not step.finish(problems):
                failed += 1
    except KeyboardInterrupt:
        pool.shutdown(wait=False, cancel_futures=True)
        raise
    finally:
        pool.shutdown(wait=True)
        shutil.rmtree(tmp, ignore_errors=True)
    check_shared(steps, problems)
    try:
        times = json.load(open(TIMES))
    except (OSError, ValueError):
        times = {}
    for job in every:
        if job.seconds is not None:
            times[job.key] = round(job.seconds, 2)
    try:
        os.makedirs(os.path.dirname(TIMES), exist_ok=True)
        with open(TIMES + ".tmp%d" % os.getpid(), "w") as f:
            json.dump(times, f, indent=0, sort_keys=True)
        os.replace(TIMES + ".tmp%d" % os.getpid(), TIMES)
    except OSError:
        pass
    for p in problems:
        print("par: FAILED: " + p, file=sys.stderr)
    return 1 if problems or failed else 0


def main():
    args = sys.argv[1:]
    if args[:1] == ["--worker"]:
        return worker(os.path.abspath(args[1]), args[2], args[4:])
    jobs = jobs_default()
    plan = None
    while args and args[0].startswith("--"):
        if args[0] == "--jobs" and len(args) > 1:
            jobs, args = max(1, int(args[1])), args[2:]
        elif args[0] == "--plan" and len(args) > 1:
            plan, args = args[1], args[2:]
        else:
            sys.exit(__doc__)
    python = sys.executable
    cwd = os.getcwd()
    if plan:
        lines = [l.rstrip("\n").split("\t")[:-1] for l in open(plan, encoding="utf-8")]
        steps = [make_step(argv, cwd, python) for argv in lines if argv]
    elif args:
        steps = [make_step([python, "-I"] + args, cwd, python)]
    else:
        sys.exit(__doc__)
    return run(steps, jobs)


if __name__ == "__main__":
    sys.exit(main())
