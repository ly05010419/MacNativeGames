# Instructions for AI agents

Read [CONTRIBUTING.md](CONTRIBUTING.md) before changing anything; it is the full rule set, and it wins over this page. [docs/how-it-works.md](docs/how-it-works.md) explains the port.

## Never

- **No game content in git:** no game code, RomFS or ExeFS files, art, text, audio, translated shaders or libraries, keys, firmware or saves. Never download them or help anyone obtain them; the user supplies their own dumps.
- **No CPU emulation:** no interpreter, JIT, dynamic binary translation or emulator CPU backend. The game's own ARM64 code runs natively. An emulator is only a reference for comparing a picture or a behaviour.
- **No copied code** from GPL projects or other mods. Third-party code keeps its licence and goes into [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- **No hand-edited bytes and no patching of already rewritten binaries.** Every game patch checks the original word at its address and stops with an error when it differs.

## Before committing

- Turn on the commit guard once per clone, so a commit with keys or game files is stopped:
  `git config core.hooksPath .githooks`
- Run the tests from the repository root: `python -m unittest discover -s tests`
- Build every game version you changed; shared runtime or `d2r_gfx` code must also build for iOS (`build_app.py --platform ios`).
- One topic per commit or pull request. Subject: `<type>: <description>`, with type one of feat, fix, refactor, docs, test, chore, perf, ci.
- Say how it was tested and on which platform. "Verified" means a check with a recorded result, never "it looked right".
- Ask before deleting files, pushing, or anything hard to undo.
