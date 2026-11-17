#pragma once
#define GAME_ACNH 1
#ifndef ACNH_VERSION_RAW
#define ACNH_VERSION_RAW 458752
#endif
#if ACNH_VERSION_RAW == 458752
#define TOTK_GAME_VERSION "1.2.1"
#define TOTK_GAME_DIR "acnh121"
#define GAME_FOLDER "ACNH"
#elif ACNH_VERSION_RAW == 2097152
#define TOTK_GAME_VERSION "3.0.1"
#define TOTK_GAME_DIR "acnh301"
#define GAME_FOLDER "ACNH-3.0.1"
#elif ACNH_VERSION_RAW == 2228224
#define TOTK_GAME_VERSION "3.0.3"
#define TOTK_GAME_DIR "acnh303"
#define GAME_FOLDER "ACNH-3.0.3"
#else
#error Unsupported ACNH version
#endif
/* 3.0.1 and 3.0.3 carry the same rtld, sdk and subsdk0 (byte for byte); only main differs. What follows from the
   SDK alone (the NVN version it asks for, its pctl import) is keyed on this, never on an address of main. */
#define ACNH_SDK_3_0 (ACNH_VERSION_RAW == 2097152 || ACNH_VERSION_RAW == 2228224)
#define GAME_NAME "Animal Crossing: New Horizons"
#define GAME_LOG "acnh"
#define GAME_TITLE_ID 0x01006F8002326000ULL
