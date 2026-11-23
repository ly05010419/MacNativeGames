"""The depth model the Launcher's case pictures are parted by (MNG-Launcher mac/Sources/MNGBuilder/DepthScene.swift),
fetched once per Mac into the deps cache, each file checked against the SHA-256 pins.py names.

    from tools.deps.depth_model import depth_model
    folder = depth_model()   # DepthAnythingV2SmallF16P8.mlpackage, README.md (the model card), LICENSE (Apache-2.0)

The package goes into the app as it is: Core ML compiles it on the player's Mac or iPhone when it is first used, so a
Launcher built with only the Command Line Tools (no coremlcompiler) carries it too. Nothing of it is in the repository.
"""
import hashlib
import shutil
from pathlib import Path

FORMAT = 1   # changes the cache folder when what is made here changes


def depth_model():
    """The cache folder with the model package, its card and its licence, made the first time (about 25 MB)."""
    from .core import cache, fetch, install, locked, say
    from .pins import (DEPTH_MODEL_CARD, DEPTH_MODEL_FILES, DEPTH_MODEL_LICENSE, DEPTH_MODEL_PACKAGE,
                       DEPTH_MODEL_REPO)
    identity = hashlib.sha256(repr((FORMAT, DEPTH_MODEL_REPO, DEPTH_MODEL_PACKAGE, DEPTH_MODEL_FILES,
                                    DEPTH_MODEL_CARD, DEPTH_MODEL_LICENSE)).encode()).hexdigest()[:12]
    folder = cache() / f'depth-model-{identity}'
    done = folder / '.done'
    if done.is_file():
        return folder
    with locked():
        if done.is_file():
            return folder
        say('fetching the depth model of the case pictures (Depth Anything V2 Small, Apache-2.0), once for this Mac')
        repo, commit = DEPTH_MODEL_REPO

        def url(path):
            return f'https://huggingface.co/{repo}/resolve/{commit}/{path}'

        fetched = [(path, fetch(url(f'{DEPTH_MODEL_PACKAGE}/{path}'), digest,
                                f'depth-{commit[:8]}-' + path.replace('/', '-'))) for path, digest in DEPTH_MODEL_FILES]
        card = fetch(url(DEPTH_MODEL_CARD[0]), DEPTH_MODEL_CARD[1], f'depth-{commit[:8]}-README.md')
        licence = fetch(DEPTH_MODEL_LICENSE[0], DEPTH_MODEL_LICENSE[1], 'depth-anything-v2-LICENSE')

        def steps(stage):
            for path, source in fetched:
                target = Path(stage) / DEPTH_MODEL_PACKAGE / path
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
            shutil.copy2(card, Path(stage) / 'README.md')
            shutil.copy2(licence, Path(stage) / 'LICENSE')
            (Path(stage) / '.done').touch()

        install(folder, steps)
    return folder


def package():
    """The model package itself (the folder an app carries)."""
    from .pins import DEPTH_MODEL_PACKAGE
    return depth_model() / DEPTH_MODEL_PACKAGE
