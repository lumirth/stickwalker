set(POKEWALKER_UPSTREAM "${CMAKE_CURRENT_LIST_DIR}/upstream")
set(POKEWALKER_PORT "${CMAKE_CURRENT_LIST_DIR}/port")
set(POKEWALKER_ARTWORK "${CMAKE_CURRENT_BINARY_DIR}/artwork")
set(SW_RESIDENT_ASSETS_HEADER "" CACHE FILEPATH
    "Resident artwork header extracted from an original Pokewalker ROM")

if(NOT CMAKE_SCRIPT_MODE_FILE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${POKEWALKER_PORT}/check_upstream.py"
    COMMAND_ERROR_IS_FATAL ANY)
  if(SW_RESIDENT_ASSETS_HEADER)
    configure_file("${SW_RESIDENT_ASSETS_HEADER}" "${POKEWALKER_ARTWORK}/rom_assets.h" COPYONLY)
  else()
    configure_file("${POKEWALKER_PORT}/include/rom_assets.h" "${POKEWALKER_ARTWORK}/rom_assets.h" COPYONLY)
  endif()
  file(GLOB_RECURSE upstream_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/upstream/*")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${upstream_inputs}
    "${POKEWALKER_PORT}/check_upstream.py"
    "${CMAKE_CURRENT_LIST_DIR}/upstream.sha256")
endif()
set(POKEWALKER_SOURCES
  ${POKEWALKER_PORT}/drivers/boot.c
  ${POKEWALKER_PORT}/overrides/pw_rtc.c
  ${POKEWALKER_PORT}/overrides/h8_resetprg.c
  ${POKEWALKER_PORT}/overrides/pw_power.c
  ${POKEWALKER_PORT}/overrides/pw_main.c
  ${POKEWALKER_PORT}/overrides/lib_common.c
  ${POKEWALKER_PORT}/overrides/pw_pokeradar.c
  ${POKEWALKER_PORT}/overrides/pw_dowsing.c
  ${POKEWALKER_PORT}/overrides/pw_friend.c
  ${POKEWALKER_PORT}/overrides/pw_pack.c
  ${POKEWALKER_PORT}/overrides/pw_pedometer.c
  ${POKEWALKER_PORT}/eeprom.c
  ${POKEWALKER_PORT}/lifecycle.c
  ${POKEWALKER_PORT}/drivers/pw_battery.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_battle.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_builtin.c
  ${POKEWALKER_PORT}/drivers/pw_buzzer.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_carry_overflow.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_cutscenes.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_diary.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_dowsing.c
  ${POKEWALKER_PORT}/drivers/pw_eeprom_m95512_io.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_follower_events.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_follower_prompts.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_fourier.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_friend.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_home.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_local_settings.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_main.c
  ${POKEWALKER_PORT}/drivers/pw_nt7508.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_pack.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_pedometer.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_pictogram_menu.c
  ${POKEWALKER_PORT}/drivers/pw_player_input.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_pokeradar.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_power.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_rtc.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_storage.c
  ${POKEWALKER_UPSTREAM}/src/application/pw_trainer.c
  ${POKEWALKER_UPSTREAM}/src/globals.c
  ${POKEWALKER_UPSTREAM}/src/startup/h8_resetprg.c
  ${POKEWALKER_UPSTREAM}/src/startup/h8_rominfo.c
  ${POKEWALKER_PORT}/drivers/ir.c
  ${POKEWALKER_UPSTREAM}/src/support/lib_clear.c
  ${POKEWALKER_UPSTREAM}/src/support/lib_common.c
)
set(POKEWALKER_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/include")
set(POKEWALKER_PRIVATE_INCLUDE_DIRS "${POKEWALKER_ARTWORK}"
  "${POKEWALKER_PORT}/include" "${POKEWALKER_UPSTREAM}/include" "${POKEWALKER_PORT}")
function(pokewalker_configure_target target)
  target_include_directories(${target} PUBLIC "${POKEWALKER_PUBLIC_INCLUDE_DIR}"
    PRIVATE ${POKEWALKER_PRIVATE_INCLUDE_DIRS})
  target_compile_features(${target} PUBLIC c_std_11)
  target_compile_options(${target} PRIVATE -fno-strict-aliasing -fwrapv -fpack-struct=2
    -Wno-unknown-pragmas -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
    "SHELL:-include \"${POKEWALKER_PORT}/target.h\"")

  # Only the upstream definitions are weak. The ordinary port sources above
  # provide strong aliases, with each implementation required at final link.
  set_property(SOURCE
    ${POKEWALKER_UPSTREAM}/src/application/pw_rtc.c
    ${POKEWALKER_UPSTREAM}/src/startup/h8_resetprg.c
    ${POKEWALKER_UPSTREAM}/src/application/pw_main.c
    ${POKEWALKER_UPSTREAM}/src/support/lib_common.c
    ${POKEWALKER_UPSTREAM}/src/application/pw_power.c
    ${POKEWALKER_UPSTREAM}/src/application/pw_pokeradar.c
    ${POKEWALKER_UPSTREAM}/src/application/pw_dowsing.c
    ${POKEWALKER_UPSTREAM}/src/application/pw_friend.c
    ${POKEWALKER_UPSTREAM}/src/application/pw_pack.c
    ${POKEWALKER_UPSTREAM}/src/application/pw_pedometer.c
    APPEND PROPERTY COMPILE_OPTIONS "-include" "${POKEWALKER_PORT}/upstream_bindings.h")
  # The unused H8 estimator assigns a const table to a mutable cursor.
  set_property(SOURCE ${POKEWALKER_UPSTREAM}/src/application/pw_pedometer.c
    APPEND PROPERTY COMPILE_OPTIONS -Wno-discarded-qualifiers)
  target_link_options(${target} INTERFACE
    "LINKER:--require-defined=pw_port_RadarChooseEncounter"
    "LINKER:--require-defined=pw_port_DowsingAwardBonus"
    "LINKER:--require-defined=pw_port_DowsingChooseReward"
    "LINKER:--require-defined=pw_port_PeerAwardGift"
    "LINKER:--require-defined=pw_port_InventoryMasks"
    "LINKER:--require-defined=pw_port_MotionEstimateQ9"
    "LINKER:--require-defined=pw_port_ScratchReset"
    "LINKER:--require-defined=pw_port_ScratchAlloc"
    "LINKER:--require-defined=pw_port_BulkDecode"
    "LINKER:--require-defined=pw_port_MainTick"
    "LINKER:--require-defined=pw_port_BeepTick"
    "LINKER:--require-defined=pw_port_MotionSessionWake"
    "LINKER:--require-defined=pw_port_MotionSessionStart"
    "LINKER:--require-defined=pw_port_MotionSessionEnd"
    "LINKER:--require-defined=pw_port_IRQ0Interrupt"
    "LINKER:--require-defined=pw_port_PowerOnReset"
    "LINKER:--require-defined=pw_port_CommitStagedWalk"
    "LINKER:--require-defined=pw_port_RtcSetTime"
    "LINKER:--require-defined=pw_port_RtcReadStable")
endfunction()
