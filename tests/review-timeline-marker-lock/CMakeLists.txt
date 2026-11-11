# Review finding kit-overlay-raven-timeline-option-review-1: a locked marker
# must not move with a clip it is selected with. Builds Raven as a subproject.
#   cmake -S tests/review-timeline-marker-lock -B build/rev-lock -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/rev-lock && build/rev-lock/timeline_marker_lock
cmake_minimum_required(VERSION 3.16)
project(timeline_marker_lock C)
set(RAVEN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RAVEN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RAVEN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. raven)
add_executable(timeline_marker_lock timeline_marker_lock.c)
target_include_directories(timeline_marker_lock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(timeline_marker_lock PRIVATE raven::raven)
if(NOT WIN32)
	target_link_libraries(timeline_marker_lock PRIVATE m)
endif()
