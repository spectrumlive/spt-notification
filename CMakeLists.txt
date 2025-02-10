cmake_minimum_required(VERSION 3.16...3.25)

option(ENABLE_NOTIFICATION "Enable notification source plugin (required Chromium Embedded Framework)" OFF)

if(NOT ENABLE_NOTIFICATION)
  target_disable(spt-notification)
  target_disable_feature(spt-notification "Notification sources are not enabled by default (set CEF_ROOT_DIR and ENABLE_NOTIFICATION)")

  return()
endif()

find_package(CEF 95 REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)

add_library(spt-notification MODULE)
add_library(SPT::notification ALIAS spt-notification)

option(ENABLE_NOTIFICATION_PANELS "Enable Qt web notification panel support" ON)
mark_as_advanced(ENABLE_NOTIFICATION_PANELS)

target_sources(
  spt-notification
  PRIVATE # cmake-format: sortable
          notification-app.cpp
          notification-app.hpp
          notification-client.cpp
          notification-client.hpp
          notification-scheme.cpp
          notification-scheme.hpp
          notification-version.h
          cef-headers.hpp
          deps/base64/base64.cpp
          deps/base64/base64.hpp
          deps/signal-restore.cpp
          deps/signal-restore.hpp
          deps/wide-string.cpp
          deps/wide-string.hpp
          spt-notification-plugin.cpp
          spt-notification-source-audio.cpp
          spt-notification-source.cpp
          spt-notification-source.hpp)

target_include_directories(spt-notification PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps")

target_compile_features(spt-notification PRIVATE cxx_std_17)
target_link_libraries(spt-notification PRIVATE SPT::libobs SPT::frontend-api SPT::websocket-api nlohmann_json::nlohmann_json)

if(OS_WINDOWS)
  include(cmake/os-windows.cmake)
elseif(OS_MACOS)
  include(cmake/os-macos.cmake)
elseif(OS_LINUX)
  include(cmake/os-linux.cmake)
endif()

if(ENABLE_NOTIFICATION_PANELS)
  include(cmake/feature-panels.cmake)
endif()

set_target_properties_obs(spt-notification PROPERTIES FOLDER plugins/spt-notification PREFIX "")
