/* ipred_prepare_tmpl.c for 16 bits per component: meson compiles each template once per bit depth with -DBITDEPTH; a file of its own here, so the iOS project (one shared set of defines) builds it too */
#define BITDEPTH 16
#include "src/ipred_prepare_tmpl.c"
