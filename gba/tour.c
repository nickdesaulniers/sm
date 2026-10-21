// For testing (-DSM_ROOM_TOUR=ON): takes Samus through a door into each
// room of the game in turn, to compare what the GBA shows of each with what
// the SNES would (tools/room-tour.sh).
//
// The rooms are those the doors lead to, from the landing site's on:
// each room's door list, in bank $8F, points to door definitions in bank
// $83, each with the room it leads to. Once Samus has stood in a room for a
// while, the next goes through its door as if she had walked into it.
#include <stdio.h>

#include "ida_types.h"
#include "funcs.h"
#include "sm_rtl.h"
#include "tour.h"
#include "variables.h"

enum {
  kMaxRooms = 256,  // the doors lead to 254
#ifdef SM_TOUR_FIRST
  // Where Samus starts, as -DSM_TOUR_FIRST=0xdf45 for Ceres's elevator
  // room (a build that starts the game there, -DSM_SKIP_INTRO=ON): Ceres's
  // rooms aren't through Zebes's doors. With -DSM_TOUR_ONLY=0xe0b5, for
  // Ridley's fight.
  kFirstRoom = SM_TOUR_FIRST,
#else
  kFirstRoom = 0x91f8,  // the landing site
#endif
  kLastRoom = 0xe0b5,   // Ceres's last: not the debug room after it
};
// Frames in each room before the next door, at least (more, to stay in
// each).
#ifndef SM_TOUR_SETTLE
#define SM_TOUR_SETTLE 90
#endif
enum { kSettleFrames = SM_TOUR_SETTLE };
// Each room's speed: the vblanks of kSpeedFrames frames, once the camera
// has stood still for kStillFrames (it scrolls to Samus first, for seconds
// and with pauses), or after kMaxStillWait.
enum { kSpeedFrames = 60, kStillFrames = 60, kMaxStillWait = 300 };
extern volatile uint32_t g_vblank_count;

static uint16_t tour_rooms[kMaxRooms], tour_doors[kMaxRooms];
static int tour_count, tour_next, tour_frames;
static int still_frames, speed_from;  // speed_from: the frame it's from, or -1
static uint16_t camera_x, camera_y;
static uint32_t speed_vblanks;
static bool tour_shot;
// Where Samus came into the room: she's held there, or she can fall back
// through a door in the floor, and on and on.
static uint16_t pin_x, pin_y;
static bool tour_door;

// ROM's words at odd addresses, which the ARM can't load whole.
static uint16_t Word(const uint8_t *p) {
  return p[0] | p[1] << 8;
}

static bool IsRoom(uint16_t room) {
  return room >= kFirstRoom && room <= kLastRoom && get_RoomDefHeader(room)->area_index_ <= 6;
}

// Each room, by the first door found to lead to it, breadth first.
static void FindRooms(void) {
  tour_rooms[tour_count++] = kFirstRoom;
  for (int i = 0; i < tour_count && tour_count < kMaxRooms; i++) {
    uint16_t list = Word((const uint8_t *)&get_RoomDefHeader(tour_rooms[i])->ptr_to_doorout);
    // The list has no end marker: it ends at what isn't a door. (A door
    // to room 0 is where an elevator's platform is.)
    for (;; list += 2) {
      uint16_t door = Word(RomPtr_8F(list));
      if (door < 0x8000)
        break;
      uint16_t room = Word((const uint8_t *)&get_DoorDef(door)->room_definition_ptr);
      if (room == 0)
        continue;
      if (!IsRoom(room))
        break;
      int j = 0;
      while (j < tour_count && tour_rooms[j] != room)
        j++;
      if (j == tour_count && tour_count < kMaxRooms) {
        tour_rooms[tour_count] = room;
        tour_doors[tour_count++] = door;
      }
    }
  }
#ifdef SM_TOUR_ONLY
  // Just these rooms (and the landing site, where Samus starts), as
  // -DSM_TOUR_ONLY=0xb3a5,0xd913: to measure them.
  static const uint16_t kOnly[] = {SM_TOUR_ONLY};
  int kept = 1;
  for (int i = 1; i < tour_count; i++) {
    for (unsigned j = 0; j < sizeof(kOnly) / sizeof(*kOnly); j++) {
      if (tour_rooms[i] == kOnly[j]) {
        tour_rooms[kept] = tour_rooms[i];
        tour_doors[kept++] = tour_doors[i];
      }
    }
  }
  tour_count = kept;
#endif
  printf("tour: %d rooms\n", tour_count);
}

bool Tour_BeforeFrame(void) {
  if (!tour_count) {
    if (game_state != kGameState_8_MainGameplay)
      return false;
    FindRooms();
    tour_next = 1;  // past the landing site, where Samus is
  }
  // Samus can't die on the way.
  samus_health = samus_max_health = 1499;
  if (game_state == kGameState_9_HitDoorBlock && !tour_door)
    game_state = kGameState_8_MainGameplay;  // a door she touched: not the tour's
  if (game_state != kGameState_8_MainGameplay) {
    tour_frames = 0;
    tour_shot = false;
    return false;
  }
  tour_door = false;
  if (tour_frames == 0) {
    pin_x = samus_x_pos, pin_y = samus_y_pos;
    still_frames = 0;
    speed_from = -1;
  }
  samus_x_pos = pin_x, samus_y_pos = pin_y;
  still_frames = layer1_x_pos == camera_x && layer1_y_pos == camera_y ? still_frames + 1 : 0;
  camera_x = layer1_x_pos, camera_y = layer1_y_pos;
  if (speed_from < 0 && (still_frames >= kStillFrames || tour_frames >= kMaxStillWait)) {
    speed_from = tour_frames;
    speed_vblanks = g_vblank_count;
  } else if (speed_from >= 0 && tour_frames - speed_from == kSpeedFrames) {
    printf("tour: speed %04x %u\n", room_ptr, (unsigned)(g_vblank_count - speed_vblanks));
  }
  if (!tour_shot) {
    // The shot: of the frame before the next door, settled.
    tour_shot = ++tour_frames >= kSettleFrames - 1 && speed_from >= 0 &&
                tour_frames - speed_from > kSpeedFrames && tour_next <= tour_count;
    return false;
  }
  // Time for the next room's door.
  tour_shot = false;
  tour_frames = 0;
  if (tour_next >= tour_count) {
    if (tour_next++ == tour_count)
      printf("tour: done\n");
    return false;
  }
  door_def_ptr = tour_doors[tour_next];
  door_transition_function = (uint16_t)fnDoorTransitionFunction_HandleElevator;
  game_state = kGameState_9_HitDoorBlock;
  tour_door = true;
  printf("tour: to %04x by door %04x\n", tour_rooms[tour_next++], door_def_ptr);
  return false;
}

bool Tour_AfterFrame(void) {
  if (game_state == kGameState_8_MainGameplay && tour_shot) {
    printf("tour: shot %04x\n", room_ptr);
    return true;
  }
  return false;
}
