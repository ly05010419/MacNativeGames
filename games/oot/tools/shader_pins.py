# Pinned from MacNativeGames 5843ee56; isolated shader tool build inputs.
SPIRV_CROSS = ('https://github.com/KhronosGroup/SPIRV-Cross.git', 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165')
SPIRV_CROSS_SOURCES = ('main.cpp', 'spirv_cfg.cpp', 'spirv_cpp.cpp', 'spirv_cross.cpp', 'spirv_cross_parsed_ir.cpp', 'spirv_cross_util.cpp', 'spirv_glsl.cpp', 'spirv_hlsl.cpp', 'spirv_msl.cpp', 'spirv_parser.cpp', 'spirv_reflect.cpp')
SPIRV_CROSS_FLAGS = ('-O3', '-DNDEBUG', '-std=c++11', '-arch', 'arm64', '-Wno-deprecated-declarations')
