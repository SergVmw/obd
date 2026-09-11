#!/usr/bin/env python3
"""Generate the H2 Gauge three-sheet principle/reference schematic as SVG.

The drawing is intentionally module-level for ESP32 DevKit, GC9A01 and MP1584,
but pin-level for every external connection and for the protection, CAN, LPG and
backlight circuits.  No EDA package is required; the output is editable SVG.
"""
from __future__ import annotations

import base64
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
W, H = 1600, 1131
REG = ROOT / "assets" / "fonts" / "H2GaugeSans.woff"
BOLD = ROOT / "assets" / "fonts" / "H2GaugeSans-Bold.woff"


def data_font(path: str) -> str:
    p = Path(path)
    if not p.exists():
        return ""
    return base64.b64encode(p.read_bytes()).decode("ascii")


FONT_CSS = f"""
@font-face{{font-family:H2Sans;src:url(data:font/woff;base64,{data_font(REG)}) format('woff');font-weight:400}}
@font-face{{font-family:H2Sans;src:url(data:font/woff;base64,{data_font(BOLD)}) format('woff');font-weight:700}}
"""

STYLE = FONT_CSS + """
:root{color-scheme:light}
.page{fill:#ffffff}
.frame{fill:none;stroke:#111827;stroke-width:2.2}
.wire{fill:none;stroke:#111827;stroke-width:2.25;stroke-linecap:round;stroke-linejoin:round}
.bus{fill:none;stroke:#0f4c81;stroke-width:3.2;stroke-linecap:round;stroke-linejoin:round}
.comp{fill:#fff;stroke:#111827;stroke-width:2.2;stroke-linejoin:round}
.ic{fill:#f8fafc;stroke:#111827;stroke-width:2.4}
.module{fill:#f7fbff;stroke:#0f4c81;stroke-width:2.6}
.pin{fill:none;stroke:#111827;stroke-width:2.0;stroke-linecap:round}
.node{fill:#111827}
.nc{stroke:#b42318;stroke-width:2.2;stroke-linecap:round}
.txt{font-family:H2Sans,'DejaVu Sans',sans-serif;fill:#111827;font-size:15px}
.small{font-family:H2Sans,'DejaVu Sans',sans-serif;fill:#334155;font-size:12px}
.tiny{font-family:H2Sans,'DejaVu Sans',sans-serif;fill:#475569;font-size:10.5px}
.ref{font-family:H2Sans,'DejaVu Sans',sans-serif;fill:#0f172a;font-weight:700;font-size:14px}
.val{font-family:H2Sans,'DejaVu Sans',sans-serif;fill:#0f172a;font-size:13px}
.pintext{font-family:H2Sans,'DejaVu Sans',sans-serif;fill:#111827;font-size:12px}
.title{font-family:H2Sans,'DejaVu Sans',sans-serif;fill:#0f172a;font-weight:700;font-size:25px}
.subtitle{font-family:H2Sans,'DejaVu Sans',sans-serif;fill:#334155;font-size:14px}
.netbox{fill:#eaf4ff;stroke:#0f4c81;stroke-width:1.5}
.nettext{font-family:H2Sans,'DejaVu Sans',sans-serif;fill:#0f4c81;font-weight:700;font-size:12px}
.note{fill:#fff8e6;stroke:#b7791f;stroke-width:1.6}
.warn{fill:#fff1f0;stroke:#b42318;stroke-width:1.8}
.ok{fill:#eefbf3;stroke:#16794c;stroke-width:1.5}
.sheetline{stroke:#64748b;stroke-width:1.2}
"""


def t(x, y, text, cls="txt", anchor="start", rotate=None):
    tr = f' transform="rotate({rotate} {x} {y})"' if rotate is not None else ""
    return f'<text x="{x}" y="{y}" class="{cls}" text-anchor="{anchor}"{tr}>{escape(str(text))}</text>'


def line(x1, y1, x2, y2, cls="wire"):
    return f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" class="{cls}"/>'


def poly(points, cls="wire"):
    pts = " ".join(f"{x},{y}" for x, y in points)
    return f'<polyline points="{pts}" class="{cls}"/>'


def rect(x, y, w, h, cls="comp", rx=0):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" class="{cls}"/>'


def path(d, cls="wire", fill="none"):
    return f'<path d="{d}" class="{cls}" fill="{fill}"/>'


def node(x, y, r=4):
    return f'<circle cx="{x}" cy="{y}" r="{r}" class="node"/>'


def nc(x, y, s=7):
    return line(x-s, y-s, x+s, y+s, "nc") + line(x-s, y+s, x+s, y-s, "nc")


def net_label(x, y, label, side="right"):
    w = max(76, len(label) * 7.3 + 18)
    if side == "left":
        bx = x - w
        tip = f'<path d="M {x} {y} l -10 -10 H {bx} V {y+10} H {x-10} Z" class="netbox"/>'
        tx = x - 13
        anc = "end"
    else:
        bx = x
        tip = f'<path d="M {x} {y} l 10 -10 H {bx+w} V {y+10} H {x+10} Z" class="netbox"/>'
        tx = x + 13
        anc = "start"
    return tip + t(tx, y+4, label, "nettext", anc)


def ground(x, y, label="GND"):
    return (
        line(x, y, x, y+10)
        + line(x-14, y+10, x+14, y+10)
        + line(x-9, y+16, x+9, y+16)
        + line(x-4, y+22, x+4, y+22)
        + t(x+19, y+18, label, "tiny")
    )


def pwr(x, y, label="+3V3"):
    return (
        line(x, y, x, y-14)
        + f'<path d="M {x-8} {y-14} L {x} {y-27} L {x+8} {y-14} Z" fill="#111827"/>'
        + t(x, y-33, label, "nettext", "middle")
    )


