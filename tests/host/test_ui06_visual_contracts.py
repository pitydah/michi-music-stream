#!/usr/bin/env python3
"""UI-06 host-only optical/animation contracts (run after render_ui_previews)."""
from __future__ import annotations
import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path

from PIL import Image

HERE=Path(__file__).resolve()
ROOT=HERE.parents[2]
PREVIEWS=ROOT/'build'/'previews'
GEN=ROOT/'firmware'/'components'/'michi_display'/'tools'/'gen_precision_icons.py'
HEADER=ROOT/'firmware'/'components'/'michi_display'/'assets'/'icons'/'michi_precision_icons_data.h'

def load(name):
    im=Image.open(PREVIEWS/name)
    assert im.size==(320,240)
    return im.convert('RGB')

class UI06VisualContracts(unittest.TestCase):
    def test_generated_glyphs_reproduce_exactly(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'precise.h'
            subprocess.run(['python3',str(GEN),'--output',str(out)],check=True,
                           stdout=subprocess.DEVNULL)
            self.assertEqual(hashlib.sha256(out.read_bytes()).digest(),
                             hashlib.sha256(HEADER.read_bytes()).digest())

    def test_icons_have_nonzero_coverage_and_distinct_shapes(self):
        with tempfile.TemporaryDirectory() as d:
            subprocess.run(['python3',str(GEN),'--output',str(Path(d)/'icons.h'),
                            '--preview',str(Path(d)/'previews')],check=True,stdout=subprocess.DEVNULL)
            for size in [12,16,20,32]:
                data=[]
                for kind in ['wifi','receiver','room','speaker']:
                    im=Image.open(Path(d)/'previews'/f'{kind}_{size}.png')
                    self.assertEqual(im.size,(size,size))
                    self.assertGreater(sum(im.get_flattened_data() if hasattr(im,"get_flattened_data") else im.getdata()), 300)
                    data.append(im.tobytes())
                self.assertEqual(len(set(data)),4)

    def test_motion_restricts_changes_to_the_wave_band(self):
        a=load('ui-21-wave-00.ppm');b=load('ui-21-wave-01.ppm')
        self.assertNotEqual(a.tobytes(),b.tobytes())
        for y in list(range(0,163))+list(range(192,240)):
            self.assertEqual(a.crop((0,y,320,y+1)).tobytes(),
                             b.crop((0,y,320,y+1)).tobytes())

    def test_pause_frame_does_not_animate(self):
        a=load('ui-22-paused-phase-a.ppm');b=load('ui-22-paused-phase-b.ppm')
        self.assertEqual(a.tobytes(),b.tobytes())

    def test_utf8_long_track_crops_are_inside_safe_region(self):
        im=load('ui-11-playing-spanish-utf8.ppm')
        self.assertEqual(im.size,(320,240))
        self.assertTrue(im.crop((22,78,303,159)).getbbox() is not None)

if __name__=='__main__': unittest.main()
