# TMNT iPad bring-up

The UIKit and Metal device app compiles, links, signs, and installs on arm64
iPadOS. On one connected iPad, the private signed build launched without the
initial crash. Its device log showed the linked shader translator producing
vertex and pixel variants, the opening video decoder running, audio buffers
being produced, and a sustained approximately 60 FPS frame loop. The user
confirmed that this build displays and runs normally on that iPad. This is a
startup observation, not a completed gameplay, input, save, or long-running
stability test. The signed private app contains the user's own game assets;
neither it nor its diagnostic logs are part of the source contribution.

All game inputs and generated images stay outside the source tree. The local
`Gen` directory is an ignored link to a private build folder. It contains
verified generated module and NRO assembly, headers, and a private
`tmnt.xcconfig` with the paths to locally built static dependencies. Do not
commit generated assembly, game resources, binaries, saves, keys, or this
private config. `tools/gen_embed.py` and `tools/gen_nro_embed.py` validate the
rewrites before generating signed executable sections. iPadOS maps those
sections instead of attempting to execute anonymously generated code.

Source build steps, using an existing verified private ExeFS/RomFS/NRO
preparation from this game's scripts:

1. Build the iOS shader translator and SPIRV-Cross from the shared pin:
   `tools/totk_xlate/publish_ios_lib.sh /path/to/dotnet /private/shader` and
   `python3 ios/tools/build_spirv_cross.py --sdk /path/to/iPhoneOS.sdk --out /private/shader`.
2. Build pinned iPad codec libraries with
   `python3 ios/tools/build_deps.py /private/codecs`. The source tags are
   Opus 1.6.1 and libvpx 1.16.0.
3. Generate `ios/Gen` privately from verified rewrites using the two embed
   tools and the existing Mac `export-ios` mode. Place the generated
   `ios_embed.h`, `ios_nro_embed.h`, `embed.S`, `nro_embed.S`, runtime headers,
   dependency include paths, and a private `tmnt.xcconfig` there. The config
   links the two codec archives, `libtmnt_xlate_bootstrap.o`,
   `libtmnt_xlate.a`, `libtmnt_spirv_cross.a`, platform frameworks, C++, zlib
   and ICU. The source tree does not contain private game bytes or absolute
   machine paths.
4. Make an unsigned offline build with `xcodegen generate --spec
   games/tmnt/ios/project.yml --project games/tmnt/ios`, then MobileBuildMCP
   `device build` with scheme `TMNTiPad` and a private derived-data path. The
   checked-in project template disables signing so offline compilation is
   possible.
5. Package only verified locally owned assets into that unsigned build using
   `python3 games/tmnt/ios/tools/package_private_resources.py --bundle /private/verified-bundle --app /private/DerivedData/Build/Products/Debug-iphoneos/TMNTiPad.app --build-headers /private/build-headers`.
   This copies the RomFS and six fonts, and only the rewrite metadata needed
   at runtime. It never leaves a link to an external disk in the app. The
   resulting app contains copyrighted game data: keep it private.
6. Point the ignored `ios/Gen/TMNT` at the private packaged `TMNT` folder,
   regenerate the project, then make a **separate signed build** with
   MobileBuildMCP `device build`, overriding `CODE_SIGNING_ALLOWED=YES`,
   `CODE_SIGN_STYLE=Automatic`, and `DEVELOPMENT_TEAM=<your team>` with
   provisioning updates enabled. Xcode copies the private folder into the
   application before signing. Verify the resulting app signature and that
   its `TMNT` folder contains no symbolic links. Do not commit `Gen/TMNT`.
7. Install the signed app with MobileBuildMCP, trust the development team on
   the device, then launch it. The UIKit view attaches its Metal layer after
   receiving nonzero layout dimensions. The iPad reserves a smaller guest
   stack/heap layout and a measured alias region, and links the shader
   translator into the app so the first graphics variants can be generated
   on device. The app must use a private copy of the user's own game inputs.

The UIKit shell stores saves, the RomFS verification record, and shader cache
inside the app's Documents container. It enables the Mac port's strict frame
dependencies, split command recording, and DrawTexture orientation settings.
The Mac shader service is replaced on iPad by the linked NativeAOT translator.
The on-device opening graphics, decoder, and frame loop have been observed on
one iPad. Gameplay input, saves, lifecycle recovery, sustained memory usage,
and other device models still need validation. Mac results do not establish
iPad results.
