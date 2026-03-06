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

static MStatus createMenu()
{
    std::string mel;
    mel += "if (`menu -exists " + std::string(kMenuName) + "`) deleteUI " + kMenuName + ";\n";
    mel += "global string $gMainWindow;\n";
    mel += "menu -parent $gMainWindow -tearOff true -label \"Pipeline Tools\" " + std::string(kMenuName) + ";\n";
    mel += "menuItem -label \"Open Without References\" -command \"safeOpenScene\" -annotation \"Open a scene without loading references.\";\n";
    mel += "menuItem -label \"Reference Checker\" -command \"refChecker\" -annotation \"Scan dependencies and repair broken paths.\";\n";
    mel += "menuItem -label \"Safe Load References\" -command \"safeLoadRefs\" -annotation \"Inspect, load, unload, or remove scene references safely.\";\n";
    mel += "menuItem -divider true;\n";
    mel += "menuItem -label \"Batch Animation Exporter\" -command \"batchAnimExporter\" -annotation \"Bake and export camera, skeleton, and blendshape animation to FBX.\";\n";
    return MGlobal::executeCommand(utf8ToMString(mel));
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

    PluginLog::init();

    auto rollbackRegistrations = [&plugin]() {
        deleteMenu();
        plugin.deregisterCommand(SafeLoaderCmd::kCommandName);
        plugin.deregisterCommand(SafeOpenCmd::kCommandName);
        plugin.deregisterCommand(BatchExporterCmd::kCommandName);
        plugin.deregisterCommand(RefCheckerCmd::kCommandName);
    };

    status = plugin.registerCommand(
        RefCheckerCmd::kCommandName,
        RefCheckerCmd::creator,
        RefCheckerCmd::newSyntax
    );
    if (!status) {
        PluginLog::error("Plugin", "Failed to register command: refChecker");
        PluginLog::shutdown();
        return status;
    }

    status = plugin.registerCommand(
        BatchExporterCmd::kCommandName,
        BatchExporterCmd::creator,
        BatchExporterCmd::newSyntax
    );
    if (!status) {
        PluginLog::error("Plugin", "Failed to register command: batchAnimExporter");
        rollbackRegistrations();
        PluginLog::shutdown();
        return status;
    }

    status = plugin.registerCommand(
        SafeOpenCmd::kCommandName,
        SafeOpenCmd::creator,
        SafeOpenCmd::newSyntax
    );
    if (!status) {
        PluginLog::error("Plugin", "Failed to register command: safeOpenScene");
        rollbackRegistrations();
        PluginLog::shutdown();
        return status;
    }

    status = plugin.registerCommand(
        SafeLoaderCmd::kCommandName,
        SafeLoaderCmd::creator,
        SafeLoaderCmd::newSyntax
    );
    if (!status) {
        PluginLog::error("Plugin", "Failed to register command: safeLoadRefs");
        rollbackRegistrations();
        PluginLog::shutdown();
        return status;
    }

    status = createMenu();
    if (!status) {
        PluginLog::error("Plugin", "Failed to create Pipeline Tools menu.");
        rollbackRegistrations();
        PluginLog::shutdown();
        return status;
    }

    PluginLog::logEnvironment();
    PluginLog::info("Plugin", "PipelineTools v1.1.0 loaded successfully.");
    PluginLog::info("Plugin", std::string("Build: ") + __DATE__ + " " + __TIME__);
    return MS::kSuccess;
}

__declspec(dllexport) MStatus uninitializePlugin(MObject obj)
{
    MFnPlugin plugin(obj);
    MStatus result = MS::kSuccess;

    deleteMenu();

    MStatus status = plugin.deregisterCommand(RefCheckerCmd::kCommandName);
    if (!status) {
        PluginLog::error("Plugin", "Failed to deregister command: refChecker");
        result = status;
    }

    status = plugin.deregisterCommand(BatchExporterCmd::kCommandName);
    if (!status) {
        PluginLog::error("Plugin", "Failed to deregister command: batchAnimExporter");
        result = status;
    }

    status = plugin.deregisterCommand(SafeOpenCmd::kCommandName);
    if (!status) {
        PluginLog::error("Plugin", "Failed to deregister command: safeOpenScene");
        result = status;
    }

    status = plugin.deregisterCommand(SafeLoaderCmd::kCommandName);
    if (!status) {
        PluginLog::error("Plugin", "Failed to deregister command: safeLoadRefs");
        result = status;
    }

    PluginLog::info("Plugin", "PipelineTools v1.1.0 unloaded.");
    PluginLog::shutdown();
    return result;
}
