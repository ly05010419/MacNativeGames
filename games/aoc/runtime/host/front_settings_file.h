// controller-layout.txt, next to the save (front_mac.m pad_settings_load / pad_settings_save): reading and writing it, with the
// environment's decisions for the stats pane and the frame graph (front_prefs.h). Shared with the settings UI test, which goes
// through the real file: a saved choice the environment overrides for one run is written back as it was.
// One key=value per line: layout (auto / position / label), swap_ab, fullscreen, output (0-4), fps_target, dynamic_fps, stats
// (0-2), frame_graph (0-1), aspect (0 = 16:9, 1 = 16:10). Unknown keys, such as the hints= of older versions, are ignored.
#pragma once
#import <Foundation/Foundation.h>
#include "front_prefs.h"
typedef struct { int layout, swap_ab, fullscreen, output, fps_target, dynamic_fps, stats, frame_graph, aspect; } DmSettingsFile;   // -1: not in the file
static inline DmSettingsFile dm_settings_file_read(NSString *path) {
    DmSettingsFile f = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    NSString *text = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] : nil;
    for (NSString *raw in [text componentsSeparatedByString:@"\n"]) {
        NSArray<NSString *> *pair = [[raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] componentsSeparatedByString:@"="];
        if (pair.count != 2) continue;
        NSString *key = [pair[0] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSString *value = [pair[1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        int n = value.intValue;
        if ([key isEqualToString:@"layout"]) f.layout = [value isEqualToString:@"position"] ? 1 : [value isEqualToString:@"label"] ? 2 : 0;
        else if ([key isEqualToString:@"swap_ab"]) f.swap_ab = n != 0;
        else if ([key isEqualToString:@"fullscreen"]) f.fullscreen = n != 0;
        else if ([key isEqualToString:@"output"]) f.output = n >= 0 && n < 5 ? n : -1;
        else if ([key isEqualToString:@"fps_target"]) f.fps_target = n;
        else if ([key isEqualToString:@"dynamic_fps"]) f.dynamic_fps = n != 0;
        else if ([key isEqualToString:@"stats"]) f.stats = n;
        else if ([key isEqualToString:@"frame_graph"]) f.frame_graph = n;
        else if ([key isEqualToString:@"aspect"]) f.aspect = n == 1 ? 1 : n == 0 ? 0 : -1;
    }
    return f;
}
// The file's values, with stats and frame graph resolved against DM_AOC_STATS / DM_AOC_FRAME_GRAPH (passed in, getenv in the game).
typedef struct { DmSettingsFile file; DmPref stats, graph; int stats_rejected, graph_rejected; } DmSettingsLoaded;
static inline DmSettingsLoaded dm_settings_load(NSString *path, const char *env_stats, const char *env_graph) {
    DmSettingsLoaded l;
    l.file = dm_settings_file_read(path);
    l.stats = dm_pref_resolve(l.file.stats, env_stats, 2, 0, &l.stats_rejected);
    l.graph = dm_pref_resolve(l.file.frame_graph, env_graph, 1, 0, &l.graph_rejected);
    return l;
}
// current: the other settings as in force. Stats and frame graph are written as their saved preferences, never as a value the
// environment decided for this run.
static inline BOOL dm_settings_save(NSString *path, DmSettingsFile current, DmPref stats, DmPref graph, NSError **error) {
    static const char *layout[] = {"auto", "position", "label"};
    NSString *text = [NSString stringWithFormat:@"layout=%s\nswap_ab=%d\nfullscreen=%d\noutput=%d\nfps_target=%d\ndynamic_fps=%d\nstats=%d\nframe_graph=%d\naspect=%d\n",
        layout[current.layout >= 0 && current.layout < 3 ? current.layout : 0], current.swap_ab > 0, current.fullscreen > 0,
        current.output >= 0 && current.output < 5 ? current.output : 0, current.fps_target, current.dynamic_fps > 0, stats.saved, graph.saved, current.aspect == 1];
    return [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:error];
}
