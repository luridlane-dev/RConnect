cmake_minimum_required(VERSION 3.21)
# The version is in the VERSION file; build-release.bat raises it with every release.
if(NOT DEFINED RCONNECT_VERSION)
    file(STRINGS VERSION RCONNECT_VERSION LIMIT_COUNT 1)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)

project(RConnect VERSION ${RCONNECT_VERSION} DESCRIPTION "Remote Desktop connection manager" LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT WIN32)
    message(FATAL_ERROR "RConnect builds on Windows only.")
endif()

option(RCONNECT_DEPLOY_QT "Run windeployqt after each build" ON)

# ActiveQt needs the MSVC kit.
find_package(Qt6 REQUIRED COMPONENTS Widgets AxContainer Sql Network LinguistTools)

configure_file(src/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/version.txt ${PROJECT_VERSION})

# Headers listed so lupdate scans them.
add_executable(RConnect WIN32
    src/main.cpp
    src/resources.qrc
    ${CMAKE_CURRENT_BINARY_DIR}/app.rc
    src/core/AccessLog.h         src/core/AccessLog.cpp
    src/core/Autostart.h         src/core/Autostart.cpp
    src/core/Contacts.h          src/core/Contacts.cpp
    src/core/GeoIp.h             src/core/GeoIp.cpp
    src/core/HostNames.h         src/core/HostNames.cpp
    src/core/Language.h          src/core/Language.cpp
    src/core/Log.h               src/core/Log.cpp
    src/core/Priority.h          src/core/Priority.cpp
    src/core/Secrets.h           src/core/Secrets.cpp
    src/core/ServerImport.h      src/core/ServerImport.cpp
    src/core/StatusMonitor.h     src/core/StatusMonitor.cpp
    src/core/Threats.h           src/core/Threats.cpp
    src/core/UpdateChecker.h     src/core/UpdateChecker.cpp
    src/core/UsageDiag.h        src/core/UsageDiag.cpp
    src/core/UsageEngine.h       src/core/UsageEngine.cpp
    src/core/UsageLog.h          src/core/UsageLog.cpp
    src/core/WinRm.h             src/core/WinRm.cpp
    src/core/WtsQuery.h          src/core/WtsQuery.cpp
    src/model/ServerProfile.h    src/model/ServerProfile.cpp
    src/model/ServerStatus.h
    src/model/ServerStore.h      src/model/ServerStore.cpp
    src/rdp/RdpSession.h         src/rdp/RdpSession.cpp
    src/ui/AccessLogPage.h       src/ui/AccessLogPage.cpp
    src/ui/ContactsDialog.h      src/ui/ContactsDialog.cpp
    src/ui/HomePage.h            src/ui/HomePage.cpp
    src/ui/LiveSessionsPage.h    src/ui/LiveSessionsPage.cpp
    src/ui/ServerDelegate.h      src/ui/ServerDelegate.cpp
    src/ui/SettingsDialog.h      src/ui/SettingsDialog.cpp
    src/ui/ServerEditDialog.h    src/ui/ServerEditDialog.cpp
    src/ui/ServerPanel.h         src/ui/ServerPanel.cpp
    src/ui/ServerSectionModel.h  src/ui/ServerSectionModel.cpp
    src/ui/SessionTabs.h         src/ui/SessionTabs.cpp
    src/ui/SessionWindow.h       src/ui/SessionWindow.cpp
    src/ui/UsagePage.h           src/ui/UsagePage.cpp
    src/ui/Theme.h               src/ui/Theme.cpp
    src/ui/Workspace.h           src/ui/Workspace.cpp
)

target_include_directories(RConnect PRIVATE src)
target_compile_definitions(RConnect PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    QT_NO_FOREACH
    RCONNECT_VERSION="${PROJECT_VERSION}")
target_link_libraries(RConnect PRIVATE
    Qt6::Widgets Qt6::AxContainer Qt6::Sql Qt6::Network
    bcrypt crypt32 dwmapi mpr ole32 oleaut32 secur32 wtsapi32 ws2_32)

if(MSVC)
    target_compile_options(RConnect PRIVATE /utf-8 /W3 /MP)
    target_compile_options(RConnect PRIVATE /Zc:inline)
    target_compile_options(RConnect PRIVATE $<$<CONFIG:Release>:/O2 /GL /Gy /Gw>)
    target_link_options(RConnect PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO>)
endif()

qt_add_translations(RConnect
    TS_FILES i18n/rconnect_tr.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete -locations none)

# Country data and flags (refreshed with tools/update_geodata.py)
file(GLOB _flags RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/flags/*.png")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/geoip.bin")
    qt_add_resources(RConnect geodata PREFIX / BASE src/assets
        FILES src/assets/geoip.bin ${_flags})
else()
    # Not in the repository (third-party data); without it countries and flags are simply not shown.
    message(WARNING "src/assets/geoip.bin is missing: run `python tools/update_geodata.py` for country flags.")
endif()

# Qt's own strings (context menus, standard buttons)
set(_qt_tr_dir "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}")
if(EXISTS "${_qt_tr_dir}/qtbase_tr.qm")
    qt_add_resources(RConnect qt_translations PREFIX /i18n BASE "${_qt_tr_dir}"
        FILES "${_qt_tr_dir}/qtbase_tr.qm")
endif()

if(RCONNECT_DEPLOY_QT)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
    find_program(WINDEPLOYQT windeployqt HINTS "${_qt_bin}")
    if(WINDEPLOYQT)
        add_custom_command(TARGET RConnect POST_BUILD
            COMMAND "${WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
                    --no-compiler-runtime "$<TARGET_FILE:RConnect>"
            COMMENT "Deploying Qt runtime")
    endif()
endif()
