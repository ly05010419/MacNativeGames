#pragma once
#define GAME_OT0 1
#define GAME_TITLE_ID 0x01005270232f2000ULL
#define GUEST_ALIAS_HIGH 1
#if defined(OT0_GAME_V103)
#define TOTK_GAME_VERSION "1.0.3"
#define TOTK_GAME_DIR "v103"
#define GAME_MAX_TICK_RATE 0x25f8940
#elif defined(OT0_GAME_V104)
#define TOTK_GAME_VERSION "1.0.4"
#define TOTK_GAME_DIR "v104"
#define GAME_MAX_TICK_RATE 0x25f8940
#else
#error "build with -DOT0_GAME_V103 or -DOT0_GAME_V104 (the Makefile's VERSION)"
#endif
