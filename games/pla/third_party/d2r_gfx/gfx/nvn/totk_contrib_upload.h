// The shared iPhone / iPad app's main.m includes this header for the shader inbox (games/totk/ios/Sources). Pokémon Legends:
// Arceus has none on the phone, as Animal Crossing (main.m GAME_PLA: its contribution rows stay in Documents,
// totk_shader_contrib.h), and no settings panel that sends records, so nothing else is needed here. Without this file the
// app's include would reach Tears of the Kingdom's copy (the build's -idirafter), whose own totk_shader_contrib.h clashes
// with this graphics layer's.
#pragma once
#include "totk_shader_contrib.h"