def resistor_h(x1, x2, y, ref, val, above=True):
    cx = (x1+x2)/2
    bw = min(76, max(48, abs(x2-x1)-32))
    xa, xb = cx-bw/2, cx+bw/2
    yy = y-15 if above else y+33
    return (
        line(x1, y, xa, y) + rect(xa, y-10, bw, 20) + line(xb, y, x2, y)
        + t(cx, yy, ref, "ref", "middle") + t(cx, yy+(16 if above else 16), val, "val", "middle")
    )


def resistor_v(x, y1, y2, ref, val, right=True):
    cy = (y1+y2)/2
    bh = min(76, max(48, abs(y2-y1)-32))
    ya, yb = cy-bh/2, cy+bh/2
    tx = x+16 if right else x-16
    anc = "start" if right else "end"
    return (
        line(x, y1, x, ya) + rect(x-10, ya, 20, bh) + line(x, yb, x, y2)
        + t(tx, cy-4, ref, "ref", anc) + t(tx, cy+13, val, "val", anc)
    )


def cap_v(x, y1, y2, ref, val, right=True, polarized=False):
    cy = (y1+y2)/2
    tx = x+18 if right else x-18
    anc = "start" if right else "end"
    plus = t(x+15, cy-10, "+", "ref") if polarized else ""
    return (line(x,y1,x,cy-7)+line(x-17,cy-7,x+17,cy-7)+line(x-17,cy+7,x+17,cy+7)+line(x,cy+7,x,y2)
            +plus+t(tx,cy-18,ref,"ref",anc)+t(tx,cy+31,val,"val",anc))


def cap_h(x1, x2, y, ref, val, above=True, polarized=False):
    cx=(x1+x2)/2
    plus=t(cx-17,y-15,"+","ref") if polarized else ""
    yy=y-25 if above else y+34
    return (line(x1,y,cx-7,y)+line(cx-7,y-17,cx-7,y+17)+line(cx+7,y-17,cx+7,y+17)+line(cx+7,y,x2,y)
            +plus+t(cx,yy,ref,"ref","middle")+t(cx,yy+17,val,"val","middle"))


def fuse_h(x1, x2, y, ref, val):
    cx=(x1+x2)/2; bw=72
    return (line(x1,y,cx-bw/2,y)+rect(cx-bw/2,y-12,bw,24)+line(cx-bw/2+8,y,cx+bw/2-8,y)
            +line(cx+bw/2,y,x2,y)+t(cx,y-20,ref,"ref","middle")+t(cx,y+36,val,"val","middle"))


def inductor_h(x1, x2, y, ref, val):
    cx=(x1+x2)/2; start=cx-44; end=cx+44
    d=f"M {start} {y} "
    for i in range(4):
        x=start+i*22
        d += f"C {x+3} {y-24}, {x+19} {y-24}, {x+22} {y} "
    return line(x1,y,start,y)+path(d)+line(end,y,x2,y)+t(cx,y-30,ref,"ref","middle")+t(cx,y+25,val,"val","middle")


def diode_between(x1, y1, x2, y2, ref="", val="", label_offset=-20, tvs=False):
    import math
    dx,dy=x2-x1,y2-y1
    length=math.hypot(dx,dy)
    angle=math.degrees(math.atan2(dy,dx))
    cx=(x1+x2)/2; cy=(y1+y2)/2
    lead=max(0,length/2-24)
    cath = '<path d="M 10 -15 V -6 l 7 -5 M 10 15 V 6 l -7 5" class="wire"/>' if tvs else '<line x1="10" y1="-16" x2="10" y2="16" class="wire"/>'
    g=(f'<g transform="translate({cx} {cy}) rotate({angle})">'
       f'<line x1="{-length/2}" y1="0" x2="-22" y2="0" class="wire"/>'
       '<path d="M -21 -15 L 7 0 L -21 15 Z" fill="#111827" stroke="#111827" stroke-width="1.4"/>'
       +cath+f'<line x1="10" y1="0" x2="{length/2}" y2="0" class="wire"/></g>')
    if ref:
        g += t(cx,cy+label_offset,ref,"ref","middle")+t(cx,cy+label_offset+16,val,"val","middle")
    return g


def diode_v(x, y1, y2, ref, val, tvs=False, right=True):
    # Direction is from y1 (anode) to y2 (cathode).
    s=diode_between(x,y1,x,y2,tvs=tvs)
    tx=x+24 if right else x-24; anc="start" if right else "end"
    return s+t(tx,(y1+y2)/2-5,ref,"ref",anc)+t(tx,(y1+y2)/2+13,val,"val",anc)


def transistor_npn(x, y, ref, val, pins="1=B  2=E  3=C"):
    # base enters from left, collector exits top, emitter exits bottom
    return (
        f'<circle cx="{x}" cy="{y}" r="31" class="comp"/>'
        + line(x-31,y,x-10,y)+line(x-10,y-20,x-10,y+20)
        + line(x-10,y-12,x+15,y-27)+line(x+15,y-27,x+15,y-43)
        + line(x-10,y+12,x+15,y+27)+line(x+15,y+27,x+15,y+43)
        + f'<path d="M {x+4} {y+18} l 10 10 l -14 -2 Z" fill="#111827"/>'
        + t(x+42,y-17,ref,"ref")+t(x+42,y, val,"val")+t(x+42,y+17,pins,"tiny")
    )


