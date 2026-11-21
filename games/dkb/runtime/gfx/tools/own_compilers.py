#!/usr/bin/env python3
# dmsh_build.py as a player's Mac runs it: the NSP script builds the shader libraries through this file. dmsh_build.py stops when any tool differs from dmsh_chain.lock;
# the translator (dmsh_xlate_bin), bundled SPIRV-Cross and Metal compilers (macOS and iOS) must match the lock. Issue #242 reported corruption with an untested Metal compiler;
# its role remains unconfirmed. Another Metal compiler stops the build with installation instructions unless DM_ALLOW_OTHER_METAL=1 (the NSP script's --allow-other-metal).
# The override reports the actual compiler, leaves the lock and chain version unchanged, and uses separate shader and variant cache entries keyed by that compiler and its SDK.
# A missing tool, another translator or another SPIRV-Cross always fails (a missing iOS compiler only when the build compiles for iOS). On the maintainer's Mac dmsh_build.py keeps the strict check. This wrapper is not part of the chain version.
# Usage: own_compilers.py <dmsh_build.py arguments>   (the environment as for dmsh_build.py)
#        own_compilers.py --check-metal              only the Metal compiler check: exit 1 with the message when it would stop the build (the NSP script runs it before its shader steps)
import os, re, sys, runpy, subprocess
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); import dmsh_chain, tool_lock
OWN = re.compile(r'(metal_macosx|metal_iphoneos): lock (.*) / actual (?!missing$)(.*)')   # a missing compiler still stops the build
ALLOW = 'DM_ALLOW_OTHER_METAL'
XCODE_VERSION = '27.2'   # the Xcode whose Metal compiler dmsh_chain.lock records; README.md names it too
XCODE = f'Xcode {XCODE_VERSION} (macOS 26.6 or later)'
XCODE_APP = f'/Applications/Xcode-{XCODE_VERSION}.app'   # the name the message suggests for it beside a newer Xcode.app
strict = tool_lock.check_tools

def allowed(): return os.environ.get(ALLOW) == '1'

def metal_version(sdk):   # as tool_lock.tools_now records it: the first line of metal --version, 'missing' without one
    r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '--version'], capture_output=True, text=True)
    return (r.stdout.splitlines() or ['missing'])[0]

def selected_xcode():   # ' (selected: Xcode 27.3 in /Applications/Xcode.app/Contents/Developer)', '' when it cannot be read; both tools follow DEVELOPER_DIR as xcrun does
    try:
        dev = subprocess.run(['xcode-select', '-p'], capture_output=True, text=True).stdout.strip()
        ver = (subprocess.run(['xcodebuild', '-version'], capture_output=True, text=True).stdout.splitlines() or [''])[0]
    except OSError: return ''
    return f' (selected: {ver or "no Xcode"} in {dev})' if dev else ''

def other_metal_message(metal):
    """The stop message for the Metal compilers that differ from the lock: [(lock key, tested, actual)]. Each Xcode downloads and uses its own Metal toolchain (a system asset),
    whatever the macOS version, so with a newer Xcode selected -downloadComponent fetches that Xcode's compiler again: the tested Xcode must be selected first (GitHub issue #717)"""
    return ('[RED] this Mac\'s Metal compiler is not the one the shaders were tested with (dm/gfx/tools/dmsh_chain.lock): ' +
            '; '.join(f'{k} {now} (tested: {want})' for k, want, now in metal) +
            f'. Untested compilers are unsupported (GitHub issue #242). The Metal compiler is the selected Xcode\'s{selected_xcode()}: install {XCODE} '
            f'from https://developer.apple.com/download/all/ (beside a newer Xcode, as {XCODE_APP}), select it (sudo xcode-select -s {XCODE_APP}, '
            f'or DEVELOPER_DIR={XCODE_APP}/Contents/Developer for one build), then download its Metal toolchain (xcodebuild -downloadComponent MetalToolchain), '
            f'or set {ALLOW}=1 (the NSP script\'s --allow-other-metal) to build with this one anyway')

def other_metal():
    """The Metal compilers (macOS, iOS) that differ from the lock and are present: [(lock key, tested, actual)]; a missing one is dmsh_build.py's to report"""
    with open(dmsh_chain.LOCK) as f: want = dict(l.rstrip('\n').split('\t', 1) for l in f if '\t' in l and not l.startswith('#'))
    out = []
    for sdk in ('macosx', 'iphoneos'):
        now = metal_version(sdk)
        if now != 'missing' and now != want.get('metal_' + sdk): out.append(('metal_' + sdk, want.get('metal_' + sdk), now))
    return out

def check_metal():   # --check-metal: the stop message, '' when the build may go on
    metal = other_metal()
    return other_metal_message(metal) if metal and not allowed() else ''

def ios_target():   # as dmsh_build.py picks its targets: a Mac-only build (the NSP script's --platform mac / mac-app) compiles nothing for iOS
    return 'ios' in (os.environ.get('DMSH_TARGETS') or ('mac,ios' if os.environ.get('IOSOUT') else 'mac')).split(',')

def check_tools():
    bad, own = [], []
    for b in strict():
        m = OWN.fullmatch(b)
        if m: own.append((m[1], m[2], m[3]))   # (lock key, tested, actual)
        elif not (b.startswith('metal_iphoneos: ') and b.endswith(' / actual missing') and not ios_target()): bad.append(b)   # a Mac without the iOS platform
    if bad: return bad
    metal = own
    if metal and not allowed(): sys.exit(other_metal_message(metal))
    if own: print('note: built with this Mac\'s compilers, not the ones the shaders were tested with (dmsh_chain.lock): ' +
                  '; '.join(f'{k} {now} (tested: {want})' for k, want, now in own) + (f' ({ALLOW}=1)' if metal else ''), flush=True)
    return []

if __name__ == '__main__':
    if sys.argv[1:] == ['--check-metal']:
        msg = check_metal()
        if msg: print(msg)
        sys.exit(1 if msg else 0)
    tool_lock.check_tools = check_tools   # dmsh_build.py imports the same module object and calls it through the module
    sys.argv = [os.path.join(HERE, 'dmsh_build.py'), *sys.argv[1:]]
    runpy.run_path(sys.argv[0], run_name='__main__')
