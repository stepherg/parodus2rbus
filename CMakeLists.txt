cmake_minimum_required(VERSION 3.10)

project(parodus2rbus C)

add_library(parodus2rbus_objs OBJECT
  src/rbus_adapter.c
  src/parodus_iface.c
  src/protocol.c
  src/config.c
  src/log.c
  src/main.c
)

# Reuse parent include discovery (RBUS, cJSON, JANSSON) via target_include_directories from parent
# We expose public include for our own headers

target_include_directories(parodus2rbus_objs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(parodus2rbus_objs PRIVATE ${RBUS_INCLUDE_DIR} ${RTMSG_INCLUDE_DIR} ${CJSON_INCLUDE_DIR} ${JANSSON_INCLUDE_DIR})

add_executable(parodus2rbus $<TARGET_OBJECTS:parodus2rbus_objs>)

# Link against the already-found libraries from parent scope if available
# They should be cached variables after parent configuration.

target_link_libraries(parodus2rbus PRIVATE ${RBUS_LIBRARY} ${RBUS_CORE_LIBRARY} ${CJSON_LIBRARY} ${JANSSON_LIBRARY})
if(TARGET libparodus_lite)
  target_link_libraries(parodus2rbus PRIVATE libparodus_lite)
  target_compile_definitions(parodus2rbus PRIVATE PARODUS_USE_LITE=1)
endif()

target_compile_features(parodus2rbus PRIVATE c_std_99)

target_compile_options(parodus2rbus PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS parodus2rbus RUNTIME DESTINATION bin)