def pmos_v(x, y, ref, val):
    # P-channel MOSFET; source top pin 2, drain bottom pin 3, gate left pin 1.
    return (
        f'<circle cx="{x}" cy="{y}" r="34" class="comp"/>'
        + line(x,y-52,x,y-24)+line(x,y+24,x,y+52)
        + line(x,y-24,x,y+24)+line(x-27,y-18,x-27,y+18)+line(x-52,y,x-27,y)
        + f'<path d="M {x-18} {y} l 10 -6 v 12 Z" fill="#111827"/>'
        + diode_between(x+15,y+19,x+15,y-19)
        + t(x+43,y-17,ref,"ref")+t(x+43,y,val,"val")+t(x+43,y+17,"1=G  2=S  3=D","tiny")
    )


def switch_to_ground(x, y_top, y_bottom, ref, val="NO, active LOW"):
    # vertical normally-open switch
    ym=(y_top+y_bottom)/2
    return (line(x,y_top,x,ym-22)+f'<circle cx="{x}" cy="{ym-17}" r="4" class="node"/>'
            +f'<circle cx="{x}" cy="{ym+17}" r="4" class="node"/>'
            +line(x,ym+17,x,y_bottom)+line(x-1,ym-17,x+22,ym+9)
            +t(x+32,ym-5,ref,"ref")+t(x+32,ym+13,val,"val"))


def pin_left(body_x, y, name, num, lead=28):
    return line(body_x-lead,y,body_x,y,"pin")+t(body_x+8,y+4,f"{num}  {name}","pintext")


def pin_right(body_x2, y, name, num, lead=28):
    return line(body_x2,y,body_x2+lead,y,"pin")+t(body_x2-8,y+4,f"{name}  {num}","pintext","end")


def pin_top(x, body_y, name, num, lead=28):
    return line(x,body_y-lead,x,body_y,"pin")+t(x+4,body_y+17,f"{num} {name}","pintext",rotate=90)


def pin_bottom(x, body_y2, name, num, lead=28):
    return line(x,body_y2,x,body_y2+lead,"pin")+t(x-4,body_y2-17,f"{name} {num}","pintext",rotate=90)


def module_box(x,y,w,h,ref,value,subtitle=""):
    return (rect(x,y,w,h,"module",8)+t(x+w/2,y+29,ref,"ref","middle")+t(x+w/2,y+50,value,"val","middle")
            +(t(x+w/2,y+69,subtitle,"tiny","middle") if subtitle else ""))


def ic_box(x,y,w,h,ref,value,subtitle=""):
    return (rect(x,y,w,h,"ic",4)+t(x+w/2,y+28,ref,"ref","middle")+t(x+w/2,y+48,value,"val","middle")
            +(t(x+w/2,y+66,subtitle,"tiny","middle") if subtitle else ""))


def header(title, subtitle, sheet):
    return (rect(30,25,W-60,H-55,"frame")+t(50,61,title,"title")+t(50,85,subtitle,"subtitle")
            +t(W-52,61,f"ЛИСТ {sheet}/4","ref","end")+line(45,99,W-45,99,"sheetline"))


def title_block(sheet_title, sheet):
    y=1030
    return (line(45,y,W-45,y,"sheetline")+t(55,y+24,"H2 GAUGE  •  REV 0.1.9","ref")
            +t(55,y+47,"ПРИНЦИПИАЛЬНАЯ / REFERENCE SCHEMATIC","small")
            +t(530,y+24,sheet_title,"ref")+t(530,y+47,"Дата: 2026-09-09  •  Формат: A3 landscape","small")
            +t(1320,y+24,f"Лист {sheet} из 4","ref")+t(1320,y+47,"Не для серийного производства","small"))


def note_box(x,y,w,h,lines,kind="note", title=None):
    s=rect(x,y,w,h,kind,6)
    yy=y+23
    if title:
        s+=t(x+12,yy,title,"ref"); yy+=22
    for ln in lines:
        s+=t(x+12,yy,ln,"small"); yy+=18
    return s


def esp_module(x,y,ref,auto=False):
    w,h=300,610
    s=module_box(x,y,w,h,ref,"ESP32 DevKit V1","ESP32-WROOM-32 + onboard 3.3 V regulator")
    # left power and inputs
    left=[(y+105,"VIN / 5V","VIN"),(y+155,"3V3 OUT","3V3"),(y+205,"GND","GND"),
          (y+470,"GPIO34 / LPG","34"),(y+530,"GPIO32 / BUTTON","32")]
    for yy,name,num in left:s+=pin_left(x,yy,name,num)
    right=[(y+105,"GPIO16 / TWAI_TX","16"),(y+150,"GPIO17 / TWAI_RX","17"),
           (y+260,"GPIO13 / MOSI","13"),(y+305,"GPIO14 / SCLK","14"),
           (y+350,"GPIO33 / TFT_CS","33"),(y+395,"GPIO27 / TFT_DC","27"),
           (y+440,"GPIO26 / TFT_RST","26"),(y+500,"GPIO25 / BL_PWM","25")]
    for yy,name,num in right:s+=pin_right(x+w,yy,name,num)
    if not auto:
        # USB-facing internal board nets, shown separately from header pins.
        s+=pin_left(x,y+265,"USB VBUS","USB1")+pin_left(x,y+305,"USB D−","USB2")+pin_left(x,y+345,"USB D+","USB3")
    s+=t(x+w/2,y+h-24,"Остальные выводы — NC в данном проекте","tiny","middle")
    return s


def can_ic(x,y,ref):
    w,h=260,250
    s=ic_box(x,y,w,h,ref,"SN65HVD230","3.3-V high-speed CAN transceiver")
    s+=pin_left(x,y+90,"D / TXD",1)+pin_left(x,y+135,"R / RXD",4)+pin_left(x,y+190,"Rs",8)
    s+=pin_right(x+w,y+90,"CANH",7)+pin_right(x+w,y+135,"CANL",6)+pin_right(x+w,y+190,"Vref",5)
    s+=pin_top(x+42,y,"VCC",3)+pin_bottom(x+42,y+h,"GND",2)
    return s


