# The minimum macOS of the Mac host (README: macOS 26.6 or later), on its compile lines (dm/host/Makefile; the NSP script runs the same line, make -n). Without it clang takes the SDK's
# version (27.0 with Xcode 27.1), and pack_app.py, which reads the host's minos into LSMinimumSystemVersion, made a Mac app that macOS 26.x does not open (GitHub issue #673)
MACOS_MIN := 26.6
