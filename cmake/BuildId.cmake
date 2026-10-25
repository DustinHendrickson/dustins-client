# Writes OUTPUT, a header defining WOWDECOMP_BUILD_ID as the short commit of SOURCE_DIR (with "+" when the tree has
# changes), only when it differs from what is there, so builds don't recompile for nothing.

execute_process(
    COMMAND git -c safe.directory=* rev-parse --short HEAD
    WORKING_DIRECTORY ${SOURCE_DIR}
    OUTPUT_VARIABLE id
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if (NOT id)
    set(id unknown)
else ()
    execute_process(
        COMMAND git -c safe.directory=* status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE changes
        ERROR_QUIET
    )

    if (changes)
        set(id "${id}+")
    endif ()
endif ()

set(content "#define WOWDECOMP_BUILD_ID \"${id}\"\n")

if (EXISTS ${OUTPUT})
    file(READ ${OUTPUT} current)
endif ()

if (NOT current STREQUAL content)
    file(WRITE ${OUTPUT} "${content}")
endif ()
