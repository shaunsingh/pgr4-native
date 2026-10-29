# Compiles one Fennel file to Lua: cmake -DLUA=<lua> -DFENNEL=<fennel> -DGLOBALS=<a,b,...>
# -DIN=<file.fnl> -DOUT=<file.lua> -P fennel.cmake. Fennel writes the Lua to standard output; a
# failed compile leaves no output behind, so the next build tries again.
execute_process(
    COMMAND "${LUA}" "${FENNEL}" --globals-only "${GLOBALS}" --compile "${IN}"
    OUTPUT_VARIABLE lua
    ERROR_VARIABLE errors
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    file(REMOVE "${OUT}")
    message(FATAL_ERROR "${IN}:\n${errors}")
endif()
file(WRITE "${OUT}" "${lua}")
