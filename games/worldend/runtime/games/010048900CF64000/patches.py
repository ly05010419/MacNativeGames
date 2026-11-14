# WORLDEND SYNDROME (010048900CF64000): the rewriter's patches of this game's own builds.
# dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's
# NSO build ID. CUTS, OBS and FRAME_STEP as described there ("Registered patches").
# Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py.
#
# Scaffold: no game-code cuts until first boot measures a needed site. A patch must
# name the original NSO build ID and assert its original instruction.
CUTS = {}
