# SPDX-FileCopyrightText: 2023 Fushan Wen <qydwhotmail@gmail.com>
# SPDX-License-Identifier: BSD-3-Clause

add_library(KPipeWireMonitor)
target_sources(KPipeWireMonitor PRIVATE mediamonitor.cpp)

ecm_add_qml_module(KPipeWireMonitor URI "org.kde.pipewire.monitor" GENERATE_PLUGIN_SOURCE VERSION 1.0 DEPENDENCIES QtCore)
target_link_libraries(KPipeWireMonitor PRIVATE Qt::Quick PkgConfig::PipeWire KPipeWire)
ecm_finalize_qml_module(KPipeWireMonitor)

ecm_generate_export_header(KPipeWireMonitor
    BASE_NAME KPipeWireMonitor
    VERSION ${PROJECT_VERSION}
)

ecm_generate_headers(KPipeWireMonitor_HEADERS
  HEADER_NAMES
    MediaMonitor
  REQUIRED_HEADERS KPipeWireMonitor_HEADERS
)

target_include_directories(KPipeWireMonitor PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
                                            PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
                                            INTERFACE "$<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}/KPipeWire>"
)

set_target_properties(KPipeWireMonitor PROPERTIES
                                       VERSION ${PROJECT_VERSION}
                                       SOVERSION ${PROJECT_VERSION_MAJOR}
)

install(TARGETS KPipeWireMonitor EXPORT KPipeWireTargets ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

install(FILES
  ${KPipeWireMonitor_HEADERS}
  ${CMAKE_CURRENT_BINARY_DIR}/kpipewiremonitor_export.h
  DESTINATION ${KDE_INSTALL_INCLUDEDIR}/KPipeWire COMPONENT Devel
)

if(BUILD_TESTING)
    add_subdirectory(autotests)
endif()
