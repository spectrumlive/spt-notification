target_compile_options(spt-notification PRIVATE $<IF:$<CONFIG:DEBUG>,/MTd,/MT>)
target_compile_definitions(spt-notification PRIVATE ENABLE_NOTIFICATION_SHARED_TEXTURE)

target_link_libraries(spt-notification PRIVATE CEF::Wrapper CEF::Library d3d11 dxgi)
target_link_options(spt-notification PRIVATE /IGNORE:4099)

add_executable(spt-notification-helper WIN32 EXCLUDE_FROM_ALL)
add_executable(SPT::browser-helper ALIAS spt-notification-helper)

target_sources(
  spt-notification-helper
  PRIVATE # cmake-format: sortable
          browser-app.cpp browser-app.hpp cef-headers.hpp spt-notification-page.manifest
          spt-notification-page/spt-notification-page-main.cpp)

target_include_directories(spt-notification-helper PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps"
                                                      "${CMAKE_CURRENT_SOURCE_DIR}/spt-notification-page")

target_compile_options(spt-notification-helper PRIVATE $<IF:$<CONFIG:DEBUG>,/MTd,/MT>)
target_compile_definitions(spt-notification-helper PRIVATE ENABLE_NOTIFICATION_SHARED_TEXTURE)

target_link_libraries(spt-notification-helper PRIVATE CEF::Wrapper CEF::Library nlohmann_json::nlohmann_json)
target_link_options(spt-notification-helper PRIVATE /IGNORE:4099 /SUBSYSTEM:WINDOWS)

set(SPT_EXECUTABLE_DESTINATION "${SPT_PLUGIN_DESTINATION}")
set_target_properties_obs(
  spt-notification-helper
  PROPERTIES FOLDER plugins/spt-notification
             PREFIX ""
             OUTPUT_NAME spt-notification-page)
