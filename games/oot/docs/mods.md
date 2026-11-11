# Ocarina of Time: mods and save editing

Not part of the build: optional tools other people make, for an app you already built ([install guide](install.md)). Contributed in PR #2060.

The Switch 2 OoT port can load RomFS replacements for title ID `0400486024DEC000` on update `v131072`. Mods for the N64 and 3DS releases use different formats. Switch 2 ExeFS patches, such as community first-person and ultrawide mods, need a native port.

[MacNative Mods' OoT support branch](https://github.com/christianfurr/macnative-mods/tree/feat/ocarina-support) can import Switch 2 `romfs/` packs and prepare the native overlay. Select the installed OoT app and a compatible OoT tools checkout, import the mod, then enable it and choose **Apply & Launch** with the game closed. It supports builds with bundled game data or an external game-data library. If the image is on a separate drive, open OoT once and locate its data before importing.

[PlayStation, Xbox, and Steam Deck button prompts](https://github.com/exgde/oot-switch2-button-prompts/releases/tag/v1.2) are available as RomFS packs. Use one at a time because they replace the same UI files. The PlayStation pack was verified on the native port in the **System → Basic Controls** screen, where the DualSense diagram and button labels appeared.

[OoT Editor 0.3.0](https://github.com/christianfurr/oot-editor/releases/tag/v0.3.0) edits saves offline. Open **Edit Player Save Copy** and load the copy in Save Workshop. With OoT closed, **Apply to Profile** checks that the destination still matches and backs it up before replacing the edited files. **Quick Start** uses a separate test profile.
