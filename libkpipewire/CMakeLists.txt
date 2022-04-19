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

add_library(KPipewire STATIC
    pipewirecore.cpp
    pipewiresourceitem.cpp
    pipewiresourcestream.cpp
    screencasting.cpp
    screencastingrequest.cpp
    ${kpipewire_SRCS}
)

target_link_libraries(KPipewire PRIVATE Qt::Quick Qt::GuiPrivate KF5::I18n
    Wayland::Client KF5::WaylandClient Qt5::Gui_EGL KF5::CoreAddons
    PkgConfig::PipeWire Qt::WaylandClient)

if (${Qt5Gui_OPENGL_IMPLEMENTATION} STREQUAL "GLESv2")
    target_link_libraries(KPipewire PRIVATE Qt5::Gui_GLESv2)
else()
    target_link_libraries(KPipewire PRIVATE Qt5::Gui_GL)
endif()

target_include_directories(KPipewire PRIVATE ${CMAKE_CURRENT_BINARY_DIR})


add_library(KPipewireRecord STATIC pipewirerecord.cpp)
target_link_libraries(KPipewireRecord PRIVATE KPipewire Qt::Core Qt::Gui
    KF5::CoreAddons
    PkgConfig::AVCodec PkgConfig::AVUtil PkgConfig::AVFormat PkgConfig::GBM PkgConfig::SWScale epoxy::epoxy # PipeWireRecord
)

add_library(KPipewireDeclarative STATIC
    kpipewiredeclarativeplugin.cpp)
target_link_libraries(KPipewireDeclarative PRIVATE KPipewire Qt::Quick)

add_library(KPipewireRecordDeclarative STATIC
    kpipewirerecorddeclarativeplugin.cpp)
target_link_libraries(KPipewireRecordDeclarative PRIVATE KPipewire KPipewireRecord Qt::Quick)
