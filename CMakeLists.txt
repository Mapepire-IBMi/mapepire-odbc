cmake_minimum_required(VERSION 3.16)

project(mapepire_odbc VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(WIN32)
    set(_openssl_default OFF)
else()
    set(_openssl_default ON)
endif()
option(MAPEPIRE_WITH_OPENSSL "Enable TLS (wss://) in the portable socket transport using OpenSSL" ${_openssl_default})
option(MAPEPIRE_BUILD_TESTS "Build the unit tests" ON)

set(MAPEPIRE_SOURCES
    src/mapepire_util.c
    src/mapepire_json.c
    src/mapepire_ws.c
    src/mapepire_ws_winhttp.c
    src/mapepire_ws_socket.c
    src/mapepire_client.c
    src/drv_diag.c
    src/drv_conf.c
    src/drv_handles.c
    src/drv_conn.c
    src/drv_types.c
    src/drv_stmt.c
    src/drv_info.c
    src/drv_catalog.c
)

# All driver code is compiled once and shared by the driver and the unit tests.
add_library(mapepire_objs OBJECT ${MAPEPIRE_SOURCES})
set_target_properties(mapepire_objs PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_include_directories(mapepire_objs PUBLIC src)

if(MSVC)
    target_compile_options(mapepire_objs PRIVATE /W4 /wd4100 /wd4996)
    target_compile_definitions(mapepire_objs PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(mapepire_objs PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

set(MAPEPIRE_LINK_LIBS)
if(WIN32)
    target_compile_definitions(mapepire_objs PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    list(APPEND MAPEPIRE_LINK_LIBS winhttp ws2_32 crypt32 bcrypt odbccp32)
else()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(mapepire_objs PUBLIC _GNU_SOURCE)
    endif()
    find_package(Threads REQUIRED)
    list(APPEND MAPEPIRE_LINK_LIBS Threads::Threads)
    # unixODBC (or iODBC) headers and the installer library for SQLGetPrivateProfileString
    find_path(ODBC_INCLUDE_DIR sql.h PATH_SUFFIXES odbc unixodbc)
    find_library(ODBCINST_LIBRARY NAMES odbcinst iodbcinst)
    if(NOT ODBC_INCLUDE_DIR OR NOT ODBCINST_LIBRARY)
        message(FATAL_ERROR "unixODBC development files were not found (install unixodbc-dev / unixODBC-devel)")
    endif()
    target_include_directories(mapepire_objs PUBLIC ${ODBC_INCLUDE_DIR})
    list(APPEND MAPEPIRE_LINK_LIBS ${ODBCINST_LIBRARY})
endif()

if(MAPEPIRE_WITH_OPENSSL)
    find_package(OpenSSL 1.1.1 REQUIRED)
    target_compile_definitions(mapepire_objs PUBLIC MAPEPIRE_HAVE_OPENSSL)
    target_link_libraries(mapepire_objs PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# The driver itself: mapepireodbc.dll on Windows, libmapepireodbc.so elsewhere.
if(WIN32)
    add_library(mapepireodbc SHARED $<TARGET_OBJECTS:mapepire_objs> src/mapepireodbc.def)
    set_target_properties(mapepireodbc PROPERTIES PREFIX "")
else()
    add_library(mapepireodbc SHARED $<TARGET_OBJECTS:mapepire_objs>)
endif()
target_link_libraries(mapepireodbc PRIVATE ${MAPEPIRE_LINK_LIBS})
if(MAPEPIRE_WITH_OPENSSL)
    target_link_libraries(mapepireodbc PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
set_target_properties(mapepireodbc PROPERTIES C_VISIBILITY_PRESET hidden)

install(TARGETS mapepireodbc
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)

if(MAPEPIRE_BUILD_TESTS)
    enable_testing()
    add_executable(unit_tests tests/unit/unit_tests.c $<TARGET_OBJECTS:mapepire_objs>)
    target_include_directories(unit_tests PRIVATE src)
    target_link_libraries(unit_tests PRIVATE ${MAPEPIRE_LINK_LIBS})
    if(MAPEPIRE_WITH_OPENSSL)
        target_link_libraries(unit_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
    if(WIN32)
        target_compile_definitions(unit_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    else()
        target_include_directories(unit_tests PRIVATE ${ODBC_INCLUDE_DIR})
    endif()
    add_test(NAME unit_tests COMMAND unit_tests)
endif()
