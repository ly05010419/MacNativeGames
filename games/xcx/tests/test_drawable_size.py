"""Retina/full-screen presentation stays bounded without stretching the canvas."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "drawable_size.h"
int main(void){
 const double cases[][4]={{1280,720,1280,720},{2560,1440,1920,1080},
 {3600,2024,1920,1079},{3840,2160,1920,1080},{7680,2160,1920,540},
 {1080,1920,607,1080},{1,8192,1,1080},{0,720,0,0},{NAN,720,0,0},{INFINITY,720,0,0}};
 for(unsigned i=0;i<sizeof cases/sizeof *cases;i++){
  double w=cases[i][0],h=cases[i][1];xcx_drawable_size(&w,&h);
  if(w!=cases[i][2]||h!=cases[i][3])return 9;
 }
 for(int w=320;w<=8192;w+=97)for(int h=180;h<=8192;h+=131){
  double x=w,y=h;xcx_drawable_size(&x,&y);
  if(x<1||y<1||x>1920||y>1080||x>w||y>h)return 9;
  /* Rounding to whole pixels may cost less than one pixel on each axis. */
  double k=fmin(1,fmin(1920.0/w,1080.0/h));
  if(fabs(x-w*k)>=1||fabs(y-h*k)>=1)return 9;
 }
 return 0;
}
'''


class DrawableSizeTest(unittest.TestCase):
    def test_retina_aspect_ratios_and_invalid_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / 'probe.c', Path(tmp) / 'probe'
            source.write_text(HARNESS)
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'runtime'), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
