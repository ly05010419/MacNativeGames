/* Spatial AA shares the renderer's precise math policy, including standalone GPU probes. */
#pragma once
static MTLCompileOptions *spatial_aa_compile_options(void) {
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.mathMode = MTLMathModeSafe;
    options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
    return options;
}
