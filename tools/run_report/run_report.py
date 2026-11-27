#!/usr/bin/env python3
"""Reads a title run's runtime log and says how the run went.

The runtime writes a statistics report every ten seconds and when the title
ends or crashes (core/runtime/stats.cpp): the process's commit and working
set, the system's commit, and every named counter. This script turns those
reports into:

- frames a second per report (d3d12.presents over the report's interval),
  the longest frame of each interval (d3d12.frame_gap_max_ms) and frames over
  50 ms;
- memory: commit and working set at the start and end of the steady part of
  the run and their growth a minute, the least free system commit;
- leak suspects: counters that grew in every report of the steady part and
  are not known running totals (pipelines compiled, presents, ...), and
  host video memory growth.

The steady part starts after --warmup seconds (default 60: loading). With
--min-fps the exit code is 1 when a steady interval falls below it, 2 when a
leak is suspected, 3 for both; 0 otherwise, and 4 when the log has no
reports. --self-test checks the parser on built-in logs.

  run_report.py <runtime.log> [--warmup SECONDS] [--min-fps FPS]
  run_report.py --self-test
"""

import re
import sys

HEADER = re.compile(
    r'\[\s*([0-9.]+)\] I \[stats\] (.+?) \(([0-9.]+) s\): commit ([0-9.]+) GiB \(peak ([0-9.]+)\), '
    r'working set ([0-9.]+) GiB; system commit ([0-9.]+) of ([0-9.]+) GiB')
COUNTERS = re.compile(r'\[\s*[0-9.]+\] I \[stats\]   ([a-z0-9_]+):(.*)$')
PAIR = re.compile(r'([a-z0-9_]+)=(-?[0-9]+)(?:\([+-][0-9]+\))?')

# Counters that only ever add up events (not live objects or held bytes).
RUNNING_TOTALS = {
    'd3d12.presents', 'd3d12.pipeline_compiles', 'd3d12.pipeline_compile_ms', 'd3d12.pipeline_compiles_at_use',
    'd3d12.pipeline_wait_ms', 'd3d12.program_reloads', 'd3d12.texture_refreshes', 'd3d12.cached_page_refreshes',
    'd3d12.gpu_heaps_released', 'd3d12.frame_gaps_over_50ms', 'd3d12.host_pipelines_compiled',
    'xmem.implicit_page_faults', 'cpu.sse4a_instructions',
    'd3d12.pipelines_refused', 'd3d12.resources_used_after_release', 'd3d12.address_translations_locked',
    'd3d12.cached_pages_rejected', 'memory.committed_on_first_touch_bytes',
}
# Per-interval values (reset at each report), not totals.
PER_INTERVAL = {'d3d12.frame_gap_max_ms'}


class Report:
    def __init__(self, time, reason, interval, commit, peak, working_set, system_commit, system_limit):
        self.time = time
        self.reason = reason
        self.interval = interval
        self.commit = commit
        self.peak = peak
        self.working_set = working_set
        self.system_commit = system_commit
        self.system_limit = system_limit
        self.counters = {}


def parse(lines):
    reports = []
    for line in lines:
        header = HEADER.search(line)
        if header:
            values = [float(header.group(i)) for i in (1, 3, 4, 5, 6, 7, 8)]
            reports.append(Report(values[0], header.group(2), values[1], *values[2:]))
            continue
        counters = COUNTERS.search(line)
        if counters and reports:
            component = counters.group(1)
            for name, value in PAIR.findall(counters.group(2)):
                reports[-1].counters[component + '.' + name] = int(value)
    return reports