def tft_conn(x,y,ref):
    w,h=300,410
    s=module_box(x,y,w,h,ref,"GC9A01 240×240 TFT","8-pin module connector; verify board silk")
    pins=[(y+100,"SDA / MOSI",4),(y+145,"SCL / SCLK",3),(y+190,"CS",7),(y+235,"DC",6),
          (y+280,"RES / RST",5),(y+325,"BLK / BL",8)]
    for yy,name,num in pins:s+=pin_left(x,yy,name,num)
    s+=pin_top(x+265,y,"VCC",2)+pin_bottom(x+265,y+h,"GND",1)
    return s


def obd_partial(x,y,ref,pins):
    h=62+len(pins)*45
    s=module_box(x,y,170,h,ref,"OBD-II (SAE J1962)","connector contacts used here")
    for i,(num,name,side) in enumerate(pins):
        yy=y+72+i*45
        if side=="left":s+=pin_left(x,yy,name,num,25)
        else:s+=pin_right(x+170,yy,name,num,25)
    return s


def usb_conn(x,y):
    w,h=190,235
    s=module_box(x,y,w,h,"J101","USB Micro-B","connector fitted on A101 DevKit")
    names=[("VBUS",1),("D−",2),("D+",3),("ID",4),("GND",5)]
    for i,(name,num) in enumerate(names):s+=pin_right(x+w,y+75+i*35,name,num,24)
    s+=pin_bottom(x+55,y+h,"SHIELD","S",24)
    return s


def sheet1():
    s=header("H2 Gauge — стенд с питанием от USB", "Полная принципиальная схема текущего настольного варианта", 1)
    # Main modules
    s+=usb_conn(70,195)
    s+=esp_module(470,170,"A101",auto=False)
    s+=can_ic(930,145,"U101")
    s+=tft_conn(1025,515,"J102")
    s+=obd_partial(1355,170,"J103",[(6,"CANH","left"),(14,"CANL","left"),(5,"SIG_GND","left"),(16,"VBAT +12 V","left")])

    # USB connector to DevKit internal USB nets
    usb_y=[270,305,340,375,410]
    esp_y=[435,475,515]
    s+=poly([(284,usb_y[0]),(360,usb_y[0]),(360,435),(442,435)])
    s+=poly([(284,usb_y[1]),(345,usb_y[1]),(345,475),(442,475)])
    s+=poly([(284,usb_y[2]),(330,usb_y[2]),(330,515),(442,515)])
    s+=nc(284,usb_y[3])
    s+=poly([(284,usb_y[4]),(310,usb_y[4]),(310,770),(470,770)])+ground(310,770)
    s+=ground(125,454,"SHIELD")
    # ESP power. VIN is fed internally from USB; show no external 5V.
    s+=nc(442,275)+pwr(405,325,"+3V3_USB")+line(405,325,442,325)+line(442,375,415,375)+ground(415,375)

    # CAN connections TX/RX direct
    s+=line(798,275,902,275)+poly([(902,275),(902,235),(902,235),(902,235)])
    # U left pins y235 and 280; ESP y275/320
    s+=poly([(798,275),(870,275),(870,235),(902,235)])
    s+=poly([(798,320),(885,320),(885,280),(902,280)])
    # CAN supply, decoupling, Rs, Vref
    s+=pwr(972,117,"+3V3_USB")
    s+=line(972,395,972,428)+ground(972,428)
    s+=line(902,335,875,335)+ground(875,335)
    s+=nc(1218,335)
    s+=cap_v(855,120,190,"C101","100 nF",right=False)+pwr(855,120,"+3V3_USB")+ground(855,190)
    # CAN bus and OBD pins
    s+=poly([(1218,235),(1225,235),(1225,242),(1355,242)],"bus")+node(1225,235)
    s+=poly([(1218,280),(1270,280),(1270,287),(1355,287)],"bus")+node(1270,280)
    s+=line(1225,235,1225,315)+diode_v(1225,315,415,"D101","PESD1CAN-U",False,False)+ground(1225,415)
    s+=line(1270,280,1270,325)+diode_v(1270,325,415,"D102","PESD1CAN-U",False,True)+ground(1270,415)
    # OBD ground and no power
    s+=poly([(1330,332),(1340,332),(1340,425)])+ground(1340,425)
    s+=nc(1330,377)+t(1308,400,"OBD pin 16: NC", "tiny","end")
    s+=note_box(1335,700,205,100,["120 Ω: DNP","не устанавливать;","CANH/CANL — витая пара"],"warn","CAN")

    # TFT SPI lines. ESP outputs to TFT left pins.
    ey=[430,475,520,565,610]
    ty=[615,660,705,750,795]
    for yy1,yy2 in zip(ey,ty):
        s+=poly([(798,yy1),(900,yy1),(900,yy2),(997,yy2)])
    # Reset pulldown
    s+=line(955,795,955,900)+resistor_v(955,820,900,"R101","10 kΩ",False)+ground(955,900)
    # TFT VCC/GND + decoupling and BL direct 3v3
    s+=pwr(1290,487,"+3V3_USB")
    s+=line(1290,925,1290,945)+ground(1290,945)
    s+=line(997,840,935,840)+pwr(935,840,"+3V3_USB")
    s+=cap_v(1420,600,680,"C102","100 nF",False)+pwr(1420,600,"+3V3_USB")+ground(1420,680)
    s+=cap_v(1500,600,680,"C103","10 µF",False,True)+pwr(1500,600,"+3V3_USB")+ground(1500,680)

    # GPIO25 intentionally NC on bench
    s+=nc(798,670)+t(813,691,"GPIO25 свободен", "tiny")
    # Button GPIO32
    s+=poly([(442,700),(405,700),(405,905)])
    s+=resistor_v(405,830,760,"R102","10 kΩ",True)+pwr(405,760,"+3V3_USB")
    s+=switch_to_ground(405,905,970,"SW101")+ground(405,970)
    # GPIO34 NC
    s+=nc(442,640)+t(429,660,"LPG не подключён", "tiny","end")

    s+=note_box(55,860,300,105,["• питание A101 только через USB;","• OBD J103: pins 6, 14, 5;","• pin 16 (+12 V) остаётся NC;","• J102 pin 8 BLK → 3.3 V."],"ok","СТЕНД")
    s+=title_block("Вариант 1 — USB-стенд",1)
    return s


