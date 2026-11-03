// The player's game directory, as both games' launchers look into it
// (game_dir.cpp).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "runtime/runtime.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace ra2 {

#if TARGET_OS_IOS
// iOS: the game files. scripts/ios/install_device.py puts the player's own
// install into the app (<app>/Game, read-only: what the game writes goes to
// Documents through the runtime's overlay, so saves outlive reinstalls);
// an app installed without it reads Documents/RA2, where the player can
// copy the install instead.
std::string IosGameDir();
#endif

// The file `name` in the game directory in whatever case it is spelled
// there (the Steam install's names are lower case, the games ask for upper
// case; the runtime's own lookups ignore case like NTFS, but iOS file
// systems do not), else game_dir/name.
std::filesystem::path InGameDir(const std::string& game_dir, const std::string& name);

// The languages a game is played in. English is the install's own (the
// Steam release's MIX files); the Chinese ones are made by
// scripts/languages.py from the player's own Traditional Chinese language
// pack (Steam's, official) into <game dir>/lang/<code>/: RA2.CSF and
// RA2MD.CSF (the string tables, which the game reads before those in its
// MIX files), and for Simplified Chinese GAME.FNT with the glyphs Big5
// lacks.
enum class Language { English, TraditionalChinese, SimplifiedChinese };

const char* LanguageCode(Language l);  // "en", "zh-Hant", "zh-Hans"
const char* LanguageName(Language l);  // in its own language
bool ParseLanguage(const std::string& code, Language* l);

// The languages the game directory has, English first.
std::vector<Language> Languages(const std::string& game_dir);

// The language to play in, among those the game directory has:
// <prefix>_LANGUAGE (en, zh-Hant or zh-Hans), else the system's preferred
// one (Chinese of Taiwan, Hong Kong or Macau: Traditional; other Chinese:
// Simplified, else Traditional), else English.
Language ChooseLanguage(const std::string& game_dir, const char* prefix);

// Plays in `l`: the string tables and font the game reads come from
// lang/<code>/ (GameConfig::install_files; English: none, the MIX files'
// own), and the ANSI code page of its text (combo box items, edit fields)
// is the language's: 1252, 950 (Big5) or 936 (GBK). A game directory
// without lang/ is used as it is (a Chinese string table in it is taken as
// Traditional: 950).
void UseLanguage(recomp::GameConfig& g, const std::string& game_dir, Language l);

}  // namespace ra2
