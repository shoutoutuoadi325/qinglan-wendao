# Qinglan store artwork

Original vector brand artwork uses a mountain, sword and cloud emblem with a
deep teal, jade and pale gold palette. Cover lettering contains only the game
name. These are cover assets, not gameplay screenshots.

| Store field | File | Dimensions |
| --- | --- | --- |
| Poster art | `assets/Qinglan-Poster-720x1080.png` | 720 x 1080 |
| High-resolution poster | `assets/Qinglan-Portrait-1440x2160.png` | 1440 x 2160 |
| Box art | `assets/Qinglan-Square-Cover-1080.png` | 1080 x 1080 |
| App tile | `../../packaging/windows/assets/Square300x300Logo.png` | 300 x 300 |
| Superhero art, no title | `assets/Qinglan-Superhero-1920x1080.png` | 1920 x 1080 |
| Titled hero | `assets/Qinglan-Landscape-1920x1080.png` | 1920 x 1080 |

The Windows package uses the same emblem at 44, 100 and 150 pixels; additional
300, 480 and 1080 pixel versions are available. `SplashScreenImage.png` uses the
titled landscape art.

SVG sources in `assets/source/` have outlined text and require no fonts when
viewed or rendered. `generate-brand-assets.py` can recreate them with Python
fontTools, Node.js and sharp. It uses macOS Songti SC Bold and Georgia to outline
the lettering; font files are not included. Other installations can set
`QINGLAN_ASSET_CN_FONT`, `QINGLAN_ASSET_CN_FONT_INDEX` and
`QINGLAN_ASSET_EN_FONT`. Set `QINGLAN_ASSET_NODE` and `QINGLAN_ASSET_SHARP` if
Node.js or the sharp module is outside the normal search path.

The asset index records file dimensions and relative source/output paths.
The contact sheet is for review and must not be uploaded as a Store asset.
