"""What every game's build fetches instead of asking the player for Homebrew (user 2026-11-12: "玩家完全不用装 Homebrew",
"全部 24 款，用共用的 tools/deps"). One list for build_app.py and all games/*: a version is changed here, once.

Sources are pinned by SHA-256 (release archives; the .NET SDK by the SHA-512 Microsoft publishes) or by a git commit.
Where this Mac had Homebrew's build of one (opus, libvpx, dav1d, zstd, spirv-cross, glslang, xcodegen), the pin is
that version, so a game builds as before; each was released at least two weeks before it was pinned.
"""

# name: (version, url, digest). An archive URL is checked against the digest; a .git URL is fetched at that commit.
SOURCES = {
    # The games' audio (opus) and VP8 / VP9 movies (libvpx), linked statically by every runtime.
    'opus': ('1.6.1', 'https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz',
             '6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1'),
    'libvpx': ('1.16.0', 'https://github.com/webmproject/libvpx.git', '1024874c5919305883187e2953de8fcb4c3d7fa6'),
    # Ocarina of Time's AV1 fallback decoder (games/oot/runtime/hle_movie_av1.c): the VideoLAN release.
    'dav1d': ('1.5.3', 'https://downloads.videolan.org/pub/videolan/dav1d/1.5.3/dav1d-1.5.3.tar.xz',
              '732010aa5ef461fa93355ed2c6c5fedb48ddc4b74e697eaabe8907eaeb943011'),
    # Ogg Vorbis audio (Animal Crossing, Super Smash Bros. Ultimate): Xiph's releases (SHA256SUMS on downloads.xiph.org).
    'ogg': ('1.3.6', 'https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.gz',
            '83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638'),
    'vorbis': ('1.3.7', 'https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz',
               '0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab'),
    # The zstd program (the nxnative builds unpack with it): the release tarball and its published .sha256.
    'zstd': ('1.5.7', 'https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz',
             'eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3'),
    # The spirv-cross program (nxnative's SPIR-V to MSL step): tag vulkan-sdk-1.4.363.0, what Homebrew had.
    'spirv-cross': ('1.4.363.0', 'https://github.com/KhronosGroup/SPIRV-Cross.git', 'f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7'),
    # glslangValidator (an OpenGL game's GLSL to SPIR-V): tag 16.6.0.
    'glslang': ('16.6.0', 'https://github.com/KhronosGroup/glslang.git', 'e1b562a8bed273a02f30b59b66a5d499793cede5'),
    # XcodeGen (the iPhone / iPad Xcode project): the release zip, a universal binary; the digest is GitHub's for the asset.
    'xcodegen': ('2.46.0', 'https://github.com/yonaskolb/XcodeGen/releases/download/2.46.0/xcodegen.zip',
                 '4d9e34b62172d645eed6457cac13fc222569974098ef4ee9c3368bedf0196806'),
    # The .NET SDK (nxnative's dmsh_xlate runs on it, nx_extract is built with it): Microsoft's tarball, SHA-512 from
    # builds.dotnet.microsoft.com/dotnet/release-metadata/10.0/releases.json.
    'dotnet': ('10.0.401', 'https://builds.dotnet.microsoft.com/dotnet/Sdk/10.0.401/dotnet-sdk-10.0.401-osx-arm64.tar.gz',
               '69f64eb00dc045398755c440b152225d544301a345a146a16e86a56a0c52b7c94b2c331520e976dbb821f18d31930aafbd25bb85961e3517e0665414ce0cbcff'),
}

PACKAGES = {
    'libhac': ('libhac.0.19.0.nupkg', 'https://api.nuget.org/v3-flatcontainer/libhac/0.19.0/libhac.0.19.0.nupkg',
               '1431267861db5ff68216315b164f10c0edab4df162cedf5429bf8a143b76de19'),
}

# meson and ninja build dav1d; cmake builds glslang and the iPhone / iPad libraries (games/*/ios/tools/build_deps.py).
# PyPI wheels pinned by SHA-256, in a virtual environment under the cache. The folder name is a hash of exactly this
# mapping for the tools asked for (build-tools-7678300352e8 = meson + ninja): keep the values (version, sha256) tuples.
BUILD_TOOLS = {'meson': ('1.12.1', '930bc7542cbd9f57009e182fd014eba48cf1a0180a7b9006d2d32d8f168d8b02'),
               'ninja': ('1.13.2', 'fd82e26c0706ad4ab88e5fdd26f3fab0a987a90f810160f6c322e752c6af298b'),
               'cmake': ('4.4.4', '883962a72b1b3a16ff445427f24c8caca9797dd9e563bbe208cbcd89e877f9d3')}

# UI reconstruction, CPython 3.14 / Apple silicon. PyPI wheels, all released before 2026-10-31.
PYTHON_ENVIRONMENTS = {
    'ui-pack': (
        ('numpy-2.5.4-cp314-cp314-macosx_14_0_arm64.whl',
         'https://files.pythonhosted.org/packages/d5/b2/c6ce165acffceb15a82c07b9cc77d391f86b3f379ba62911908ae5d34b91/numpy-2.5.4-cp314-cp314-macosx_14_0_arm64.whl',
         '956555e0603a4d38019ae6925711cb9dc43195c076a928accf7ea5d50bddfe53'),
        ('scipy-1.18.1-cp314-cp314-macosx_14_0_arm64.whl',
         'https://files.pythonhosted.org/packages/70/e8/6b0c288c50942d78193696c9f15f9a0874f5178aa0ddf40f83d9924b3e8d/scipy-1.18.1-cp314-cp314-macosx_14_0_arm64.whl',
         '011413b7426b75012840e35649e00fe0a2c3bae89fed433876e3a99251572efc'),
        ('pillow-13.0.0-cp314-cp314-macosx_11_0_arm64.whl',
         'https://files.pythonhosted.org/packages/b5/f4/0bf6f1ac9ba7ee2ffe9914aeb410a1035a0c1a3c42f122388d7fc950bc4b/pillow-13.0.0-cp314-cp314-macosx_11_0_arm64.whl',
         'd9b0a2bee3f3e03e9f29057aedb807dbf1e1d96ee1b9d0f422d9db6c8583271d'),
        ('contourpy-1.4.0-cp314-cp314-macosx_11_0_arm64.whl',
         'https://files.pythonhosted.org/packages/04/5a/513484208742f65af2648c519f0b7ab034616e183c4403538024c136a1ab/contourpy-1.4.0-cp314-cp314-macosx_11_0_arm64.whl',
         '072a702e2e178f4fcf96f04775d0c059e4c917925abf0d3208ebb6865df3c23d'),
    ),
}
