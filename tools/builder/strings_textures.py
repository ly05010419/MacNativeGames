"""The texts of the Mods tab's Textures section (S9.2), by id; strings.py merges them into its table (kept apart so
each file stays under 300 lines). Same rules as strings.py: a missing key raises KeyError, caught by the drift test."""

TEXTURES = {
    'gui.textures.title': 'Texture pack for {name} {version}',
    'gui.textures.note': 'A texture pack replaces the game\'s textures with bigger ones. It needs more memory: Apply '
                         'fits its size tables to your app, Build gives the app the pack\'s bigger memory heaps, and '
                         'Play then starts the game in the 8 GB mode. One pack at a time; its files stay yours: get '
                         'it from its author and follow their terms.',
    'gui.textures.memory': 'This Mac has {gb} GB of memory.',
    'gui.textures.memory_low': 'This Mac has {gb} GB of memory: big packs were checked on a 24 GB Mac and may not '
                               'fit in less.',
    'gui.textures.none': 'No texture pack yet.',
    'gui.textures.add': 'Add pack folder...',
    'gui.textures.add_archive': 'Add pack archive...',
    'gui.textures.replace': 'Replace pack...',
    'gui.textures.remove': 'Remove',
    'gui.textures.confirm_remove': 'Remove the texture pack "{name}" from the builder? Its files where you got them '
                                   'stay. Apply afterwards to take it out of the game.',
    'gui.textures.files': '{files} files, {size} GB.',
    'gui.textures.invalid': 'Not valid for this app ({reason}): it cannot be switched on.',
    'gui.textures.not_applied': 'Not in the game yet: Apply.',
    'gui.textures.build_on': 'Build the app again (Build tab): the pack needs the bigger memory heaps it brings.',
    'gui.textures.build_off': 'The app still has the pack\'s bigger memory heaps: Play keeps the 8 GB mode; build '
                              'again to drop them.',
    'gui.textures.ready': 'The app has the pack\'s memory heaps: Play starts the game in the 8 GB mode.',
    'gui.textures.adding': 'Adding the pack (a big pack takes minutes: it is extracted, copied and checked)...',
    'gui.textures.added': 'Pack added and switched on. Apply, then build the app again.',
}
