"""Build Ocarina of Time from the first part of a multipart RAR."""
import sys

from build import build_helpers


def main():
    builder = build_helpers()
    args = builder.parse_args(sys.argv[1:])
    source = args.game.expanduser().resolve()
    if not source.is_file() or not source.name.lower().endswith(".part1.rar"):
        raise SystemExit("Pass the .part1.rar file with all parts in the same directory.")
    try:
        builder.build_oot(args, source)
    except (builder.BuildError, OSError, ValueError) as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    main()