def tps_ic(x,y):
    w,h=380,540
    s=ic_box(x,y,w,h,"U201","TPS26600PWP","60-V eFuse, HTSSOP-16, auto-retry")
    left=[(y+75,"IN", "1,2"),(y+165,"UVLO","3"),(y+255,"OVP","5"),
          (y+345,"SHDN","7"),(y+435,"RTN","8"),(y+495,"MODE","6")]
    right=[(y+75,"OUT","15,16"),(y+165,"GND","9"),(y+255,"ILIM","11"),
           (y+345,"dVdT","12"),(y+435,"FLT","14"),(y+495,"IMON","10")]
    for yy,n,num in left:s+=pin_left(x,yy,n,num)
    for yy,n,num in right:s+=pin_right(x+w,yy,n,num)
    s+=pin_bottom(x+145,y+h,"N.C.","4,13")
    s+=pin_bottom(x+280,y+h,"PowerPAD→RTN","EP")
    # Stylized internal back-to-back FET path.
    s+=line(x+80,y+75,x+135,y+75)+diode_between(x+135,y+75,x+190,y+75)+diode_between(x+245,y+75,x+190,y+75)+line(x+245,y+75,x+300,y+75)
    s+=t(x+190,y+107,"internal back-to-back MOSFETs", "tiny","middle")
    return s


def mp_module(x,y):
    w,h=270,260
    s=module_box(x,y,w,h,"A201","MP1584 buck module","adjust trimmer to 5.00 V before load")
    s+=pin_left(x,y+95,"IN+",1)+pin_left(x,y+170,"IN−",2)
    s+=pin_right(x+w,y+95,"OUT+",3)+pin_right(x+w,y+170,"OUT−",4)
    s+=t(x+w/2,y+h-24,"VIN absolute maximum of IC: 28 V", "tiny","middle")
    return s


