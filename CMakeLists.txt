cmake_minimum_required(VERSION 3.21)

project(FOXAssetBrowser
    VERSION 0.1.0
    DESCRIPTION "Native C++/Qt6 asset browser and bulk extractor for Fox Engine games (MGSV TPP / Ground Zeroes / Survive)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A bare `cmake -B build` with no build type produces a binary with NO
# optimisation flags at all on single-config generators — not a debug build, an
# UNOPTIMISED one, which measured about four times slower than Release on the
# parsing and decoding this program spends its time in. The presets always set
# a type; this is the safety net for a plain configure. Multi-config generators
# (Visual Studio, Xcode) pick per-build and are left alone.
get_property(_isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _isMultiConfig AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "No CMAKE_BUILD_TYPE set — defaulting to Release")
endif()

# ── Dependencies (resolved by the vcpkg toolchain — see CMakePresets.json) ──
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Svg)
# QAR entry payloads and .ftexs chunks are zlib streams.
find_package(ZLIB REQUIRED)

qt_standard_project_setup()

# AUTORCC is NOT enabled by qt_standard_project_setup() — without this the
# .qrc is accepted silently and never compiled (learned the hard way on the D4
# browser: the window icon just stayed default).
set(CMAKE_AUTORCC ON)

# The exe-file icon resource is Windows-only (rc.exe). rc.exe reads app.ico
# THROUGH app.rc; tell ninja about the hidden dependency or icon swaps look
# like no-ops.
set(FOX_WIN_RES)
if(WIN32)
    set(FOX_WIN_RES res/app.rc)
    set_property(SOURCE res/app.rc APPEND PROPERTY
                 OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")
endif()

qt_add_executable(FOXAssetBrowser WIN32
    ${FOX_WIN_RES}
    res/app.qrc
    src/main.cpp

    src/app/AppLog.h            src/app/AppLog.cpp
    src/app/LogConsole.h        src/app/LogConsole.cpp
    src/app/AppPaths.h
    src/app/Config.h            src/app/Config.cpp
    src/app/ExportNotifier.h
    src/app/Hotkeys.h
    src/app/StatusLine.h
    src/app/MainWindow.h        src/app/MainWindow.cpp
    src/app/SehGuard.h          src/app/SehGuard.cpp
    src/app/SettingsDialog.h    src/app/SettingsDialog.cpp

    src/anim/AnimMath.h         src/anim/AnimMath.cpp
    src/anim/AnimPose.h         src/anim/AnimPose.cpp
    src/anim/FrdvFile.h         src/anim/FrdvFile.cpp
    src/anim/RigBind.h          src/anim/RigBind.cpp
    src/anim/AnimBind.h         src/anim/AnimBind.cpp

    src/audio/WemFile.h         src/audio/WemFile.cpp

    src/fox/BcDecode.h          src/fox/BcDecode.cpp
    src/fox/FoxCity.h           src/fox/FoxCity.cpp
    src/fox/FoxHash.h           src/fox/FoxHash.cpp
    src/fox/FmdlFile.h          src/fox/FmdlFile.cpp
    src/fox/FcnpFile.h          src/fox/FcnpFile.cpp
    src/fox/DfrmFile.h          src/fox/DfrmFile.cpp
    src/fox/FovaFile.h          src/fox/FovaFile.cpp
    src/fox/GrxlaFile.h         src/fox/GrxlaFile.cpp
    src/fox/FoxMaterial.h       src/fox/FoxMaterial.cpp
    src/fox/FmttFile.h          src/fox/FmttFile.cpp
    src/fox/LangFile.h          src/fox/LangFile.cpp
    src/fox/Fox2File.h          src/fox/Fox2File.cpp
    src/fox/Fox2Refs.h          src/fox/Fox2Refs.cpp
    src/fox/FoxZlib.h           src/fox/FoxZlib.cpp
    src/fox/FpkFile.h           src/fox/FpkFile.cpp
    src/fox/FrigFile.h          src/fox/FrigFile.cpp
    src/fox/FtexFile.h          src/fox/FtexFile.cpp
    src/fox/FtexWriter.h      src/fox/FtexWriter.cpp
    src/fox/GaniAnim.h          src/fox/GaniAnim.cpp
    src/fox/GzsFile.h           src/fox/GzsFile.cpp
    src/fox/MtarFile.h          src/fox/MtarFile.cpp
    src/fox/PftxsFile.h         src/fox/PftxsFile.cpp
    src/fox/QarFile.h           src/fox/QarFile.cpp
    src/fox/SbpFile.h           src/fox/SbpFile.cpp

    src/gl/ThumbnailRenderer.h
    src/gl/ThumbnailRenderer.cpp
    src/gl/GLModelWidget.h      src/gl/GLModelWidget.cpp
    src/gl/ViewEnvironment.h    src/gl/ViewEnvironment.cpp

    src/view/ViewGlyphs.h
    src/view/ViewportPanel.h    src/view/ViewportPanel.cpp
    src/view/AnimationsPanel.h  src/view/AnimationsPanel.cpp
    src/view/StringsPanel.h     src/view/StringsPanel.cpp
    src/view/ViewportBar.h      src/view/ViewportBar.cpp
    src/view/ViewportHud.h      src/view/ViewportHud.cpp
    src/view/FilterPopup.h
    src/view/FileInfoPanel.h
    src/view/FileInfoPanel.cpp
    src/view/TextureInfoPanel.h src/view/TextureInfoPanel.cpp
    src/view/ChannelStrip.h     src/view/ChannelStrip.cpp
    src/view/TipBar.h           src/view/TipBar.cpp
    src/view/PanelBox.h
    src/view/NPanel.h           src/view/NPanel.cpp
    src/view/InfoPanel.h        src/view/InfoPanel.cpp
    src/view/DisplayModeButton.h src/view/DisplayModeButton.cpp
    src/view/ModelOutliner.h    src/view/ModelOutliner.cpp
    src/view/ViewportGizmo.h    src/view/ViewportGizmo.cpp
    src/view/FilterChips.h      src/view/FilterChips.cpp
    src/view/AttachmentsPanel.h src/view/AttachmentsPanel.cpp

    src/export/GifEncoder.h     src/export/GifEncoder.cpp
    src/export/ViewCapture.h    src/export/ViewCapture.cpp
    src/export/ExportOptions.h  src/export/ExportOptions.cpp
    src/export/BulkLedger.h     src/export/BulkLedger.cpp

    src/index/NameCatalog.h      src/index/NameCatalog.cpp
    src/index/EquipCatalog.h     src/index/EquipCatalog.cpp
    src/index/IconCatalog.h      src/index/IconCatalog.cpp
    src/index/LayerColors.h      src/index/LayerColors.cpp
    src/index/MaterialPresets.h  src/index/MaterialPresets.cpp
    src/index/TexThumbCache.h    src/index/TexThumbCache.cpp
    src/index/PartCatalog.h
    src/index/CharacterCatalog.h src/index/CharacterCatalog.cpp
    src/index/MechaCatalog.h     src/index/MechaCatalog.cpp
    src/index/BuildTimer.h
    src/index/AnimCatalog.h      src/index/AnimCatalog.cpp
    src/index/MgoGearConfig.h    src/index/MgoGearConfig.cpp
    src/util/AnimCombo.h         src/util/AnimCombo.cpp
    src/index/WeaponCatalog.h    src/index/WeaponCatalog.cpp
    src/index/PlayerCatalog.h
    src/index/PlayerCatalog.cpp
    src/index/AvatarPresets.h
    src/index/AvatarPresets.cpp
    src/index/AvatarTextures.h
    src/index/AvatarTextures.cpp
    src/index/ModelTags.h        src/index/ModelTags.cpp
    src/index/TextureUsers.h     src/index/TextureUsers.cpp
    src/index/GameId.h
    src/index/GameId.cpp
    src/index/ArchiveIndex.h    src/index/ArchiveIndex.cpp

    src/model/GlbExporter.h     src/model/GlbExporter.cpp

    src/preview/ImageView.h     src/preview/ImageView.cpp
    src/preview/ModelLoader.h   src/preview/ModelLoader.cpp
    src/preview/MaterialInspector.h src/preview/MaterialInspector.cpp
    src/preview/PreviewPane.h   src/preview/PreviewPane.cpp

    src/tabs/BulkExtractorTab.h src/tabs/BulkExtractorTab.cpp
    src/tabs/CustomizeTab.h     src/tabs/CustomizeTab.cpp
    src/tabs/CustomizeTab_Weapon.cpp
    src/tabs/FilesTab.h         src/tabs/FilesTab.cpp
    src/tabs/ModelsTab.h        src/tabs/ModelsTab.cpp
    src/tabs/TexturesTab.h      src/tabs/TexturesTab.cpp

    src/util/Extract.h          src/util/Extract.cpp
    src/util/ModFolder.h        src/util/ModFolder.cpp
    src/util/ZipWriter.h        src/util/ZipWriter.cpp
    src/util/ModPackage.h       src/util/ModPackage.cpp
    src/app/MgsvMetaDialog.h    src/app/MgsvMetaDialog.cpp
    src/util/ExportLayout.h
    src/util/PartMenu.h
    src/util/PartMenu.cpp
    src/util/MenuText.h
    src/util/MenuContext.h
    src/util/TableCopy.h
    src/util/ExportActions.h    src/util/ExportActions.cpp
    src/util/SearchableCombo.h  src/util/SearchableCombo.cpp
    src/util/SceneTree.h        src/util/SceneTree.cpp
    src/util/CheckStyle.h       src/util/CheckStyle.cpp
    src/util/RowShading.h       src/util/RowShading.cpp
    src/util/HoverPreview.h     src/util/HoverPreview.cpp
    src/util/PanelPersist.h
    src/util/QueryTerm.h
    src/util/SearchBox.h
    src/util/SearchQuery.h
    src/util/TagFilterPopup.h    src/util/TagFilterPopup.cpp
    src/util/ShadowDisplay.h
)

target_include_directories(FOXAssetBrowser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(FOXAssetBrowser PRIVATE
    FOXAB_VERSION="${PROJECT_VERSION}"
    NOMINMAX
)

# /EHa: C++ try/catch can intercept structured exceptions (access violations
# from corrupt archive bytes) so one bad file cannot kill the session.
if(MSVC)
    target_compile_options(FOXAssetBrowser PRIVATE /EHa /MP)
endif()

# Front-load the ubiquitous Qt/std headers for faster incremental rebuilds.
target_precompile_headers(FOXAssetBrowser PRIVATE
    <QtCore/QString>
    <QtCore/QByteArray>
    <QtCore/QStringList>
    <QtCore/QHash>
    <QtCore/QMap>
    <QtCore/QList>
    <QtCore/QVector>
    <QtCore/QFile>
    <QtCore/QDir>
    <QtCore/QFileInfo>
    <QtCore/QDebug>
    <QtCore/QObject>
    <QtGui/QImage>
    <QtWidgets/QWidget>
    <vector>
    <memory>
    <algorithm>
    <cstdint>
)

target_link_libraries(FOXAssetBrowser PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Svg
    ZLIB::ZLIB
)
if(WIN32)
    target_link_libraries(FOXAssetBrowser PRIVATE winmm)   # PlaySound (.wem preview)
endif()

# ── Packaging ────────────────────────────────────────────────────────────────
# Static Qt (x64-windows-static triplet) → one self-contained exe; dynamic Qt →
# run.bat copies the DLLs + plugins beside the exe (windeployqt does not ship
# with vcpkg's Qt).
get_target_property(_qt_core_type Qt6::Core TYPE)
if(_qt_core_type STREQUAL "STATIC_LIBRARY")
    message(STATUS "Qt is STATIC → single self-contained FOXAssetBrowser.exe.")
    set(_fox_wanted_plugins
        Qt6::QWindowsIntegrationPlugin
        Qt6::QWindowsVistaStylePlugin
        Qt6::QJpegPlugin
        Qt6::QICOPlugin
        Qt6::QSvgPlugin
        Qt6::QSvgIconPlugin)
    set(_fox_have_plugins)
    foreach(_p IN LISTS _fox_wanted_plugins)
        if(TARGET ${_p})
            list(APPEND _fox_have_plugins ${_p})
        endif()
    endforeach()
    if(_fox_have_plugins)
        qt_import_plugins(FOXAssetBrowser INCLUDE ${_fox_have_plugins})
    endif()
    if(MSVC)
        set_property(TARGET FOXAssetBrowser PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

install(TARGETS FOXAssetBrowser
    RUNTIME DESTINATION .
    BUNDLE  DESTINATION .)

# ── Console probe (development validation harness, off by default) ───────────
option(FOX_BUILD_PROBE "Build the foxab_probe console format-layer harness" OFF)
if(FOX_BUILD_PROBE)
    add_executable(foxab_probe
        tools/probe.cpp
        src/anim/AnimMath.h src/anim/AnimMath.cpp
        src/anim/AnimPose.h src/anim/AnimPose.cpp
        src/anim/FrdvFile.h src/anim/FrdvFile.cpp
        src/fox/FoxCity.h   src/fox/FoxCity.cpp
        src/fox/FoxHash.h   src/fox/FoxHash.cpp
        src/fox/FmdlFile.h  src/fox/FmdlFile.cpp
        src/fox/FoxZlib.h   src/fox/FoxZlib.cpp
        src/fox/FpkFile.h   src/fox/FpkFile.cpp
        src/fox/FrigFile.h  src/fox/FrigFile.cpp
        src/fox/FtexFile.h  src/fox/FtexFile.cpp
        src/fox/GaniAnim.h  src/fox/GaniAnim.cpp
        src/fox/GzsFile.h   src/fox/GzsFile.cpp
        src/fox/MtarFile.h  src/fox/MtarFile.cpp
        src/fox/PftxsFile.h src/fox/PftxsFile.cpp
        src/fox/QarFile.h   src/fox/QarFile.cpp
        src/fox/SbpFile.h   src/fox/SbpFile.cpp
    )
    target_include_directories(foxab_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(foxab_probe PRIVATE Qt6::Core ZLIB::ZLIB)
endif()
