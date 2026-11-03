// WinNativeGames: Yuri's Revenge (1.001), recompiled.
//
//   yr [--game <install dir>] [--resolution auto|WxH|ini] [--fps-cap <n>]
//      [--sidebar-scale <s>] [--no-command-bar] [game arguments...]
//
// The install dir is the player's own Red Alert 2 install with Yuri's
// Revenge (the Steam one holds both); it defaults to RA2_GAME_DIR, then to
// the repository's game/install. On iOS it is the one Red Alert 2 uses
// (ra2::IosGameDir: in the app, or Documents/RA2); the log is Documents/yr-log.txt. gamemd.exe is the process image;
// Blowfish.dll (the COM server that decrypts MIX headers, the same as Red
// Alert 2's) is lifted with it. Before the game starts: RA2MD.INI's screen
// size, no movies and the default game speed (settings.h); battlefield
// frames shown at an even rate (src/ra2/frame_pacing.h). The touch
// controls, the sidebar's scale and the command bar are Red Alert 2's
// (src/ra2/controls.h), on the same code at Yuri's Revenge's addresses
// (src/ra2/addresses.h, hooks.cpp).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "../app/games.h"
#include "../ra2/addresses.h"
#include "../ra2/controls.h"
#include "../ra2/devtools.h"
#include "../ra2/game_dir.h"
#include "../ra2/handshake.h"
#include "settings.h"
#include "runtime/lifted.h"
#include "runtime/runtime.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if TARGET_OS_IOS && !WNG_CHOOSER
#include <SDL3/SDL_main.h>
#endif

// Its lifted code's table, renamed where it is compiled (CMakeLists.txt).
namespace recomp {
extern const LiftedFn kLiftedFunctionsYr[];
extern const uint32_t kLiftedFunctionCountYr;
}  // namespace recomp

namespace {

constexpr const char* kGuestDir = "C:\\Westwood\\RA2";
constexpr const char* kRa2Key = "HKLM\\SOFTWARE\\Westwood\\Red Alert 2";
constexpr const char* kYrKey = "HKLM\\SOFTWARE\\Westwood\\Yuri's Revenge";
// The text gamemd.exe checks in the launcher's mapping (0x81F560).
constexpr const char* kLauncherCheck = "UIDATA,3DDATA,MAPS";

void BeforeEntry(recomp::Ctx& c) {
  yr::ApplySettings();
  ra2::StartDevTools();
  ra2::StartLauncherHandshake(c, kLauncherCheck);
}

recomp::GameConfig Config(const std::string& game_dir) {
  recomp::GameConfig g;
  g.id = "yr";
  g.guest_dir = kGuestDir;
  g.exe_name = "gamemd.exe";
  // What the retail installers write: Yuri's Revenge is installed into Red
  // Alert 2's directory and needs Red Alert 2's key as well.
  const std::string dir = std::string(kGuestDir) + "\\";
  g.registry.push_back({kRa2Key, "InstallPath", dir + "Ra2.exe"});
  g.registry.push_back({kRa2Key, "Name", "Red Alert 2"});
  g.registry.push_back({kYrKey, "InstallPath", dir + "RA2MD.exe"});
  g.registry.push_back({kYrKey, "Name", "Yuri's Revenge"});
  g.lifted_dlls = {
      {"Blowfish.dll"},  // CoCreateInstance'd by gamemd.exe (MIX header decryption)
  };
  // English, or Simplified Chinese with RA2MD.CSF (src/ra2/game_dir.h).
  ra2::UseLanguage(g, game_dir, ra2::ChooseLanguage(game_dir, "YR"));
  ra2::AddPlayControls(g);
  g.before_entry = BeforeEntry;
  return g;
}

}  // namespace

int yr::RunGame(int argc, char** argv) {
  recomp::RegisterLiftedFunctions(recomp::kLiftedFunctionsYr, recomp::kLiftedFunctionCountYr);
  ra2::UseAddresses(ra2::kYrAddresses);
  const char* env = std::getenv("RA2_GAME_DIR");
  if (const char* r = std::getenv("YR_RESOLUTION")) yr::SetResolution(r);
  ra2::ReadPlayOptions("YR");
#if TARGET_OS_IOS
  const char* home = std::getenv("HOME");
  const std::string docs = std::string(home ? home : ".") + "/Documents";
  // Unbuffered, so it survives a hang or a kill; YR_LOG_CONSOLE=1 keeps
  // it on the console (xcrun devicectl device process launch --console).
  const char* console = std::getenv("YR_LOG_CONSOLE");
  if (!console || *console != '1') std::freopen((docs + "/yr-log.txt").c_str(), "w", stderr);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  setenv("XDG_DATA_HOME", docs.c_str(), 1);
  std::string game_dir = env ? env : ra2::IosGameDir();
  std::fprintf(stderr, "yr: starting, game dir %s\n", game_dir.c_str());
#else
  std::string game_dir = env ? env : YR_DEFAULT_GAME_DIR;
#endif
  std::string args = "-CD.";
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) {
      game_dir = argv[++i];
      continue;
    }
    // --language en|zh-Hant|zh-Hans (game_dir.h), as YR_LANGUAGE.
    if (a == "--language" && i + 1 < argc) {
      setenv("YR_LANGUAGE", argv[++i], 1);
      continue;
    }
    if (a == "--resolution" && i + 1 < argc) {
      yr::SetResolution(argv[++i]);
      continue;
    }
    // --fps-cap, --sidebar-scale, --no-command-bar (src/ra2/controls.h).
    if (ra2::PlayOption(argc, argv, &i)) continue;
    args += " " + a;
  }
  const std::filesystem::path exe = ra2::InGameDir(game_dir, "gamemd.exe");
  if (!std::filesystem::is_regular_file(exe)) {
    std::fprintf(stderr, "yr: %s not found (pass --game <install dir>)\n", exe.c_str());
    return 2;
  }
  return recomp::RunProcess(Config(game_dir), exe.string(), game_dir, args);
}

#if !WNG_CHOOSER
int main(int argc, char** argv) { return yr::RunGame(argc, argv); }
#endif
