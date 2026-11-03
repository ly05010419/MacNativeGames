// The player's game directory (game_dir.h).

#include "game_dir.h"

#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>
#if TARGET_OS_IOS
#include <SDL3/SDL_filesystem.h>
#endif

namespace ra2 {

#if TARGET_OS_IOS
std::string IosGameDir() {
  if (const char* base = SDL_GetBasePath()) {  // the app bundle, ending in '/'
    // Not "RA2": the bundle's executable is "ra2", and a Mac's disk, where
    // the app is put together, ignores case.
    const std::string bundled = std::string(base) + "Game";
    if (std::filesystem::is_regular_file(InGameDir(bundled, "GAME.EXE"))) return bundled;
  }
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/Documents/RA2";
}
#endif

std::filesystem::path InGameDir(const std::string& game_dir, const std::string& name) {
  auto lower = [](std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
  };
  const std::string want = lower(name);
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(game_dir, ec))
    if (lower(e.path().filename().string()) == want) return e.path();
  return std::filesystem::path(game_dir) / name;
}

namespace {

constexpr Language kAll[] = {Language::English, Language::TraditionalChinese,
                             Language::SimplifiedChinese};
// The files a language replaces at the top of the install.
constexpr const char* kLanguageFiles[] = {"RA2.CSF", "RA2MD.CSF", "GAME.FNT"};

std::filesystem::path LanguageDir(const std::string& game_dir, Language l) {
  return InGameDir(InGameDir(game_dir, "lang").string(), LanguageCode(l));
}

bool Has(const std::string& game_dir, Language l) {
  if (l == Language::English) return true;
  return std::filesystem::is_regular_file(
      InGameDir(LanguageDir(game_dir, l).string(), "RA2.CSF"));
}

// A Chinese string table at the top of the install (an older setup, without
// lang/): the CSF header's language field.
bool ChineseTable(const std::string& game_dir, const char* csf) {
  constexpr uint32_t kCsfChinese = 9;
  unsigned char h[24] = {};
  std::ifstream f(InGameDir(game_dir, csf), std::ios::binary);
  if (!f.read(reinterpret_cast<char*>(h), sizeof h) || std::memcmp(h, " FSC", 4) != 0) return false;
  return (h[20] | h[21] << 8 | h[22] << 16 | uint32_t(h[23]) << 24) == kCsfChinese;
}

}  // namespace

const char* LanguageCode(Language l) {
  switch (l) {
    case Language::TraditionalChinese: return "zh-Hant";
    case Language::SimplifiedChinese: return "zh-Hans";
    default: return "en";
  }
}

const char* LanguageName(Language l) {
  switch (l) {
    case Language::TraditionalChinese: return "\u7e41\u9ad4\u4e2d\u6587";  // 繁體中文
    case Language::SimplifiedChinese: return "\u7b80\u4f53\u4e2d\u6587";   // 简体中文
    default: return "English";
  }
}

bool ParseLanguage(const std::string& code, Language* l) {
  std::string c;
  for (char ch : code) c += char(std::tolower(static_cast<unsigned char>(ch)));
  for (Language x : kAll) {
    std::string want;
    for (const char* p = LanguageCode(x); *p; p++) want += char(std::tolower(*p));
    if (c == want) {
      *l = x;
      return true;
    }
  }
  return false;
}

std::vector<Language> Languages(const std::string& game_dir) {
  std::vector<Language> out;
  for (Language l : kAll)
    if (Has(game_dir, l)) out.push_back(l);
  return out;
}

Language ChooseLanguage(const std::string& game_dir, const char* prefix) {
  Language l;
  const std::string var = std::string(prefix) + "_LANGUAGE";
  if (const char* e = std::getenv(var.c_str())) {
    if (ParseLanguage(e, &l) && Has(game_dir, l)) return l;
    std::fprintf(stderr, "%s=%s: not one this game directory has (en%s%s)\n", var.c_str(), e,
                 Has(game_dir, Language::TraditionalChinese) ? ", zh-Hant" : "",
                 Has(game_dir, Language::SimplifiedChinese) ? ", zh-Hans" : "");
  }
  int n = 0;
  SDL_Locale** locales = SDL_GetPreferredLocales(&n);
  Language want = Language::English;
  if (locales && n > 0 && locales[0]->language && !std::strcmp(locales[0]->language, "zh")) {
    const char* c = locales[0]->country ? locales[0]->country : "";
    const bool traditional = !std::strcmp(c, "TW") || !std::strcmp(c, "HK") || !std::strcmp(c, "MO");
    want = traditional ? Language::TraditionalChinese : Language::SimplifiedChinese;
  }
  SDL_free(locales);
  if (want == Language::SimplifiedChinese && !Has(game_dir, want))
    want = Language::TraditionalChinese;
  return Has(game_dir, want) ? want : Language::English;
}

void UseLanguage(recomp::GameConfig& g, const std::string& game_dir, Language l) {
  if (!std::filesystem::is_directory(InGameDir(game_dir, "lang"))) {
    g.ansi_code_page = ChineseTable(game_dir, "RA2.CSF") || ChineseTable(game_dir, "RA2MD.CSF")
                           ? 950
                           : 1252;
    return;
  }
  g.ansi_code_page = l == Language::TraditionalChinese  ? 950
                     : l == Language::SimplifiedChinese ? 936
                                                        : 1252;
  for (const char* name : kLanguageFiles) {
    std::string lower;
    for (const char* p = name; *p; p++) lower += char(std::tolower(*p));
    std::string host;  // none: the MIX files' own
    if (l != Language::English) {
      const std::filesystem::path f = InGameDir(LanguageDir(game_dir, l).string(), name);
      if (std::filesystem::is_regular_file(f)) host = f.string();
    }
    g.install_files[lower] = host;
  }
  std::fprintf(stderr, "language: %s\n", LanguageCode(l));
}

}  // namespace ra2
