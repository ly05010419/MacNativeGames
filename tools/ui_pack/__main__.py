"""python3.14 -m tools.ui_pack build ...: the entry point; see cli.py."""
import sys
import os
from pathlib import Path

if __name__ == '__main__':
    from tools.deps import DepsError, python_environment
    try:
        python = python_environment('ui-pack')
    except DepsError as error:
        sys.exit(f'ui_pack: {error}')
    if Path(sys.prefix).resolve() != python.parent.parent.resolve():
        os.execv(str(python), [str(python), '-m', 'tools.ui_pack', *sys.argv[1:]])
    from tools.ui_pack.cli import main
    sys.exit(main())
