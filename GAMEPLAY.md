# gameplay branch

Anton's personal gameplay line of this fork. It is NOT the port.

- `main` is the macOS port and follows its charter: nothing in synced code changes behaviour (AGENTS.md, docs/PORTING_PRINCIPLES.md).
- `gameplay` = `main` + gameplay changes for single-player skirmish: the `Mutators` setting (local mod archives), `SetUnitHealth{resetDecay = true}`, engine squads.
- `PINNED_VERSION` (`2026.07.04-0-g0000000 gameplay`) makes the engine report a version no server accepts as the public release, so a gameplay build can never join a game with official builds and desync it. It must use the dev format `<tag>-<n>-g<hex> <branch>`: UtilVersion.cmake's get_version_from_file drops anything else and the version comes out empty. Check `spring --version` after every build.
- Never PR this branch into `main` and never send it upstream. Rebase it onto `main` when the port moves.
- Builds: `make app` from this branch, installed as "BAR Launcher (dev).app" (online play stays disabled, BAR_ONLINE=0 default).
