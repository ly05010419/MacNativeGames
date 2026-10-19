/*
 * <TargetConditionals.h> for hosts that do not ship the Apple SDK.
 *
 * The runtime selects its platform with TARGET_OS_IPHONE / TARGET_OS_OSX /
 * TARGET_OS_SIMULATOR. On a non-Apple host none of those platforms applies, so
 * every TARGET_OS_* is 0 and the guarded branches stay off. That is enough to
 * syntax-check the platform-neutral code, and it makes clear which files are
 * syntax-clean but still platform-dependent (tests/check_linux_compile.sh
 * reports those as PASS*, never as PASS).
 *
 * This directory is only placed on the include path of non-Apple builds: on
 * macOS the real SDK header is used instead, so nothing changes there.
 */
#ifndef MACNATIVEGAMES_COMPAT_TARGETCONDITIONALS_H
#define MACNATIVEGAMES_COMPAT_TARGETCONDITIONALS_H

#define TARGET_OS_OSX       0
#define TARGET_OS_MAC       0
#define TARGET_OS_IPHONE    0
#define TARGET_OS_IOS       0
#define TARGET_OS_WATCH     0
#define TARGET_OS_TV        0
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_EMBEDDED  0
#define TARGET_OS_DRIVERKIT 0

#define TARGET_OS_LINUX     1

#endif /* MACNATIVEGAMES_COMPAT_TARGETCONDITIONALS_H */
