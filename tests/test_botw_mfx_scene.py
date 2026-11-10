"""Run BotW's native scene/UI classifier for direct-compose and upscale frames."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

NVN = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'requires the Apple SDK')
class SceneBoundary(unittest.TestCase):
    def test_botw190_scene_boundary(self):
        self.run_classifier(0)

    def test_botw160_scene_boundary(self):
        self.run_classifier(1)

    def run_classifier(self, compose_index):
        text = (NVN / 'nvn_mtl.m').read_text()
        start = text.index('static int g_botw_scene_ready;')
        end = text.index('\n}\n', text.index('int botw_mfx_ui_draw(', start)) + 3
        classifier = text[start:end]
        header = (NVN / 'nvn_obj.h').read_text()
        # The first two compose definitions are BotW 1.9.0 and 1.6.0; the last is TotK.
        compose_macro = re.findall(r'^#define TAA_COMPOSE\(fs\).*$', header, re.M)[compose_index]
        macros = re.search(r'^#define BOTW_MFXUP_KEY.*$', header, re.M)[0] + '\n' + compose_macro
        compose = re.search(r'"([0-9a-f]{64}-)"', compose_macro)[1]
        source = '''#import <Foundation/Foundation.h>
#include <assert.h>
#include <string.h>
typedef struct { const char *key; } NvShader;
@interface NvProg : NSObject { @public NvShader *sh[6]; }
@end
@implementation NvProg
@end
''' + macros + '\n' + classifier + r'''
int main(void) { @autoreleasepool {
    NvProg *program = [NvProg new];
    NvShader shader = { "unrelated-draw" };
    program->sh[1] = &shader;
    void *p = (__bridge void *)program;
    botw_mfx_frame_reset();
    assert(!botw_mfx_ui_draw(p) && !g_botw_scene_ready);
    const char *scene[] = { BOTW_MFXUP_KEY "probe", "COMPOSE_KEYprobe" };
    for (unsigned i = 0; i < sizeof scene / sizeof *scene; i++) {
        botw_mfx_frame_reset();
        shader.key = scene[i];
        assert(!botw_mfx_ui_draw(p)); /* The scene must not enter the UI layer. */
        assert(g_botw_scene_ready);
        shader.key = "following-ui-draw";
        assert(botw_mfx_ui_draw(p));
        botw_mfx_frame_reset();
        assert(!botw_mfx_ui_draw(p) && !g_botw_scene_ready);
    }
    program->sh[1] = NULL;
    assert(!botw_mfx_ui_draw(p) && !g_botw_scene_ready);
} }
'''.replace('COMPOSE_KEY', compose)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'scene.m').write_text(source)
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                            str(path / 'scene.m'), '-framework', 'Foundation', '-o', str(path / 'scene')], check=True)
            subprocess.run([str(path / 'scene')], check=True)


if __name__ == '__main__':
    unittest.main()
