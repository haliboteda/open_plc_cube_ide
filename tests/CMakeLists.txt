# Host-side tests for the bootloader: the real IAPServer/*.c plus stubs, built
# with the PC's own compiler, and two static checks over this repo's sources.
#
# A standalone project: configure this directory directly (cmake -S tests),
# never add_subdirectory() it from the top-level CMakeLists.txt, which forces
# arm-none-eabi-gcc. How to run: tests/README.md.
cmake_minimum_required(VERSION 3.20)
project(boot_host_tests C)

set(BOOT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "bootloader repo root")
set(IAP "${BOOT_ROOT}/IAPServer")
if(NOT EXISTS "${IAP}/owner_slot.c")
  message(FATAL_ERROR "Not the bootloader repo: ${BOOT_ROOT}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -O0 -g)

enable_testing()
set(RUN_STEPS "${CMAKE_CURRENT_SOURCE_DIR}/run_steps.cmake")

# One ctest entry = one ordered run of the binary, each step a fresh process, in
# a private working directory: the harnesses keep scratch files in the current
# directory, and ctest -j must not make two entries share them.
# STEPS: steps separated by "|", arguments within a step by ",".
function(add_steps_test name exe steps)
  set(wd "${CMAKE_CURRENT_BINARY_DIR}/work/${name}")
  file(MAKE_DIRECTORY "${wd}")
  add_test(NAME ${name}
           COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${exe}> "-DSTEPS=${steps}" -P ${RUN_STEPS}
           WORKING_DIRECTORY "${wd}")
endfunction()

# --- T1-16: certificate / auth core -------------------------------------------
add_executable(iap_hosttest
  bootloader_unit/stubs/hal_stub.c
  bootloader_unit/stubs/bootloader_state_stub.c
  bootloader_unit/stubs/owner_slot_stub.c
  ${IAP}/sha256.c ${IAP}/iap_keyderive.c ${IAP}/iap_cert.c ${IAP}/fw_verify.c
  ${IAP}/uecc/uECC.c ${IAP}/iap_auth.c ${IAP}/net_rand.c
  bootloader_unit/test_main.c)
target_include_directories(iap_hosttest PRIVATE bootloader_unit bootloader_unit/stubs ${IAP})
add_test(NAME T1-16 COMMAND iap_hosttest)

# --- owner area: one entry per phase group, named after its case --------------
add_executable(owner_capacity_test
  owner_capacity/stubs/fake_owner_flash.c
  owner_capacity/stubs/reclaim_stub.c
  common_stubs/iap_keyderive_stub.c
  ${IAP}/owner_slot.c ${IAP}/sha256.c ${IAP}/fw_verify.c ${IAP}/uecc/uECC.c
  owner_capacity/test_main.c)
target_include_directories(owner_capacity_test PRIVATE
  owner_capacity/stubs common_stubs ${IAP} ${IAP}/uecc)
target_compile_options(owner_capacity_test PRIVATE
  -include ${CMAKE_CURRENT_SOURCE_DIR}/owner_capacity/stubs/fake_owner_flash.h)
# case : phases run in order (a later phase reads what the earlier one left)
foreach(entry
    "T2-22-T2-23:capacity"
    "T1-33:compact|compact-verify"
    "T2-24:wipe|wipe-verify"
    "T2-27:self-revoke"
    "T2-31:no-root"
    "T2-32:reset"
    "T2-33:rotate")
  string(REPLACE ":" ";" parts "${entry}")
  list(GET parts 0 case)
  list(GET parts 1 phases)
  add_steps_test(${case} owner_capacity_test "${phases}")
endforeach()

# --- T2-34: sector-15 reclaim under power cuts --------------------------------
add_executable(sector15_reclaim_test
  sector15_reclaim/stubs/fake_flash.c
  common_stubs/iap_keyderive_stub.c
  ${IAP}/bootloader_state.c ${IAP}/bkp_stash.c ${IAP}/owner_slot.c
  ${IAP}/sha256.c ${IAP}/fw_verify.c ${IAP}/uecc/uECC.c
  sector15_reclaim/test_main.c)
target_include_directories(sector15_reclaim_test PRIVATE
  sector15_reclaim/stubs common_stubs ${IAP} ${IAP}/uecc)
target_compile_options(sector15_reclaim_test PRIVATE
  -include ${CMAKE_CURRENT_SOURCE_DIR}/sector15_reclaim/stubs/host_s15.h)

# The reclaim has 7 flash operations; a cut lands before each, or after the last.
set(RECLAIM_OPS 7)
add_steps_test(T2-34.fresh  sector15_reclaim_test "fresh")
add_steps_test(T2-34.legacy sector15_reclaim_test "legacy")
foreach(k RANGE 0 ${RECLAIM_OPS})
  # A cut between the erase and the calibration write leaves no calibration.
  if(k EQUAL 1)
    set(calib 0)
  else()
    set(calib 1)
  endif()
  add_steps_test(T2-34.cut${k}.intact sector15_reclaim_test "cut,${k}|boot,1,${calib}")
  if(k LESS ${RECLAIM_OPS})
    # Before the erase nothing was lost, so the root survives without the copy.
    if(k EQUAL 0)
      set(root 1)
    else()
      set(root 0)
    endif()
    foreach(how battery-dead corrupt)
      add_steps_test(T2-34.cut${k}.${how} sector15_reclaim_test "cut,${k}|${how}|boot,${root},${calib}")
    endforeach()
  endif()
endforeach()

# --- static checks over this repo's sources -----------------------------------
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME P16 COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/checks/check_icache_is_restored.py)
  add_test(NAME P17 COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/checks/check_cproject_ld.py)
else()
  message(WARNING "No Python 3 found: P16 and P17 are not registered")
endif()

# --- T1-36: every output at 0 from the first bootloader statement (decision 81)
add_executable(safe_outputs_test ${IAP}/safe_outputs.c safe_outputs/test_main.c)
target_include_directories(safe_outputs_test PRIVATE safe_outputs/stubs ${IAP})
add_test(NAME T1-36 COMMAND safe_outputs_test)