def sheet2():
    s=header("H2 Gauge — защищённое питание автомобиля", "OBD-II +12 V → TPS26600 → LC → MP1584 5 V; pin-level protection circuit",2)
    s+=obd_partial(55,155,"J201A",[(16,"VBAT +12 V","right"),(5,"SIG_GND / RTN","right"),(4,"CHASSIS","right")])
    yrail, rtny, gndy = 210, 840, 760

    # Connector, fuse and protected-input rail.
    s+=line(225,227,250,227)+fuse_h(250,390,227,"F201","1 A, inline")
    s+=poly([(390,227),(470,227),(470,yrail),(662,yrail)])+net_label(415,227,"VBAT_FUSED","right")
    s+=poly([(225,272),(275,272),(275,rtny),(660,rtny)])+net_label(305,rtny,"PWR_RTN","right")
    s+=line(225,317,190,317)+nc(190,317)+t(178,340,"pin 4 chassis: NC", "tiny","end")

    # Bidirectional TVS and non-polar input capacitors.  These are upstream of reverse-polarity isolation.
    s+=line(350,yrail,350,300)+diode_v(350,300,470,"D201","SMCJ30CA",True,False)+line(350,470,350,rtny)+node(350,yrail)+node(350,rtny)
    s+=line(415,yrail,415,300)+cap_v(415,300,650,"C201","100 nF / 100 V",False)+line(415,650,415,rtny)+node(415,yrail)+node(415,rtny)
    s+=line(490,yrail,490,500)+cap_v(490,500,rtny,"C202","1 µF film / 100 V",False)+node(490,yrail)+node(490,rtny)
    s+=t(392,525,"TVS/C201/C202 → PWR_RTN", "tiny","middle")

    # eFuse and exact UVLO/OVP divider.
    ux,uy=690,135
    s+=tps_ic(ux,uy)
    s+=line(662,yrail,ux-28,yrail)
    xdiv=610
    s+=line(xdiv,yrail,xdiv,yrail+8)+resistor_v(xdiv,yrail+8,uy+165,"R201","487 kΩ / 1%",False)
    s+=node(xdiv,uy+165)+line(xdiv,uy+165,ux-28,uy+165)
    s+=resistor_v(xdiv,uy+165,uy+255,"R202","90.9 kΩ / 1%",False)
    s+=node(xdiv,uy+255)+line(xdiv,uy+255,ux-28,uy+255)
    s+=resistor_v(xdiv,uy+255,700,"R203","30.1 kΩ / 1%",False)+line(xdiv,700,xdiv,rtny)+node(xdiv,rtny)

    # SHDN enabled from input; RTN and MODE are distinct from system GND.
    s+=poly([(ux-28,uy+345),(560,uy+345),(560,470)])
    s+=resistor_h(470,560,470,"R204","100 kΩ",True)+line(470,470,470,yrail)+node(470,yrail)
    s+=poly([(ux-28,uy+435),(655,uy+435),(655,rtny)])+node(655,rtny)
    s+=poly([(ux-28,uy+495),(635,uy+495),(635,rtny)])+node(635,rtny)

    # NC and exposed pad.
    s+=nc(ux+145,uy+568)+t(ux+145,uy+592,"pins 4, 13: NC", "tiny","middle")
    s+=poly([(ux+280,uy+568),(ux+280,805),(680,805),(680,rtny)])+node(680,rtny)

    # ILIM and dV/dt support parts return to RTN (not to protected GND).
    s+=poly([(ux+408,uy+255),(1135,uy+255),(1135,560)])
    s+=resistor_v(1135,560,rtny,"R205","16.2 kΩ / 1%",True)+node(1135,rtny)
    s+=poly([(ux+408,uy+345),(1200,uy+345),(1200,570)])
    s+=cap_v(1200,570,rtny,"C203","22 nF",True)+node(1200,rtny)
    s+=nc(ux+408,uy+435)+t(ux+424,uy+439,"FLT NC", "tiny")
    s+=nc(ux+408,uy+495)+t(ux+424,uy+499,"IMON NC", "tiny")

    # Protected system ground is the GND pin side of the internal reverse-polarity path.
    s+=poly([(ux+408,uy+165),(1100,uy+165),(1100,gndy)])
    s+=line(1080,gndy,1550,gndy)+net_label(1430,gndy,"GND_PROTECTED","right")

    # eFuse output, COUT, LC filter and MP1584 module.
    outy=uy+75
    s+=line(ux+408,outy,1160,outy)
    s+=cap_v(1120,outy,gndy,"C204","10 µF / 63 V",False,True)+node(1120,outy)+node(1120,gndy)
    s+=inductor_h(1160,1320,outy,"L201","22 µH, Isat ≥ 1 A")
    s+=poly([(1320,outy),(1320,300),(1200,300),(1200,430),(1232,430)])+node(1320,outy)
    # Post-L capacitor is drawn horizontally above A201; its right plate returns to protected GND.
    s+=poly([(1320,outy),(1370,outy),(1370,285)])
    s+=cap_h(1370,1490,285,"C205","47 µF / 63 V",True,True)
    s+=poly([(1490,285),(1550,285),(1550,gndy)])+node(1550,gndy)
    s+=mp_module(1260,335)
    s+=poly([(1232,505),(1180,505),(1180,gndy)])+node(1180,gndy)
    s+=poly([(1558,505),(1540,505),(1540,gndy)])+node(1540,gndy)

    # Five-volt output and local output capacitors, routed below the module body.
    s+=poly([(1558,430),(1560,430),(1560,610),(1350,610)])+net_label(1350,610,"+5V_PROTECTED","left")
    s+=cap_v(1460,610,gndy,"C206","100 µF / 10 V",True,True)+node(1460,610)+node(1460,gndy)
    s+=cap_v(1390,610,gndy,"C207","100 nF",False)+node(1390,610)+node(1390,gndy)

    s+=note_box(55,875,500,125,["R201/R202/R203: 487 k / 90.9 k / 30.1 k (1%).","UVLO rising ≈ 5.98 V; falling ≈ 5.53 V.","OVP cutoff ≈ 24.03 V (VTH nominal 1.19 V).","R205 = 16.2 kΩ → current limit ≈ 0.74 A."],"ok","РАСЧЁТ TPS26600")
    s+=note_box(585,875,955,125,["RTN (pin 8) НЕ соединять напрямую с GND (pin 9): это отключит защиту от переполюсовки.","C201/C202 до U201 неполярные. PowerPAD соединить с RTN-полигоном несколькими vias.","TPS26600/MP1584 не AEC-Q: это reference design; проверить ISO 7637-2/ISO 16750-2, тепло и load dump."],"warn","КРИТИЧЕСКИ ВАЖНО")
    s+=title_block("Вариант 2 — автомобиль, лист питания",2)
    return s


def optocoupler(x,y):
    w,h=190,170
    s=ic_box(x,y,w,h,"U203","PC817C / EL817C","galvanically isolated LPG input")
    # input LED pins left
    s+=pin_left(x,y+62,"A",1)+pin_left(x,y+118,"K",2)
    # LED symbol and light arrows
    s+=diode_between(x+45,y+62,x+45,y+118)
    s+=line(x,y+62,x+45,y+62)+line(x+45,y+118,x,y+118)
    s+=line(x+72,y+72,x+103,y+57)+line(x+78,y+86,x+109,y+71)
    s+=f'<path d="M {x+101} {y+57} l -9 1 l 5 8 Z" fill="#111827"/>'
    s+=f'<path d="M {x+107} {y+71} l -9 1 l 5 8 Z" fill="#111827"/>'
    # phototransistor right, collector pin 4, emitter pin 3
    s+=line(x+125,y+53,x+125,y+117)+line(x+125,y+65,x+153,y+50)+line(x+153,y+50,x+190,y+50)
    s+=line(x+125,y+105,x+153,y+120)+line(x+153,y+120,x+190,y+120)
    s+=f'<path d="M {x+142} {y+112} l 10 8 l -13 1 Z" fill="#111827"/>'
    s+=t(x+w-8,y+54,"C  4","pintext","end")+t(x+w-8,y+124,"E  3","pintext","end")
    # isolation barrier
    s+=line(x+93,y+38,x+93,y+135,"sheetline")+line(x+99,y+38,x+99,y+135,"sheetline")
    return s


