This game-local copy of the shader translator is the exact binary shipped with this repo and validated by `totk_xlate.sha256`.

- `totk_xlate` is the native translator used during shader preparation and packaging.
- `totk_xlate.sha256` is its SHA-256; `build_app.py` refuses to package a different file.
- This copy is kept inside the game folder so the app can build without network access or external game-data paths.
