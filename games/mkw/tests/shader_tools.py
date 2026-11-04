"""The shader service command line for tests, from the paths make shader-tools builds (scripts/build_shader_tools.py)."""
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parent.parent
SERVICE = GAME_DIR / 'build/shader-service/ampere_service'
SPIRV_CROSS = GAME_DIR / 'build/spirv-cross/spirv-cross'
XLATE = GAME_DIR / 'build/dmsh_xlate/dmsh_xlate'
MESH_WRAPPER = GAME_DIR / 'tools/wrap_mesh_stage.py'


def service_command(out, chain):
    for tool in (SERVICE, SPIRV_CROSS, XLATE):
        if not tool.is_file():
            raise SystemExit(f'{tool} is missing: run make shader-tools')
    return [str(SERVICE), '--service', '--spirv-cross', str(SPIRV_CROSS), '--xlate', str(XLATE),
            '--mesh-wrapper', str(MESH_WRAPPER), '--out', str(out), '--chain', chain]