def sheet3():
    s=header("H2 Gauge — автомобиль: ESP32, CAN, TFT и кнопка", "Основная логика; detailed LPG/PWM circuits continue on sheet 4",3)
    ex,ey=330,150
    s+=esp_module(ex,ey,"A202",auto=True)

    # Protected 5 V input and 3.3 V rail generated by the DevKit module.
    s+=net_label(ex-28,ey+105,"+5V_PROTECTED","left")
    s+=pwr(250,ey+155,"+3V3")+line(250,ey+155,ex-28,ey+155)
    s+=line(ex-28,ey+205,275,ey+205)+ground(275,ey+205)
    s+=cap_v(60,180,260,"C208","100 nF",True)+pwr(60,180,"+3V3")+ground(60,260)
    s+=cap_v(130,180,260,"C209","10 µF",True,True)+pwr(130,180,"+3V3")+ground(130,260)

    # CAN transceiver and exact ESP32 TWAI connections.
    cx,cy=850,135
    s+=can_ic(cx,cy,"U202")
    s+=poly([(ex+328,ey+105),(750,ey+105),(750,cy+90),(cx-28,cy+90)])
    s+=poly([(ex+328,ey+150),(770,ey+150),(770,cy+135),(cx-28,cy+135)])
    s+=pwr(cx+42,cy-28,"+3V3")
    s+=line(cx+42,cy+278,cx+42,430)+ground(cx+42,430)
    s+=line(cx-28,cy+190,805,cy+190)+ground(805,cy+190)
    s+=nc(cx+288,cy+190)
    s+=cap_v(760,120,190,"C210","100 nF",False)+pwr(760,120,"+3V3")+ground(760,190)

    # OBD-II CAN contacts.  Power/ground contacts are J201A on sheet 2.
    s+=obd_partial(1370,135,"J201B",[(6,"CANH","left"),(14,"CANL","left")])
    s+=poly([(cx+288,cy+90),(1210,cy+90),(1210,207),(1370,207)],"bus")+node(1210,cy+90)
    s+=poly([(cx+288,cy+135),(1260,cy+135),(1260,252),(1370,252)],"bus")+node(1260,cy+135)
    s+=line(1210,cy+90,1210,320)+diode_v(1210,320,430,"D206","PESD1CAN-U",False,False)+ground(1210,430)
    s+=line(1260,cy+135,1260,330)+diode_v(1260,330,430,"D207","PESD1CAN-U",False,True)+ground(1260,430)
    s+=note_box(1310,330,235,82,["Rterm 120 Ω: DNP","НЕ УСТАНАВЛИВАТЬ","CANH/CANL — витая пара"],"warn","CAN")

    # TFT module and five direct SPI/control nets.
    tx,ty=850,480
    s+=tft_conn(tx,ty,"J202")
    eys=[ey+260,ey+305,ey+350,ey+395,ey+440]
    tys=[ty+100,ty+145,ty+190,ty+235,ty+280]
    for idx,(ya,yb) in enumerate(zip(eys,tys)):
        jog=700+idx*20
        s+=poly([(ex+328,ya),(jog,ya),(jog,yb),(tx-28,yb)])
    # Required TFT reset pull-down.
    s+=line(800,ty+280,800,920)+resistor_v(800,830,920,"R209","10 kΩ",False)+ground(800,920)
    s+=node(800,ty+280)

    # TFT power, decoupling and backlight net from the high-side switch on sheet 4.
    s+=pwr(tx+265,ty-28,"+3V3")
    s+=line(tx+265,ty+438,tx+265,950)+ground(tx+265,950)
    s+=cap_v(1285,500,580,"C211","100 nF",False)+pwr(1285,500,"+3V3")+ground(1285,580)
    s+=cap_v(1365,500,580,"C212","10 µF",False,True)+pwr(1365,500,"+3V3")+ground(1365,580)
    s+=line(tx-28,ty+325,785,ty+325)+net_label(785,ty+325,"TFT_BL","left")

    # Off-sheet LPG and PWM signals, continued as complete circuits on sheet 4.
    s+=line(ex-28,ey+470,260,ey+470)+net_label(260,ey+470,"LPG_SENSE","left")
    s+=line(ex+328,ey+500,690,ey+500)+net_label(690,ey+500,"BL_PWM_GPIO25","right")

    # One active-low MODE/WAKE button on GPIO32.
    s+=poly([(ex-28,ey+530),(280,ey+530),(280,900)])+node(280,ey+530)
    s+=line(280,ey+530,100,ey+530)+resistor_v(100,ey+530,590,"R216","10 kΩ",True)+pwr(100,590,"+3V3")
    s+=switch_to_ground(280,900,970,"SW201","MODE / WAKE")+ground(280,970)

    s+=note_box(55,740,230,115,["SW201 active LOW.","GPIO32 wakes from deep sleep.","Timer wake-up: 30 s.","Low-voltage software threshold: 11.5 V."],"ok","КНОПКА / SLEEP")
    s+=note_box(1235,650,310,110,["J202 pin order shown as a common 8-pin module.","Before assembly verify the actual GC9A01 silkscreen,","especially VCC/GND and BLK.  R209 holds RES low","during ESP32 start-up to prevent a display flash."],"note","ДИСПЛЕЙ")
    s+=title_block("Вариант 2 — автомобиль, основная логика",3)
    return s


