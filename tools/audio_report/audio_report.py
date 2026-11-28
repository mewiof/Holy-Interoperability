"""Reports what a recording of a title's audio sounds like, without listening to it.

Reads a 16-bit PCM WAV (holy run --record-audio writes <data>/logs/audio.wav) and reports, per second:
loudness (dBFS), how much of the second is silent in 10 ms windows, how often sound and silence alternate,
and the strongest frequency below 2 kHz. A stream the title cannot feed fast enough shows as sound broken by
short silences many times a second (dropouts); audio played slower than recorded shows its pitch lowered.

Usage: python audio_report.py <file.wav> [--from <s>] [--to <s>]
Exit code: 0 always (a report, not a check), 2 when the file cannot be read.
"""
import argparse
import array
import cmath
import math
import sys
import wave

WINDOW_MS = 10
SILENCE_DBFS = -60.0


def dbfs(rms):
    return 20.0 * math.log10(rms / 32768.0) if rms > 0 else -120.0


def dominant_frequency(samples, rate):
    """Strongest frequency (Hz) below 2 kHz in `samples` (a 4096-sample window), by a plain DFT of the bins."""
    n = len(samples)
    if n < 256:
        return 0.0
    mean = sum(samples) / n
    window = [(samples[i] - mean) * (0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1))) for i in range(n)]
    best, best_bin = 0.0, 0
    top = min(n // 2, int(2000 * n / rate))
    step = 2 * math.pi / n
    for k in range(2, top):
        w = cmath.exp(-1j * step * k)
        acc = 0j
        z = 1 + 0j
        for x in window[::4]:  # decimated sum: enough to rank bins
            acc += x * z
            z *= w ** 4
        mag = abs(acc)
        if mag > best:
            best, best_bin = mag, k
    return best_bin * rate / n


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('wav')
    parser.add_argument('--from', dest='start', type=float, default=0.0)
    parser.add_argument('--to', dest='end', type=float, default=None)
    args = parser.parse_args(argv)
    try:
        with wave.open(args.wav, 'rb') as w:
            rate, channels, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
            frames = w.readframes(w.getnframes())
    except (OSError, wave.Error) as e:
        print('cannot read %s: %s' % (args.wav, e))
        return 2
    if width != 2:
        print('only 16-bit PCM is supported')
        return 2
    data = array.array('h', frames)
    mono = [sum(data[i:i + channels]) // channels for i in range(0, len(data), channels)]
    total = len(mono) / rate
    end = min(args.end if args.end is not None else total, total)
    print('%s: %.1f s, %d Hz, %d channels' % (args.wav, total, rate, channels))
    print('  second   dBFS  silent  sound/silence changes  strongest Hz')
    win = rate * WINDOW_MS // 1000
    overall_changes = overall_windows = overall_silent = 0
    for second in range(int(args.start), int(math.ceil(end))):
        chunk = mono[second * rate:(second + 1) * rate]
        if not chunk:
            break
        rms = math.sqrt(sum(x * x for x in chunk) / len(chunk))
        silent_flags = []
        for i in range(0, len(chunk) - win + 1, win):
            part = chunk[i:i + win]
            silent_flags.append(dbfs(math.sqrt(sum(x * x for x in part) / win)) < SILENCE_DBFS)
        changes = sum(1 for a, b in zip(silent_flags, silent_flags[1:]) if a != b)
        silent = sum(silent_flags)
        overall_changes += changes
        overall_windows += len(silent_flags)
        overall_silent += silent
        freq = dominant_frequency(chunk[:4096], rate) if dbfs(rms) > SILENCE_DBFS else 0.0
        print('  %6d  %5.1f  %5.0f%%  %21d  %12.0f' % (second, dbfs(rms), 100.0 * silent / max(1, len(silent_flags)),
                                                     changes, freq))
    if overall_windows:
        sound = overall_windows - overall_silent
        print('overall: %.0f%% silent; %d sound/silence changes in %.1f s of sound (%.1f a second of sound)' %
              (100.0 * overall_silent / overall_windows, overall_changes, sound * WINDOW_MS / 1000.0,
               overall_changes / max(0.01, sound * WINDOW_MS / 1000.0)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
