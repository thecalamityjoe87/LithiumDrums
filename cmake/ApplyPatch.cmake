# Applies a patch to a fetched dependency, once. Run as a FetchContent
# PATCH_COMMAND, from the dependency's source folder:
#
#   cmake -DPATCH_FILE=/path/to/fix.patch -P ApplyPatch.cmake
#
# The patch step can run again on a source folder that already has the patch
# (after a reconfigure, say), so a patch that's already in is left alone rather
# than failing the build. One that neither applies nor is already in stops the
# configure: the dependency has changed under it and the patch needs updating.

find_package(Git REQUIRED)

if(NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "ApplyPatch: no such patch: ${PATCH_FILE}")
endif()

execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH_FILE}"
                RESULT_VARIABLE already_applied OUTPUT_QUIET ERROR_QUIET)

if(already_applied EQUAL 0)
    message(STATUS "ApplyPatch: already applied: ${PATCH_FILE}")
    return()
endif()

execute_process(COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${PATCH_FILE}"
                RESULT_VARIABLE result ERROR_VARIABLE error)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "ApplyPatch: couldn't apply ${PATCH_FILE}:\n${error}")
endif()

message(STATUS "ApplyPatch: applied ${PATCH_FILE}")
