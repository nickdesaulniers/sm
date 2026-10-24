// For testing (-DSM_SCENARIOS=ON, tools/scenarios.sh): plays a list of
// scenarios one after another without a player, to check each one's
// picture, speed and sound. Each spawns Samus with the developer tools
// (dev_tools.c: a load station, or a boss's door, with a loadout and a set
// of beams), then presses buttons by a script, frame by frame, printing
// "tour: shot NAME-N" at the script's shots, for smrun's SHOT_ON (as
// tour.c's do), and "scenario: NAME" as each starts. The game's own
// "sm: 60 frames in N vblanks" lines give the speed (not counting a shot's
// hold). After a script the game is reset, back to the title, for the
// next scenario.
//
// Which scenarios run is read from the buttons smrun holds as the cartridge
// starts, KEYS=1:100:FIRST,COUNT (as of the game's frames 20 and 130): the ten
// buttons the bits of a number (A is 1, B 2, Select 4, and so on to L 512):
// the scenario to start with, and how many (0: to the list's end), with
// COUNT's bit 9 (L) for no holds on shots (to record the sound). With
// -DSM_SCENARIO_ONLY=name1,name2 the list is only those, by name (a
// trailing * for a prefix).
//
// A script is words, separated by spaces:
//   BUTTONS*N  holds the buttons, joined by '+' (A B SELECT START RIGHT LEFT
//              UP DOWN R L, or '-' for none), for N frames
//   @          a shot of the frame the next word begins
//   ( ... )xN  the words between, N times
//   >X,Y       Samus to that place (pixels)
//   ~N         the lava, acid or water's surface to N pixels below Samus's
//              centre (-N for above), at its base: for a room's lava where she
//              can't go (the camera follows her only so far)
// The buttons are the GBA's (input.c makes the game's of them): B shoots, R
// readies missiles (with Select, the others), L aims. Samus can't die, and
// never runs out of ammunition.
#include "scenarios.h"

#include <stdio.h>
#include <string.h>

#include "dev_tools.h"
#include "funcs.h"
#include "gba_hw.h"
#include "ida_types.h"
#include "input.h"
#include "sm_rtl.h"
#include "variables.h"

typedef struct Scenario {
  const char *name;
  // The load station of the area (0 Crateria, 1 Brinstar, 2 Norfair, 3
  // Wrecked Ship, 4 Maridia, 5 Tourian), as the map numbers them (16 on
  // are the debug stations); or, with |boss| from 1, the area's boss door
  // of that number (dev_tools.c's, in the map's order).
  uint8_t area, station, boss;
  // The loadout is ALL ITEMS; with these beams (the game's bits) instead, if
  // not -1.
  int16_t beams;
  const char *script;
} Scenario;

// The beam sets (wave 1, ice 2, spazer 4, plasma 8) in the game's bits,
// without the charge beam (0x1000) and with.
#define BEAMS(n) (n)
#define CHARGED(n) (0x1000 + (n))

// ---- The weapons, in the room east of the landing site (Crateria's debug
// station 17, 94FD): Samus stands on a ledge, 90 pixels from a door that she
// faces, with a block ramp under her and water below it. Shots hit the door
// and the blocks.
#define kRoom 0, 17, 0
// Facing the door (a tap on Left turns her), shots in the air and at the door,
// the aimed ones, and a charged one: held, let go, in flight, at the door.
#define kBeamScript \
  "-*20 LEFT*4 -*30 @ B*2 -*4 @ -*4 @ -*12 @ (B*2 -*8)x3 @ -*40 @ " \
  "L+DOWN+B*3 -*5 @ -*10 @ UP+B*3 -*5 @ -*20 @ " \
  "B*130 @ -*1 -*5 @ -*8 @ -*30 @"

#define BEAM(n, name) \
  {"beam-" name, kRoom, BEAMS(n), kBeamScript}, \
  {"beam-" name "-charge", kRoom, CHARGED(n), kBeamScript}

// Samus faces the door; R readies missiles (then, with Select, the next of
// supers, the grapple beam and the X-ray scope).
#define kFace "-*20 LEFT*4 -*30 @ "
#define kSelectSupers "R*2 R+SELECT*2 R*2 "
#define kSelectGrapple kSelectSupers kSelectSupers
#define kSelectXray kSelectGrapple kSelectSupers
// Down twice: crouch, then the morph ball.
#define kMorph "DOWN*10 -*6 DOWN*10 -*20 "

