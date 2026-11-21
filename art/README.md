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
