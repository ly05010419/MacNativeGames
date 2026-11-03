// WinNativeGames: Red Alert 2 (1.006), recompiled.
//
//   ra2 [--game <install dir>] [--resolution auto|WxH|ini] [--sidebar-scale <s>]
//       [--no-command-bar] [--language en|zh-Hant|zh-Hans]
//       [--fps-cap <n>] [game arguments...]
//
// The install dir is the player's own Red Alert 2 install; it defaults to
// RA2_GAME_DIR, then to the repository's game/install. GAME.EXE is the
// process image; Blowfish.dll (the COM server that decrypts MIX headers) is
// lifted with it.
//
// On iOS the install is in the app (put there by install_device.py), or in
// the app's Documents/RA2 (ra2::IosGameDir); saves, settings and the log
// (ra2-log.txt) go in Documents.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "addresses.h"
#include "controls.h"
#include "game_dir.h"
#include "launcher.h"
#include "resolution.h"
#include "../app/games.h"

#include "runtime/lifted.h"
#include "runtime/runtime.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if TARGET_OS_IOS
// SDL runs UIApplicationMain and calls main (as SDL_main) on the main thread;
// in the app that holds both games, main is the chooser's.
#include <SDL3/SDL.h>
#if !WNG_CHOOSER
#include <SDL3/SDL_main.h>
#endif
#endif

namespace {

constexpr const char* kGuestDir = "C:\\Westwood\\RA2";
constexpr const char* kRa2Key = "HKLM\\SOFTWARE\\Westwood\\Red Alert 2";

recomp::GameConfig Config(const std::string& game_dir) {
  recomp::GameConfig g;
  g.id = "ra2";
  g.guest_dir = kGuestDir;
  g.exe_name = "GAME.EXE";
  // What the retail installer writes under the game's key (observed values
  // are recorded in reference/ once the oracle install exists).
  const std::string dir = std::string(kGuestDir) + "\\";
  g.registry.push_back({kRa2Key, "InstallPath", dir + "Ra2.exe"});
  g.registry.push_back({kRa2Key, "Name", "Red Alert 2"});
  g.lifted_dlls = {
      {"Blowfish.dll"},  // CoCreateInstance'd by GAME.EXE (MIX header decryption)
  };
  ra2::UseLanguage(g, game_dir, ra2::ChooseLanguage(game_dir, "RA2"));
  ra2::AddPlayControls(g);
  // GAME.EXE only runs when started by its launcher (launcher.cpp).
  g.before_entry = ra2::BeforeEntry;
  return g;
}

}  // namespace

#if TARGET_OS_IOS
// The app's Documents directory: where the player puts the game, and where
// its data and log go.
std::string IosDocuments() {
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/Documents";
}
#endif

int ra2::RunGame(int argc, char** argv) {
  recomp::RegisterLiftedFunctions(recomp::kLiftedFunctions, recomp::kLiftedFunctionCount);
  ra2::UseAddresses(ra2::kRa2Addresses);
  const char* env = std::getenv("RA2_GAME_DIR");
  if (const char* r = std::getenv("RA2_RESOLUTION")) ra2::SetResolution(r);
  ra2::ReadPlayOptions("RA2");
#if TARGET_OS_IOS
  const std::string docs = IosDocuments();
  // The log goes to Documents/ra2-log.txt, unbuffered so it survives a
  // hang or a kill; RA2_LOG_CONSOLE=1 keeps it on the console instead
  // (xcrun devicectl device process launch --console).
  const char* console = std::getenv("RA2_LOG_CONSOLE");
  if (!console || *console != '1') std::freopen((docs + "/ra2-log.txt").c_str(), "w", stderr);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  setenv("XDG_DATA_HOME", docs.c_str(), 1);
  std::string game_dir = env ? env : ra2::IosGameDir();
  std::fprintf(stderr, "ra2: starting, game dir %s\n", game_dir.c_str());
#else
  std::string game_dir = env ? env : RA2_DEFAULT_GAME_DIR;
#endif
  // -CD<path>: the game's own switch for running with every file local
  // (no CD drive); the install directory stands in for the CD.
  std::string args = "-CD.";
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) {
      game_dir = argv[++i];
      continue;
    }
    // --language en|zh-Hant|zh-Hans (game_dir.h), as RA2_LANGUAGE.
    if (a == "--language" && i + 1 < argc) {
      setenv("RA2_LANGUAGE", argv[++i], 1);
      continue;
    }
    // The screen size the game runs at (resolution.h): by default the
    // screen's own shape, so it is filled.
    if (a == "--resolution" && i + 1 < argc) {
      ra2::SetResolution(argv[++i]);
      continue;
    }
    // --fps-cap, --sidebar-scale, --no-command-bar (controls.h).
    if (ra2::PlayOption(argc, argv, &i)) continue;
    args += (args.empty() ? "" : " ") + a;
  }
  const std::filesystem::path exe = ra2::InGameDir(game_dir, "GAME.EXE");
  if (!std::filesystem::is_regular_file(exe)) {
    std::fprintf(stderr, "ra2: %s not found (pass --game <install dir>)\n", exe.c_str());
#if TARGET_OS_IOS
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Red Alert 2",
                             "GAME.EXE not found. Install the app with your own Red Alert 2 "
                             "files (install_device.py --game), or copy your install folder "
                             "into this app's Documents as \"RA2\" (Finder > iPhone > Files), "
                             "then start the app again.",
                             nullptr);
#endif
    return 2;
  }
  return recomp::RunProcess(Config(game_dir), exe.string(), game_dir, args);
}

#if !WNG_CHOOSER
int main(int argc, char** argv) { return ra2::RunGame(argc, argv); }
#endif
