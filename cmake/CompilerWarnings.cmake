# Advertencias estrictas comunes a todos los targets de LanCast.
function(lancast_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc
            /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311
            /w14545 /w14546 /w14547 /w14549 /w14555 /w14640 /w14826 /w14905 /w14906 /w14928)
        if(LANCAST_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(LANCAST_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
