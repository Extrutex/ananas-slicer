get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests.cpp
    test_limits.cpp
)

target_link_libraries(${_TEST_NAME}_tests test_common ananas_engine_core Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)

# End-to-end run of the engine binary over its JSON-RPC protocol: presets, schema, limits and a real slice.
# Needs python3 and a POSIX host (the engine moves its protocol channel with dup2).
find_program(ANANAS_TEST_PYTHON NAMES python3)
if (UNIX AND TARGET ananas-engine AND ANANAS_TEST_PYTHON)
    add_test(NAME engine_protocol
        COMMAND ${CMAKE_COMMAND} -E env
            ANANAS_ENGINE_BIN=$<TARGET_FILE:ananas-engine>
            ANANAS_PROFILES_DIR=${PROFILES_DIR}
            ANANAS_TEST_DATA_DIR=${TEST_DATA_DIR}
            ${ANANAS_TEST_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_engine_protocol.py)
    set_tests_properties(engine_protocol PROPERTIES LABELS "Engine;RequiresApp" TIMEOUT 300)
endif ()
