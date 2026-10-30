/* Local diagnostic bundle launcher. Game data remains in the external work directory. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    CFBundleRef bundle = CFBundleGetMainBundle();
    CFTypeRef value = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("XCXWorkFolder"));
    char work[4096], binary[4096], path[8192];
    if (!value || CFGetTypeID(value) != CFStringGetTypeID() ||
        !CFStringGetCString(value, work, sizeof work, kCFStringEncodingUTF8)) return 2;
    CFURLRef executable = CFBundleCopyAuxiliaryExecutableURL(bundle, CFSTR("xcx-native"));
    if (!executable) return 2;
    int ok = CFURLGetFileSystemRepresentation(executable, true, (UInt8 *)binary, sizeof binary);
    CFRelease(executable);
    if (!ok || chdir(work)) return 2;
    CFTypeRef extended = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("XCXExtendedVA"));
    if (extended == kCFBooleanTrue) {
        if (setenv("XCX_EXTENDED_VA", "1", 1)) return 2;
    } else if (unsetenv("XCX_EXTENDED_VA")) return 2;
    if(setenv("XCX_SHADER_ROOT",work,1))return 2;
    CFTypeRef gpu=CFBundleGetValueForInfoDictionaryKey(bundle,CFSTR("XCXGPUSubmit"));
    if(gpu==kCFBooleanTrue){if(setenv("TOTK_GPU_ALLOW_SUBMIT","1",1))return 2;}
    else if(unsetenv("TOTK_GPU_ALLOW_SUBMIT"))return 2;
    snprintf(path, sizeof path, "%s/logs/app-launch.log", work);
    if (!freopen(path, "a", stderr)) return 2;
    if (dup2(fileno(stderr), STDOUT_FILENO) < 0) return 2;
    snprintf(path, sizeof path, "%s/user-profile", work);
    if (setenv("TOTK_PROFILE_DIR", path, 1) || setenv("TOTK_SLOT_OWNER", "user", 1)) return 2;
    snprintf(path, sizeof path, "%s/analysis/game-slot", work);
    if (setenv("TOTK_GAME_SLOT_DIR", path, 1)) return 2;
    unsetenv("TOTK_AUTOLOAD");
    unsetenv("TOTK_INPUT_SCRIPT");
    execl(binary, binary, "rewritten/v100", "boot", (char *)NULL);
    perror("xcx-native");
    return 2;
}
