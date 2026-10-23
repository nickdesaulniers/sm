// The GBA's controls. The GBA has no X and Y, so in gameplay they work as
// in Metroid: Zero Mission and Fusion, mapped onto the game's default
// controls:
//   A       jump (SNES A)
//   B       shoot (SNES X)
//   R       hold to ready missiles, or the item Select picked; power bombs
//           in morph ball; hold to look with the X-ray scope
//   L       hold to aim diagonally: up, or down with Down held (SNES R, L)
//   Select  with R held: pick what R readies, the HUD showing it: missiles,
//           super missiles, grapple beam, X-ray scope, of those Samus has,
//           round and round. The pick is kept for the next time R is held.
//           Without R, Select does nothing
//   Start   pause
// and Samus always runs (SNES B held). Elsewhere, menus and the pause
// screen included, the buttons are the SNES's buttons of the same names.
//
// The game picks items with Select and Y; on the GBA, its item switching
// (HandleSwitchingHudSelection) asks GbaWantedHudItem instead.
#include "input.h"

#include <stdbool.h>

#include "dev_tools.h"
#include "gba_hw.h"
#include "ida_types.h"
#include "sm_rtl.h"
#include "variables.h"

// HUD items, as hud_item_index.
enum {
  kItemNone = 0,
  kItemMissiles = 1,
  kItemSuperMissiles = 2,
  kItemPowerBombs = 3,
  kItemGrapple = 4,
  kItemXray = 5,
};

static uint16_t keys;
// What Select picked for R, or none, for missiles.
static uint8_t r_item = kItemNone;

static bool HasItem(int item) {
  switch (item) {
  case kItemMissiles: return samus_max_missiles != 0;
  case kItemSuperMissiles: return samus_max_super_missiles != 0;
  case kItemPowerBombs: return samus_max_power_bombs != 0;
  case kItemGrapple: return (equipped_items & 0x4000) != 0;
  case kItemXray: return (equipped_items & 0x8000) != 0;
  default: return true;
  }
}

static void PickNextRItem(void) {
  static const uint8_t kRItems[] = {kItemNone, kItemSuperMissiles, kItemGrapple, kItemXray};
  int i = 0;
  while (i < 4 && kRItems[i] != r_item)
    i++;
  for (int n = 1; n <= 4; n++) {
    int item = kRItems[(i + n) % 4];
    if (HasItem(item)) {
      r_item = item;
      return;
    }
  }
}

static bool InMorphBall(void) {
  switch (samus_movement_type) {
  case kMovementType_04_MorphBallOnGround:
  case kMovementType_08_MorphBallFalling:
  case kMovementType_11_SpringBallOnGround:
  case kMovementType_12_SpringBallInAir:
  case kMovementType_13_SpringBallFalling:
    return true;
  default:
    return false;
  }
}

// The HUD item the game should have selected, once a frame.
uint16 GbaWantedHudItem(void) {
  // Letting go of R mustn't drop Samus from the grapple beam.
  if (samus_movement_type == kMovementType_16_Grappling && hud_item_index == kItemGrapple)
    return kItemGrapple;
  if (keys & kKeyR) {
    if (InMorphBall())
      return kItemPowerBombs;
    if (!HasItem(r_item))
      r_item = kItemNone;
    int item = r_item != kItemNone ? r_item : kItemMissiles;
    return HasItem(item) ? item : kItemNone;
  }
  return kItemNone;
}

uint16_t Input_ReadJoypad(void) {
  uint16_t prev = keys;
  keys = ~REG_KEYINPUT & 0x3ff;
  uint16_t pressed = keys & ~prev;
  // The developer tools (dev_tools.c): turned on by L and R held with Start,
  // on the title or the file select; they may use up a frame's buttons.
  if ((pressed & kKeyStart) && (keys & (kKeyL | kKeyR)) == (kKeyL | kKeyR))
    Dev_TryEnable();
  if (enable_debug && Dev_Frame(keys, pressed))
    return 0;
  uint16_t joy = 0;
  if (keys & kKeyUp) joy |= kSnesUp;
  if (keys & kKeyDown) joy |= kSnesDown;
  if (keys & kKeyLeft) joy |= kSnesLeft;
  if (keys & kKeyRight) joy |= kSnesRight;
  if (keys & kKeyStart) joy |= kSnesStart;
  if (keys & kKeyA) joy |= kSnesA;
  bool gameplay = game_state >= 7 && game_state <= 11;
  // Message boxes come up in gameplay, and are closed with any button, or,
  // saving, answered with A or B and switched with Select: as elsewhere,
  // their buttons are the SNES's of the same names, with nothing held for
  // running (which would close them as soon as a button could). B is the
  // SNES's X too, so it isn't new to gameplay, and doesn't shoot, once the
  // box has closed.
  bool message_box = gameplay && queued_message_box_index != 0;
  if (!gameplay || message_box) {
    if (keys & kKeyB) joy |= message_box ? kSnesB | kSnesX : kSnesB;
    if (keys & kKeySelect) joy |= kSnesSelect;
    if (keys & kKeyL) joy |= kSnesL;
    if (keys & kKeyR) joy |= kSnesR;
    return joy;
  }
  // The SNES's buttons as above do what they do by default: a save could
  // have others (the controller setting screen won't change them).
  button_config_shoot_x = kSnesX;
  button_config_jump_a = kSnesA;
  button_config_run_b = kSnesB;
  button_config_itemcancel_y = kSnesY;
  button_config_itemswitch = kSnesSelect;
  button_config_aim_up_R = kSnesR;
  button_config_aim_down_L = kSnesL;
  // The X-ray scope shines while the run button is held, with it selected:
  // so, while it's selected, as R holds it, R is the run button. Otherwise
  // Select picking it would turn it on, and letting go of R wouldn't turn it
  // off.
  if (hud_item_index != kItemXray || (keys & kKeyR))
    joy |= kSnesB;
  if (keys & kKeyB) joy |= kSnesX;
  if (keys & kKeyL) {
    if (keys & kKeyDown)
      joy = (joy & ~kSnesDown) | kSnesL;
    else
      joy = (joy & ~kSnesUp) | kSnesR;
  }
  // Only with R held, so the pick is what R is readying, not a state that
  // times out. Not while it's power bombs or the grapple beam holding Samus.
  if ((pressed & kKeySelect) && (keys & kKeyR) && !InMorphBall() &&
      samus_movement_type != kMovementType_16_Grappling)
    PickNextRItem();
  return joy;
}

// The SNES's second controller, which the game reads, with enable_debug set,
// to step through the load stations on the room map with its Select
// (FileSelectMap_10_RoomSelectMap): the GBA's Select. (For ReadReg.)
uint16_t Input_ReadJoypad2(void) {
  return game_state == 5 && (keys & kKeySelect) && Dev_SelectFree() ? kSnesSelect : 0;
}
