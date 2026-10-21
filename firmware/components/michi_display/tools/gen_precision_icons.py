#!/usr/bin/env python3
"""UI-06 precision icon masters. Deterministic 2-bpp glyphs for RGB565 SPI LCD.

Inputs are authored curves in normalized 24x24 geometry. Output is a
row-major 2-bpp coverage table, four pixels per byte, MSB first. The
rendering firmware interpolates between the foreground and existing surface.
This is build-time only, with no font, SVG, heap or Pillow on the ESP32.
"""
from __future__ import annotations
import argparse
from pathlib import Path
from PIL import Image, ImageDraw

SIZES=(12,16,20,32)
KINDS=('wifi','receiver','room','speaker')
SCALE=8

def make_icon(kind: str, size: int) -> Image.Image:
    high = Image.new('L',(size*SCALE,size*SCALE),0)
    d=ImageDraw.Draw(high)
    f = size*SCALE/24.0
    def pt(x,y): return (round(x*f),round(y*f))
    def line(pts,weight=1.5):
        d.line([pt(*q) for q in pts], fill=255,width=max(1,round(weight*f)),joint='curve')
        for px,py in (pts[0],pts[-1]):
            r=round(weight*f/2)
            cx,cy=pt(px,py)
            d.ellipse((cx-r,cy-r,cx+r,cy+r),fill=255)
    def arc(box,a0,a1,width=1.5):
        d.arc(tuple(pt(*q) for q in ((box[0],box[1]),(box[2],box[3]))),start=a0,end=a1,fill=255,width=max(1,round(width*f)))
    if kind=='wifi':
        # Top arcs only, ample whitespace around the dot: no cloud silhouette.
        arc((2.0,4.0,22.0,24.0),215,325,1.55 if size>=20 else 1.45)
        arc((6.25,8.25,17.75,19.75),214,326,1.55)
        cx,cy=pt(12.0,19.2); r=max(1,round((1.35 if size>=16 else 1.55)*f))
        d.ellipse((cx-r,cy-r,cx+r,cy+r),fill=255)
    elif kind=='receiver':
        # Audiophile appliance front, not a three-U rack with huge thick lines.
        x0,y0=pt(3.7,5.4);x1,y1=pt(20.3,18.6)
        d.rounded_rectangle((x0,y0,x1,y1),radius=max(1,round(2.0*f)),outline=255,width=max(1,round(1.55*f)))
        line([(7.0,10.1),(14.2,10.1)],1.4)
        line([(7.0,13.8),(14.2,13.8)],1.4)
        cx,cy=pt(17.0,15.0);r=max(1,round(0.85*f))
        d.ellipse((cx-r,cy-r,cx+r,cy+r),fill=255)
    elif kind=='room':
        # The label says Living Room / Michi Studio: a house/room, not geolocation.
        line([(4.0,11.6),(12.0,4.4),(20.0,11.6)],1.65)
        line([(6.2,10.3),(6.2,19.6),(17.8,19.6),(17.8,10.3)],1.55)
        line([(10.4,19.5),(10.4,14.4),(13.6,14.4),(13.6,19.5)],1.35)
    elif kind=='speaker':
        line([(3.5,9.2),(7.2,9.2),(12.2,5.5),(12.2,18.5),(7.2,14.8),(3.5,14.8),(3.5,9.2)],1.35)
        arc((7.0,6.4,20.6,19.6),-54,54,1.4) # right-side arc around loudspeaker
    # Downsample supersampled artist-authored linework to 2-bpp alpha levels.
    result=high.resize((size,size),Image.Resampling.LANCZOS)
    result=result.point(lambda a: 0 if a<30 else (85 if a<119 else (170 if a<210 else 255)))
    return result

def encode(img):
    size=img.width
    vals=[img.getpixel((x,y))//85 for y in range(size) for x in range(size)]
    data=[]
    for i in range(0,len(vals),4):
        v=vals[i:i+4]+[0]*max(0,4-len(vals[i:i+4]))
        data.append((v[0]<<6)|(v[1]<<4)|(v[2]<<2)|v[3])
    return data

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--preview',type=Path)
    args=p.parse_args()
    lines=['/* Michi UI-06 precision icons: 2bpp coverage, row major, MSB first. */', '#pragma once','#include <stdint.h>']
    for kind in KINDS:
        for size in SIZES:
            dat=encode(make_icon(kind,size))
            lines.append(f'static const uint8_t michi_precision_{kind}_{size}[] = {{')
            for i in range(0,len(dat),16):lines.append('    '+', '.join(f'0x{x:02X}' for x in dat[i:i+16])+',')
            lines.append('};')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text('\n'.join(lines)+'\n')
    if args.preview:
        args.preview.mkdir(parents=True,exist_ok=True)
        for kind in KINDS:
            for size in SIZES:
                make_icon(kind,size).save(args.preview/f'{kind}_{size}.png')
    print(f'generated {len(KINDS)*len(SIZES)} icons in {args.output}')
if __name__=='__main__':main()