// ---- The bosses: each with the ALL ITEMS loadout, and a script that fights
// for 70 seconds, a shot every 10: shots, jumps, charged shots, missiles, and
// then super missiles.
#define kFight \
  "(RIGHT*30 B*3 -*5 A+RIGHT*25 L+B*3 -*10 LEFT*30 B*100 -*2 R+B*3 -*8 " \
  "A+LEFT*25 B*3 -*5 UP+B*3 -*10)x2"
#define kBossScript \
  "-*30 @ (" kFight " -*70 @)x3 " kSelectSupers "(" kFight " -*70 @)x3"

#define BOSS(name, area, n) {"boss-" name, area, 0, n, -1, kBossScript}

// The lava (or acid) first at Samus's feet and then rising past her, from a
// station of a room that has it.
#define kLavaScript "-*30 @ ~40 -*60 @ ~20 -*60 @ ~0 -*60 @ ~-30 -*60 @"

static const Scenario kScenarios[] = {
  BEAM(0, "power"),
  BEAM(1, "wave"),
  BEAM(2, "ice"),
  BEAM(3, "ice-wave"),
  BEAM(4, "spazer"),
  BEAM(5, "spazer-wave"),
  BEAM(6, "spazer-ice"),
  BEAM(7, "spazer-ice-wave"),
  BEAM(8, "plasma"),
  BEAM(9, "plasma-wave"),
  BEAM(10, "plasma-ice"),
  BEAM(11, "plasma-ice-wave"),
  {"missile", kRoom, BEAMS(0),
   kFace "R*10 @ R+B*2 -*5 @ -*10 @ (R+B*2 -*25)x3 @ -*40 @"},
  {"super", kRoom, BEAMS(0),
   kFace kSelectSupers "R*10 @ R+B*2 -*5 @ -*10 @ (R+B*2 -*25)x3 @ -*40 @"},
  {"bomb", kRoom, BEAMS(0),
   kFace kMorph "@ B*2 -*30 @ -*8 @ -*8 @ (B*2 -*12)x3 -*20 @ -*10 @ -*60 @"},
  {"power-bomb", kRoom, BEAMS(0),
   kFace kMorph "R*10 @ R+B*2 -*10 @ -*20 @ -*30 @ -*40 @ -*60 @"},
  {"xray", kRoom, BEAMS(0),
   kFace kSelectXray "R*30 @ R*60 @ -*30 @"},
  {"grapple", 4, 17, 0, BEAMS(0),
   "-*20 >136,140 -*200 LEFT*4 -*60 @ " kSelectGrapple "R+UP+B*20 @ R+UP+B*40 @ R+UP+B*40 @ -*30 @"},
  {"screw-attack", kRoom, BEAMS(0),
   kFace "A+LEFT*30 @ A*30 @ A*30 @ -*60 @"},
  {"space-jump", kRoom, BEAMS(0),
   kFace "A+LEFT*30 -*4 (A*4 -*6)x12 @ A+RIGHT*30 @ -*60 @"},
  // Running right from the Wrecked Ship's debug station 16 (CA08), on 60
  // blocks of flat floor: the speed booster builds up in about 3 seconds.
  // Then a shinespark: crouch at speed, jump, aim.
  {"speed-booster", 3, 16, 0, BEAMS(0),
   "-*30 @ RIGHT*60 @ RIGHT*60 @ RIGHT*60 @ RIGHT*60 @ RIGHT*40 @ -*30 @"},
  {"shinespark", 3, 16, 0, BEAMS(0),
   "-*30 RIGHT*140 @ DOWN*4 -*20 @ A*4 -*4 RIGHT*3 -*10 @ -*20 @ -*20 @ -*40 @ -*60 @"},
  // ---- Norfair's lava and acid, in the rooms of its debug stations.
  {"lava-aab5", 2, 0, 0, "ALL ITEMS", 0, kLavaScript},
  {"lava-b0dd", 2, 1, 0, "ALL ITEMS", 0, kLavaScript},
  {"lava-b741", 2, 5, 0, "ALL ITEMS", 0, kLavaScript},
  {"lava-a7de", 2, 8, 0, "ALL ITEMS", 0, kLavaScript},
  {"lava-af3f", 2, 9, 0, "ALL ITEMS", 0, kLavaScript},
  {"lava-b236", 2, 10, 0, "ALL ITEMS", 0, kLavaScript},
  {"lava-a923", 2, 17, 0, "ALL ITEMS", 0, kLavaScript},
  {"lava-b37a", 2, 18, 0, "ALL ITEMS", 0, kLavaScript},
  {"lava-b283", 2, 21, 0, "ALL ITEMS", 0, kLavaScript},
  {"lava-b283-2", 2, 22, 0, "ALL ITEMS", 0, kLavaScript},
  BOSS("bomb-torizo", 0, 1),
  BOSS("spore-spawn", 1, 1),
  BOSS("kraid", 1, 2),
  BOSS("crocomire", 2, 1),
  BOSS("ridley", 2, 2),
  BOSS("phantoon", 3, 1),
  BOSS("botwoon", 4, 1),
  BOSS("draygon", 4, 2),
  BOSS("mother-brain", 5, 1),
  // The Golden Torizo's room, at its left door (Norfair's debug station 22).
  {"boss-golden-torizo", 2, 22, 0, -1, kBossScript},
};
enum { kScenarioCount = sizeof(kScenarios) / sizeof(kScenarios[0]) };

