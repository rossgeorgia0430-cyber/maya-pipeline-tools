#include <maya/MFnPlugin.h>
#include <maya/MGlobal.h>
#include <maya/MObject.h>
#include <maya/MStatus.h>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "RefCheckerCmd.h"
#include "BatchExporterCmd.h"
#include "SafeOpenCmd.h"
#include "SafeLoaderCmd.h"
#include "PluginLog.h"

// Convert UTF-8 std::string to MString safely on Windows
static MString utf8ToMString(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return MString();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return MString(utf8.c_str());
    // wlen includes the null terminator when cbMultiByte=-1.
    std::wstring wstr(wlen, L'\0');
    int ret = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], wlen);
    if (ret <= 0) return MString(utf8.c_str());
    if (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
    return MString(wstr.c_str());
#else
    return MString(utf8.c_str());
#endif
}

static const char* kMenuName = "RefCheckerPluginMenu";

static void createMenu()
{
    // Create a top-level menu on Maya's main menu bar.
    // Build as std::string and convert via utf8ToMString so that Chinese
    // annotation text is preserved on non-UTF-8 Windows (ACP=936/GBK).
    // The source file MUST be saved as UTF-8 (with or without BOM).
    std::string mel;
    mel += "if (`menu -exists " + std::string(kMenuName) + "`) deleteUI " + kMenuName + ";\n";
    mel += "global string $gMainWindow;\n";
    mel += "menu -parent $gMainWindow -tearOff true -label \"Pipeline Tools\" " + std::string(kMenuName) + ";\n";
    mel += u8"menuItem -label \"Open Without References\" -command \"safeOpenScene\" -annotation \"打开场景文件但不加载任何引用，避免因缺失引用导致卡死\";\n";
    mel += u8"menuItem -label \"Reference Checker\" -command \"refChecker\" -annotation \"扫描场景中所有依赖文件（引用、贴图、缓存、音频），检查缺失并批量修复路径\";\n";
    mel += u8"menuItem -label \"Safe Load References\" -command \"safeLoadRefs\" -annotation \"逐个查看和加载/卸载场景中的引用，可移除找不到文件的引用\";\n";
    mel += "menuItem -divider true;\n";
    mel += u8"menuItem -label \"Batch Animation Exporter\" -command \"batchAnimExporter\" -annotation \"批量导出场景中的相机、骨骼动画和 BlendShape 为 FBX 文件\";\n";
    MGlobal::executeCommand(utf8ToMString(mel));
}

static void deleteMenu()
{
    MString cmd;
    cmd += "if (`menu -exists " + MString(kMenuName) + "`) deleteUI " + MString(kMenuName) + ";";
    MGlobal::executeCommand(cmd);
}

__declspec(dllexport) MStatus initializePlugin(MObject obj)
{
    MStatus status;

    MFnPlugin plugin(obj, "RefCheckerPlugin", "1.1.0", "Any", &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    // Register refChecker command
    status = plugin.registerCommand(
        RefCheckerCmd::kCommandName,
        RefCheckerCmd::creator,
        RefCheckerCmd::newSyntax
    );
    if (!status) {
        PluginLog::error("Plugin", "Failed to register command: refChecker");
        return status;
    }

    // Register batchAnimExporter command
    status = plugin.registerCommand(
        BatchExporterCmd::kCommandName,
        BatchExporterCmd::creator,
        BatchExporterCmd::newSyntax
    );
    if (!status) {
        PluginLog::error("Plugin", "Failed to register command: batchAnimExporter");
        return status;
    }

    // Register safeOpenScene command
    status = plugin.registerCommand(
        SafeOpenCmd::kCommandName,
        SafeOpenCmd::creator,
        SafeOpenCmd::newSyntax
    );
    if (!status) {
        PluginLog::error("Plugin", "Failed to register command: safeOpenScene");
        return status;
    }

    // Register safeLoadRefs command
    status = plugin.registerCommand(
        SafeLoaderCmd::kCommandName,
        SafeLoaderCmd::creator,
        SafeLoaderCmd::newSyntax
    );
    if (!status) {
        PluginLog::error("Plugin", "Failed to register command: safeLoadRefs");
        return status;
    }

    // Create menu
    createMenu();

    PluginLog::init();
    PluginLog::logEnvironment();
    PluginLog::info("Plugin", "PipelineTools v1.1.0 loaded successfully.");
    PluginLog::info("Plugin", std::string("Build: ") + __DATE__ + " " + __TIME__);
    return MS::kSuccess;
}

__declspec(dllexport) MStatus uninitializePlugin(MObject obj)
{
    MStatus status;

    MFnPlugin plugin(obj);

    // Delete menu
    deleteMenu();

    // Deregister refChecker command
    status = plugin.deregisterCommand(RefCheckerCmd::kCommandName);
    if (!status) {
        PluginLog::error("Plugin", "Failed to deregister command: refChecker");
        return status;
    }

    // Deregister batchAnimExporter command
    status = plugin.deregisterCommand(BatchExporterCmd::kCommandName);
    if (!status) {
        PluginLog::error("Plugin", "Failed to deregister command: batchAnimExporter");
        return status;
    }

    // Deregister safeOpenScene command
    status = plugin.deregisterCommand(SafeOpenCmd::kCommandName);
    if (!status) {
        PluginLog::error("Plugin", "Failed to deregister command: safeOpenScene");
        return status;
    }

    // Deregister safeLoadRefs command
    status = plugin.deregisterCommand(SafeLoaderCmd::kCommandName);
    if (!status) {
        PluginLog::error("Plugin", "Failed to deregister command: safeLoadRefs");
        return status;
    }

    PluginLog::info("Plugin", "PipelineTools v1.1.0 unloaded.");
    PluginLog::shutdown();
    return MS::kSuccess;
}
