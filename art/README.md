# art/

Images the launcher shows. Everything here can be deleted in one go: the code never
requires these files and falls back to neutral art when one is missing. See the art
decision in [`ROADMAP/2026-11-20/game-menus.md`](../ROADMAP/2026-11-20/game-menus.md).

Not affiliated with or endorsed by SEGA. Sonic the Hedgehog and its characters are
trademarks of SEGA.

## covers/

One square cover per game, `covers/<game id>.jpg`, shown in the picker's row of covers
and, blurred, as its backdrop.
The picker shows covers at 4:5, so it trims the sides of the square art: evenly, except
where `cover_anchor_x()` in `src/launcher/picker.cpp` says otherwise: Sonic CD keeps the
disc as close to the right edge as to the bottom, Sonic 3 keeps Robotnik.

| File | Source | Changes |
| --- | --- | --- |
| `covers/sonic1.jpg` | play.germade (`/assets/covers/sonic_cover.jpg`) | 1024 to 512 px, JPEG q88 |
| `covers/sonic2.jpg` | play.germade (`/assets/covers/sonic2_cover.jpg`) | 1024 to 512 px, JPEG q88 |
| `covers/soniccd.jpg` | play.germade (`/assets/covers/soniccd_cover.jpg`) | 1024 to 512 px, JPEG q88 |
| `covers/s3air.jpg` | play.germade (`/assets/covers/sonic3air_cover.jpg`) | 1024 to 512 px, JPEG q88 |
| `covers/mania.jpg` | play.germade (`/assets/covers/sonicmania_cover.jpg`) | 1024 to 512 px, JPEG q88 |

The play.germade covers were made by the project owner with an image generator (Gemini),
in the style of the games; they are fan art, not SEGA's box art.

## players/ and stages/

The save-slot screen of Sonic 1 and 2 (`draw_slot_card()` in `src/launcher/picker.cpp`),
which redraws rsdkv4-wasm's load-game screen (craft's slot cards).

| Files | Source | Changes |
| --- | --- | --- |
| `players/{sonic,tails,knuckles,sonictails}.png` | rsdkv4-wasm `src/craft/assets/player-*.svg` (commit `a53c681`) | SVG rendered to 256 x 256 PNG with a transparent background (Chromium) |
| `players/{amy,amytails,knucklestails,mighty,ray}.png` | Provided by the project owner on 2026-11-26 as 2000 x 2000 JPEGs of pixel art on white; where they come from is not recorded yet | Pixel grid (about 27.5 px per pixel) sampled back to the sprite, cell by cell (median of each cell's centre, which drops the JPEG noise); the white background connected to the border made transparent; scaled to fit 256 x 256 (bottom-aligned, centred) with a transparent background. Plus characters (Amy, Amy & Tails, Knuckles & Tails: Sonic CD and/or Sonic 1/2; Mighty, Ray: Sonic Mania); not used by the code yet |
| `stages/sonic1/zone-1..7.png`, `stages/sonic1/special.png` | rsdkv4-wasm `src/craft/assets/stages-sonic1/` | none (140 x 80 PNG) |
| `stages/sonic2/zone-1..10.png`, `stages/sonic2/special.png` | rsdkv4-wasm `src/craft/assets/stages-sonic2/` | none (140 x 80 PNG) |

`zone-N.png` is the N-th zone of the game's regular stage list (the table in
`src/core/saves_rsdkv4.cpp`); zones past the last file show the card's plain background.
Missing files fall back to the character's name and an empty portrait.
