find_package(Qt6 REQUIRED Widgets)

add_library(notification-panels INTERFACE)
add_library(OBS::notification-panels ALIAS notification-panels)

target_sources(notification-panels INTERFACE panel/notification-panel.hpp)

target_include_directories(notification-panels INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/panel")

target_compile_definitions(notification-panels INTERFACE NOTIFICATION_AVAILABLE)

target_sources(
  spt-notification
  PRIVATE # cmake-format: sortable
          panel/notification-panel-client.cpp panel/notification-panel-client.hpp panel/notification-panel-internal.hpp
          panel/notification-panel.cpp)

target_link_libraries(spt-notification PRIVATE OBS::notification-panels Qt::Widgets)

set_target_properties(
  spt-notification
  PROPERTIES AUTOMOC ON
             AUTOUIC ON
             AUTORCC ON)

if(OS_WINDOWS)
  set_property(SOURCE notification-app.hpp PROPERTY SKIP_AUTOMOC TRUE)
endif()
