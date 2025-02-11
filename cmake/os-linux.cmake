find_package(X11 REQUIRED)

target_compile_definitions(spt-notification PRIVATE ENABLE_NOTIFICATION_QT_LOOP)

target_link_libraries(spt-notification PRIVATE CEF::Wrapper CEF::Library X11::X11)
set_target_properties(spt-notification PROPERTIES BUILD_RPATH "$ORIGIN/" INSTALL_RPATH "$ORIGIN/")

add_executable(notification-helper)
add_executable(OBS::notification-helper ALIAS notification-helper)

target_sources(
  notification-helper PRIVATE # cmake-format: sortable
                         notification-app.cpp notification-app.hpp cef-headers.hpp spt-notification-page/spt-notification-page-main.cpp)

target_include_directories(notification-helper PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps"
                                                  "${CMAKE_CURRENT_SOURCE_DIR}/spt-notification-page")

target_link_libraries(notification-helper PRIVATE CEF::Wrapper CEF::Library)

set(OBS_EXECUTABLE_DESTINATION "${OBS_PLUGIN_DESTINATION}")

# cmake-format: off
set_target_properties_obs(
  notification-helper
  PROPERTIES FOLDER plugins/spt-notification
             BUILD_RPATH "$ORIGIN/"
             INSTALL_RPATH "$ORIGIN/"
             PREFIX ""
             OUTPUT_NAME spt-notification-page)
# cmake-format: on
