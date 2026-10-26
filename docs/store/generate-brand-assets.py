#!/usr/bin/env python3
"""Render original vector identity and Microsoft Store artwork for Qinglan.

Requires fontTools and sharp. Text is outlined in SVG; font files are not copied.
"""
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
import json
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'docs/store/assets'
SRC = OUT / 'source'
PACKAGE = ROOT / 'packaging/windows/assets'
NODE = os.environ.get('QINGLAN_ASSET_NODE', shutil.which('node') or 'node')
SHARP = os.environ.get('QINGLAN_ASSET_SHARP', 'sharp')
CN = TTFont(os.environ.get('QINGLAN_ASSET_CN_FONT', '/System/Library/Fonts/Supplemental/Songti.ttc'), fontNumber=int(os.environ.get('QINGLAN_ASSET_CN_FONT_INDEX', '1')))
EN = TTFont(os.environ.get('QINGLAN_ASSET_EN_FONT', '/System/Library/Fonts/Supplemental/Georgia.ttf'))


def outlined(text, font, x, baseline, size, spacing=0, fill='#eadbb1'):
    """Center an outlined line; keep the SVG self-contained and portable."""
    glyphs = font.getGlyphSet()
    cmap = font.getBestCmap()
    units = font['head'].unitsPerEm
    scale = size / units
    widths = [font['hmtx'][cmap[ord(c)]][0] * scale for c in text]
    start = x - (sum(widths) + spacing * (len(text) - 1)) / 2
    paths = []
    for char, width in zip(text, widths):
        pen = SVGPathPen(glyphs)
        glyphs[cmap[ord(char)]].draw(pen)
        paths.append(f'<path d="{pen.getCommands()}" transform="translate({start:.3f} {baseline}) scale({scale:.6f} {-scale:.6f})"/>')
        start += width + spacing
    return f'<g fill="{fill}">' + ''.join(paths) + '</g>'


DEFS = '''<defs>
  <linearGradient id="sky" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#173b3c"/><stop offset=".54" stop-color="#0b292f"/><stop offset="1" stop-color="#071b24"/>
  </linearGradient>
  <radialGradient id="halo"><stop offset="0" stop-color="#5f9d86" stop-opacity=".24"/><stop offset="1" stop-color="#264846" stop-opacity="0"/></radialGradient>
  <linearGradient id="jade" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#b7d5ba"/><stop offset=".47" stop-color="#6da58e"/><stop offset="1" stop-color="#2b665d"/>
  </linearGradient>
  <linearGradient id="gold" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#f0e5bc"/><stop offset="1" stop-color="#bfa66e"/>
  </linearGradient>
  <linearGradient id="fade" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#153b3b" stop-opacity="0"/><stop offset="1" stop-color="#071b24"/>
  </linearGradient>
  <g id="emblem">
    <circle cx="0" cy="-30" r="234" fill="none" stroke="#c9b987" stroke-opacity=".7" stroke-width="3"/>
    <path d="M-284 132 -163-33 -97 40 0-143 119 60 183-13 284 132Z" fill="url(#jade)"/>
    <path d="m0-143 25 138-40-37-82 82Z" fill="#d9e4ca" fill-opacity=".67"/>
    <path d="m-163-33 15 102-49-20-87 83Z M183-13 14 145 284 132Z" fill="#95c1a8" fill-opacity=".48"/>
    <path d="M0-284 19-219 14-17 0 12-14-17-19-219Z" fill="url(#gold)"/>
    <path d="M0-258 0-22" stroke="#fff9dc" stroke-opacity=".7" stroke-width="3"/>
    <path d="M-53 22Q-28 5 0 9 28 5 53 22L49 34Q27 25 12 29L12 83Q0 100-12 83L-12 29Q-27 25-49 34Z" fill="url(#gold)"/>
    <path d="M-15 96Q0 108 15 96L12 111Q0 124-12 111Z" fill="#d0bc83"/>
    <g fill="none" stroke="#c8dfcc" stroke-width="6" stroke-linecap="round">
      <path d="M-290 149H-174Q-205 121-176 104-143 85-117 116H-67"/>
      <path d="M290 173H140Q180 141 151 124 125 108 103 133H49"/>
      <path d="M-196 198H-66Q-94 175-71 162-46 146-24 171H95"/>
      <path d="M-102 228H116" stroke-opacity=".45" stroke-width="3"/>
    </g>
  </g>
</defs>'''


