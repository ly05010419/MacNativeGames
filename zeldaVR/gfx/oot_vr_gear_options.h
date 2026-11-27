/* zeldaVR, Ocarina of Time: the VR page's options for the sword and the shield in the hands (vr.md rows 42, 44, 46, 47, 52, 53).
   Included by totk_vr.m (GAME_OOT), after g_oot_locked_on. Every one is a VR first-person feature. Defaults (merge 2026-11-27, the
   user's rule: nothing that changes how the game plays is on by default until it is verified in the game): everything that takes
   over a button or the game's own action is off, so the default is the play players have now; the author's choice is in brackets.
     sword_tilt     0   degrees the held weapon leans forward in the hand, about the controller's side axis (zeldaVR/runtime/oot_vr_gear.h).
                        HookshotVR's 35 is from the fist's axis; ours is from the game's own hold, so 0 is the hold players have now.
     shield_pose    0   0 along the forearm (as now and HookshotVR's default), 1 turned to the fist while guarding, 2 always so.
     draw_call      0   [1] the draw gesture asks the game's own draw and keeps Link drawn without his slash (games/oot/runtime/oot_vr_stance.h);
                        HookshotVR's Blade mode does so. 0: the gesture presses Y as before.
     blade_contact  0   [1] the blade's own contact with what is near (games/oot/runtime/oot_vr_touch.h), HookshotVR's Blade mode.
     jump_hits      0   [1] a hit while Link is in the air is the jump slash's (HookshotVR's design).
     guard_body     0   [1] an untargeted guard faces the way the stick walks Link (games/oot/runtime/oot_vr_guard.h). HookshotVR's option
                        903; its default is not in the checkout we read (only rx_option(903, 0) at its call sites). */
#pragma once

int vr_oot_sword_tilt(void) { const int v = vr_option("sword_tilt", 0); return v < -30 ? -30 : v > 60 ? 60 : v; }
void vr_oot_set_sword_tilt(int deg) { vr_option_set("sword_tilt", deg < -30 ? -30 : deg > 60 ? 60 : deg); VLOG("the sword's forward tilt in the hand: %d degrees", vr_oot_sword_tilt()); }
int vr_oot_shield_pose(void) { const int v = vr_option("shield_pose", 0); return v < 0 || v > 2 ? 0 : v; }
void vr_oot_set_shield_pose(int v) { vr_option_set("shield_pose", v < 0 || v > 2 ? 0 : v); VLOG("the shield: %s", v == 1 ? "turned to the fist while guarding" : v == 2 ? "always turned to the fist" : "along the forearm"); }
int vr_oot_draw_call(void) { const char *e = getenv("TOTK_VR_DRAW_CALL"); return e ? *e != '0' : vr_option("draw_call", 0) != 0; }
void vr_oot_set_draw_call(int on) { vr_option_set("draw_call", on != 0); VLOG("drawing the sword: %s", on ? "the game's own draw, no slash" : "the attack button"); }
int vr_oot_blade_contact(void) { const char *e = getenv("TOTK_VR_BLADE_CONTACT"); return e ? *e != '0' : vr_option("blade_contact", 0) != 0; }
void vr_oot_set_blade_contact(int on) { vr_option_set("blade_contact", on != 0); VLOG("the blade's own contact: %s", on ? "on" : "off"); }
int vr_oot_jump_hits(void) { return vr_option("jump_hits", 0) != 0; }
void vr_oot_set_jump_hits(int on) { vr_option_set("jump_hits", on != 0); VLOG("a hit in the air: %s", on ? "the jump slash's" : "as on the ground"); }
int vr_oot_guard_body(void) { return vr_option("guard_body", 0) != 0; }
void vr_oot_set_guard_body(int on) { vr_option_set("guard_body", on != 0); VLOG("an untargeted guard: %s", on ? "faces the way the stick walks" : "the game's own facing"); }
/* The game's ZL is held (lock-on or the shield), from the pad's last step: the shield's pose "while guarding" (row 52). */
int vr_oot_zl_held(void) { return atomic_load(&g_oot_locked_on); }
extern void runtime_oot_vr_sword_draw(void) __attribute__((weak_import));     /* games/oot/runtime/oot_vr_stance.h */
extern void runtime_oot_vr_sword_held(int held) __attribute__((weak_import));
