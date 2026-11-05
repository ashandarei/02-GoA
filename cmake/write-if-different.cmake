# Preserve timestamps during CMake reconfiguration so unchanged generated
# UE4SS overrides do not invalidate native objects and downstream DLLs.
function(kh3ap_write_if_different path content)
    if(EXISTS "${path}")
        file(READ "${path}" previous_content)
        if("${previous_content}" STREQUAL "${content}")
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()
