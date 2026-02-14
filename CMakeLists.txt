cmake_minimum_required(VERSION 3.12)
project(MayaRefCheckerPlugin VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Maya SDK
# ---------------------------------------------------------------------------
if(NOT DEFINED MAYA_LOCATION)
    if(DEFINED ENV{MAYA_LOCATION})
        set(MAYA_LOCATION $ENV{MAYA_LOCATION})
    else()
        set(MAYA_LOCATION "C:/Program Files/Autodesk/Maya2026")
    endif()
endif()

message(STATUS "MAYA_LOCATION = ${MAYA_LOCATION}")

if(NOT EXISTS "${MAYA_LOCATION}/include/maya/MFn.h")
    message(FATAL_ERROR "Maya SDK not found at ${MAYA_LOCATION}. "
        "Set MAYA_LOCATION to your Maya install directory.")
endif()

set(MAYA_INCLUDE_DIR "${MAYA_LOCATION}/include")
set(MAYA_LIB_DIR     "${MAYA_LOCATION}/lib")

# ---------------------------------------------------------------------------
# Qt6 from Maya's bundled Qt
# ---------------------------------------------------------------------------
set(MAYA_QT_DIR "${MAYA_LOCATION}")

# Qt headers may live inside Maya's include dir or in a separate extracted
# location (Maya 2026 ships them as a zip that must be unpacked first).
if(NOT DEFINED QT_INCLUDE_DIR)
    # Check common locations
    if(EXISTS "${MAYA_LOCATION}/include/QtCore")
        set(QT_INCLUDE_DIR "${MAYA_LOCATION}/include")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/qt_include/QtCore")
        set(QT_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/qt_include")
    else()
        message(FATAL_ERROR
            "Qt headers not found. Extract qt_6.5.3_vc14-include.zip to "
            "${CMAKE_CURRENT_SOURCE_DIR}/qt_include/ or set QT_INCLUDE_DIR.")
    endif()
endif()
message(STATUS "QT_INCLUDE_DIR = ${QT_INCLUDE_DIR}")

# Qt6 modules we need
set(QT6_MODULES Core Gui Widgets)

# Create imported targets for each Qt module
foreach(_mod ${QT6_MODULES})
    if(NOT TARGET Qt6::${_mod})
        add_library(Qt6::${_mod} SHARED IMPORTED)
        set_target_properties(Qt6::${_mod} PROPERTIES
            IMPORTED_IMPLIB "${MAYA_QT_DIR}/lib/Qt6${_mod}.lib"
            IMPORTED_LOCATION "${MAYA_QT_DIR}/bin/Qt6${_mod}.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${QT_INCLUDE_DIR};${QT_INCLUDE_DIR}/Qt${_mod}"
        )
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Maya libraries
# ---------------------------------------------------------------------------
set(MAYA_LIBS
    OpenMaya
    OpenMayaAnim
    OpenMayaFX
    OpenMayaRender
    OpenMayaUI
    Foundation
)

# ---------------------------------------------------------------------------
# MOC setup (manual moc since we don't use find_package(Qt6))
# ---------------------------------------------------------------------------
set(MOC_EXE "${MAYA_QT_DIR}/bin/moc.exe")
if(NOT EXISTS "${MOC_EXE}")
    message(FATAL_ERROR "moc.exe not found at ${MOC_EXE}")
endif()

# Function to run moc on a header
function(maya_moc _header _output)
    get_filename_component(_abs_header "${_header}" ABSOLUTE)
    add_custom_command(
        OUTPUT "${_output}"
        COMMAND "${MOC_EXE}"
            "-I${QT_INCLUDE_DIR}"
            "-I${QT_INCLUDE_DIR}/QtCore"
            "-I${QT_INCLUDE_DIR}/QtGui"
            "-I${QT_INCLUDE_DIR}/QtWidgets"
            "${_abs_header}"
            -o "${_output}"
        DEPENDS "${_abs_header}"
        COMMENT "MOC: ${_header}"
        VERBATIM
    )
endfunction()

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(PLUGIN_SOURCES
    src/pluginMain.cpp
    src/RefCheckerCmd.cpp
    src/RefCheckerUI.cpp
    src/BatchExporterCmd.cpp
    src/BatchExporterUI.cpp
    src/AnimExporter.cpp
    src/SceneScanner.cpp
    src/FileAnalyzer.cpp
    src/NamingUtils.cpp
    src/ExportLogger.cpp
    src/PluginLog.cpp
    src/SafeOpenCmd.cpp
    src/SafeLoaderCmd.cpp
    src/SafeLoaderUI.cpp
)

set(PLUGIN_HEADERS
    src/RefCheckerCmd.h
    src/RefCheckerUI.h
    src/BatchExporterCmd.h
    src/BatchExporterUI.h
    src/AnimExporter.h
    src/SceneScanner.h
    src/FileAnalyzer.h
    src/NamingUtils.h
    src/ExportLogger.h
    src/PluginLog.h
    src/SafeOpenCmd.h
    src/SafeLoaderCmd.h
    src/SafeLoaderUI.h
)

# MOC headers that contain Q_OBJECT
set(MOC_HEADERS
    src/RefCheckerUI.h
    src/BatchExporterUI.h
    src/SafeLoaderUI.h
)

set(MOC_OUTPUTS)
foreach(_hdr ${MOC_HEADERS})
    get_filename_component(_name "${_hdr}" NAME_WE)
    set(_moc_out "${CMAKE_CURRENT_BINARY_DIR}/moc_${_name}.cpp")
    maya_moc("${_hdr}" "${_moc_out}")
    list(APPEND MOC_OUTPUTS "${_moc_out}")
endforeach()

# ---------------------------------------------------------------------------
# Plugin target (.mll)
# ---------------------------------------------------------------------------
add_library(MayaRefCheckerPlugin SHARED
    ${PLUGIN_SOURCES}
    ${PLUGIN_HEADERS}
    ${MOC_OUTPUTS}
)

# Maya .mll extension
set_target_properties(MayaRefCheckerPlugin PROPERTIES
    SUFFIX ".mll"
    PREFIX ""
)

target_compile_definitions(MayaRefCheckerPlugin PRIVATE
    WIN32
    _WIN32
    _WINDOWS
    NT_PLUGIN
    REQUIRE_IOSTREAM
    _CRT_SECURE_NO_WARNINGS
)

# Qt6 requires MSVC to report correct __cplusplus value and strict conformance
target_compile_options(MayaRefCheckerPlugin PRIVATE /Zc:__cplusplus /permissive- /utf-8)

target_include_directories(MayaRefCheckerPlugin PRIVATE
    "${MAYA_INCLUDE_DIR}"
    "${QT_INCLUDE_DIR}"
    "${QT_INCLUDE_DIR}/QtCore"
    "${QT_INCLUDE_DIR}/QtGui"
    "${QT_INCLUDE_DIR}/QtWidgets"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

# Link Maya libs
foreach(_lib ${MAYA_LIBS})
    target_link_libraries(MayaRefCheckerPlugin PRIVATE
        "${MAYA_LIB_DIR}/${_lib}.lib"
    )
endforeach()

# Link Qt6 libs
foreach(_mod ${QT6_MODULES})
    target_link_libraries(MayaRefCheckerPlugin PRIVATE Qt6::${_mod})
endforeach()

# Windows system libs
target_link_libraries(MayaRefCheckerPlugin PRIVATE
    Shlwapi.lib
)

# Export all symbols needed by Maya
set_target_properties(MayaRefCheckerPlugin PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
)

# Install
install(TARGETS MayaRefCheckerPlugin
    RUNTIME DESTINATION plug-ins
    LIBRARY DESTINATION plug-ins
)
