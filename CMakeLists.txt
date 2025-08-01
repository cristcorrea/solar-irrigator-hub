# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

file(TO_CMAKE_PATH "$ENV{IDF_PATH}" FIXED_IDF_PATH)
set(ENV{IDF_PATH} "${FIXED_IDF_PATH}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hub_station)
