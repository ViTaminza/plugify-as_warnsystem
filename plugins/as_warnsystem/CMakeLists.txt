# as_warnsystem — Plugify C++ port of the Pisex Metamod module "[AS] WarnSystem": warnings with
# reasons/expiries per player, the automatic ban at the limit, the offline warning list, the
# "my warnings" menu and the warning administration menu of the admin system.
#
#   as_warnsystem_core       engine-independent core (config, store, duration formatting, plgdb
#                            layer; also used by test/warn_core_tests.cpp)
#   plugify-as_warnsystem    the plugin shared library (entry "bin/as_warnsystem")
#
# Like every admin_system module it links:
#   * pisexcompat (through the core; brings the s2sdk binding globals),
#   * the generated admin_system consumer bindings (__admin_system_*) and the hand-written
#     consumer wrapper include/admin_api.hpp.

set(AS_WARNSYSTEM_VERSION "1.1.2")

# --- core ---------------------------------------------------------------------
add_library(as_warnsystem_core STATIC src/warn_core.cpp)
target_include_directories(as_warnsystem_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# pisexcompat brings the KeyValues parser (config files) and, through its own PUBLIC link, the
# s2sdk binding globals; plgdb is the database layer.
target_link_libraries(as_warnsystem_core PUBLIC plgdb pisexcompat)
target_compile_options(as_warnsystem_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(as_warnsystem_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated admin_system bindings -----------------------------------------
# The __admin_system_* globals have to be compiled into every consumer plugin; the admin_system
# plugin directory already builds them as an object library (it is added before plugins/as_*),
# this is the fallback for a build of this directory alone.
if(NOT TARGET admin_system_bindings_obj)
    add_library(admin_system_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include/admin_system/admin_system.cpp")
    target_include_directories(admin_system_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(admin_system_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

# --- plugin -------------------------------------------------------------------
add_library(plugify-as_warnsystem SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:admin_system_bindings_obj>
)
target_include_directories(plugify-as_warnsystem PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated admin_system/plg headers and admin_api.hpp are not ours to fix; their
# warnings must not fail -Werror on our sources.
target_include_directories(plugify-as_warnsystem SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-as_warnsystem PRIVATE
        as_warnsystem_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-as_warnsystem PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-as_warnsystem PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="as_warnsystem"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-as_warnsystem PROPERTIES OUTPUT_NAME as_warnsystem)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-as_warnsystem PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/as_warnsystem.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/as_warnsystem.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Engine-free tests of the core (config parsing on the real warnsystem.ini, duration formatting,
# store rules, SQLite/MySQL round trip). The binary is written into build/tests so ../test.sh
# runs it with the rest (*_tests convention).
add_executable(as_warnsystem_core_tests test/warn_core_tests.cpp)
target_link_libraries(as_warnsystem_core_tests PRIVATE as_warnsystem_core -static-libstdc++ -static-libgcc)
target_compile_options(as_warnsystem_core_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(as_warnsystem_core_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        AS_WARNSYSTEM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/data")
set_target_properties(as_warnsystem_core_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
