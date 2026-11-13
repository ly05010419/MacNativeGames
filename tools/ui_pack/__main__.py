"""python3.14 -m tools.ui_pack build ...: the entry point; see cli.py."""
import sys

from tools.ui_pack.cli import main

if __name__ == '__main__':
    sys.exit(main())
