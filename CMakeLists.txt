cmake_minimum_required(VERSION 3.10)

project(parodus2rbus C)

option(USE_LOCAL_LIBPARODUS "Build and link in-tree libparodus for debugging" ON)

set(TEST_ENVIRONMENT 1)

if(USE_LOCAL_LIBPARODUS)
  message(STATUS "USE_LOCAL_LIBPARODUS=ON: building libparodus from source tree")
  # Add subdirectory only once (guard if already added by parent)
  if(NOT TARGET libparodus AND EXISTS ${CMAKE_SOURCE_DIR}/../libparodus/CMakeLists.txt)
    add_subdirectory(${CMAKE_SOURCE_DIR}/../libparodus ${CMAKE_BINARY_DIR}/libparodus-build)
    set(LIBPARODUS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/../libparodus/src)
    set(LIBPARODUS_LIBRARY libparodus.shared)
  endif()
endif()

# If built standalone (not from parent) the RBUS / cjson / jansson variables will be undefined.
# Provide local discovery replicating logic from parent CMake.
if(NOT RBUS_LIBRARY)
  find_library(RBUS_LIBRARY NAMES rbus)
endif()
if(NOT RBUS_CORE_LIBRARY)
  find_library(RBUS_CORE_LIBRARY NAMES rbuscore)
endif()
if(NOT RBUS_INCLUDE_DIR)
  find_path(RBUS_INCLUDE_DIR NAMES rbus.h PATH_SUFFIXES rbus)
endif()
if(NOT RTMSG_INCLUDE_DIR)
  find_path(RTMSG_INCLUDE_DIR NAMES rtMessage.h PATH_SUFFIXES rtmessage)
endif()
if(NOT CJSON_LIBRARY)
  find_library(CJSON_LIBRARY NAMES cjson)
endif()
if(NOT CJSON_INCLUDE_DIR)
  find_path(CJSON_INCLUDE_DIR NAMES cJSON.h PATH_SUFFIXES cjson)
endif()
if(NOT JANSSON_LIBRARY)
  find_library(JANSSON_LIBRARY NAMES jansson)
endif()
if(NOT JANSSON_INCLUDE_DIR)
  find_path(JANSSON_INCLUDE_DIR NAMES jansson.h)
endif()

# cimplog (needed by wrp-c/libparodus)
find_library(CIMPLOG_LIBRARY NAMES cimplog)
if(NOT CIMPLOG_LIBRARY)
  message(FATAL_ERROR "cimplog library not found (required by libparodus)")
endif()

foreach(var RBUS_LIBRARY RBUS_CORE_LIBRARY RBUS_INCLUDE_DIR RTMSG_INCLUDE_DIR CJSON_LIBRARY CJSON_INCLUDE_DIR JANSSON_LIBRARY JANSSON_INCLUDE_DIR)
  if(NOT ${var})
    message(FATAL_ERROR "Missing dependency variable: ${var}. Ensure dependencies are installed or build from project root.")
  endif()
endforeach()

# Try to locate a system-installed full libparodus (headers + library)
find_path(LIBPARODUS_INCLUDE_DIR NAMES libparodus.h parodus.h PATH_SUFFIXES libparodus)
find_library(LIBPARODUS_LIBRARY NAMES parodus libparodus)
if(NOT USE_LOCAL_LIBPARODUS)
  if(LIBPARODUS_LIBRARY AND LIBPARODUS_INCLUDE_DIR)
    message(STATUS "Found system libparodus: ${LIBPARODUS_LIBRARY}")
  else()
    message(STATUS "System libparodus not found; will use lite variant if available.")
  endif()
else()
  message(STATUS "Using local libparodus target: ${LIBPARODUS_LIBRARY}")
endif()

# wrp-c (dependency of libparodus not always linked transitively)
find_library(WRP_C_LIBRARY NAMES wrp-c)
if(NOT WRP_C_LIBRARY)
  message(FATAL_ERROR "wrp-c library not found (required by libparodus)")
endif()

add_library(parodus2rbus_objs OBJECT
  src/rbus_adapter.c
  src/parodus_iface.c
  src/protocol.c
  src/config.c
  src/log.c
)

target_include_directories(parodus2rbus_objs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(parodus2rbus_objs PRIVATE ${RBUS_INCLUDE_DIR} ${RTMSG_INCLUDE_DIR} ${CJSON_INCLUDE_DIR} ${JANSSON_INCLUDE_DIR} ${LIBPARODUS_INCLUDE_DIR})

add_executable(parodus2rbus $<TARGET_OBJECTS:parodus2rbus_objs> src/main.c)

target_link_libraries(parodus2rbus PRIVATE ${RBUS_LIBRARY} ${RBUS_CORE_LIBRARY} ${CJSON_LIBRARY} ${JANSSON_LIBRARY} ${CIMPLOG_LIBRARY} ${WRP_C_LIBRARY})
target_link_options(parodus2rbus PRIVATE -Wl,--no-as-needed)
if(LIBPARODUS_LIBRARY AND LIBPARODUS_INCLUDE_DIR)
  target_link_libraries(parodus2rbus PRIVATE ${LIBPARODUS_LIBRARY})
  target_include_directories(parodus2rbus PRIVATE ${LIBPARODUS_INCLUDE_DIR})
elseif(TARGET libparodus)
  # Fallback: static libparodus target name if shared not present
  target_link_libraries(parodus2rbus PRIVATE libparodus)
  target_include_directories(parodus2rbus PRIVATE ${LIBPARODUS_INCLUDE_DIR})
elseif(TARGET libparodus_lite)
  target_link_libraries(parodus2rbus PRIVATE libparodus_lite)
  target_compile_definitions(parodus2rbus PRIVATE PARODUS_USE_LITE=1)
endif()

target_include_directories(parodus2rbus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${RBUS_INCLUDE_DIR} ${RTMSG_INCLUDE_DIR} ${CJSON_INCLUDE_DIR} ${JANSSON_INCLUDE_DIR})

target_compile_features(parodus2rbus PRIVATE c_std_99)
target_compile_options(parodus2rbus PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS parodus2rbus RUNTIME DESTINATION bin)
