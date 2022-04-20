include(ECMGenerateExportHeader)
include(ECMQmlModule)

ecm_qt_declare_logging_category(kpipewire_SRCS
                            HEADER logging.h
                            IDENTIFIER PIPEWIRE_LOGGING
                            CATEGORY_NAME kde_recordme_pipewire_logging
                            DESCRIPTION "PipeWire components for KDE"
                            EXPORT KDERecordmePipeWire)
ecm_qt_install_logging_categories(
    EXPORT KDERecordmePipeWire
    FILE recordme.categories
    DESTINATION "${KDE_INSTALL_LOGGINGCATEGORIESDIR}"
)

ecm_add_qtwayland_client_protocol(kpipewire_SRCS
    PROTOCOL ${PLASMA_WAYLAND_PROTOCOLS_DIR}/screencast.xml
    BASENAME zkde-screencast-unstable-v1
)

add_library(KPipeWire
    pipewirecore.cpp
    pipewiresourceitem.cpp
    pipewiresourcestream.cpp
    screencasting.cpp
    screencastingrequest.cpp
    ${kpipewire_SRCS}
)

target_link_libraries(KPipeWire PRIVATE Qt::Quick Qt::GuiPrivate KF5::I18n
    Wayland::Client KF5::WaylandClient Qt5::Gui_EGL KF5::CoreAddons
    PkgConfig::PipeWire Qt::WaylandClient)

if (${Qt5Gui_OPENGL_IMPLEMENTATION} STREQUAL "GLESv2")
    target_link_libraries(KPipeWire PRIVATE Qt5::Gui_GLESv2)
else()
    target_link_libraries(KPipeWire PRIVATE Qt5::Gui_GL)
endif()

target_include_directories(KPipeWire PRIVATE ${CMAKE_CURRENT_BINARY_DIR})


add_library(KPipeWireRecord pipewirerecord.cpp)
target_link_libraries(KPipeWireRecord PRIVATE KPipeWire Qt::Core Qt::Gui
    KF5::CoreAddons
    PkgConfig::AVCodec PkgConfig::AVUtil PkgConfig::AVFormat PkgConfig::GBM PkgConfig::SWScale epoxy::epoxy # PipeWireRecord
)

ecm_add_qml_module(KPipeWireDeclarative URI "org.kde.pipewire" VERSION 1.0)
target_sources(KPipeWireDeclarative PRIVATE kpipewiredeclarativeplugin.cpp)
target_link_libraries(KPipeWireDeclarative PRIVATE KPipeWire Qt::Quick)
ecm_finalize_qml_module(KPipeWireDeclarative DESTINATION ${KDE_INSTALL_QMLDIR})

ecm_add_qml_module(KPipeWireRecordDeclarative URI "org.kde.pipewire.record" VERSION 1.0)
target_sources(KPipeWireRecordDeclarative PRIVATE kpipewirerecorddeclarativeplugin.cpp)
target_link_libraries(KPipeWireRecordDeclarative PRIVATE KPipeWire KPipeWireRecord Qt::Quick)
ecm_finalize_qml_module(KPipeWireRecordDeclarative DESTINATION ${KDE_INSTALL_QMLDIR})

ecm_generate_export_header(KPipeWire
    BASE_NAME KPipeWire
    VERSION ${PROJECT_VERSION}
)

install(TARGETS KPipeWire KPipeWireRecord ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
