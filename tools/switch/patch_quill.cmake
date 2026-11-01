set(_quill_switch_patch "${CMAKE_CURRENT_LIST_DIR}/patches/quill-switch.patch")

execute_process(
    COMMAND patch --batch --dry-run --forward -p1 -i "${_quill_switch_patch}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _quill_switch_forward_result
    OUTPUT_QUIET
    ERROR_QUIET)

if(_quill_switch_forward_result EQUAL 0)
    execute_process(
        COMMAND patch --batch --forward -p1 -i "${_quill_switch_patch}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _quill_switch_apply_result
        ERROR_VARIABLE _quill_switch_apply_error)
    if(NOT _quill_switch_apply_result EQUAL 0)
        message(FATAL_ERROR "Could not apply the Switch Quill patch: ${_quill_switch_apply_error}")
    endif()
else()
    execute_process(
        COMMAND patch --batch --dry-run --reverse -p1 -i "${_quill_switch_patch}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _quill_switch_reverse_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT _quill_switch_reverse_result EQUAL 0)
        message(FATAL_ERROR "The Switch Quill patch no longer matches the fetched dependency")
    endif()
endif()