def sheet4():
    s=header("H2 Gauge — автомобиль: полный вход LPG и PWM подсветки", "Discrete diode bridge + PC817C isolation; AO3401A/MMBT3904 high-side switch",4)

    # LPG valve connector and a true four-diode full bridge.
    s+=module_box(55,180,175,160,"J203","BRC valve tap","parallel to ONE coil")
    s+=pin_right(230,235,"VALVE_A",1,24)+pin_right(230,305,"VALVE_B",2,24)
    A=(285,235); B=(285,305); P=(455,165); N=(455,390)
    s+=line(254,235,*A)+line(254,305,*B)
    s+=diode_between(*A,*P)+diode_between(*B,*P)+diode_between(*N,*A)+diode_between(*N,*B)
    s+=node(*A)+node(*B)+node(*P)+node(*N)
    s+=t(350,140,"ПОЛНЫЙ МОСТ D202…D205", "ref","middle")
    s+=t(335,190,"D202", "ref","middle")+t(335,208,"1N4007", "tiny","middle")
    s+=t(392,240,"D203", "ref","middle")+t(392,258,"1N4007", "tiny","middle")
    s+=t(330,365,"D204", "ref","middle")+t(330,383,"1N4007", "tiny","middle")
    s+=t(408,332,"D205", "ref","middle")+t(408,350,"1N4007", "tiny","middle")

    # Split pulse-rated input resistance and optocoupler LED.
    s+=poly([P,(500,165),(500,190)])
    s+=resistor_h(500,680,190,"R213","2.2 kΩ / 0.25 W",True)
    s+=resistor_h(680,860,190,"R214","2.2 kΩ / 0.25 W",True)
    s+=optocoupler(900,130)
    s+=poly([(860,190),(872,190),(872,192)])
    s+=poly([(872,248),(820,248),(820,390),N])

    # Isolated output, pull-up and RC filter into GPIO34.
    sense=(1190,180)
    s+=line(1090,180,*sense)+node(*sense)
    s+=resistor_v(1190,180,110,"R215","10 kΩ",True)+pwr(1190,110,"+3V3")
    s+=cap_v(1260,180,315,"C213","100 nF",True)+ground(1260,315)
    s+=poly([(1090,250),(1140,250),(1140,315)])+ground(1140,315)
    s+=line(*sense,1340,180)+net_label(1340,180,"LPG_SENSE","right")
    s+=t(1190,210,"active LOW → A202 GPIO34", "nettext","middle")

    s+=note_box(55,435,710,115,["J203 подключать ПАРАЛЛЕЛЬНО двум проводам одной катушки клапана BRC; не соединять разные клапаны.","VALVE_A/VALVE_B не имеют соединения с GND ESP32. D202…D205 допускают любую полярность катушки.","PC817C/EL817C: pin 1 anode, 2 cathode, 4 collector, 3 emitter. R213/R214 — pulse-rated."],"note","ВХОД КЛАПАНА LPG")
    s+=note_box(800,435,745,115,["При 12…14.4 V ток LED ≈ 2…2.7 mA. Проверить гарантированный CTR выбранной PC817C/EL817C.","Выход U203 открыт: R215 подтягивает GPIO34 к 3.3 V, C213 фильтрует помехи.","До подключения измерить катушку мультиметром и осциллографом на стоящем автомобиле."],"ok","ПРОВЕРКА LPG")

    # High-side PWM: GPIO25 drives NPN, which pulls the P-MOS gate down.
    y=755
    s+=net_label(85,y,"BL_PWM_GPIO25","right")+line(85,y,195,y)
    s+=resistor_h(195,385,y,"R211","4.7 kΩ",True)
    s+=transistor_npn(430,y,"Q202","MMBT3904")
    s+=line(385,y,399,y)
    s+=poly([(445,y-43),(445,690),(835,690)])
    s+=pmos_v(887,690,"Q201","AO3401A P-MOS")
    s+=line(835,690,835,690)+node(835,690)
    # gate pull-up and GPIO base pull-down
    s+=resistor_v(770,690,610,"R210","100 kΩ",False)+pwr(770,610,"+3V3")+line(770,690,835,690)+node(770,690)
    s+=line(350,y,350,870)+resistor_v(350,790,870,"R212","100 kΩ",False)+ground(350,870)+node(350,y)
    # transistor emitter and PMOS source/drain
    s+=line(445,y+43,445,870)+ground(445,870)
    s+=pwr(887,638,"+3V3")
    s+=line(887,742,1040,742)+net_label(1040,742,"TFT_BL","right")
    s+=t(887,790,"Q201: 1=G, 2=S, 3=D", "tiny","middle")

    s+=note_box(1080,620,465,155,["GPIO25 HIGH → Q202 ON → Q201 gate LOW → BLK = 3.3 V.","GPIO25 LOW/Hi-Z → R210 closes Q201; R212 keeps Q202 OFF.","LEDC duty 0 = backlight OFF; duty 100% = ON.","Use only when GC9A01 BLK is an active-HIGH power/control input.","If BLK is an LED cathode or lacks current limiting, redesign with an LED driver."],"warn","HIGH-SIDE PWM ПОДСВЕТКИ")
    s+=note_box(55,900,960,90,["Q201 AO3401A SOT-23: pin 1 gate, 2 source, 3 drain. Q202 MMBT3904 SOT-23: pin 1 base, 2 emitter, 3 collector.","R210 and R212 guarantee no display flash before firmware configures GPIO25. Grounds on this sheet are GND_PROTECTED."],"ok","ЛОГИКА И РАСПИНОВКА")
    s+=title_block("Вариант 2 — автомобиль, LPG и подсветка",4)
    return s


def svg_document(content, width_mm="420mm", height_mm="297mm", view_h=H):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width_mm}" height="{height_mm}" viewBox="0 0 {W} {view_h}">'
            f'<style>{STYLE}</style><rect x="0" y="0" width="{W}" height="{view_h}" class="page"/>{content}</svg>')


def main():
    DOCS.mkdir(parents=True, exist_ok=True)
    sheets=[sheet1(),sheet2(),sheet3(),sheet4()]
    names=[]
    for i,content in enumerate(sheets,1):
        name=DOCS/f"h2-gauge-principle-schematic-sheet{i}.svg"
        name.write_text(svg_document(content),encoding="utf-8")
        names.append(name)
    groups="".join(f'<g transform="translate(0 {i*H})">{content}</g>' for i,content in enumerate(sheets))
    combined=DOCS/"h2-gauge-principle-schematic.svg"
    combined.write_text(svg_document(groups,"420mm",f"{297*4}mm",H*4),encoding="utf-8")
    print("generated:")
    for n in names+[combined]:print(n)


if __name__ == "__main__":
    main()
