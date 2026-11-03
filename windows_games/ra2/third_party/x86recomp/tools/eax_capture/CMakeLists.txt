# eax_render: the capture cases rendered by recomp_audio, for analyze.py
# to compare with eaxcap.exe's hardware recordings (eaxcap.exe itself is
# cross-compiled with the Makefile here).
add_executable(eax_render eax_render.cpp cases.c)
target_link_libraries(eax_render PRIVATE recomp_audio m)

add_test(NAME eax_render COMMAND eax_render --out "${CMAKE_CURRENT_BINARY_DIR}/render_test"
         --provider eax3 --cases "level_tone,env_00_generic,obst_m3000" --quiet)