def svg(width, height, body):
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-label="Qinglan: Path of Ascension">{DEFS}{body}</svg>'


def landscape(width, height):
    """Decorative vector mountains; brand artwork, not a gameplay screenshot."""
    return f'''<g transform="scale({width / 1920} {height / 1080})">
      <path d="M0 780 178 635 312 724 459 502 661 742 822 625 1011 777 1201 605 1379 742 1596 560 1760 718 1920 631V1080H0Z" fill="#56887c" opacity=".17"/>
      <path d="M0 900 266 714 396 821 570 692 788 914 953 804 1121 925 1370 751 1530 860 1715 714 1920 893V1080H0Z" fill="#689a87" opacity=".13"/>
      <path d="M0 970Q321 828 630 937T1194 923 1920 906V1080H0Z" fill="#071b24" opacity=".73"/>
      <g fill="none" stroke="#97bba5" stroke-opacity=".15" stroke-width="2">
       <path d="M0 837Q188 817 376 839T757 835 1150 840 1550 833 1920 839"/>
       <path d="M0 855Q188 835 376 857T757 853 1150 858 1550 851 1920 857"/>
       <path d="M0 1024Q330 961 651 1008T1275 1018 1920 984"/>
      </g>
    </g>'''


def icon():
    return svg(1080, 1080, '''<rect width="1080" height="1080" fill="url(#sky)"/>
      <circle cx="540" cy="465" r="500" fill="url(#halo)"/>
      <rect x="66" y="66" width="948" height="948" rx="184" fill="none" stroke="#c7b685" stroke-width="3" stroke-opacity=".46"/>
      <g transform="translate(540 555) scale(1.37)"><use href="#emblem"/></g>''')


def square_cover():
    body = '''<rect width="1080" height="1080" fill="url(#sky)"/>
    <circle cx="540" cy="365" r="490" fill="url(#halo)"/>
    <g transform="translate(540 395) scale(.98)"><use href="#emblem"/></g>'''
    body += landscape(1080, 1080)
    body += outlined('\u9752\u5c9a', CN, 540, 845, 154, 14)
    body += outlined('QINGLAN', EN, 540, 917, 43, 13, '#cfdfc7')
    body += outlined('PATH OF ASCENSION', EN, 540, 970, 22, 5, '#c2ae79')
    return svg(1080, 1080, body)


def horizontal():
    body = '''<rect width="1920" height="1080" fill="url(#sky)"/>
    <ellipse cx="575" cy="440" rx="610" ry="590" fill="url(#halo)"/>
    <path d="M98 98H1822V982H98Z" fill="none" stroke="#bfa96f" stroke-opacity=".29" stroke-width="2"/>
    <g transform="translate(555 500) scale(1.18)"><use href="#emblem"/></g>'''
    body += landscape(1920, 1080)
    body += outlined('\u9752\u5c9a', CN, 1287, 460, 219, 20)
    body += outlined('QINGLAN', EN, 1287, 563, 68, 15, '#cfdfc7')
    body += '<path d="M1032 609H1542" stroke="#c6af75" stroke-width="1.5" opacity=".65"/>'
    body += outlined('PATH OF ASCENSION', EN, 1287, 665, 30, 6, '#c5b37e')
    return svg(1920, 1080, body)


