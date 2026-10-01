# cmake -DEXE=<binary> -DSTEPS="a,b|c" -P run_steps.cmake
# Runs each step as its own process, in order, stopping at the first failure.
string(REPLACE "|" ";" steps "${STEPS}")
foreach(step IN LISTS steps)
  string(REPLACE "," ";" args "${step}")
  message("===== ${step}")
  execute_process(COMMAND "${EXE}" ${args} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "step '${step}' exited ${rc}")
  endif()
endforeach()
