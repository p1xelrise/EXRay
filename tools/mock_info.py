# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
# Mock-up of the shortcuts window using the same layout rules as info_window.cpp
# (Liberation Sans stands in for Segoe UI). Output: argv[1]
import sys
from PIL import Image, ImageDraw, ImageFont
R='/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf'
B='/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf'
S=lambda v: v  # 100% scaling
title=ImageFont.truetype(B,22); header=ImageFont.truetype(B,15); body=ImageFont.truetype(R,13); chipf=ImageFont.truetype(B,12)
cols=[
 [('H','Files and folders'),('K','Ctrl|O','Open a file'),('K','Ctrl|Shift|O','Open a folder as a grid'),('K','Drop','Drop a file or folder on the window'),('K','F5','Refresh the folder'),('K','←|→','Previous / next file'),('K','Home|End','First / last file'),('K','Ctrl|E','Export as PNG or JPEG'),
  ('H','Switching views'),('K','G','Show the folder grid'),('K','Esc','From an image back to the grid'),('K','Enter','Open the selected image'),('K','Double-click','Open the image under the mouse'),('K','F1','This window')],
 [('H','Grid'),('K','Arrows','Move the selection'),('K','Shift|Arrows','Extend the selection'),('K','Ctrl|Click','Add or remove one image'),('K','Shift|Click','Select a range'),('K','Ctrl|A','Select all'),('K','Right-click','Open / export menu'),('K','Ctrl|Wheel','Thumbnail size'),('K','Ctrl|0','Thumbnails back to 100%'),
  ('H','Exposure and tonemap'),('K','+|−','Exposure up / down by 0.5 EV'),('K','R','Reset exposure to 0'),('K','T','Next tonemap: Clip, Reinhard, ACES'),('K','Wheel','On the slider: fine steps'),('K','Double-click','On the slider: reset')],
 [('H','Image view'),('K','Wheel','Zoom in / out at the mouse'),('K','Drag','Move around a zoomed image'),('K','Double-click','Switch between Fit and 100%'),('K','F','Fit to window'),('K','1','100%: one image pixel per screen pixel'),('K','Ctrl|+','Zoom in'),('K','Ctrl|−','Zoom out'),('K','Ctrl|C','Copy the pixel values'),
  ('H','EXR layers'),('K','L','Choose a layer'),('K','[|]','Previous / next layer'),('K','I','Invert a depth view'),
  ('H','Cryptomatte'),('K','Click','Show only that object as a mask'),('K','Ctrl|Click','Add or remove objects'),('K','Esc','Show all objects again')]]
colW=330; gap=40; margin=28; pad=9; cgap=4; ch=22; footer=56; textGap=14
tw=lambda f,t: int(f.getlength(t))
def chipsw(k):
    parts=k.split('|'); return sum(tw(chipf,p)+2*pad for p in parts)+cgap*(len(parts)-1)
keyw=[max(chipsw(it[1]) for it in c if it[0]=='K') for c in cols]
headerH=margin+30+22+14
hs=[]
for c in cols:
    y=0;first=True
    for it in c:
        if it[0]=='H': y+= (0 if first else 16)+30
        else: y+=28
        first=False
    hs.append(y)
W=2*margin+3*colW+2*gap; H=headerH+max(hs)+16+footer
im=Image.new('RGB',(W,H),(0x1e,0x1e,0x1e)); d=ImageDraw.Draw(im)
d.text((margin,margin+4),'Controls and shortcuts',font=title,fill=(240,240,240))
d.text((margin,margin+34),'HDR & EXR file preview — keyboard and mouse',font=body,fill=(0x8a,0x8a,0x8a))
for ci,c in enumerate(cols):
    x0=margin+ci*(colW+gap); y=headerH; first=True
    for it in c:
        if it[0]=='H':
            if not first: y+=16
            d.text((x0,y+30-6-16),it[1],font=header,fill=(0xe6,0xe6,0xe6)); d.line((x0,y+27,x0+colW,y+27),fill=(0x34,0x34,0x34)); y+=30
        else:
            cx=x0; cy=y+(28-ch)//2
            for p in it[1].split('|'):
                w=tw(chipf,p)+2*pad
                d.rounded_rectangle((cx,cy,cx+w,cy+ch),radius=4,fill=(0x33,0x33,0x33),outline=(0x46,0x46,0x46))
                d.text((cx+pad,cy+4),p,font=chipf,fill=(0xe8,0xe8,0xe8)); cx+=w+cgap
            d.text((x0+keyw[ci]+textGap,y+7),it[2],font=body,fill=(0xbc,0xbc,0xbc)); y+=28
        first=False
d.rectangle((0,H-footer,W,H),fill=(0x25,0x25,0x25))
d.rounded_rectangle((W-margin-96,H-footer+12,W-margin,H-footer+44),radius=5,fill=(0x3a,0x7b,0xc8)); d.text((W-margin-66,H-footer+21),'Close',font=body,fill=(255,255,255))
im.save(sys.argv[1]); print(W,H)
