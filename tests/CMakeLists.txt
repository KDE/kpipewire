add_executable(recordme main.cpp recordme.qrc ${SRCS} PlasmaRecordMe.cpp)
target_link_libraries(recordme KF5::WaylandClient Wayland::Client Qt5::Quick Qt::DBus KPipeWire)

configure_file(org.kde.recordme.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/org.kde.recordme.desktop @ONLY)

install(TARGETS recordme DESTINATION ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/org.kde.recordme.desktop DESTINATION ${KDE_INSTALL_APPDIR} )

#########

qt5_add_dbus_interface(
   XDP_SRCS
   org.freedesktop.portal.ScreenCast.xml
   xdp_dbus_screencast_interface
)

add_executable(xdp-recordme xdp-main.cpp recordme.qrc ${XDP_SRCS} RecordMe.cpp)
target_link_libraries(xdp-recordme Qt5::DBus Qt5::Quick KPipeWire)
install(TARGETS xdp-recordme DESTINATION ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})