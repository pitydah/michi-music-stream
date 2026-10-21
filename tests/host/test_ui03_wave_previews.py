#!/usr/bin/env python3
"""Post-render invariant checks for UI-03 (stdlib only).
Usage: python3 tests/host/test_ui03_wave_previews.py <preview-directory>
This is HOST validation, never physical LCD certification.
"""
import pathlib
import sys


def pixels(path):
    with open(path, 'rb') as f:
        assert f.readline() == b'P6\n'
        dims = f.readline().split()
        width, height = [int(x) for x in dims]
        assert f.readline() == b'255\n'
        blob = f.read()
    assert width == 320 and height == 240 and len(blob) == 320 * 240 * 3
    return blob


def px(frame, x, y):
    offset = (y * 320 + x) * 3
    return frame[offset:offset + 3]


def main(folder):
    base = pathlib.Path(folder)
    p0 = pixels(base / 'ui-21-wave-00.ppm')
    p6 = pixels(base / 'ui-21-wave-06.ppm')
    paused = pixels(base / 'ui-13-paused-meta.ppm')
    playing = pixels(base / 'ui-10-playing-meta.ppm')
    assert p0 != p6, 'wave is frozen in active playback'
    changed = []
    for i in range(320 * 240):
        off = 3 * i
        if p0[off:off + 3] != p6[off:off + 3]:
            changed.append((i % 320, i // 320))
    assert changed and all(19 <= x <= 301 and 165 <= y <= 189 for x, y in changed), (
        'animation touched title, footer, status, or other non-wave pixels')
    for frame in (paused, playing):
        bg = px(frame, 16, 100)
        assert bg == px(frame, 16, 130), 'old vertical accent stripe remains'
    assert px(playing, 18, 42) == px(paused, 18, 42), (
        'playback has a colored header accent but pause does not')
    print(f'UI-03 PASS: {len(changed)} changed pixels confined to wave, accent removed')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('Usage: test_ui03_wave_previews.py PREVIEW_DIR')
    main(sys.argv[1])
