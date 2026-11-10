# BOTW picture defaults on four-GB iPads

BOTW now starts a new configuration on an iPad with at most 4 GiB physical RAM in
handheld mode with a 30 fps cap. This selects the game's 960×540 scene and
1280×720 output instead of the 1600×900 scene / 1920×1080 docked output.

The first device-specific defaults are stored so the iOS Settings pane agrees
with the game. Existing saved picture and frame-rate choices are preserved.
For an existing installation, turn Full HD off and choose 30 fps in the game's
settings to use this combination. Choosing Full HD again remains possible.

Larger iPads, iPhones, an unknown memory size, other games, and visionOS retain
their existing defaults. This changes picture defaults, not the application
memory pool, texture quality, signing capabilities, or the guest address layout.

The reason for this default is the measured load on an iPad mini 6 (A15, 4 GB,
iPadOS 27.2.1): docked BOTW 1.9 reached GPU frame times around 25 ms and a
2859 MiB process footprint before an allocation abort. A lighter initial picture
reduces GPU work and render-target memory, but sustained gameplay still needs
device validation; a 30 fps selection is not a guarantee of 30 fps everywhere.

`test_ios_device_defaults.py` compiles the production helper against Foundation,
loads the real Settings pane and checks fresh four-/three-/eight-GB configurations,
unknown memory, iPhone defaults and stored player choices in isolated
`NSUserDefaults` domains.
