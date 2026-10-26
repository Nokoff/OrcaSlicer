# slic3rutils tests require libslic3r_gui (contains Http.cpp).
# Skip entirely when GUI is disabled.
if (NOT SLIC3R_GUI)
    return()
endif()

get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    SnapLogClient_test.cpp
    PartPlate_test.cpp
    ../../src/sentry_wrapper/SentryWrapper.cpp
    ../../src/bury_cfg/bury_point.cpp
    ../../src/common_func/common_func.cpp
    )
target_compile_definitions(${_TEST_NAME}_tests PRIVATE BURY_EXPORTS)
if (MSVC)
    target_sources(${_TEST_NAME}_tests PRIVATE ../../src/dev-utils/BaseException.cpp ../../src/dev-utils/StackWalker.cpp)
    set_source_files_properties(../../src/common_func/common_func.cpp PROPERTIES
        COMPILE_OPTIONS /FI"${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/pchheader.hpp")
endif()

find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(libnoise REQUIRED)
find_package(Qhull 7.2 QUIET)
if(UNIX AND NOT APPLE)
    target_link_options(${_TEST_NAME}_tests PRIVATE "-Wl,--no-as-needed")
endif()
if(TARGET libslic3r_gui)
    # BitmapCache.cpp supplies nanoSVG when the full GUI is linked.
    set(_GUI_TEST_LINK_LIBS ${TEST_LINK_LIBS})
    list(REMOVE_ITEM _GUI_TEST_LINK_LIBS test_nanosvg_impl)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE libslic3r_gui ${_GUI_TEST_LINK_LIBS})
else()
    target_link_libraries(${_TEST_NAME}_tests PRIVATE ${TEST_LINK_LIBS})
endif()
if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE Setupapi.lib)
endif ()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
    elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
    else()
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
    endif()
endif()

catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: " EXTRA_ARGS ${CATCH_EXTRA_ARGS})

# Exercise the merge planner without linking the whole GUI application.
find_package(wxWidgets REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})
add_executable(filament_merge_tests
    FilamentMergePlan_test.cpp
    ../../src/slic3r/GUI/filamentsync/FilamentMergePlan.cpp
    ../../src/slic3r/GUI/filamentsync/FilamentSyncAlgorithm.cpp
    ../../src/slic3r/Utils/ColorSpaceConvert.cpp
)
target_link_libraries(filament_merge_tests PRIVATE ${TEST_LINK_LIBS} ${wxWidgets_LIBRARIES})
set_property(TARGET filament_merge_tests PROPERTY FOLDER "tests")
catch_discover_tests(filament_merge_tests TEST_PREFIX "filament_merge: " EXTRA_ARGS ${CATCH_EXTRA_ARGS})
# add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests "${CATCH_EXTRA_ARGS} exclude:[NotWorking]")

# Test the transport without linking or starting the GUI. The broker uses loopback only.
find_package(Python3 QUIET COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    add_executable(mqtt_recovery_probe MQTTRecovery_probe.cpp ../../src/slic3r/Utils/MQTT.cpp)
    target_include_directories(mqtt_recovery_probe PRIVATE ../../src)
    target_link_libraries(mqtt_recovery_probe PRIVATE paho-mqttpp3-static Boost::log Boost::filesystem Boost::thread)
    set_property(TARGET mqtt_recovery_probe PROPERTY FOLDER "tests")
    add_test(NAME mqtt_recovery
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mqtt_recovery_broker.py $<TARGET_FILE:mqtt_recovery_probe>)
    add_test(NAME mqtt_recovery_cancel
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mqtt_recovery_broker.py $<TARGET_FILE:mqtt_recovery_probe> cancel)
    set_tests_properties(mqtt_recovery mqtt_recovery_cancel PROPERTIES TIMEOUT 75 LABELS "network")
endif()
