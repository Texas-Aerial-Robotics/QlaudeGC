# ============================================================================
# QlaudeGC custom build
#   - stock QGC UI
#   - built-in MAVLink router (mavlink-router compatible main.conf)
#   - Claude chat sidebar (runs the local Claude Code CLI)
#   - ROS bag recording on the companion computer (over ssh)
#   - tar_router_cli: the same router as a standalone console program
# ============================================================================

message(STATUS "QGC: Adding TAR custom plugin")

list(APPEND CUSTOM_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/custom.qrc)
set(QGC_RESOURCES ${QGC_RESOURCES} ${CUSTOM_RESOURCES} CACHE STRING "Paths to .qrc Resources" FORCE)

set(TAR_ROUTER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Router/MavlinkFrameParser.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Router/MavlinkFrameParser.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Router/RouterConfig.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Router/RouterConfig.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Router/RouterCore.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Router/RouterCore.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Router/RouterEndpoint.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Router/RouterEndpoint.h
    CACHE INTERNAL "" FORCE
)

set(CUSTOM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TarPlugin.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TarPlugin.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Router/TarRouter.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Router/TarRouter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Assistant/TarAssistant.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Assistant/TarAssistant.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Assistant/TarMcpObjects.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Assistant/TarMcpServer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Assistant/TarMcpServer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Field/TarFieldNetwork.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Field/TarFieldNetwork.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rosbag/TarRosbag.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rosbag/TarRosbag.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rosbag/TarRosbagRecorder.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rosbag/TarRosbagRecorder.h
    ${TAR_ROUTER_SOURCES}
    CACHE INTERNAL "" FORCE
)

set(CUSTOM_INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Router
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Assistant
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Field
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rosbag
    CACHE INTERNAL "" FORCE
)

set(CUSTOM_DEFINITIONS
    QGC_CUSTOM_BUILD
    CUSTOMHEADER="TarPlugin.h"
    CUSTOMCLASS=TarPlugin
    CACHE INTERNAL "" FORCE
)

set(CUSTOM_QT_COMPONENTS
    Core
    Gui
    Quick
    Network
    SerialPort
    CACHE INTERNAL "" FORCE
)

# ----------------------------------------------------------------------------
# Standalone router (no GUI). Defined after src/ so the generated MAVLink headers are known.
# ----------------------------------------------------------------------------
function(tar_add_router_cli)
    if(ANDROID OR IOS)
        return()
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Core Network SerialPort)
    if(NOT mavlink_BINARY_DIR)
        set(mavlink_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/mavlink-build")
    endif()
    qt_add_executable(tar_router_cli
        ${TAR_ROUTER_SOURCES}
        ${CMAKE_SOURCE_DIR}/${QGC_CUSTOM_DIR}/src/Router/tar_router_cli.cc
    )
    set_target_properties(tar_router_cli PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)
    target_compile_features(tar_router_cli PRIVATE cxx_std_20)
    target_include_directories(tar_router_cli PRIVATE
        ${CMAKE_SOURCE_DIR}/${QGC_CUSTOM_DIR}/src/Router
        ${CMAKE_SOURCE_DIR}/src/MAVLink
        ${mavlink_BINARY_DIR}/include/mavlink
        ${mavlink_BINARY_DIR}/include/mavlink/${QGC_MAVLINK_DIALECT}
    )
    target_link_libraries(tar_router_cli PRIVATE Qt6::Core Qt6::Network Qt6::SerialPort)
    add_dependencies(tar_router_cli mavlink)
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL tar_add_router_cli)
