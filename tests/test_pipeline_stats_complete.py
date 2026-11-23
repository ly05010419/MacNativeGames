"""Every Metal library and pipeline the graphics layer makes is counted with the frame's libraries / pipelines (the
long-frame line's "新建管线 / 着色器库" and the per-frame record's @lib_* / @pso_*): user 2026-11-15, "确定 着色器和管线 统计 …
都全 没有出现 落下的 没有统计的". Mesh pipelines were left out until then (32 of them, 75 s, in one world entry), so did the
geometry stage's and our own passes'.

A Metal call that makes a library or a pipeline may stand only in
- the counted makers (nvn_mtl.m mtl_counted_*), which add to the counts themselves;
- the game's own paths, counted where they are called: lib_from_file / totk_msl_library (shader_get, the variants'
  statistics), pso_new (pso_get), cs_new_ (the dispatch), mtk_build (mtk_pso);
- a probe (a function or file named so): tests, never a frame.
Anything else fails here, so a new pass cannot be left out again."""
from pathlib import Path
import re
import unittest

GFX = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn'
CALL = re.compile(r'\bnew(LibraryWithSource|LibraryWithData|LibraryWithURL|RenderPipelineStateWithDescriptor|'
                  r'RenderPipelineStateWithMeshDescriptor|RenderPipelineStateWithTileDescriptor|'
                  r'ComputePipelineStateWithDescriptor|ComputePipelineStateWithFunction):')
FUNCTION = re.compile(r'^(?:static\s+|inline\s+)*[A-Za-z_][\w<>\s\*]*?\b(\w+)\s*\([^;]*$')
COUNTED_MAKERS = {'mtl_counted_library', 'mtl_counted_render', 'mtl_counted_compute'}
# par4_make (nvn_pso4_build.h, the Metal 4 pipeline archive, 2026-11-17): the pipeline pso_new / cs_new_ are making, taken
# from the archive or compiled by the period's compiler; reached only from those two, so it is counted with them
GAME_PATHS = {'lib_from_file', 'totk_msl_library', 'pso_new', 'cs_new_', 'mtk_build', 'par4_make'}
# par4_capture_chunk_n: a period of the archive whose file could not be written is compiled again in halves, and one
# dropped after a compiler service death is compiled again whole, on the archive's own queue (par4_write -> par4_resplit /
# par4_recapture); the frame that first made each of those pipelines counted it
ARCHIVE_REWRITES = {'par4_capture_chunk_n'}


def calls():
    """(file, line, enclosing function) of every Metal library / pipeline call outside comments."""
    for path in sorted(GFX.iterdir()):
        if path.suffix not in ('.m', '.h', '.inc', '.c'):
            continue
        function = ''
        for number, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
            if line and not line[0].isspace() and not line.startswith(('#', '//', '/*', '*', '}')):
                m = FUNCTION.match(line)
                if m:
                    function = m.group(1)
            code = line.split('//')[0]
            if CALL.search(code):
                yield path.name, number, function


class StatsComplete(unittest.TestCase):
    def test_every_library_and_pipeline_is_counted(self):
        loose = [f'{name}:{number} ({function or "?"})' for name, number, function in calls()
                 if function not in COUNTED_MAKERS | GAME_PATHS | ARCHIVE_REWRITES and 'probe' not in function and 'probe' not in name]
        self.assertEqual(loose, [], 'a Metal library or pipeline made outside the counted makers: use mtl_counted_*')

    def test_the_archive_makes_pipelines_only_for_the_counted_paths(self):
        mtl = (GFX / 'nvn_mtl.m').read_text()
        callers = re.findall(r'par4_make\((PAR_\w+),', mtl)
        self.assertEqual(sorted(callers), ['PAR_COMPUTE', 'PAR_RENDER'], 'par4_make is called from pso_new and cs_new_ only')
        new = mtl[mtl.index('static id<MTLRenderPipelineState> pso_new('):]
        self.assertIn('par4_make(PAR_RENDER,', new[:new.index('\n}\n')])
        build = (GFX / 'nvn_pso4_build.h').read_text()
        resplit = build[build.index('static void par4_resplit('):]
        self.assertIn('par4_capture_chunk(', resplit[:resplit.index('\n}\n')])
        self.assertEqual(len(re.findall(r'\bpar4_capture_chunk\(', mtl)), 0, 'the rewrite is the archive file\'s, not a draw\'s')

    def test_the_counted_makers_count(self):
        text = (GFX / 'nvn_mtl.m').read_text()
        for maker, counter in (('mtl_counted_library', 'g_bp_lib_n'), ('mtl_counted_render', 'g_bp_pso_n'),
                               ('mtl_counted_compute', 'g_bp_pso_n')):
            start = text.index(f'{maker}(id<MTLDevice>')
            body = text[start:text.index('\n}', start)]
            self.assertIn(counter, body, maker)
            self.assertIn(counter.replace('_n', '_ns'), body, maker)

    def test_the_game_paths_count_where_they_are_called(self):
        mtl = (GFX / 'nvn_mtl.m').read_text()
        self.assertIn('g_bp_lib_n++; atomic_fetch_add_explicit(&g_bp_lib_ns', mtl)    # shader_get
        self.assertGreaterEqual(mtl.count('g_bp_pso_n++; atomic_fetch_add_explicit(&g_bp_pso_ns'), 2)   # pso_get, the dispatch
        self.assertIn('g_bp_pso_n++;atomic_fetch_add_explicit(&g_bp_pso_ns', (GFX / 'nvn_mesh_tasks.inc').read_text())   # mtk_pso

    def test_the_scan_sees_the_known_calls(self):
        found = {(name, function) for name, _, function in calls()}
        self.assertIn(('nvn_mtl.m', 'pso_new'), found)          # the scan works on the real sources
        self.assertIn(('nvn_mesh_tasks.inc', 'mtk_build'), found)


if __name__ == '__main__':
    unittest.main()
