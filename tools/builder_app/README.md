# MNG Launcher

SwiftUI Launcher and its portable distribution tools. User/developer overview: [builder-app.md](../../docs/builder-app.md).

Players installing the official DMG simply drag the app to Applications; no compilation, re-signing or signing
credentials are needed. The commands below are only for people choosing to compile their own source build.

Build locally with `python3 tools/builder_app/make_app.py`, or add `--portable` for a committed source snapshot.
See the overview for dependencies, output paths and ZIP options. Local builds use an ad-hoc signature and do not
need the maintainer's signing tools or credentials.

Run the public tests with `python3 -m unittest discover -s tests -p 'test_launcher_*.py' -v`.
Official signing, notarization and release automation are maintained separately from this repository.
