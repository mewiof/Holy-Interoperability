#!/usr/bin/env python3
"""Runs a title for a while and reports how the run went (run_report.py).

  check_title.py <package-dir> [--seconds N] [--min-fps FPS] [--warmup SECONDS] [--holy PATH] [-- holy run options]

Runs `holy run <package-dir> --timeout N` (default 120 s), then reads the
runtime log in the title's data directory (%LOCALAPPDATA%Low/HolyInterop/
titles/<data key>/logs/runtime.log; the data key from `holy inspect`) and
prints the run report. The exit code is the report's (0: as required; 1: a
steady interval under --min-fps; 2: a leak suspect; 3: both; 4: no reports),
or 5 when the title could not be started. The log is copied next to the
report as runtime-<time>.log in the data directory's logs folder, so runs
can be compared. Nothing is written to the package directory.
"""

import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_report  # noqa: E402


def option(argv, name, default):
    if name in argv:
        i = argv.index(name)
        value = argv[i + 1]
        del argv[i:i + 2]
        return value
    return default


def main(argv):
    if not argv or argv[0].startswith('-'):
        print(__doc__)
        return 5
    extra = []
    if '--' in argv:
        i = argv.index('--')
        argv, extra = argv[:i], argv[i + 1:]
    package = argv[0]
    rest = argv[1:]
    seconds = option(rest, '--seconds', '120')
    min_fps = option(rest, '--min-fps', None)
    warmup = option(rest, '--warmup', '60')
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    holy = option(rest, '--holy', os.path.join(root, 'build', 'clang-cl', 'bin', 'holy.exe'))
    inspect = subprocess.run([holy, 'inspect', package], capture_output=True, text=True, errors='replace')
    key = re.search(r'data key\s+([0-9a-fA-F]+)', inspect.stdout)
    if not key:
        print('holy inspect gave no data key:\n' + inspect.stdout + inspect.stderr)
        return 5
    local_low = os.path.join(os.path.dirname(os.environ.get('LOCALAPPDATA', '')), 'LocalLow')
    log_path = os.path.join(local_low, 'HolyInterop', 'titles', key.group(1), 'logs', 'runtime.log')
    started = time.time()
    run = subprocess.run([holy, 'run', package, '--timeout', seconds] + extra)
    print('holy run exited with %d after %.0f s' % (run.returncode, time.time() - started))
    if not os.path.exists(log_path) or os.path.getmtime(log_path) < started:
        print('no runtime log from this run at ' + log_path)
        return 5
    kept = os.path.join(os.path.dirname(log_path), time.strftime('runtime-%Y%m%d-%H%M%S.log'))
    shutil.copyfile(log_path, kept)
    print('log kept as ' + kept)
    report_args = [kept, '--warmup', warmup] + (['--min-fps', min_fps] if min_fps else [])
    return run_report.main(report_args)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
