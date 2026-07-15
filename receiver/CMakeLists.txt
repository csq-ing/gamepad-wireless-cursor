cmake_minimum_required(VERSION 3.16)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

file(GLOB DEVICE_COMPONENT_DIRS LIST_DIRECTORIES true
    "${PROJECT_ROOT}/components/Devices/*"
)

set(EXTRA_COMPONENT_DIRS
    "${PROJECT_ROOT}/components/Bsp"
    "${PROJECT_ROOT}/components/Algorithm"
    ${DEVICE_COMPONENT_DIRS}
)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gamepad_receiver)
