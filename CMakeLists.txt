cmake_minimum_required(VERSION 3.16)
project(KPipewire)
set(PROJECT_VERSION "5.27.8")
set(PROJECT_VERSION_MAJOR 5)

set(KF5_MIN_VERSION "5.102.0")
find_package(ECM ${KF5_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)
include(KDEInstallDirs)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(KDEClangFormat)
include(KDEGitCommitHooks)
include(ECMQtDeclareLoggingCategory)

find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS DBus Quick WaylandClient)
find_package(KF5 REQUIRED COMPONENTS Wayland I18n CoreAddons)
find_package(Wayland REQUIRED COMPONENTS Client)

find_package(PlasmaWaylandProtocols REQUIRED)
if (QT_MAJOR_VERSION EQUAL "5")
    find_package(QtWaylandScanner REQUIRED)
else()
    find_package(EGL REQUIRED)
endif()

pkg_check_modules(PipeWire IMPORTED_TARGET libpipewire-0.3 REQUIRED)

# ffmpeg, for recording
pkg_check_modules(AVCodec IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUtil IMPORTED_TARGET libavutil)
pkg_check_modules(AVFormat IMPORTED_TARGET libavformat)
pkg_check_modules(GBM IMPORTED_TARGET gbm)
pkg_check_modules(SWScale IMPORTED_TARGET libswscale)
find_package(epoxy 1.3 REQUIRED)
find_package(Libdrm 2.4.62)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# add clang-format target for all our real source files
file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES *.cpp *.h)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)

# Prevent EGL headers from including platform headers, in particular Xlib.h.
add_definitions(-DMESA_EGL_NO_X11_HEADERS)
add_definitions(-DEGL_NO_X11)
add_definitions(-DEGL_NO_PLATFORM_SPECIFIC_TYPES)

add_subdirectory(src)
if (BUILD_TESTING)
   add_subdirectory(tests)
endif()

ki18n_install(po)
