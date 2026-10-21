#!/usr/bin/env python3
"""UI04 reproducibility checks. Run locally after regenerating host PPM previews.

This is NOT a DEVICE_E2E hardware certification and not a PCM analyzer.
Usage: python3 tests/host/test_ui04_visual_invariants.py <preview_dir>
"""
from pathlib import Path
import sys

W,H=320,240

def ppm(name):
    with Path(name).open('rb') as f:
        assert f.readline()==b'P6\n'
        assert f.readline()==b'320 240\n'
        assert f.readline()==b'255\n'
        data=f.read()
    assert len(data)==W*H*3
    return data

def difference(a,b):
    result=[]
    for i in range(W*H):
        if a[i*3:i*3+3]!=b[i*3:i*3+3]:
            result.append((i%W,i//W))
    return result

def run(folder):
    base=Path(folder)
    p0=ppm(base/'ui-21-wave-00.ppm')
    p6=ppm(base/'ui-21-wave-06.ppm')
    paused=ppm(base/'ui-13-paused-meta.ppm')
    playing=ppm(base/'ui-10-playing-meta.ppm')
    spanish=ppm(base/'ui-11-playing-spanish-utf8.ppm')
    buffering=ppm(base/'ui-08-buffering-meta.ppm')
    changed=difference(p0,p6)
    assert changed, 'The active wave does not move'
    assert all(20<=x<=299 and 163<=y<=190 for x,y in changed), 'Motion bleeds into text/footer or neighboring band'
    # The muted paused line remains in the reserved wave area; it may be flat.
    # The former playback-only orange stripe must not return at x=16,y=91..160.
    def pixel(frame,x,y): return frame[(y*W+x)*3:(y*W+x)*3+3]
    for frame in (playing,spanish,paused,buffering):
        assert pixel(frame,16,112)==pixel(frame,16,130), 'Vertical accent stripe returned'
        assert pixel(frame,18,42)==pixel(paused,18,42), 'Playback-only header color line returned'
    # Paused must be completely independent of the phase supplied to its renderer.
    assert ppm(base/'ui-22-paused-phase-a.ppm') == ppm(base/'ui-22-paused-phase-b.ppm'), (
        'Paused waves change with phase')
    print('UI04 visual invariants PASS: cropped AA wave, layout zones, accent removal')

if __name__=='__main__':
    if len(sys.argv)!=2: raise SystemExit('Usage: ... PREVIEW_DIR')
    run(sys.argv[1])
