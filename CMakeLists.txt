# Copyright (C) 2026 ClutchReframe
# SPDX-License-Identifier: GPL-2.0-only

cmake_minimum_required(VERSION 3.24)

project(RiftReframeObsPlugin C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(RIFTREFRAME_OBS_PLUGIN_WARNINGS_AS_ERRORS "Treat warnings as errors." OFF)

set(OBS_SOURCE_ROOT "" CACHE PATH "Path to OBS source root containing libobs/ and build_x64/.")
set(OBS_INCLUDE_DIR "" CACHE PATH "Path to OBS include directory containing obs-module.h")
set(OBS_LIB_DIR "" CACHE PATH "Path to OBS library directory containing libobs.lib")
set(OBS_CONFIG_INCLUDE_DIR "" CACHE PATH "Path to OBS generated config headers containing obsconfig.h")

if (NOT OBS_SOURCE_ROOT AND DEFINED ENV{OBS_SOURCE_ROOT})
    set(OBS_SOURCE_ROOT "$ENV{OBS_SOURCE_ROOT}")
endif()

if (NOT OBS_INCLUDE_DIR AND DEFINED ENV{OBS_INCLUDE_DIR})
    set(OBS_INCLUDE_DIR "$ENV{OBS_INCLUDE_DIR}")
endif()

if (NOT OBS_LIB_DIR AND DEFINED ENV{OBS_LIB_DIR})
    set(OBS_LIB_DIR "$ENV{OBS_LIB_DIR}")
endif()

if (NOT OBS_CONFIG_INCLUDE_DIR AND DEFINED ENV{OBS_CONFIG_INCLUDE_DIR})
    set(OBS_CONFIG_INCLUDE_DIR "$ENV{OBS_CONFIG_INCLUDE_DIR}")
endif()

function(rr_append_if_exists list_var candidate_path)
    if (candidate_path AND EXISTS "${candidate_path}")
        set(_tmp "${${list_var}}")
        list(APPEND _tmp "${candidate_path}")
        set(${list_var} "${_tmp}" PARENT_SCOPE)
    endif()
endfunction()

set(_rr_obs_roots "")
rr_append_if_exists(_rr_obs_roots "${OBS_SOURCE_ROOT}")
rr_append_if_exists(_rr_obs_roots "C:/Dev/obs-studio-32.0.4")
rr_append_if_exists(_rr_obs_roots "C:/Dev/obs-studio")

set(_rr_obs_include_candidates "")
rr_append_if_exists(_rr_obs_include_candidates "${OBS_INCLUDE_DIR}")
foreach(_root IN LISTS _rr_obs_roots)
    rr_append_if_exists(_rr_obs_include_candidates "${_root}/libobs")
endforeach()

find_path(OBS_INCLUDE_PATH
    NAMES obs-module.h
    PATHS ${_rr_obs_include_candidates}
    NO_DEFAULT_PATH)

if (NOT OBS_INCLUDE_PATH)
    message(FATAL_ERROR
        "obs-module.h was not found.\n"
        "  Tried: ${_rr_obs_include_candidates}\n"
        "  Fix: pass -DOBS_SOURCE_ROOT=C:/Dev/obs-studio-32.0.4\n"
        "       or -DOBS_INCLUDE_DIR=<path to libobs>.")
endif()

set(_rr_obs_lib_candidates "")
rr_append_if_exists(_rr_obs_lib_candidates "${OBS_LIB_DIR}")
foreach(_root IN LISTS _rr_obs_roots)
    rr_append_if_exists(_rr_obs_lib_candidates "${_root}/build_x64/libobs/Release")
    rr_append_if_exists(_rr_obs_lib_candidates "${_root}/build/libobs/Release")
    rr_append_if_exists(_rr_obs_lib_candidates "${_root}/build64/libobs/Release")
endforeach()

find_library(OBS_LIBOBS_LIBRARY
    NAMES libobs obs
    PATHS ${_rr_obs_lib_candidates}
    NO_DEFAULT_PATH)

if (NOT OBS_LIBOBS_LIBRARY)
    message(FATAL_ERROR
        "libobs/obs library was not found.\n"
        "  Tried: ${_rr_obs_lib_candidates}\n"
        "  Fix: pass -DOBS_SOURCE_ROOT=<obs source root>\n"
        "       or -DOBS_LIB_DIR=<path containing obs.lib>.")
endif()

set(_rr_obs_config_candidates "")
rr_append_if_exists(_rr_obs_config_candidates "${OBS_CONFIG_INCLUDE_DIR}")
foreach(_root IN LISTS _rr_obs_roots)
    rr_append_if_exists(_rr_obs_config_candidates "${_root}/build_x64/config")
    rr_append_if_exists(_rr_obs_config_candidates "${_root}/build/config")
    rr_append_if_exists(_rr_obs_config_candidates "${_root}/build64/config")
endforeach()
rr_append_if_exists(_rr_obs_config_candidates "${OBS_INCLUDE_PATH}/../build_x64/config")
rr_append_if_exists(_rr_obs_config_candidates "${OBS_INCLUDE_PATH}/../build/config")
rr_append_if_exists(_rr_obs_config_candidates "${OBS_INCLUDE_PATH}/../build64/config")

find_path(OBS_CONFIG_INCLUDE_PATH
    NAMES obsconfig.h
    PATHS ${_rr_obs_config_candidates}
    NO_DEFAULT_PATH)

if (NOT OBS_CONFIG_INCLUDE_PATH)
    message(FATAL_ERROR
        "obsconfig.h was not found.\n"
        "  Tried: ${_rr_obs_config_candidates}\n"
        "  Fix: pass -DOBS_SOURCE_ROOT=<obs source root>\n"
        "       or -DOBS_CONFIG_INCLUDE_DIR=<path containing obsconfig.h>.")
endif()

message(STATUS "OBS include: ${OBS_INCLUDE_PATH}")
message(STATUS "OBS lib: ${OBS_LIBOBS_LIBRARY}")
message(STATUS "OBS config include: ${OBS_CONFIG_INCLUDE_PATH}")

add_library(riftreframe_obs_plugin MODULE
    src/plugin-main.c
    src/plugin-version.rc
    src/roi_ipc_client.c
    src/roi_interpolator.c
    src/roi_filter_source.c
    include/roi_ipc_client.h
    include/roi_interpolator.h
    include/roi_frame_msg_v1.h
    include/plugin_version.h
)

target_include_directories(riftreframe_obs_plugin
    PRIVATE
        "${OBS_INCLUDE_PATH}"
        "${OBS_CONFIG_INCLUDE_PATH}"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(riftreframe_obs_plugin
    PRIVATE
        "${OBS_LIBOBS_LIBRARY}"
)

if (MSVC)
    target_compile_options(riftreframe_obs_plugin PRIVATE /W4 /utf-8)
    if (RIFTREFRAME_OBS_PLUGIN_WARNINGS_AS_ERRORS)
        target_compile_options(riftreframe_obs_plugin PRIVATE /WX)
    endif()
endif()

set_target_properties(riftreframe_obs_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "clutchreframe-obs-plugin"
)

install(TARGETS riftreframe_obs_plugin
    RUNTIME DESTINATION "bin/64bit"
    LIBRARY DESTINATION "bin/64bit"
    ARCHIVE DESTINATION "bin/64bit"
)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini"
    DESTINATION "data/locale"
)
