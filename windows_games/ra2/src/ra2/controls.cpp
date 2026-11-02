// The play options and touch controls both games share (controls.h).

#include "controls.h"

#include <cstdint>
#include <cstdlib>
#include <string>

#include "commandbar.h"
#include "frame_pacing.h"
#include "sidebar.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace ra2 {

// The stick and buttons on the screen: on a touch screen; on the Mac only
// when <prefix>_SCREEN_CONTROLS=1 (trying the phone's layout there).
static bool g_screen_controls = TARGET_OS_IOS;

void ReadPlayOptions(const char* prefix) {
#if TARGET_OS_IOS
  SetCommandBarHidden(true);
#endif
  const std::string p = prefix;
  if (const char* c = std::getenv((p + "_SCREEN_CONTROLS").c_str())) g_screen_controls = *c != '0';
  if (const char* s = std::getenv((p + "_SIDEBAR_SCALE").c_str()))
    SetSidebarScale(std::strtof(s, nullptr));
  if (const char* f = std::getenv((p + "_FPS_CAP").c_str())) SetFrameCap(std::atoi(f));
  if (const char* b = std::getenv((p + "_COMMAND_BAR").c_str())) SetCommandBarHidden(*b == '0');
}

bool PlayOption(int argc, char** argv, int* i) {
  const std::string a = argv[*i];
  // The most battlefield frames shown a second (0: all).
  if (a == "--fps-cap" && *i + 1 < argc) {
    SetFrameCap(std::atoi(argv[++*i]));
    return true;
  }
  // The sidebar laid out for a screen 1/s as tall.
  if (a == "--sidebar-scale" && *i + 1 < argc) {
    SetSidebarScale(std::strtof(argv[++*i], nullptr));
    return true;
  }
  // The battlefield over the command bar's lines.
  if (a == "--no-command-bar") {
    SetCommandBarHidden(true);
    return true;
  }
  return false;
}

// The stick and buttons on the screen (iOS; on the Mac with
// <prefix>_SCREEN_CONTROLS=1).
static void AddScreenControls(recomp::GameConfig& g) {
  g.touch_stick = recomp::GameConfig::Stick::Query;
  // Five buttons on an arc around the stick, from above it to its right
  // (the game's default keys, Keyboard.ini):
  //   1, 2 (and 3, 4 in the bottom row): a tap makes the selection that
  //     team (TeamCreate_1 = Ctrl+'1'); a double tap selects that team and
  //     centres the view on it ('1' twice: TeamSelect_1 = '1', pressed
  //     again it centres);
  //   deploy: DeployObject = 'D';
  //   force attack: Ctrl held for the next click;
  //   cancel: a right click in the battlefield (left of the sidebar at any
  //     width the game runs at) deselects, or drops a building being placed.
  using Button = recomp::GameConfig::ScreenButton;
  auto team = [](const char* label, uint8_t key, float angle) {
    Button b;
    b.action = Button::Action::Key;
    b.icon = Button::Icon::Label;
    b.label = label;
    b.vk = key;
    b.mod = 0x11;  // VK_CONTROL
    b.double_vk = key;
    b.double_presses = 2;  // pressed twice, the game also centres on the team
    b.angle = angle;
    return b;
  };
  Button deploy;
  deploy.action = Button::Action::Key;
  deploy.icon = Button::Icon::Deploy;
  deploy.vk = 'D';
  deploy.angle = 45;
  Button force;
  force.action = Button::Action::StickyKey;
  force.icon = Button::Icon::Target;
  force.vk = 0x11;  // VK_CONTROL
  force.angle = 17.5f;
  Button cancel;
  cancel.action = Button::Action::RightClick;
  cancel.icon = Button::Icon::Cross;
  cancel.click_x = 0.4f;
  cancel.click_y = 0.45f;
  cancel.angle = -10;
  // Teams 3 and 4 only in the bottom row (the arc has room for five).
  g.screen_buttons = {team("1", '1', 100), team("2", '2', 72.5f), deploy, force, cancel};
  if (CommandBarHidden())
    g.screen_buttons = {team("1", '1', 0), team("2", '2', 0), team("3", '3', 0),
                        team("4", '4', 0), deploy, force, cancel};
  // Without the command bar (commandbar.h) they take its place: a row along
  // the bottom.
  if (CommandBarHidden()) g.button_layout = recomp::GameConfig::ButtonLayout::BottomRow;
  // Only in a match (the battlefield's loop runs: not the menus, loading
  // or the in-match options; sidebar.h).
  g.controls_shown = SidebarShown;
}

void AddPlayControls(recomp::GameConfig& g) {
  // On a touch screen: tap, drag (box select), hold or two-finger tap
  // (right click), two-finger drag (right-drag scrolling).
  g.touch = recomp::GameConfig::Touch::Pointer;
  // The game scrolls while the right button is held, past a dead zone of
  // about 10 px; 14 px off it scrolls at about 0.8 px/ms (measured).
  // Frame rate at the top left; a stick at the bottom left moves the
  // battlefield view directly (scroll.cpp).
  g.show_fps = true;
  if (g_screen_controls) AddScreenControls(g);
#if !TARGET_OS_IOS
  // On the Mac a window the player can resize (the picture keeps its
  // aspect) and take full screen with its green button;
  // <prefix>_FULLSCREEN=desktop|exclusive starts in full screen.
  g.fullscreen = recomp::GameConfig::Fullscreen::Window;
#endif
  // Without the command bar: its lines, below the screen the game runs
  // at, are not shown in a match (commandbar.cpp).
  if (CommandBarHidden()) {
    g.crop_bottom = kCommandBarHeight;
    g.crop_active = SidebarShown;
  }
  g.touch_pan_offset = 14;
  g.touch_pan_speed = 0.8f;
  // Menus and their animations shown at the battlefield's rate too
  // (frame_pacing.h).
  g.present_rate = FrameCap();
  // The sidebar enlarged beside a 1:1 battlefield, and the command bar
  // below it with it unless it is left out (sidebar.h, commandbar.h).
  if (SidebarScale() > 1.0f) {
    g.side_panel = {kSidebarWidth, SidebarScale(), SidebarShown,
                    kCommandBarHeight, kCommandBarLength, kCommandBarScale};
    if (CommandBarHidden()) g.side_panel.bar_height = 0;
  }
}

}  // namespace ra2
