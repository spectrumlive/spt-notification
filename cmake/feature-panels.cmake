find_package(Qt6 REQUIRED Widgets)

add_library(browser-panels INTERFACE)
add_library(SPT::browser-panels ALIAS browser-panels)

target_sources(browser-panels INTERFACE panel/browser-panel.hpp)

target_include_directories(browser-panels INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/panel")

target_compile_definitions(browser-panels INTERFACE NOTIFICATION_AVAILABLE)

target_sources(
  spt-notification
  PRIVATE # cmake-format: sortable
          panel/browser-panel-client.cpp panel/browser-panel-client.hpp panel/browser-panel-internal.hpp
          panel/browser-panel.cpp)

target_link_libraries(spt-notification PRIVATE SPT::browser-panels Qt::Widgets)

set_target_properties(
  spt-notification
  PROPERTIES AUTOMOC ON
             AUTOUIC ON
             AUTORCC ON)

if(OS_WINDOWS)
  set_property(SOURCE browser-app.hpp PROPERTY SKIP_AUTOMOC TRUE)
endif()
