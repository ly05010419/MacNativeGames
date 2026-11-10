"""Build one Ocarina of Time app from all volumes of a split RAR."""
import sys

from build import build_helpers


def main():
    builder = build_helpers()
    args = builder.parse_args(sys.argv[1:])
    source = args.game.expanduser().resolve()
    if not source.is_file() or not source.name.lower().endswith(".part1.rar"):
        raise SystemExit("Keep all RAR volumes together and pass .part1.rar to start one build.")
    try:
        builder.build_oot(args, source)
    except (builder.BuildError, OSError, ValueError) as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    main()