// What smrun's KEYS held as the cartridge started chose.
static uint32_t calls;
static uint16_t first_scenario, scenario_count;
static bool silent;
static uint8_t order[kScenarioCount];
static int order_count, current;

static enum {
  kBooting,
  kMenus,    // the title to the room map and the spawn
  kLoading,  // until Samus has her controls
  kScript,
  kFinished,
} phase;
static uint32_t phase_frames;
static bool shot_pending, reset_pending, spawned;
static int shot_count;

// The script's place.
static const char *script;
static struct { const char *start; int left; } loops[4];
static int loop_depth;
static uint16_t held_keys;
static int held_left;

static const char *const kKeyNames[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN",
                                        "R", "L"};
// The planet map's order of areas.
static const uint8_t kAreaOrder[6] = {0, 3, 5, 1, 4, 2};

// Whether |a| and |b| begin with the same |n| characters.
static bool Same(const char *a, const char *b, int n) {
  for (int i = 0; i < n; i++)
    if (a[i] != b[i])
      return false;
  return true;
}

static bool Picked(const char *name) {
#ifdef SM_SCENARIO_ONLY
  const char *list = SM_SCENARIO_ONLY;
  for (;;) {
    int len = 0;
    while (list[len] && list[len] != ',')
      len++;
    if (len && list[len - 1] == '*' ? Same(list, name, len - 1)
                                    : Same(list, name, len) && !name[len])
      return true;
    if (!list[len])
      return false;
    list += len + 1;
  }
#else
  return true;
#endif
}

static void Choose(void) {
  for (int i = 0; i < kScenarioCount; i++)
    if (Picked(kScenarios[i].name))
      order[order_count++] = i;
  // (For tools/scenarios.sh, to divide them.)
  for (int i = 0; i < order_count; i++)
    printf("scenario-list: %d %s\n", i, kScenarios[order[i]].name);
  int start = first_scenario < order_count ? first_scenario : order_count;
  int n = scenario_count ? scenario_count : order_count;
  if (start + n > order_count)
    n = order_count - start;
  memmove(order, order + start, n);
  order_count = n;
  printf("scenario: %d to run, of %d\n", order_count, kScenarioCount);
  phase = order_count ? kMenus : kFinished;
  if (!order_count)
    printf("scenario: all done\n");
}

static const Scenario *Current(void) {
  return &kScenarios[order[current]];
}

// Whether to press a button now: for a few frames, every so often.
static bool Pulse(int period) {
  return phase_frames % period < 4;
}

// From the title to the spawn, by the buttons a player would press.
static uint16_t Menus(void) {
  const Scenario *s = Current();
  switch (game_state) {
  case 1: case 2: case 4:
    // The title, the options and the file select: the tools on (L and R
    // with Start), then Start on, to a new game on the first file.
    if (!Pulse(40))
      return 0;
    return enable_debug ? kKeyStart : kKeyL | kKeyR | kKeyStart;
  case 5:
    if (menu_index == 6) {
      // The planet: to the area.
      if (!Pulse(12))
        return 0;
      return kAreaOrder[file_select_map_area_index] == s->area ? kKeyA : kKeyRight;
    }
    if (menu_index == 10) {
      // The area's map: the station, the loadout and the beams.
      if (!s->boss)
        load_station_index = s->station;
      Dev_Pick(s->beams, s->boss);
      if (phase_frames % 12 >= 4 || phase_frames < 30)
        return 0;
      spawned = true;
      return kKeyA;
    }
    return 0;
  case 6: case 7: case 8: case 9:
    // (The map's A has started the load: not the title's demo.)
    if (spawned) {
      phase = kLoading;
      phase_frames = 0;
      spawned = false;
    }
    return 0;
  default:
    return 0;
  }
}