def analyse(reports, warmup, min_fps):
    """Returns (text lines, fps_ok, leak_free)."""
    out = []
    if not reports:
        return ['no statistics reports in the log'], False, False
    last = reports[-1]
    out.append('%d reports over %.0f s; the run ended with "%s"' % (len(reports), last.time, last.reason))
    fps = []
    previous = None
    for report in reports:
        presents = report.counters.get('d3d12.presents')
        if previous is not None and presents is not None and report.interval > 0:
            rate = (presents - previous.counters.get('d3d12.presents', 0)) / report.interval
            fps.append((report, rate))
        previous = report
    steady = [r for r in reports if r.time >= warmup]
    steady_fps = [(r, rate) for r, rate in fps if r.time >= warmup and r.interval >= 5]
    if fps:
        out.append('frames a second per report: ' + ' '.join('%.0f@%.0fs' % (rate, r.time) for r, rate in fps))
    fps_ok = True
    if steady_fps:
        rates = [rate for _, rate in steady_fps]
        gaps = [r.counters.get('d3d12.frame_gap_max_ms', 0) for r, _ in steady_fps]
        out.append('steady (from %.0f s): %.1f frames a second on average, %.1f at the least, longest frame %d ms'
                   % (warmup, sum(rates) / len(rates), min(rates), max(gaps)))
        if min_fps is not None:
            low = [(r, rate) for r, rate in steady_fps if rate < min_fps]
            fps_ok = not low
            out.append('%s: %d of %d steady intervals below %.0f frames a second'
                       % ('FAIL' if low else 'ok', len(low), len(steady_fps), min_fps))
    else:
        out.append('no steady interval with frames (warm-up %.0f s)' % warmup)
        fps_ok = min_fps is None
    over = last.counters.get('d3d12.frame_gaps_over_50ms')
    if over is not None:
        out.append('frames over 50 ms in the run: %d' % over)
    least_free = min(r.system_limit - r.system_commit for r in reports)
    out.append('least free system commit: %.2f GiB%s' % (least_free, ' (exhausted: failures are suspect)'
                                                         if least_free < 1.0 else ''))
    leak_free = True
    if len(steady) >= 3:
        first, end = steady[0], steady[-1]
        minutes = (end.time - first.time) / 60.0
        if minutes > 0:
            out.append('steady memory: commit %.2f -> %.2f GiB (%+.3f GiB a minute), working set %.2f -> %.2f GiB'
                       % (first.commit, end.commit, (end.commit - first.commit) / minutes, first.working_set,
                          end.working_set))
        suspects = []
        for name in sorted(end.counters):
            if name in RUNNING_TOTALS or name in PER_INTERVAL:
                continue
            values = [r.counters.get(name) for r in steady]
            if any(v is None for v in values):
                continue
            if all(b > a for a, b in zip(values, values[1:])):
                suspects.append('%s %d -> %d' % (name, values[0], values[-1]))
        vram = [r.counters.get('d3d12.vram_local_mb') for r in steady]
        if suspects:
            leak_free = False
            out.append('LEAK SUSPECTS (grew in every steady report): ' + '; '.join(suspects))
        else:
            out.append('ok: no live counter grew in every steady report')
        if all(v is not None for v in vram):
            out.append('host video memory: %d -> %d MiB' % (vram[0], vram[-1]))
    else:
        out.append('too few steady reports for leak checks (%d)' % len(steady))
    return out, fps_ok, leak_free


SAMPLE = '''[    0.100] I [runtime] started
[   10.000] I [stats] periodic (10.0 s): commit 5.00 GiB (peak 5.00), working set 5.50 GiB; system commit 40.00 of 95.76 GiB
[   10.000] I [stats]   d3d12: frame_gap_max_ms=300(+300) presents=100(+100) resources=10(+10) vram_local_mb=900(+900)
[   20.000] I [stats] periodic (10.0 s): commit 5.10 GiB (peak 5.10), working set 5.50 GiB; system commit 40.00 of 95.76 GiB
[   20.000] I [stats]   d3d12: frame_gap_max_ms=40(-260) presents=400(+300) resources=12(+2) vram_local_mb=950(+50)
[   30.000] I [stats] periodic (10.0 s): commit 5.10 GiB (peak 5.10), working set 5.50 GiB; system commit 40.00 of 95.76 GiB
[   30.000] I [stats]   d3d12: frame_gap_max_ms=35(-5) presents=700(+300) resources=14(+2) vram_local_mb=950
[   30.000] I [stats]   d3d12: pipeline_compiles=9(+9)
[   40.000] I [stats] periodic (10.0 s): commit 5.10 GiB (peak 5.10), working set 5.50 GiB; system commit 40.00 of 95.76 GiB
[   40.000] I [stats]   d3d12: frame_gap_max_ms=60(+25) presents=950(+250) resources=16(+2) vram_local_mb=950
[   40.000] I [stats]   d3d12: pipeline_compiles=12(+3)
'''


def self_test():
    reports = parse(SAMPLE.splitlines())
    assert len(reports) == 4, len(reports)
    assert reports[2].counters['d3d12.pipeline_compiles'] == 9  # a component split over two lines
    assert abs(reports[1].commit - 5.10) < 1e-9 and reports[0].interval == 10.0
    text, fps_ok, leak_free = analyse(reports, 15, 28)
    joined = '\n'.join(text)
    assert not fps_ok, joined  # 25 frames a second at 40 s
    assert not leak_free and 'd3d12.resources 12 -> 16' in joined, joined  # live objects grew every report
    assert 'pipeline_compiles' not in joined.split('LEAK SUSPECTS')[1], joined  # a running total
    text, fps_ok, _ = analyse(reports, 15, 20)
    assert fps_ok, '\n'.join(text)
    assert analyse([], 60, 30)[1:] == (False, False)
    print('run_report self-test passed')
    return 0


def main(argv):
    if '--self-test' in argv:
        return self_test()
    if not argv or argv[0].startswith('-'):
        print(__doc__)
        return 4
    warmup, min_fps = 60.0, None
    if '--warmup' in argv:
        warmup = float(argv[argv.index('--warmup') + 1])
    if '--min-fps' in argv:
        min_fps = float(argv[argv.index('--min-fps') + 1])
    with open(argv[0], encoding='utf-8', errors='replace') as log:
        reports = parse(log)
    text, fps_ok, leak_free = analyse(reports, warmup, min_fps)
    print('\n'.join(text))
    if not reports:
        return 4
    return (0 if fps_ok else 1) | (0 if leak_free else 2)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
