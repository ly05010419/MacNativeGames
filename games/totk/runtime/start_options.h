/* Start options of the Mac Settings menu (runtime/frontend.m): TOTK_ variables the game reads once at start. The menu
   saves each choice in the app's preferences (a boolean named after the variable, in the defaults domain
   org.macnativegames.totk or org.macnativegames.botw), and app/launcher.m sets the variable to its value at the next
   start unless the environment already has it, so `open --env` and the documented variables keep working. Nothing saved:
   the game starts as before. Diagnostics stay environment-only and are not listed here. */
#pragma once

typedef struct { const char *env, *value, *zh, *en; } StartOption;
static const StartOption start_options[] = {
    {"TOTK_ANISO", "16", "远处地面更清晰（16 倍各向异性过滤）", "Sharper ground at a distance (16x anisotropic filtering)"},
    {"TOTK_TAA", "1", "启动时开启边缘平滑（按键 5）", "Start with edge smoothing on (key 5)"},
    {"TOTK_TITLE_FPS", "1", "窗口标题显示帧率", "Frame rate in the window title"},
};
enum { START_OPTIONS = sizeof start_options / sizeof *start_options };
