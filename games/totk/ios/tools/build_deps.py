#!/usr/bin/env python3
"""Build the audio and VP9 static libraries for a physical iOS device, or a Vision Pro (second argument xros)."""
import os, subprocess, sys, tempfile
from pathlib import Path

out = Path(sys.argv[1]).resolve(); out.mkdir(parents=True, exist_ok=True)
# libvpx's configure splits its compiler path and flags on spaces: when the work folder has one
# (e.g. "Tears of the Kingdom/TOTK_build"), sources and build trees go under a space-free root instead.
platform = sys.argv[2] if len(sys.argv) > 2 else 'iphoneos'
if platform not in ('iphoneos', 'xros'): sys.exit(f'unknown platform {platform}')
# Never reuse iOS objects for visionOS, including the space-free fallback used by libvpx.
root = out if ' ' not in str(out) else Path(tempfile.gettempdir()).resolve()/f'macnativegames-{platform}-deps'
sdk = subprocess.check_output(['xcrun','--sdk',platform,'--show-sdk-path'], text=True).strip()
# libvpx's configure asks xcrun for the iphoneos SDK whatever the target; the wrapper below answers with sdklink, which
# points at the selected SDK. Its iOS flags (-miphoneos-version-min) are overridden by the -target given here.
ios_sdk = subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'], text=True).strip()
target = 'arm64-apple-xros26.0' if platform == 'xros' else 'arm64-apple-ios26.0'
src = root/'src'; build = root/'build'; src.mkdir(parents=True, exist_ok=True); build.mkdir(exist_ok=True)
# CMake chooses the platform's flags from this path: iPhoneOS.platform gives -miphoneos-version-min even for visionOS
sdklink=root/'sdkroot/Platforms'/('XROS.platform' if platform=='xros' else 'iPhoneOS.platform')/'Developer/SDKs'/Path(sdk).name
if sdklink.is_symlink(): sdklink.unlink()
sdklink.parent.mkdir(parents=True,exist_ok=True)
if not sdklink.exists(): sdklink.symlink_to(sdk)
def run(cmd, cwd=None, env=None): subprocess.run(cmd, cwd=cwd, env=env, check=True)
def checkout(url, tag, dest):
    if not (dest/'.git').exists(): run(['git','clone','--depth','1','--branch',tag,url,str(dest)])

opus=src/'opus'; checkout('https://github.com/xiph/opus.git','v1.6.1',opus)
opus_build=build/'opus-ninja'
run(['cmake','-G','Ninja','-S',str(opus),'-B',str(opus_build),'-DCMAKE_SYSTEM_NAME='+('visionOS' if platform=='xros' else 'iOS'),f'-DCMAKE_OSX_SYSROOT={sdklink}',
     '-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0','-DOPUS_BUILD_SHARED_LIBRARY=OFF',
     '-DOPUS_BUILD_TESTING=OFF','-DOPUS_BUILD_PROGRAMS=OFF','-DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF','-DCMAKE_BUILD_TYPE=Release'])
run(['cmake','--build',str(opus_build),'--config','Release','-j'])

vpx=src/'libvpx'; checkout('https://chromium.googlesource.com/webm/libvpx.git','v1.16.0',vpx)
vpxout=build/'libvpx'; vpxout.mkdir(exist_ok=True)
bindir=build/'ios-bin'; bindir.mkdir(exist_ok=True)
real_sdk=ios_sdk; real_tools=Path(os.environ.get('DEVELOPER_DIR',subprocess.check_output(['xcode-select','-p'],text=True).strip()))/'Toolchains/XcodeDefault.xctoolchain/usr/bin'
for tool in ('clang','clang++','ar','as','strip'):
    link=bindir/tool
    if link.is_symlink(): link.unlink()
    if not link.exists(): link.symlink_to(real_tools/tool)
wrapper=bindir/'xcrun'
wrapper.write_text(f'''#!/usr/bin/env python3
import subprocess,sys
r=subprocess.run(["/usr/bin/xcrun",*sys.argv[1:]],capture_output=True,text=True)
s=r.stdout.replace({real_sdk!r},{str(sdklink)!r}).replace({str(real_tools)!r},{str(bindir)!r})
sys.stdout.write(s);sys.stderr.write(r.stderr);sys.exit(r.returncode)
''')
wrapper.chmod(0o755)
vpx_env=dict(os.environ,PATH=str(bindir)+os.pathsep+os.environ.get('PATH',''))
if platform=='xros': vpx_env['LDFLAGS']=f'-target {target} -Wno-overriding-option'   # its link checks pass only the iOS minimum
run([str(vpx/'configure'),'--target=arm64-darwin-gcc','--disable-examples','--disable-tools',
     '--disable-docs','--disable-unit-tests','--disable-webm-io','--enable-pic','--disable-shared','--enable-static',
     f'--extra-cflags=-target {target} -isysroot {sdklink}'+(' -Wno-overriding-option -Wno-incompatible-sysroot' if platform=='xros' else ' -miphoneos-version-min=26.0')], cwd=vpxout, env=vpx_env)
# make takes the environment as its variables, and a DEVELOPER_DIR with a space ("/Volumes/x/Tears of the Kingdom /Xcode.app")
# ends up in a shell command unquoted (127, 2026-10-23); the tools are the links in ios-bin, so make does not need it.
run(['make','-j',str(os.cpu_count() or 4),'libvpx.a'],cwd=vpxout,env={k:v for k,v in vpx_env.items() if k!='DEVELOPER_DIR'})
lib=out/'lib'; include=out/'include'; lib.mkdir(exist_ok=True); include.mkdir(exist_ok=True)
import shutil
shutil.copy2(opus_build/'libopus.a',lib/'libopus.a')
shutil.copy2(vpxout/'libvpx.a',lib/'libvpx.a')
shutil.copytree(opus/'include',include,dirs_exist_ok=True)
shutil.copytree(vpx/'vpx',include/'vpx',dirs_exist_ok=True)
shutil.copy2(vpxout/'vpx_config.h',include/'vpx_config.h')
print(f'IOS_DEPS={out}')
