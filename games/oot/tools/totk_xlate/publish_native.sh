#!/bin/zsh
# Usage: publish_native.sh /external/shader-work [dotnet]
set -e
HERE=${0:A:h}
[[ -n "$1" ]] || { echo 'An external workspace is required'; exit 2; }
exec python3 "$HERE/../build_shader_service.py" --work "$1" --dotnet "${2:-dotnet}"
