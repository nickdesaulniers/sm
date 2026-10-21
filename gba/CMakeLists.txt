# The GBA cartridge: sm.elf, then sm.gba via objcopy.

# The C library functions the compiler and the game expect (see libc/).
add_library(gba_libc STATIC
    libc/stdio.c
    libc/stdlib.c
    libc/string.c
)
# -fno-builtin keeps clang from turning these implementations into calls
# to themselves (they also carry no_builtin).
target_compile_options(gba_libc PRIVATE -fno-builtin)
set_source_files_properties(libc/string.c PROPERTIES COMPILE_OPTIONS "-marm")

# compiler-rt builtins for operations ARMv4T has no instructions for:
# division, 64-bit multiply, shifts and division.
sm_add_builtins(gba_builtins
    muldi3.c                # __aeabi_lmul
    udivsi3.c               # __aeabi_uidiv
    udivmodsi4.c
    arm/aeabi_uidivmod.S    # __aeabi_uidivmod
    divsi3.c                # __aeabi_idiv
    divmodsi4.c
    arm/aeabi_idivmod.S     # __aeabi_idivmod
    udivmoddi4.c
    arm/aeabi_uldivmod.S    # __aeabi_uldivmod
    divmoddi4.c
    arm/aeabi_ldivmod.S     # __aeabi_ldivmod
    ashldi3.c               # __aeabi_llsl
    lshrdi3.c               # __aeabi_llsr
    ashrdi3.c               # __aeabi_lasr
    clzsi2.c                # __clzsi2
    ctzsi2.c                # __ctzsi2
)

# The game itself, as decompiled: everything but the PC's platform layer and
# the SNES emulator it checks the game against.
set(game_dir ${CMAKE_SOURCE_DIR}/src)
add_library(sm_game STATIC
    ${game_dir}/sm_80.c
    ${game_dir}/sm_81.c
    ${game_dir}/sm_82.c
    ${game_dir}/sm_84.c
    ${game_dir}/sm_85.c
    ${game_dir}/sm_86.c
    ${game_dir}/sm_87.c
    ${game_dir}/sm_88.c
    ${game_dir}/sm_89.c
    ${game_dir}/sm_8b.c
    ${game_dir}/sm_8d.c
    ${game_dir}/sm_8f.c
    ${game_dir}/sm_90.c
    ${game_dir}/sm_91.c
    ${game_dir}/sm_92.c
    ${game_dir}/sm_93.c
    ${game_dir}/sm_94.c
    ${game_dir}/sm_9b.c
    ${game_dir}/sm_a0.c
    ${game_dir}/sm_a2.c
    ${game_dir}/sm_a3.c
    ${game_dir}/sm_a4.c
    ${game_dir}/sm_a5.c
    ${game_dir}/sm_a6.c
    ${game_dir}/sm_a7.c
    ${game_dir}/sm_a8.c
    ${game_dir}/sm_a9.c
    ${game_dir}/sm_aa.c
    ${game_dir}/sm_ad.c
    ${game_dir}/sm_b2.c
    ${game_dir}/sm_b3.c
    ${game_dir}/sm_b4.c
    ${game_dir}/spc_player.c
)
# The decompiled code is warning-heavy; keep the build log readable.
target_compile_options(sm_game PRIVATE -w)
target_include_directories(sm_game PUBLIC ${game_dir})

add_executable(sm_gba
    crt0.S
    header.c
    audio.c
    audio_mix.iwram.c
    audio_mix.S
    irq_entry.S
    input.c
    main.c
    tour.c
    rtl.c
    snes_hw.iwram.c
    render.iwram.c
    convert.iwram.c
    irq.iwram.c
)
set_target_properties(sm_gba PROPERTIES
    OUTPUT_NAME sm
    SUFFIX .elf
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gba.ld
)
option(SM_SKIP_INTRO "For testing: skip a new game's intro, to Ceres" OFF)
option(SM_AUDIO_LOG "For tools/spcref.c: log what the game asks of the sound driver" OFF)
if(SM_AUDIO_LOG)
    target_compile_definitions(sm_gba PRIVATE SM_AUDIO_LOG=1)
endif()
option(SM_START_ZEBES "With SM_SKIP_INTRO: start on Zebes, as after escaping Ceres" OFF)
if(SM_SKIP_INTRO)
    target_compile_definitions(sm_gba PRIVATE SM_SKIP_INTRO=1)
endif()
if(SM_START_ZEBES)
    target_compile_definitions(sm_gba PRIVATE SM_START_ZEBES=1)
endif()
option(SM_TRACE "For testing against the PC build (tools/hostrun.c): log the joypad and the game's state each frame" OFF)
if(SM_TRACE)
    target_compile_definitions(sm_gba PRIVATE SM_TRACE=1)
endif()
option(SM_ROOM_TOUR "For testing (tools/room-tour.sh): take Samus into each room in turn" OFF)
if(SM_ROOM_TOUR)
    target_compile_definitions(sm_gba PRIVATE SM_ROOM_TOUR=1)
endif()
option(SM_TEST_ITEMS "For testing: give Samus the morph ball, power bombs, grapple beam and X-ray scope" OFF)
if(SM_TEST_ITEMS)
    target_compile_definitions(sm_gba PRIVATE SM_TEST_ITEMS=1)
endif()

target_compile_options(sm_gba PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas)

# Hot code, linked into IWRAM by gba.ld. ARM rather than Thumb: IWRAM's
# 32-bit bus fetches an ARM instruction as fast as a Thumb one, and ARM
# does more per instruction.
get_target_property(gba_sources sm_gba SOURCES)
list(FILTER gba_sources INCLUDE REGEX "\\.iwram\\.c$")
set_source_files_properties(${gba_sources} PROPERTIES COMPILE_OPTIONS "-marm")

target_link_libraries(sm_gba PRIVATE sm_game gba_libc gba_builtins)
target_link_options(sm_gba PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/gba.ld
    -Wl,--gc-sections
)

# The engine, sm-engine.gba, has none of the game's data: make_cartridge.py
# appends it from the ROM, to make sm.gba (see cart_data.h).
add_custom_command(TARGET sm_gba POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:sm_gba> ${CMAKE_BINARY_DIR}/sm-engine.gba
    BYPRODUCTS ${CMAKE_BINARY_DIR}/sm-engine.gba
    COMMENT "Creating sm-engine.gba"
)
if(EXISTS "${SM_ROM}")
    add_custom_command(TARGET sm_gba POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/make_cartridge.py
                ${CMAKE_BINARY_DIR}/sm-engine.gba ${SM_ROM} ${CMAKE_BINARY_DIR}/sm.gba
        BYPRODUCTS ${CMAKE_BINARY_DIR}/sm.gba
        COMMENT "Creating sm.gba from ${SM_ROM}"
    )
endif()
