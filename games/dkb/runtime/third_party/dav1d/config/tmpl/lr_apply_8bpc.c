/* lr_apply_tmpl.c for 8 bits per component: meson compiles each template once per bit depth with -DBITDEPTH; a file of its own here, so the iOS project (one shared set of defines) builds it too */
#define BITDEPTH 8
#include "src/lr_apply_tmpl.c"
