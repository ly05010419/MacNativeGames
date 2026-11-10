/* Device defaults are persisted only when no player choice already exists. */
#pragma once
#import <Foundation/Foundation.h>
#include <stdint.h>

static inline void totk_ios_botw_device_defaults(NSMutableDictionary *defaults,
                                                NSMutableDictionary *pad,
                                                BOOL isPad, uint64_t physicalMemory) {
    if (!isPad || !physicalMemory || physicalMemory > (4ULL << 30)) return;
    if (defaults[@"TOTK_DOCKED"]) {
        defaults[@"TOTK_DOCKED"] = @NO;
        pad[@"TOTK_DOCKED"] = @NO;
    }
    if (defaults[@"TOTK_PHONE_FPS"]) {
        defaults[@"TOTK_PHONE_FPS"] = @"30";
        pad[@"TOTK_PHONE_FPS"] = @"30";
    }
}
