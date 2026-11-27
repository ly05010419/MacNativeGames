/* Host shortcut, before keyboard-device routing; Command alone is never a shortcut.
   Accept the comma key in non-Latin input methods as well. */
static inline BOOL d3_input_settings_shortcut(NSEvent *event) {
    if (event.type != NSEventTypeKeyDown) return NO;
    NSEventModifierFlags modifiers = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagControl |
         NSEventModifierFlagOption | NSEventModifierFlagShift);
    return modifiers == NSEventModifierFlagCommand &&
        (event.keyCode == 43 || [event.charactersIgnoringModifiers isEqualToString:@","]);
}