static int Number(const char **p) {
  int n = 0;
  while (**p >= '0' && **p <= '9')
    n = n * 10 + *(*p)++ - '0';
  return n;
}

// The next word of the script that holds buttons, or false at its end.
static bool NextWord(void) {
  for (;;) {
    while (*script == ' ')
      script++;
    switch (*script) {
    case 0:
      return false;
    case '@':
      script++;
      shot_pending = true;
      continue;
    case '>':
      // Samus to X,Y (pixels): for a place no station is near.
      script++;
      samus_x_pos = Number(&script);
      script++;
      samus_y_pos = Number(&script);
      samus_x_subpos = samus_y_subpos = 0;
      continue;
    case '~': {
      script++;
      bool above = *script == '-';
      script += above;
      int n = Number(&script);
      fx_base_y_pos = samus_y_pos + (above ? -n : n);
      continue;
    }
    case '(':
      script++;
      loops[loop_depth].start = script;
      loops[loop_depth++].left = 0;
      continue;
    case ')':
      script += 2;  // the x
      if (!loops[loop_depth - 1].left)
        loops[loop_depth - 1].left = Number(&script);
      else
        Number(&script);
      if (--loops[loop_depth - 1].left)
        script = loops[loop_depth - 1].start;
      else
        loop_depth--;
      continue;
    }
    held_keys = 0;
    while (*script && *script != '*') {
      int len = 0;
      while (script[len] && script[len] != '+' && script[len] != '*')
        len++;
      for (int i = 0; i < 10; i++)
        if (Same(script, kKeyNames[i], len) && !kKeyNames[i][len])
          held_keys |= 1 << i;
      script += len + (script[len] == '+');
    }
    script++;
    held_left = Number(&script);
    return true;
  }
}

static void Start(void) {
  const Scenario *s = Current();
  script = s->script;
  loop_depth = 0;
  held_left = 0;
  shot_count = 0;
  shot_pending = false;
  phase = kScript;
  phase_frames = 0;
  printf("scenario: %s\n", s->name);
  printf("scenario-at: room %04x samus %d,%d\n", room_ptr, samus_x_pos, samus_y_pos);
}

static void Finish(void) {
  printf("scenario: end %s\n", Current()->name);
  if (++current == order_count) {
    phase = kFinished;
    printf("scenario: all done\n");
  } else {
    // Back to the title, as a reset does.
    reset_pending = true;
    phase = kMenus;
    phase_frames = 0;
  }
}

uint16_t Scn_Keys(uint16_t real) {
  // (The buttons at the start choose: see the top.)
  calls++;
  if (calls == 20)
    first_scenario = real;
  if (calls == 130)
    scenario_count = real & 0x1ff, silent = real >> 9 & 1;
  if (calls <= 130)
    return 0;
  if (calls == 131)
    Choose();
  phase_frames++;
  switch (phase) {
  case kMenus:
    return Menus();
  case kLoading:
    // Samus's arrival is over, and through her boss's door, if she has one
    // (the game gives her the controls then).
    if (game_state == kGameState_8_MainGameplay &&
        frame_handler_beta == (uint16_t)fnSamus_FrameHandlerBeta_Func17 && !Dev_SpawnPending()) {
      if (phase_frames > 20)
        Start();
    } else {
      phase_frames = 0;
    }
    return 0;
  case kScript:
    if (!held_left && !NextWord()) {
      if (shot_pending) {
        // (The shot is of the frame after it.)
        held_keys = 0;
        held_left = 1;
      } else {
        Finish();
        return 0;
      }
    }
    held_left--;
    return held_keys;
  default:
    return 0;
  }
}

void Scn_BeforeFrame(void) {
  if (reset_pending) {
    // As the game's own reset: main.c goes on from there.
    reset_pending = false;
    game_state = 0xffff;
    Input_Reset();
  }
  if (phase == kLoading || phase == kScript) {
    samus_health = samus_max_health;
    samus_missiles = samus_max_missiles;
    samus_super_missiles = samus_max_super_missiles;
    samus_power_bombs = samus_max_power_bombs;
  }
}

bool Scn_AfterFrame(void) {
  if (!shot_pending || phase != kScript)
    return false;
  shot_pending = false;
  printf("tour: shot %s-%d\n", Current()->name, ++shot_count);
  return !silent;
}