def vertical():
    body = '''<rect width="768" height="1152" fill="url(#sky)"/>
    <ellipse cx="384" cy="350" rx="420" ry="450" fill="url(#halo)"/>
    <rect x="42" y="42" width="684" height="1068" fill="none" stroke="#bfa96f" stroke-opacity=".29" stroke-width="1.5"/>
    <g transform="translate(384 365) scale(.86)"><use href="#emblem"/></g>'''
    body += landscape(768, 1152)
    body += outlined('\u9752\u5c9a', CN, 384, 807, 150, 15)
    body += outlined('QINGLAN', EN, 384, 889, 42, 9, '#cfdfc7')
    body += '<path d="M180 932H588" stroke="#c6af75" stroke-width="1.2" opacity=".65"/>'
    body += outlined('PATH OF ASCENSION', EN, 384, 977, 20, 3.7, '#c5b37e')
    return svg(768, 1152, body)


def superhero():
    body = '''<rect width="1920" height="1080" fill="url(#sky)"/>
    <ellipse cx="960" cy="420" rx="720" ry="650" fill="url(#halo)"/>
    <g transform="translate(960 472) scale(1.35)"><use href="#emblem"/></g>'''
    body += landscape(1920, 1080)
    return svg(1920, 1080, body)


def main():
    for folder in (SRC, PACKAGE):
        folder.mkdir(parents=True, exist_ok=True)
    sources = {
        'Qinglan-Emblem.svg': icon(),
        'Qinglan-Square-Cover.svg': square_cover(),
        'Qinglan-Landscape-Cover.svg': horizontal(),
        'Qinglan-Portrait-Cover.svg': vertical(),
        'Qinglan-Superhero.svg': superhero(),
    }
    for name, data in sources.items():
        (SRC / name).write_text(data, encoding='utf-8')
    jobs = []
    for size in (44, 100, 150, 300, 480, 1080):
        name = 'StoreLogo.png' if size == 100 else f'Square{size}x{size}Logo.png'
        jobs.append({'source': str(SRC / 'Qinglan-Emblem.svg'), 'output': str(PACKAGE / name), 'width': size, 'height': size})
    jobs.extend([
        {'source': str(SRC / 'Qinglan-Landscape-Cover.svg'), 'output': str(PACKAGE / 'SplashScreenImage.png'), 'width': 1920, 'height': 1080},
        {'source': str(SRC / 'Qinglan-Landscape-Cover.svg'), 'output': str(OUT / 'Qinglan-Landscape-1920x1080.png'), 'width': 1920, 'height': 1080},
        {'source': str(SRC / 'Qinglan-Portrait-Cover.svg'), 'output': str(OUT / 'Qinglan-Portrait-768x1152.png'), 'width': 768, 'height': 1152},
        {'source': str(SRC / 'Qinglan-Portrait-Cover.svg'), 'output': str(OUT / 'Qinglan-Poster-720x1080.png'), 'width': 720, 'height': 1080},
        {'source': str(SRC / 'Qinglan-Portrait-Cover.svg'), 'output': str(OUT / 'Qinglan-Portrait-1440x2160.png'), 'width': 1440, 'height': 2160},
        {'source': str(SRC / 'Qinglan-Square-Cover.svg'), 'output': str(OUT / 'Qinglan-Square-Cover-1080.png'), 'width': 1080, 'height': 1080},
        {'source': str(SRC / 'Qinglan-Superhero.svg'), 'output': str(OUT / 'Qinglan-Superhero-1920x1080.png'), 'width': 1920, 'height': 1080},
    ])
    code = '''const sharp=require(process.argv[1]);const jobs=JSON.parse(process.argv[2]);(async()=>{for(const job of jobs){await sharp(job.source,{density:144}).resize(job.width,job.height).png().toFile(job.output);console.log(job.output);}})().catch(e=>{console.error(e);process.exit(1)});'''
    subprocess.run([NODE, '-e', code, SHARP, json.dumps(jobs)], check=True)
    portable_jobs = [{**job, 'source': str(Path(job['source']).relative_to(ROOT)), 'output': str(Path(job['output']).relative_to(ROOT))} for job in jobs]
    (OUT / 'asset-index.json').write_text(json.dumps(portable_jobs, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
