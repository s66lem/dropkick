cmake_minimum_required(VERSION 3.15)
project(DropkickCoreTests LANGUAGES CXX)

enable_testing()
set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/frontend-sdl-cpp/src")
add_executable(test_dropkick_core
    test_dropkick_core.cpp
    "${CORE_DIR}/JsonUtil.cpp"
    "${CORE_DIR}/PresetLists.cpp"
    "${CORE_DIR}/SystemMonitor.cpp"
)
target_include_directories(test_dropkick_core PRIVATE "${CORE_DIR}")
set_target_properties(test_dropkick_core PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
find_package(Threads REQUIRED)
target_link_libraries(test_dropkick_core PRIVATE Threads::Threads)
if (MSVC)
    target_compile_options(test_dropkick_core PRIVATE /W4 /WX)
else ()
    target_compile_options(test_dropkick_core PRIVATE -Wall -Wextra -Wpedantic)
endif ()
add_test(NAME dropkick_core COMMAND test_dropkick_core)
