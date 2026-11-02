// The iOS app's entry when it holds both games (CMakeLists.txt,
// WNG_CHOOSER): asks which one to play, then runs it. One game a launch: the
// runtime runs one process image, so another game means starting the app
// again. A button shows the language (when the game folder has more than
// one, ra2::Languages) and changes it; the choice is kept. Without Yuri's
// Revenge's files (gamemd.exe, ra2::IosGameDir) and with one language, Red
// Alert 2 starts without asking.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "games.h"
#include "../ra2/game_dir.h"

namespace {

// Whether the folder holds `name` in some case (iOS file systems care).
bool HasFile(const std::filesystem::path& dir, const std::string& name) {
  auto lower = [](std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
  };
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    if (lower(e.path().filename().string()) == lower(name)) return true;
  return false;
}

enum class Game { Ra2, Yr };
constexpr int kLanguageButton = 100;

// The player's language choice, kept in Documents/language.txt.
std::filesystem::path LanguageFile() {
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/Documents/language.txt";
}

ra2::Language SavedLanguage(const std::string& game_dir) {
  std::ifstream f(LanguageFile());
  std::string code;
  ra2::Language l;
  if (f >> code && ra2::ParseLanguage(code, &l))
    for (ra2::Language have : ra2::Languages(game_dir))
      if (have == l) return l;
  return ra2::ChooseLanguage(game_dir, "RA2");
}

// One button a language; the choice is saved.
ra2::Language PickLanguage(const std::vector<ra2::Language>& langs, ra2::Language current) {
  std::vector<SDL_MessageBoxButtonData> buttons;
  for (size_t i = 0; i < langs.size(); i++)
    buttons.push_back({langs[i] == current ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0u,
                       int(i), ra2::LanguageName(langs[i])});
  SDL_MessageBoxData box{};
  box.flags = SDL_MESSAGEBOX_INFORMATION;
  box.title = "Language";
  box.message = "\u8a9e\u8a00 / \u8bed\u8a00 / Language";  // 語言 / 语言
  box.numbuttons = int(buttons.size());
  box.buttons = buttons.data();
  int picked = -1;
  if (!SDL_ShowMessageBox(&box, &picked) || picked < 0 || size_t(picked) >= langs.size())
    return current;
  std::ofstream(LanguageFile()) << ra2::LanguageCode(langs[size_t(picked)]) << "\n";
  return langs[size_t(picked)];
}

// Which game, and in which language (when the game folder has more than
// one, ra2::Languages): a button shows the language and changes it.
Game Choose(const std::string& game_dir, ra2::Language* language) {
  const std::vector<ra2::Language> langs = ra2::Languages(game_dir);
  const bool yr = HasFile(game_dir, "gamemd.exe");
  for (;;) {
    if (!yr && langs.size() < 2) return Game::Ra2;
    const std::string lang = std::string("\U0001F310 ") + ra2::LanguageName(*language);
    std::vector<SDL_MessageBoxButtonData> buttons = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, int(Game::Ra2), "Red Alert 2"}};
    if (yr) buttons.push_back({0, int(Game::Yr), "Yuri's Revenge"});
    if (langs.size() > 1) buttons.push_back({0, kLanguageButton, lang.c_str()});
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    box.title = "Red Alert 2";
    box.message = yr ? "Which game?" : "Red Alert 2";
    box.numbuttons = int(buttons.size());
    box.buttons = buttons.data();
    int picked = int(Game::Ra2);
    if (!SDL_ShowMessageBox(&box, &picked)) return Game::Ra2;
    if (picked == kLanguageButton) {
      *language = PickLanguage(langs, *language);
      continue;
    }
    return picked == int(Game::Yr) ? Game::Yr : Game::Ra2;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string game_dir = ra2::IosGameDir();
  ra2::Language language = SavedLanguage(game_dir);
  const Game game = Choose(game_dir, &language);
  // Both games read it (ra2::ChooseLanguage).
  setenv("RA2_LANGUAGE", ra2::LanguageCode(language), 1);
  setenv("YR_LANGUAGE", ra2::LanguageCode(language), 1);
  return game == Game::Yr ? yr::RunGame(argc, argv) : ra2::RunGame(argc, argv);
}
