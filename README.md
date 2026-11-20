# rsdk-air

One native app (C++20, SDL2) that bundles the RSDKv3, RSDKv4 and RSDKv5U engines and
Sonic 3 A.I.R., and asks at startup which game to play. Targets Windows, Linux, macOS
and Android. No game data is included: you provide your own files.

Our own code is under the [MIT License](LICENSE). The bundled engines keep their own
licenses (non-commercial RSDK decompilation licenses, GPL-3.0 for Sonic 3 A.I.R.); see
[`ROADMAP/2026-11-20/upstreams-and-licensing.md`](ROADMAP/2026-11-20/upstreams-and-licensing.md).

Current plan: [`ROADMAP/2026-11-20/`](ROADMAP/2026-11-20/overview.md).

- [`AGENTS.md`](AGENTS.md) — rules for agents (`CLAUDE.md` symlinks to it).
- [`ROADMAP/`](ROADMAP/) — project plan by date, with Mermaid Gantt diagrams.
- [`RECORD/`](RECORD/) — project memory (`.WIP` append-only, `.completed` read-only).

All code and documentation in this repository are written in English.
