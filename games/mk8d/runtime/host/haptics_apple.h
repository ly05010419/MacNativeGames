// Shared Core Haptics output for the Mac and iOS front ends. Configure routes before guest boot.
#pragma once
#import <GameController/GameController.h>
#include "hle_haptics.h"
void dm_haptics_apple_init(void);
void dm_haptics_apple_controller(int player, GCController *controller);
void dm_haptics_apple_shutdown(void);
