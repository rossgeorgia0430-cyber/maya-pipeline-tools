#include "AnimExporter.h"
#include "NamingUtils.h"
#include "PluginLog.h"

#include <maya/MGlobal.h>
#include <maya/MString.h>
#include <maya/MStringArray.h>
#include <maya/MSelectionList.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MFnDagNode.h>
#include <maya/MPlug.h>
#include <maya/MTime.h>
#include <maya/MDoubleArray.h>

// Debug helpers (declared early so MEL wrappers can log failures)
static void debugInfo(const std::string& msg);
static void debugWarn(const std::string& msg);
static void ensureDir(const std::string& path);


#include <ctime>
#include <sstream>
#include <string_view>
#include <set>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#ifdef _WIN32
// Convert UTF-8 std::string to std::wstring
static std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring wstr(wlen, L'\0');
    int ret = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], wlen);
    if (ret <= 0) return {};
    if (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
    return wstr;
}
#endif

// Convert UTF-8 std::string to MString safely on Windows
static MString utf8ToMString(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return MString();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return MString(utf8.c_str());
    std::wstring wstr(wlen, L'\0');
    int ret = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], wlen);
    if (ret <= 0) return MString(utf8.c_str());
    if (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
    return MString(wstr.c_str());
#else
    return MString(utf8.c_str());
#endif
}

// Convert MString to UTF-8 std::string safely on Windows
static std::string toUtf8(const MString& ms) {
#ifdef _WIN32
    const wchar_t* wstr = ms.asWChar();
    if (!wstr || !*wstr) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string(ms.asChar());
    std::string result(len, '\0');
    int ret = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    if (ret <= 0) return std::string(ms.asChar());
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
#else
    return std::string(ms.asChar());
#endif
}

// Forward declarations for MEL helpers (defined later).
static bool melExec(const std::string& cmd);
static std::string melQueryString(const std::string& cmd);

static std::string basenameNoExt(const std::string& path) {
    std::string p = path;
    for (auto& c : p) {
        if (c == '\\') c = '/';
    }
    size_t slash = p.rfind('/');
    std::string name = (slash != std::string::npos) ? p.substr(slash + 1) : p;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

static std::string sanitizeMayaName(const std::string& in) {
    if (in.empty()) return std::string("exported_camera");
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            (c == '_');
        out.push_back(ok ? c : '_');
    }
    // Maya node names can start with digits, but keeping a stable prefix avoids odd edge cases.
    if (!out.empty() && (out[0] >= '0' && out[0] <= '9')) {
        out = std::string("n_") + out;
    }
    return out;
}

static bool attributeExists(const std::string& node, const std::string& attr) {
    int exists = 0;
    std::string cmd = "attributeQuery -exists \"" + attr + "\" -node \"" + node + "\"";
    MStatus st = MGlobal::executeCommand(utf8ToMString(cmd), exists);
    return (st == MS::kSuccess) && (exists != 0);
}

static bool queryWorldMatrix(const std::string& node, double out16[16]) {
    MDoubleArray arr;
    MStatus st = MGlobal::executeCommand(
        utf8ToMString("xform -q -ws -matrix \"" + node + "\""), arr);
    if (st != MS::kSuccess || arr.length() != 16) {
        return false;
    }
    for (unsigned int i = 0; i < 16; ++i) out16[i] = arr[i];
    return true;
}

static bool setWorldMatrix(const std::string& node, const double m[16]) {
    std::ostringstream cmd;
    cmd << "xform -ws -matrix";
    for (int i = 0; i < 16; ++i) {
        cmd << " " << std::setprecision(15) << m[i];
    }
    cmd << " \"" << node << "\"";
    return melExec(cmd.str());
}

static bool keyTransformAtFrame(const std::string& node, int frame) {
    std::ostringstream cmd;
    cmd << "setKeyframe -t " << frame
        << " -at \"tx\" -at \"ty\" -at \"tz\""
        << " -at \"rx\" -at \"ry\" -at \"rz\""
        << " -at \"sx\" -at \"sy\" -at \"sz\""
        << " \"" << node << "\"";
    return melExec(cmd.str());
}

static bool copyScalarAttr(const std::string& srcNode, const std::string& dstNode, const std::string& attr) {
    if (!attributeExists(srcNode, attr) || !attributeExists(dstNode, attr)) return false;
    std::string src = srcNode + "." + attr;
    std::string dst = dstNode + "." + attr;

    // Prefer live connection (so we can bake driven values). If connection fails, fall back to static copy.
    if (melExec("connectAttr -f \"" + src + "\" \"" + dst + "\"")) {
        return true;
    }

    // Static value copy (best-effort for common scalar types).
    std::string type = melQueryString("getAttr -type \"" + src + "\"");
    if (type == "bool" || type == "byte" || type == "short" || type == "long" || type == "enum") {
        int v = 0;
        if (MGlobal::executeCommand(utf8ToMString("getAttr \"" + src + "\""), v) == MS::kSuccess) {
            std::ostringstream cmd;
            cmd << "setAttr \"" << dst << "\" " << v;
            return melExec(cmd.str());
        }
        return false;
    }

    // Most camera fields are doubles (including doubleAngle).
    double dv = 0.0;
    if (MGlobal::executeCommand(utf8ToMString("getAttr \"" + src + "\""), dv) == MS::kSuccess) {
        std::ostringstream cmd;
        cmd << "setAttr \"" << dst << "\" " << std::setprecision(15) << dv;
        return melExec(cmd.str());
    }
    return false;
}

// Helper: execute MEL command.
static bool melExec(const std::string& cmd) {
    MStatus status = MGlobal::executeCommand(utf8ToMString(cmd));
    if (status != MS::kSuccess) {
        debugWarn(std::string("MEL failed: ") + cmd);
        return false;
    }
    return true;
}

// Helper: execute MEL and return string
static std::string melQueryString(const std::string& cmd) {
    MString result;
    MStatus status = MGlobal::executeCommand(utf8ToMString(cmd), result);
    if (status != MS::kSuccess) {
        debugWarn(std::string("MEL query failed: ") + cmd);
    }
    return toUtf8(result);
}
// Helper: execute MEL and return string array
static std::vector<std::string> melQueryStringArray(const std::string& cmd) {
    MStringArray result;
    MStatus status = MGlobal::executeCommand(utf8ToMString(cmd), result);
    if (status != MS::kSuccess) {
        debugWarn(std::string("MEL query failed: ") + cmd);
    }
    std::vector<std::string> vec;
    for (unsigned int i = 0; i < result.length(); ++i) {
        vec.push_back(toUtf8(result[i]));
    }
    return vec;
}
static bool melQueryStringArrayChecked(const std::string& cmd, std::vector<std::string>& out) {
    MStringArray result;
    MStatus status = MGlobal::executeCommand(utf8ToMString(cmd), result);
    out.clear();
    for (unsigned int i = 0; i < result.length(); ++i) {
        out.push_back(toUtf8(result[i]));
    }
    return status == MS::kSuccess;
}

// Optional per-export debug file (set by BatchExporterUI via env var).
// This is separate from PipelineTools.log and is meant for sharing/export troubleshooting.
static void appendExportDebugFile(const char* level, const std::string& msg) {
    const char* p = std::getenv("MAYA_REF_EXPORT_DEBUG_LOG");
    if (!p || !*p) return;

    std::string path(p);
    if (path.empty()) return;

    // Ensure parent directory exists (best-effort).
    std::string dir = path;
    size_t slash = dir.find_last_of("/\\");
    if (slash != std::string::npos) dir = dir.substr(0, slash);
    if (!dir.empty()) ensureDir(dir);

    // Append a timestamped line. Create file with UTF-8 BOM if empty/new.
#ifdef _WIN32
    std::ofstream ofs(utf8ToWide(path), std::ios::app | std::ios::binary);
#else
    std::ofstream ofs(path.c_str(), std::ios::app | std::ios::binary);
#endif
    if (!ofs.is_open()) return;

    // Compute prior size before writing, so we can add a BOM on a new/empty file.
    int64_t priorSize = 0;
#ifdef _WIN32
    struct _stat st;
    if (_wstat(utf8ToWide(path).c_str(), &st) == 0) {
        priorSize = static_cast<int64_t>(st.st_size);
    }
#else
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        priorSize = static_cast<int64_t>(st.st_size);
    }
#endif

    if (priorSize <= 0) {
        ofs << "\xEF\xBB\xBF"; // UTF-8 BOM for Windows editor auto-detect
    }

    // Reuse PluginLog timestamp format for readability.
    std::time_t t = std::time(nullptr);
    std::tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    tmv = *std::localtime(&t);
#endif
    ofs << "[" << std::put_time(&tmv, "%Y-%m-%d %H:%M:%S") << "][" << level << "][AnimExporter] " << msg << "\n";
}

static void debugInfo(const std::string& msg) {
    PluginLog::info("AnimExporter", msg);
    appendExportDebugFile("Info", msg);
}

static void debugWarn(const std::string& msg) {
    PluginLog::warn("AnimExporter", msg);
    appendExportDebugFile("Warn", msg);
}

// Helper: check if node exists
static bool nodeExists(const std::string& node) {
    int result = 0;
    std::string cmd = "objExists \"" + node + "\"";
    MGlobal::executeCommand(utf8ToMString(cmd), result);
    return result != 0;
}

static bool queryAttrAtTime(const std::string& node,
                            const char* attr,
                            int frame,
                            double& value) {
    std::ostringstream cmd;
    cmd << "getAttr -time " << frame << " \"" << node << "." << attr << "\"";
    MStatus st = MGlobal::executeCommand(utf8ToMString(cmd.str()), value);
    return st == MS::kSuccess;
}

static void debugJointMotionSample(const std::string& tag,
                                   const std::string& joint,
                                   int startFrame,
                                   int endFrame) {
    const char* attrs[] = {"tx", "ty", "tz", "rx", "ry", "rz"};
    double totalDelta = 0.0;
    int okAttrs = 0;

    for (const char* attr : attrs) {
        double v0 = 0.0;
        double v1 = 0.0;
        if (!queryAttrAtTime(joint, attr, startFrame, v0)) continue;
        if (!queryAttrAtTime(joint, attr, endFrame, v1)) continue;
        totalDelta += std::abs(v1 - v0);
        ++okAttrs;
    }

    std::ostringstream dbg;
    dbg << tag << ": joint=" << joint
        << ", sampledAttrs=" << okAttrs
        << ", frameRange=" << startFrame << "-" << endFrame
        << ", totalDelta=" << totalDelta;
    debugInfo(dbg.str());
}

// Log the root joint's world-space position at the start frame.
// Helps diagnose position mismatch issues when importing into UE.
// Note: logs both local-space translate values (from getAttr) and attempts
// world-space via xform. If xform fails, local values are still logged.
static void debugWorldSpacePosition(const std::string& tag,
                                    const std::string& joint,
                                    int startFrame) {
    // Query local-space translation at the start frame (reliable, same as queryAttrAtTime).
    const char* attrs[] = {"tx", "ty", "tz"};
    double local[3] = {0.0, 0.0, 0.0};
    bool localOk = true;
    for (int a = 0; a < 3; ++a) {
        if (!queryAttrAtTime(joint, attrs[a], startFrame, local[a])) {
            localOk = false;
            break;
        }
    }
    if (!localOk) {
        debugWarn(tag + ": failed to query position for " + joint);
        return;
    }
    std::ostringstream dbg;
    dbg << tag << ": joint=" << joint
        << ", frame=" << startFrame
        << std::fixed << std::setprecision(4)
        << ", localPos=(" << local[0] << ", " << local[1] << ", " << local[2] << ")";
    debugInfo(dbg.str());
}


struct FbxContentStats {
    int limbNodes = 0;
    int meshes = 0;
    int animCurves = 0;
    int skins = 0;
    int deformers = 0;
    int skeletons = 0;
    int nodeAttributes = 0;
    int nulls = 0;
    int blendShapes = 0;
};

static int countTokenOccurrences(const std::string& data, const char* token) {
    int count = 0;
    size_t pos = 0;
    const std::string needle(token);
    while ((pos = data.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

static FbxContentStats scanFbxContent(const std::string& fbxPath) {
    FbxContentStats stats;
#ifdef _WIN32
    std::ifstream ifs(utf8ToWide(fbxPath), std::ios::binary);
#else
    std::ifstream ifs(fbxPath.c_str(), std::ios::binary);
#endif
    if (!ifs.is_open()) return stats;

    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    stats.limbNodes = countTokenOccurrences(data, "LimbNode");
    stats.meshes = countTokenOccurrences(data, "Mesh");
    stats.animCurves = countTokenOccurrences(data, "AnimationCurve");
    stats.skins = countTokenOccurrences(data, "Skin");
    stats.deformers = countTokenOccurrences(data, "Deformer");
    stats.skeletons = countTokenOccurrences(data, "Skeleton");
    stats.nodeAttributes = countTokenOccurrences(data, "NodeAttribute");
    stats.nulls = countTokenOccurrences(data, "Null");
    stats.blendShapes = countTokenOccurrences(data, "BlendShape");
    return stats;
}

static void debugFbxContent(const std::string& tag,
                            const std::string& fbxPath,
                            const FbxContentStats& stats) {
    std::ostringstream dbg;
    dbg << tag << ": fbxContent{file='" << fbxPath
        << "', limbNodes=" << stats.limbNodes
        << ", meshes=" << stats.meshes
        << ", animCurves=" << stats.animCurves
        << ", skins=" << stats.skins
        << ", deformers=" << stats.deformers
        << ", skeletons=" << stats.skeletons
        << ", nodeAttrs=" << stats.nodeAttributes
        << ", nulls=" << stats.nulls
        << ", blendShapes=" << stats.blendShapes
        << "}";
    debugInfo(dbg.str());
}

static std::vector<std::string> collectMeshTransformsUnderNode(const std::string& node) {
    std::set<std::string> meshTransforms;
    if (node.empty()) return std::vector<std::string>();

    std::vector<std::string> meshShapes = melQueryStringArray(
        "listRelatives -allDescendents -type \"mesh\" -fullPath \"" + node + "\"");
    for (const auto& shape : meshShapes) {
        std::vector<std::string> parent = melQueryStringArray(
            "listRelatives -parent -fullPath \"" + shape + "\"");
        if (!parent.empty()) {
            meshTransforms.insert(parent[0]);
        }
    }

    return std::vector<std::string>(meshTransforms.begin(), meshTransforms.end());
}

static std::vector<std::string> collectMeshTransformsByNamespace(const std::string& node) {
    std::set<std::string> meshTransforms;
    std::string leaf = node;
    size_t pipePos = leaf.rfind('|');
    if (pipePos != std::string::npos) {
        leaf = leaf.substr(pipePos + 1);
    }
    size_t nsPos = leaf.rfind(':');
    if (nsPos == std::string::npos) {
        return std::vector<std::string>();
    }

    std::string ns = leaf.substr(0, nsPos);
    if (ns.empty()) {
        return std::vector<std::string>();
    }

    std::vector<std::string> meshShapes = melQueryStringArray(
        "ls -long -type \"mesh\" \"" + ns + ":*\"");

    for (const auto& shape : meshShapes) {
        std::vector<std::string> parent = melQueryStringArray(
            "listRelatives -parent -fullPath \"" + shape + "\"");
        if (!parent.empty()) {
            meshTransforms.insert(parent[0]);
        }
    }

    return std::vector<std::string>(meshTransforms.begin(), meshTransforms.end());
}

static std::vector<std::string> collectSkinnedMeshTransformsForJoints(const std::vector<std::string>& joints,
                                                                      int* outSkinClusterCount = nullptr,
                                                                      int* outMeshShapeCount = nullptr) {
    std::set<std::string> skinClusters;
    std::set<std::string> meshShapes;
    std::set<std::string> meshTransforms;

    for (const auto& j : joints) {
        std::vector<std::string> clusters = melQueryStringArray(
            "listConnections -source true -destination false -type \"skinCluster\" \"" + j + "\"");

        // Fallback: some rigs connect skinClusters in both directions or via intermediate nodes.
        if (clusters.empty()) {
            clusters = melQueryStringArray(
                "listConnections -source true -destination true -type \"skinCluster\" \"" + j + "\"");
        }

        skinClusters.insert(clusters.begin(), clusters.end());
    }

    for (const auto& skin : skinClusters) {
        std::vector<std::string> geos = melQueryStringArray("skinCluster -q -g \"" + skin + "\"");
        for (const auto& g : geos) {
            std::vector<std::string> full = melQueryStringArray("ls -long \"" + g + "\"");
            std::string geo = full.empty() ? g : full[0];
            std::string geoType = melQueryString("nodeType \"" + geo + "\"");

            if (geoType == "mesh") {
                meshShapes.insert(geo);
                std::vector<std::string> parent = melQueryStringArray(
                    "listRelatives -parent -fullPath \"" + geo + "\"");
                if (!parent.empty()) {
                    meshTransforms.insert(parent[0]);
                }
            } else if (geoType == "transform") {
                std::vector<std::string> shapes = melQueryStringArray(
                    "listRelatives -children -type \"mesh\" -fullPath \"" + geo + "\"");
                if (!shapes.empty()) {
                    meshShapes.insert(shapes.begin(), shapes.end());
                    meshTransforms.insert(geo);
                }
            }
        }
    }

    if (outSkinClusterCount) *outSkinClusterCount = static_cast<int>(skinClusters.size());
    if (outMeshShapeCount) *outMeshShapeCount = static_cast<int>(meshShapes.size());

    return std::vector<std::string>(meshTransforms.begin(), meshTransforms.end());
}

static void debugSelectionSnapshot(const std::string& tag) {
    std::vector<std::string> selected = melQueryStringArray("ls -sl -long");
    std::vector<std::string> joints = melQueryStringArray("ls -sl -type \"joint\" -long");
    std::vector<std::string> meshShapes = melQueryStringArray("ls -sl -type \"mesh\" -long");

    std::ostringstream dbg;
    dbg << tag << ": selection{total=" << selected.size()
        << ", joints=" << joints.size()
        << ", meshShapes=" << meshShapes.size();

    if (!selected.empty()) {
        dbg << ", sample=";
        size_t limit = (std::min)(selected.size(), static_cast<size_t>(5));
        for (size_t i = 0; i < limit; ++i) {
            if (i > 0) dbg << ";";
            dbg << selected[i];
        }
    }

    dbg << "}";
    debugInfo(dbg.str());
}

static std::string dagLeafName(const std::string& fullPath) {

    size_t pos = fullPath.rfind('|');
    return (pos != std::string::npos) ? fullPath.substr(pos + 1) : fullPath;
}

static std::string stripAllNamespaces(const std::string& name) {
    // Removes all namespace segments: A:B:Node -> Node
    size_t pos = name.rfind(':');
    return (pos != std::string::npos) ? name.substr(pos + 1) : name;
}

static int dagDepth(const std::string& fullPath) {
    return static_cast<int>(std::count(fullPath.begin(), fullPath.end(), '|'));
}

static std::string normalizeRootBoneName(const std::string& bareName) {
    // If the root bone is already "Root"/"root"/"ROOT" (case-insensitive), keep as-is.
    // Otherwise rename to "Root" so UE always sees a consistent root bone name.
    if (bareName.empty()) return bareName;

    std::string lower = bareName;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (lower == "root") return bareName;  // already Root/root/ROOT → keep
    return "Root";                         // anything else → rename to "Root"
}
// Helper: get file size (UTF-8 safe on Windows)
static int64_t getFileSize(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    if (_wstat(utf8ToWide(path).c_str(), &st) == 0) {
        return (int64_t)st.st_size;
    }
#else
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return (int64_t)st.st_size;
    }
#endif
    return 0;
}

// Helper: check if file exists (UTF-8 safe on Windows)
static bool fileExistsOnDisk(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    return (_wstat(utf8ToWide(path).c_str(), &st) == 0 && (st.st_mode & S_IFREG));
#else
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFREG));
#endif
}

// Helper: ensure directory exists (UTF-8 safe on Windows)
static void ensureDir(const std::string& path) {
    if (path.empty()) return;
#ifdef _WIN32
    struct _stat st;
    if (_wstat(utf8ToWide(path).c_str(), &st) == 0) return;
    _wmkdir(utf8ToWide(path).c_str());
#else
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return;
    mkdir(path.c_str(), 0755);
#endif
}

// Helper: get directory from path
static std::string getDirname(const std::string& path) {
    std::string p = path;
    for (auto& c : p) {
        if (c == '\\') c = '/';
    }
    size_t pos = p.rfind('/');
    if (pos == std::string::npos) return ".";
    return p.substr(0, pos);
}

// Helper: normalize path for MEL (forward slashes)
static std::string melPath(const std::string& path) {
    std::string p = path;
    for (auto& c : p) {
        if (c == '\\') c = '/';
    }
    return p;
}

// Helper: make a result struct
static ExportResult makeResult(bool success,
                               const std::string& filePath = "",
                               int64_t fileSize = 0,
                               double duration = 0.0,
                               const std::vector<std::string>& warnings = {},
                               const std::vector<std::string>& errors = {}) {
    ExportResult r;
    r.success = success;
    r.filePath = filePath;
    r.fileSize = fileSize;
    r.duration = duration;
    r.warnings = warnings;
    r.errors = errors;
    return r;
}

namespace AnimExporter {

bool ensureFbxPlugin() {
    int loaded = 0;
    MGlobal::executeCommand("pluginInfo -q -loaded \"fbxmaya\"", loaded);
    if (!loaded) {
        MStatus status = MGlobal::executeCommand("loadPlugin \"fbxmaya\"");
        if (status != MS::kSuccess) {
            PluginLog::warn("AnimExporter", "Failed to load fbxmaya plugin");
            return false;
        }
    }
    return true;
}

void setFbxExportDefaults() {
    melExec("FBXExportSmoothingGroups -v true");
    melExec("FBXExportSmoothMesh -v false");
    melExec("FBXExportReferencedAssetsContent -v false");
    melExec("FBXExportSkins -v true");
    melExec("FBXExportShapes -v true");
    melExec("FBXExportAnimationOnly -v false");
    melExec("FBXExportBakeComplexAnimation -v true");
    melExec("FBXExportConstraints -v false");
    melExec("FBXExportInputConnections -v false");
    melExec("FBXExportCameras -v true");
    melExec("FBXExportLights -v false");
    melExec("FBXExportEmbeddedTextures -v false");
    melExec("FBXExportFileVersion -v FBX201800");
    melExec("FBXExportUpAxis y");
}

void setFbxBakeRange(int start, int end) {
    std::ostringstream oss;
    oss << "FBXExportBakeComplexStart -v " << start;
    melExec(oss.str());

    oss.str("");
    oss << "FBXExportBakeComplexEnd -v " << end;
    melExec(oss.str());

    melExec("FBXExportBakeComplexStep -v 1");
    melExec("FBXExportBakeResampleAnimation -v true");
}

double querySceneFps() {
    // Use Maya's time conversion rather than unit-string heuristics.
    // This correctly handles ntsc/ntscf, as well as custom units like "30fps" or "29.97fps".
    const double fps = MTime(1.0, MTime::kSeconds).as(MTime::uiUnit());
    if (fps > 0.0) return fps;
    return 30.0; // fallback
}

std::string setSceneTimeUnit(double fps) {
    std::string prevUnit = melQueryString("currentUnit -q -time");

    // Map fps to Maya time unit name
    std::string targetUnit;
    int ifps = static_cast<int>(fps + 0.5);

    // Prefer exact integer units when requested, but fall back to legacy aliases if unavailable.
    if (ifps == 30 || ifps == 60) {
        std::ostringstream cmd;
        cmd << "currentUnit -time \"" << ifps << "fps\"";
        if (melExec(cmd.str())) {
            debugInfo("setSceneTimeUnit: set to " + std::to_string(ifps) + "fps (exact), prev=" + prevUnit);
            return prevUnit;
        }
        targetUnit = (ifps == 30) ? "ntsc" : "ntscf";
        melExec("currentUnit -time \"" + targetUnit + "\"");
        debugInfo("setSceneTimeUnit: set to " + targetUnit + " (fallback for " + std::to_string(ifps) + "fps), prev=" + prevUnit);
        return prevUnit;
    }

    if (ifps == 24)      targetUnit = "film";
    else if (ifps == 25) targetUnit = "pal";
    else if (ifps == 48) targetUnit = "show";
    else if (ifps == 50) targetUnit = "palf";
    else {
        // For non-standard fps, use the fps unit with value
        std::ostringstream cmd;
        cmd << "currentUnit -time \"" << ifps << "fps\"";
        melExec(cmd.str());
        debugInfo("setSceneTimeUnit: set to " + std::to_string(ifps) + "fps (custom), prev=" + prevUnit);
        return prevUnit;
    }

    melExec("currentUnit -time \"" + targetUnit + "\"");
    debugInfo("setSceneTimeUnit: set to " + targetUnit + " (" + std::to_string(ifps) + "fps), prev=" + prevUnit);
    return prevUnit;
}

void restoreSceneTimeUnit(const std::string& previousUnit) {
    if (!previousUnit.empty()) {
        melExec("currentUnit -time \"" + previousUnit + "\"");
        debugInfo("restoreSceneTimeUnit: restored to " + previousUnit);
    }
}

std::set<int> batchBakeAll(const std::vector<ExportItem>& selectedItems,
                           int startFrame, int endFrame) {
    std::vector<std::string> transformNodes;
    std::vector<std::string> bsAttrs;
    std::set<int> failedIndices;
    std::set<std::string> seenNodes;
    std::set<std::string> seenAttrs;
    time_t batchStartTime = std::time(nullptr);

    for (int idx = 0; idx < static_cast<int>(selectedItems.size()); ++idx) {
        const ExportItem& item = selectedItems[idx];

        if (item.type == "camera") {
            if (!nodeExists(item.node)) {
                PluginLog::warn("AnimExporter", "BatchBake: Camera node missing: " + item.node);
                failedIndices.insert(idx);
                continue;
            }
            // Camera also gets FBX bake during exportCameraFbx, so skip the global bake pass
            // to avoid double-bake on long shots.
            {
                std::ostringstream dbg;
                dbg << "batchBakeAll: camera=" << item.node << ", skipped=true";
                debugInfo(dbg.str());
            }

        } else if (item.type == "skeleton" || item.type == "skeleton+blendshape") {
            if (!nodeExists(item.node)) {
                PluginLog::warn("AnimExporter", "BatchBake: Skeleton root missing: " + item.node);
                failedIndices.insert(idx);
                continue;
            }
            // Do NOT bake skeleton joints in the global batch pass.
            // Many production rigs lock or drive joint channels; baking all joints first can
            // accidentally flatten the motion to static keys. Skeleton baking is handled in
            // exportSkeletonFbx/exportSkeletonFbxViaDuplicate per-rig.
            std::string listCmd = "listRelatives -allDescendents -type \"joint\" -fullPath \"" + item.node + "\"";
            std::vector<std::string> allJoints = melQueryStringArray(listCmd);
            allJoints.push_back(item.node);
            {
                std::ostringstream dbg;
                dbg << "batchBakeAll: skeletonRoot=" << item.node
                    << ", joints=" << allJoints.size()
                    << ", skipped=true";
                debugInfo(dbg.str());
            }

            // If skeleton has BS weight attrs attached, collect them for unified bake
            if (!item.bsWeightAttrs.empty()) {
                for (const auto& attr : item.bsWeightAttrs) {
                    if (seenAttrs.count(attr) == 0) {
                        seenAttrs.insert(attr);
                        bsAttrs.push_back(attr);
                    }
                }
                {
                    std::ostringstream dbg;
                    dbg << "batchBakeAll: skeleton '" << item.name << "' has "
                        << item.bsWeightAttrs.size() << " BS weight attrs queued for bake";
                    debugInfo(dbg.str());
                }
            }

        } else if (item.type == "blendshape") {
            if (!nodeExists(item.node)) {
                PluginLog::warn("AnimExporter", "BatchBake: Mesh node missing: " + item.node);
                failedIndices.insert(idx);
                continue;
            }            // Find blendShape nodes in history (robust path).
            std::string histCmd = "listHistory -pruneDagObjects true \"" + item.node + "\"";
            std::vector<std::string> history;
            if (!melQueryStringArrayChecked(histCmd, history)) {
                debugWarn("batchBakeAll: listHistory failed on mesh: " + item.node);
                failedIndices.insert(idx);
                continue;
            }

            bool foundBS = false;
            int blendShapeCount = 0;
            for (const auto& histNode : history) {
                std::string typeCmd = "nodeType \"" + histNode + "\"";
                MString nodeType;
                MGlobal::executeCommand(utf8ToMString(typeCmd), nodeType);
                if (toUtf8(nodeType) != "blendShape") continue;

                foundBS = true;
                ++blendShapeCount;

                std::string wcCmd = "blendShape -q -weightCount \"" + histNode + "\"";
                int weightCount = 0;
                MGlobal::executeCommand(utf8ToMString(wcCmd), weightCount);

                for (int i = 0; i < weightCount; ++i) {
                    std::ostringstream attrCmd;
                    attrCmd << "aliasAttr -q \"" << histNode << ".weight[" << i << "]\"";
                    MString alias;
                    MStatus status = MGlobal::executeCommand(utf8ToMString(attrCmd.str()), alias);

                    std::string attrName;
                    if (status == MS::kSuccess && alias.length() > 0) {
                        attrName = histNode + "." + toUtf8(alias);
                    } else {
                        std::ostringstream fallback;
                        fallback << histNode << ".weight[" << i << "]";
                        attrName = fallback.str();
                    }
                    if (seenAttrs.count(attrName) == 0) {
                        seenAttrs.insert(attrName);
                        bsAttrs.push_back(attrName);
                    }
                }
            }
            {
                std::ostringstream dbg;
                dbg << "batchBakeAll: mesh=" << item.node
                    << ", historyNodes=" << history.size()
                    << ", blendShapeNodes=" << blendShapeCount;
                debugInfo(dbg.str());
            }

            if (!foundBS) {
                PluginLog::warn("AnimExporter", "BatchBake: No blendShape found on: " + item.node);
                failedIndices.insert(idx);
            }

        }
    }

    if (transformNodes.empty() && bsAttrs.empty()) {
        PluginLog::info("AnimExporter", "BatchBake: Nothing to bake.");
        return failedIndices;
    }

    {
        std::ostringstream msg;
        msg << "BatchBake: Baking " << transformNodes.size()
            << " transform nodes, " << bsAttrs.size()
            << " blendShape attrs, frames " << startFrame << "-" << endFrame;
        PluginLog::info("AnimExporter", msg.str());
    }

    // Bake transform nodes collected in batch pass (currently cameras only) in ONE call.
    // Attribute list includes focalLength for camera exports.
    if (!transformNodes.empty()) {
        std::ostringstream cmd;
        cmd << "bakeResults -simulation true"
            << " -time \"" << startFrame << ":" << endFrame << "\""
            << " -sampleBy 1"
            << " -oversamplingRate 1"
            << " -disableImplicitControl true"
            << " -preserveOutsideKeys false"
            << " -sparseAnimCurveBake false"
            << " -removeBakedAttributeFromLayer false"
            << " -bakeOnOverrideLayer false"
            << " -minimizeRotation true"
            << " -attribute \"tx\" -attribute \"ty\" -attribute \"tz\""
            << " -attribute \"rx\" -attribute \"ry\" -attribute \"rz\""
            << " -attribute \"sx\" -attribute \"sy\" -attribute \"sz\""
            << " -attribute \"focalLength\"";
        for (const auto& node : transformNodes) {
            cmd << " \"" << node << "\"";
        }
        time_t t0 = std::time(nullptr);
        melExec(cmd.str());
        double sec = std::difftime(std::time(nullptr), t0);
        {
            std::ostringstream dbg;
            dbg << "batchBakeAll: transformBakeDuration=" << sec << "s"
                << ", nodes=" << transformNodes.size();
            debugInfo(dbg.str());
        }
        PluginLog::info("AnimExporter", "BatchBake: Transform nodes baked.");
    }

    // Bake ALL blendshape weight attrs in ONE call.
    if (!bsAttrs.empty()) {
        std::ostringstream cmd;
        cmd << "bakeResults -simulation true"
            << " -time \"" << startFrame << ":" << endFrame << "\""
            << " -sampleBy 1"
            << " -oversamplingRate 1"
            << " -disableImplicitControl true"
            << " -preserveOutsideKeys false"
            << " -sparseAnimCurveBake false";
        for (const auto& attr : bsAttrs) {
            cmd << " \"" << attr << "\"";
        }
        time_t t0 = std::time(nullptr);
        melExec(cmd.str());
        double sec = std::difftime(std::time(nullptr), t0);
        {
            std::ostringstream dbg;
            dbg << "batchBakeAll: blendShapeBakeDuration=" << sec << "s"
                << ", attrs=" << bsAttrs.size();
            debugInfo(dbg.str());
        }
        PluginLog::info("AnimExporter", "BatchBake: BlendShape attrs baked.");
    }

    double batchSec = std::difftime(std::time(nullptr), batchStartTime);
    {
        std::ostringstream dbg;
        dbg << "batchBakeAll: totalDuration=" << batchSec << "s";
        debugInfo(dbg.str());
    }
    PluginLog::info("AnimExporter", "BatchBake: Batch bake complete.");
    return failedIndices;
}

static bool isReferencedNode(const MObject& obj) {
    if (obj.isNull()) return false;
    MFnDependencyNode fn(obj);
    return fn.isFromReferencedFile();
}

static void unlockTransformChannels(const std::vector<std::string>& nodes) {
    if (nodes.empty()) return;

    static const char* kAttrs[] = {
        "translateX", "translateY", "translateZ",
        "rotateX", "rotateY", "rotateZ",
        "scaleX", "scaleY", "scaleZ"
    };

    int unlockedPlugs = 0;
    int failedPlugs = 0;

    MSelectionList sel;
    for (const auto& n : nodes) {
        sel.add(MString(n.c_str()));
    }

    for (unsigned int i = 0; i < sel.length(); ++i) {
        MObject obj;
        if (sel.getDependNode(i, obj) != MS::kSuccess) continue;

        MFnDependencyNode fn(obj);
        for (const char* attr : kAttrs) {
            MStatus st;
            MPlug plug = fn.findPlug(attr, true, &st);
            if (st != MS::kSuccess) {
                ++failedPlugs;
                continue;
            }
            if (plug.setLocked(false) != MS::kSuccess) {
                ++failedPlugs;
                continue;
            }
            plug.setKeyable(true);
            ++unlockedPlugs;
        }
    }

    std::ostringstream dbg;
    dbg << "unlockTransformChannels: nodes=" << nodes.size()
        << ", unlockedPlugs=" << unlockedPlugs
        << ", failedPlugs=" << failedPlugs;
    debugInfo(dbg.str());
}

static ExportResult exportSkeletonFbxViaDuplicate(const std::string& srcRootJoint,
                                                 const std::string& outputPath,
                                                 int startFrame, int endFrame,
                                                 const FbxExportOptions& opts) {
    std::vector<std::string> warnings;
    time_t startTime = std::time(nullptr);

    MObject dupRootObj;
    std::string dupRoot;

    auto cleanupDup = [&]() {
        try {
            if (!dupRootObj.isNull()) {
                MFnDagNode fn(dupRootObj);
                std::string p = toUtf8(fn.fullPathName());
                if (!p.empty()) {
                    melExec("delete \"" + p + "\"");
                }
            } else if (!dupRoot.empty()) {
                melExec("delete \"" + dupRoot + "\"");
            }
        } catch (...) {
        }
    };

    try {
        {
            std::ostringstream dbg;
            dbg << "exportSkeletonFbxViaDuplicate: srcRoot=" << srcRootJoint
                << ", output=" << outputPath
                << ", range=" << startFrame << "-" << endFrame;
            debugInfo(dbg.str());
        }

        std::string outDir = getDirname(outputPath);
        if (!outDir.empty()) ensureDir(outDir);

        // 1) Duplicate joint hierarchy (works even if the source is referenced/read-only)
        std::vector<std::string> dupRoots;
        if (!melQueryStringArrayChecked("duplicate -rc \"" + srcRootJoint + "\"", dupRoots) || dupRoots.empty()) {
            melQueryStringArrayChecked("duplicate \"" + srcRootJoint + "\"", dupRoots);
        }
        if (dupRoots.empty()) {
            return makeResult(false, outputPath, 0, 0.0, warnings,
                              {"Failed to duplicate skeleton root: " + srcRootJoint});
        }
        dupRoot = dupRoots[0];

        {
            MSelectionList sel;
            sel.add(MString(dupRoot.c_str()));
            sel.getDependNode(0, dupRootObj);
        }

        // 2) Make the duplicate root top-level to avoid exporting extra group/null parents
        melExec("parent -world \"" + dupRoot + "\"");
        if (!dupRootObj.isNull()) {
            MFnDagNode fn(dupRootObj);
            dupRoot = toUtf8(fn.fullPathName());
        }
        // If AnimationOnly(UI)=true, ensure the temporary duplicate hierarchy contains joints only.
        // Some rigs parent meshes/controllers under joints; FBXExport -s includes descendants, which would
        // accidentally export meshes even when we only select joints.
        if (opts.skelAnimationOnly) {
            std::vector<std::string> meshShapes = melQueryStringArray(
                "listRelatives -allDescendents -type \"mesh\" -fullPath \"" + dupRoot + "\"");

            std::set<std::string> meshParentTransforms;
            int meshShapesDeleted = 0;
            int meshParentsDeleted = 0;

            for (const auto& shape : meshShapes) {
                std::vector<std::string> parents = melQueryStringArray(
                    "listRelatives -parent -fullPath \"" + shape + "\"");
                if (parents.empty()) continue;

                std::string parent = parents[0];
                std::string parentType = melQueryString("nodeType \"" + parent + "\"");
                if (parentType == "joint") {
                    // Rare case: mesh shape directly under a joint. Delete the shape only.
                    if (melExec("delete \"" + shape + "\"")) {
                        ++meshShapesDeleted;
                    }
                } else {
                    meshParentTransforms.insert(parent);
                }
            }

            for (const auto& t : meshParentTransforms) {
                if (melExec("delete \"" + t + "\"")) {
                    ++meshParentsDeleted;
                }
            }

            std::ostringstream dbg;
            dbg << "exportSkeletonFbxViaDuplicate: removedMeshes{shapesFound=" << meshShapes.size()
                << ", shapesDeleted=" << meshShapesDeleted
                << ", parentTransformsDeleted=" << meshParentsDeleted << "}";
            debugInfo(dbg.str());
        }

        // 3) Collect original + duplicate joint lists (order must match)
        std::vector<std::string> origJoints = melQueryStringArray(
            "listRelatives -allDescendents -type \"joint\" -fullPath \"" + srcRootJoint + "\"");
        origJoints.push_back(srcRootJoint);

        std::vector<std::string> dupJoints = melQueryStringArray(
            "listRelatives -allDescendents -type \"joint\" -fullPath \"" + dupRoot + "\"");
        dupJoints.push_back(dupRoot);

        {
            std::ostringstream dbg;
            dbg << "exportSkeletonFbxViaDuplicate: origJoints=" << origJoints.size()
                << ", dupJoints=" << dupJoints.size();
            debugInfo(dbg.str());
        }

        debugJointMotionSample("exportSkeletonFbxViaDuplicate: sourceRootMotion",
                               srcRootJoint, startFrame, endFrame);
        debugWorldSpacePosition("exportSkeletonFbxViaDuplicate: sourceRootWorldPos",
                                srcRootJoint, startFrame);

        if (origJoints.size() != dupJoints.size() || dupJoints.empty()) {
            cleanupDup();
            return makeResult(false, outputPath, 0, 0.0, warnings,
                              {"Duplicate skeleton joint count mismatch"});
        }

        // 4) Constrain duplicates to originals.
        // We rely on FBX bake (BakeComplex) to sample these constraints during export.
        // This avoids doing a per-rig Maya bake pass (which is extremely slow for long shots).
        unlockTransformChannels(dupJoints);

        std::vector<std::string> constraints;
        constraints.reserve(origJoints.size());

        int created = 0;
        int returnedZero = 0;
        int failed = 0;
        int failedSamples = 0;

        for (size_t i = 0; i < origJoints.size(); ++i) {
            std::vector<std::string> c;

            // Use parentConstraint for ALL joints so both translation and rotation
            // are transferred. Previously child joints used orientConstraint which
            // dropped translation, causing position mismatch for joints with
            // animated translation (stretchy limbs, IK, translate-driven bones).
            std::string cmd = "parentConstraint \"" + origJoints[i] + "\" \"" + dupJoints[i] + "\"";

            bool ok = melQueryStringArrayChecked(cmd, c);
            if (!ok || c.empty()) {
                // Fallback: some rigs behave better with orientConstraint (rotation only).
                std::string fb = "orientConstraint \"" + origJoints[i] + "\" \"" + dupJoints[i] + "\"";
                c.clear();
                ok = melQueryStringArrayChecked(fb, c);
                cmd = fb;
            }

            if (!ok || c.empty()) {
                ++failed;
                if (failedSamples < 5) {
                    std::ostringstream dbg;
                    dbg << "exportSkeletonFbxViaDuplicate: constraintFailed idx=" << i
                        << ", src='" << origJoints[i] << "'"
                        << ", dst='" << dupJoints[i] << "'"
                        << ", srcExists=" << (nodeExists(origJoints[i]) ? "true" : "false")
                        << ", dstExists=" << (nodeExists(dupJoints[i]) ? "true" : "false")
                        << ", cmd='" << cmd << "'";
                    debugWarn(dbg.str());
                    ++failedSamples;
                }
                continue;
            }
            if (c[0] == "0") {
                ++returnedZero;
                continue;
            }
            constraints.push_back(c[0]);
            ++created;
        }

        {
            std::ostringstream dbg;
            dbg << "exportSkeletonFbxViaDuplicate: constraintsCreated=" << created
                << ", failed=" << failed
                << ", returnedZero=" << returnedZero;
            debugInfo(dbg.str());
        }

        if (created <= 0) {
            cleanupDup();
            return makeResult(false, outputPath, 0, 0.0, warnings,
                              {"Failed to create constraints for duplicate skeleton (animation would be static)"});
        }
        if (failed > 0 || returnedZero > 0) {
            std::ostringstream warn;
            warn << "Duplicate constraint creation partial: created=" << created
                 << ", failed=" << failed << ", returnedZero=" << returnedZero;
            warnings.push_back(warn.str());
            debugWarn("exportSkeletonFbxViaDuplicate: " + warn.str());
        }

        // 5) Keep constraints alive until FBX export; plugin bake samples them.

        // 6) Strip namespaces on duplicate joints + normalize root name
        {
            struct WorkItem {
                std::string fullPath;
                int depth;
                bool isRoot;
                std::string desiredBare;
                bool needsRename;
            };

            // Use original joint names as the source of truth. Duplicates may get numeric suffixes
            // (e.g. Joint1), but we want the exported bones to match the original bare names.
            std::vector<WorkItem> work;
            work.reserve(dupJoints.size());

            for (size_t i = 0; i < dupJoints.size(); ++i) {
                WorkItem wi;
                wi.fullPath = dupJoints[i];
                wi.depth = dagDepth(dupJoints[i]);
                wi.isRoot = (i == dupJoints.size() - 1);

                std::string origLeaf = dagLeafName(origJoints[i]);
                std::string desired = stripAllNamespaces(origLeaf);
                if (wi.isRoot) {
                    desired = normalizeRootBoneName(desired);
                }
                wi.desiredBare = desired;

                std::string dupLeaf = dagLeafName(dupJoints[i]);
                wi.needsRename = (stripAllNamespaces(dupLeaf) != desired) || (dupLeaf.find(':') != std::string::npos);
                work.push_back(wi);
            }

            std::sort(work.begin(), work.end(),
                      [](const WorkItem& a, const WorkItem& b) { return a.depth > b.depth; });

            for (const auto& wi : work) {
                if (!wi.needsRename) continue;
                std::string target = ":" + wi.desiredBare; // move to root namespace
                melQueryString("rename \"" + wi.fullPath + "\" \"" + target + "\"");
            }

            // Refresh duplicate root path after renames
            if (!dupRootObj.isNull()) {
                MFnDagNode fn(dupRootObj);
                dupRoot = toUtf8(fn.fullPathName());
            }

            std::vector<std::string> allJoints = melQueryStringArray(
                "listRelatives -allDescendents -type \"joint\" -fullPath \"" + dupRoot + "\"");
            allJoints.push_back(dupRoot);

            int namespacedCount = 0;
            for (const auto& j : allJoints) {
                std::string leaf = dagLeafName(j);
                if (!leaf.empty() && leaf[0] == ':') leaf = leaf.substr(1);
                if (leaf.find(':') != std::string::npos) ++namespacedCount;
            }

            {
                std::ostringstream dbg;
                dbg << "exportSkeletonFbxViaDuplicate: allJointsForExport=" << allJoints.size()
                    << ", namespacedAfterCleanup=" << namespacedCount;
                debugInfo(dbg.str());
            }

            {
                std::ostringstream dbg;
                dbg << "exportSkeletonFbxViaDuplicate: exportedBoneNames=[";
                for (size_t i = 0; i < allJoints.size() && i < 20; ++i) {
                    if (i > 0) dbg << ", ";
                    dbg << dagLeafName(allJoints[i]);
                }
                if (allJoints.size() > 20) dbg << ", ...(" << allJoints.size() << " total)";
                dbg << "]";
                debugInfo(dbg.str());
            }

            if (namespacedCount > 0) {
                cleanupDup();
                return makeResult(false, outputPath, 0, 0.0, warnings,
                                  {"Duplicate skeleton joints still contain namespaces"});
            }

            // Select all duplicate joints for export
            {
                MSelectionList sel;
                for (const auto& j : allJoints) sel.add(MString(j.c_str()));
                MGlobal::setActiveSelectionList(sel, MGlobal::kReplaceList);
                std::vector<std::string> selJoints = melQueryStringArray("ls -sl -type \"joint\"");
                std::ostringstream dbg;
                dbg << "exportSkeletonFbxViaDuplicate: selectionJoints=" << selJoints.size();
                debugInfo(dbg.str());
            }
            debugSelectionSnapshot("exportSkeletonFbxViaDuplicate: preExportSelection");

        }

        debugJointMotionSample("exportSkeletonFbxViaDuplicate: duplicateRootMotionBeforeExport",
                               dupRoot, startFrame, endFrame);
        debugWorldSpacePosition("exportSkeletonFbxViaDuplicate: dupRootWorldPos",
                                dupRoot, startFrame);

        // 7) Export FBX
        setFbxExportDefaults();
        melExec(std::string("FBXExportSkeletonDefinitions -v ") + (opts.skelSkeletonDefs ? "true" : "false"));
        melExec("FBXExportAnimationOnly -v false");
        if (!opts.skelSkeletonDefs) {
            warnings.push_back("SkeletonDefs(UI)=false: FBX skeleton hierarchy metadata may be incomplete in some DCC/engines");
        }
        if (opts.skelAnimationOnly) {
            // Maya FBX exporter's AnimationOnly flag can strip skeleton node attributes
            // and cause engines (e.g. UE) to see only groups/no skeleton. We implement
            // AnimationOnly as "export joints only" (no meshes), but keep full skeleton data.
            warnings.push_back("AnimationOnly(UI)=true: force FBXExportAnimationOnly=false to keep skeleton hierarchy");
            debugWarn("exportSkeletonFbxViaDuplicate: override FBXExportAnimationOnly=false to preserve skeleton hierarchy");
        }
        if (!opts.skelBakeComplex) {
            warnings.push_back("Duplicate skeleton export forces BakeComplex=true to sample constraints");
            debugWarn("exportSkeletonFbxViaDuplicate: overriding BakeComplex=false to true");
        }
        melExec("FBXExportBakeComplexAnimation -v true");
        melExec(std::string("FBXExportConstraints -v ") + (opts.skelConstraints ? "true" : "false"));
        const bool effectiveInputConns = (!opts.skelAnimationOnly) && opts.skelInputConns;
        melExec(std::string("FBXExportInputConnections -v ") + (effectiveInputConns ? "true" : "false"));
        melExec(std::string("FBXExportSkins -v ") + (!opts.skelAnimationOnly ? "true" : "false"));
        melExec(std::string("FBXExportShapes -v ") + (!opts.skelAnimationOnly ? "true" : "false"));
        setFbxBakeRange(startFrame, endFrame);

        melExec("FBXExportFileVersion -v " + opts.fileVersion);
        melExec("FBXExportUpAxis " + opts.upAxis);

        std::string fbxPath = melPath(outputPath);
        const bool fbxExportOk = melExec("FBXExport -f \"" + fbxPath + "\" -s");

        if (!fbxExportOk) {
            for (const auto& c : constraints) {
                melExec("delete \"" + c + "\"");
            }
            cleanupDup();
            double duration = std::difftime(std::time(nullptr), startTime);
            int64_t fileSize = fileExistsOnDisk(outputPath) ? getFileSize(outputPath) : 0;
            return makeResult(false, outputPath, fileSize, duration, warnings,
                              {"FBXExport command failed in duplicate skeleton export"});
        }

        FbxContentStats fbxStats = scanFbxContent(outputPath);
        debugFbxContent("exportSkeletonFbxViaDuplicate", outputPath, fbxStats);

        const int expectedSkeletons = static_cast<int>(dupJoints.size());
        if (expectedSkeletons > 0 && fbxStats.skeletons > 0 &&
            fbxStats.skeletons != expectedSkeletons &&
            fbxStats.skeletons != (expectedSkeletons + 1)) {
            std::ostringstream warn;
            warn << "exportSkeletonFbxViaDuplicate: exported skeleton-count mismatch, expected="
                 << expectedSkeletons << "~" << (expectedSkeletons + 1)
                 << ", actual=" << fbxStats.skeletons;
            warnings.push_back(warn.str());
            debugWarn(warn.str());
        }
        if (fbxStats.skeletons <= 0) {
            warnings.push_back("Duplicate skeleton export contains no 'Skeleton' node attributes");
            debugWarn("exportSkeletonFbxViaDuplicate: skeleton node attribute count is zero");
        }

        // Cleanup constraints created for the temporary duplicate skeleton.
        for (const auto& c : constraints) {
            melExec("delete \"" + c + "\"");
        }

        double duration = std::difftime(std::time(nullptr), startTime);
        int64_t fileSize = fileExistsOnDisk(outputPath) ? getFileSize(outputPath) : 0;
        {
            std::ostringstream dbg;
            dbg << "exportSkeletonFbxViaDuplicate: exported file size=" << fileSize
                << ", duration=" << duration << "s";
            debugInfo(dbg.str());
        }

        // 8) Cleanup duplicate skeleton
        cleanupDup();

        if (fileSize <= 0) {
            return makeResult(false, outputPath, fileSize, duration, warnings,
                              {"Skeleton export produced empty file"});
        }

        if (fbxStats.limbNodes <= 0) {
            return makeResult(false, outputPath, fileSize, duration, warnings,
                              {"Skeleton export did not contain LimbNode bones"});
        }

        warnings.push_back("Skeleton was referenced/read-only; exported via temporary duplicate skeleton");
        warnings.push_back("Namespaces stripped on duplicate skeleton during export");
        return makeResult(true, outputPath, fileSize, duration, warnings);

    } catch (...) {
        cleanupDup();
        double duration = std::difftime(std::time(nullptr), startTime);
        return makeResult(false, outputPath, 0, duration, warnings,
                          {"Skeleton export failed (duplicate path): unknown exception"});
    }
}

ExportResult exportCameraFbx(const std::string& cameraTransform,
                             const std::string& outputPath,
                             int startFrame, int endFrame,
                             const FbxExportOptions& opts) {
    std::vector<std::string> warnings;
    time_t startTime = std::time(nullptr);

    // Temp nodes created for safe camera export. Kept outside try/catch so we can always clean up.
    std::vector<std::string> tmpNodesToDelete;
    auto cleanupTmp = [&]() {
        for (const auto& n : tmpNodesToDelete) {
            if (!n.empty() && nodeExists(n)) {
                melExec("delete \"" + n + "\"");
            }
        }
        tmpNodesToDelete.clear();
    };

    if (!ensureFbxPlugin()) {
        return makeResult(false, outputPath, 0, 0.0, {}, {"fbxmaya plugin load failed"});
    }

    if (!nodeExists(cameraTransform)) {
        return makeResult(false, outputPath, 0, 0.0, {},
                          {"Camera node does not exist: " + cameraTransform});
    }

    try {
        {
            std::ostringstream dbg;
            dbg << "exportCameraFbx: node=" << cameraTransform
                << ", output=" << outputPath
                << ", range=" << startFrame << "-" << endFrame
                << ", opts{fileVersion=" << opts.fileVersion
                << ", upAxis=" << opts.upAxis << "}";
            debugInfo(dbg.str());
        }

        std::string outDir = getDirname(outputPath);
        if (!outDir.empty()) ensureDir(outDir);

        // IMPORTANT:
        // Maya's FBX exporter does not reliably CLIP existing camera anim-curve keys to the
        // BakeComplexStart/End range. In practice we can end up exporting keys far outside
        // the requested range (UE LevelSequence will import them all).
        //
        // Fix:
        // 1) Create a temporary camera.
        // 2) Sample the SOURCE camera at each frame in [startFrame, endFrame] and key the temp camera.
        // 3) Export the temp camera.
        //
        // This avoids constraints (which can fail if channels are locked) and guarantees the FBX only
        // contains keys in the requested range.
        std::string tmpCamXform;
        std::string tmpCamShape;

        if (endFrame < startFrame) std::swap(startFrame, endFrame);
        const int frameCount = (endFrame - startFrame) + 1;

        // Resolve source camera shape (optional; used for shape-attr bake).
        std::string srcShape;
        {
            std::vector<std::string> shapes = melQueryStringArray(
                "listRelatives -shapes -type \"camera\" -fullPath \"" + cameraTransform + "\"");
            if (!shapes.empty()) srcShape = shapes[0];
        }

        // Create a new camera (returns: {transform, shape}).
        {
            // The imported UE camera name typically comes from the FBX node name, not the file name.
            // Name the temp camera to match the output filename stem for predictable results.
            const std::string desired = sanitizeMayaName(basenameNoExt(outputPath));

            // If a previous export crashed and left a temp camera behind, clean it up.
            // We only delete if it's explicitly marked as a temp export camera.
            if (nodeExists(desired) && attributeExists(desired, "ptTempExportCam")) {
                int v = 0;
                if (MGlobal::executeCommand(utf8ToMString("getAttr \"" + desired + ".ptTempExportCam\""), v) == MS::kSuccess && v != 0) {
                    melExec("delete \"" + desired + "\"");
                    debugWarn("exportCameraFbx: deleted stale temp camera: " + desired);
                }
            }

            // NOTE:
            // Maya's `camera -name` has shown auto-renaming behavior that can strip/re-number
            // trailing digits. To guarantee UE sees the expected name, create with a stable
            // placeholder and then rename explicitly to `desired`.
            const std::string placeholder = "__PT_ExportCamTmp";
            std::vector<std::string> created = melQueryStringArray(
                "camera -name \"" + placeholder + "\"");
            if (created.size() >= 2) {
                tmpCamXform = created[0];
                tmpCamShape = created[1];
            }

            if (!tmpCamXform.empty()) {
                std::string renamed = melQueryString("rename \"" + tmpCamXform + "\" \"" + desired + "\"");
                const std::string got = dagLeafName(renamed);
                if (!renamed.empty() && got != desired) {
                    warnings.push_back("Temp camera rename mismatch: wanted '" + desired + "', got '" + got + "'");
                }
                if (!renamed.empty()) tmpCamXform = renamed;
            }
        }
        if (tmpCamXform.empty() || tmpCamShape.empty()) {
            warnings.push_back("Failed to create temp camera; exporting original camera (may include out-of-range keys)");
            melExec("select -replace \"" + cameraTransform + "\"");
        } else {
            // Normalize to full path for downstream MEL calls.
            {
                std::vector<std::string> full = melQueryStringArray("ls -l \"" + tmpCamXform + "\"");
                if (!full.empty()) tmpCamXform = full[0];
            }
            // Refresh shape fullPath (camera command can return short names).
            {
                std::vector<std::string> shapes = melQueryStringArray(
                    "listRelatives -shapes -type \"camera\" -fullPath \"" + tmpCamXform + "\"");
                if (!shapes.empty()) tmpCamShape = shapes[0];
            }

            // Track for cleanup using the final resolved full path.
            tmpNodesToDelete.push_back(tmpCamXform);

            // Mark the transform so we can safely delete it on future runs if needed.
            if (!attributeExists(tmpCamXform, "ptTempExportCam")) {
                melExec("addAttr -ln \"ptTempExportCam\" -at bool \"" + tmpCamXform + "\"");
                melExec("setAttr \"" + tmpCamXform + ".ptTempExportCam\" 1");
            }

            {
                std::ostringstream dbg;
                dbg << "exportCameraFbx: tempCamXform=" << tmpCamXform
                    << ", tempCamShape=" << tmpCamShape
                    << ", srcShape=" << (srcShape.empty() ? "<none>" : srcShape)
                    << ", frames=" << startFrame << "-" << endFrame
                    << " (" << frameCount << "f)";
                debugInfo(dbg.str());
            }

            // Copy/drive common camera shape attributes so bake captures what artists changed.
            // (UE camera import primarily uses transform + FOV; we still bake other fields for completeness.)
            const char* kShapeAttrs[] = {
                "focalLength",
                "horizontalFilmAperture", "verticalFilmAperture",
                "horizontalFilmOffset", "verticalFilmOffset",
                "lensSqueezeRatio",
                "filmFit", "filmFitOffset",
                "nearClipPlane", "farClipPlane",
                "fStop", "focusDistance", "shutterAngle",
                "orthographicWidth",
                "panZoomEnabled", "horizontalPan", "verticalPan", "zoom"
            };
            std::vector<std::string> drivenShapePlugs;
            int copyConnected = 0, copyStatic = 0, copyFailed = 0;
            if (!srcShape.empty() && !tmpCamShape.empty()) {
                for (const char* a : kShapeAttrs) {
                    if (copyScalarAttr(srcShape, tmpCamShape, a)) {
                        drivenShapePlugs.push_back(tmpCamShape + "." + std::string(a));
                        // Check if the copy created a connection or just set a static value
                        int isConn = 0;
                        MGlobal::executeCommand(
                            utf8ToMString("connectionInfo -isDestination \""
                                          + tmpCamShape + "." + std::string(a) + "\""), isConn);
                        if (isConn) ++copyConnected; else ++copyStatic;
                    } else {
                        ++copyFailed;
                    }
                }
            }
            {
                std::ostringstream dbg;
                dbg << "exportCameraFbx: copyShapeAttrs{driven=" << drivenShapePlugs.size()
                    << ", connected=" << copyConnected
                    << ", static=" << copyStatic
                    << ", failed=" << copyFailed << "}";
                debugInfo(dbg.str());
            }

            // Log source camera focalLength state for diagnostics
            if (!srcShape.empty()) {
                double srcFL = 0.0;
                MGlobal::executeCommand(
                    utf8ToMString("getAttr \"" + srcShape + ".focalLength\""), srcFL);
                int srcFLKeys = 0;
                MGlobal::executeCommand(
                    utf8ToMString("keyframe -q -keyframeCount \"" + srcShape + ".focalLength\""), srcFLKeys);
                int srcFLDriven = 0;
                MGlobal::executeCommand(
                    utf8ToMString("connectionInfo -isDestination \"" + srcShape + ".focalLength\""), srcFLDriven);
                std::ostringstream dbg;
                dbg << "exportCameraFbx: srcFocalLength{value=" << std::setprecision(15) << srcFL
                    << ", keys=" << srcFLKeys
                    << ", driven=" << (srcFLDriven ? "true" : "false") << "}";
                debugInfo(dbg.str());
            }

            // Sample the SOURCE camera world matrix and focalLength per-frame,
            // keying the temp camera at each frame. This is robust even when the
            // source camera is constraint-driven, expression-driven, or has a
            // static focalLength with no keys. By sampling focalLength directly
            // in the per-frame loop we guarantee UE Level Sequencer receives an
            // animation curve regardless of how the source value is produced.
            double prevTime = 0.0;
            MGlobal::executeCommand("currentTime -q", prevTime);
            struct RefreshSuspendGuard {
                int prevSuspend = 0;
                bool active = false;
                RefreshSuspendGuard() {
                    MGlobal::executeCommand("refresh -q -suspend", prevSuspend);
                    active = melExec("refresh -suspend true");
                }
                ~RefreshSuspendGuard() {
                    if (!active) return;
                    melExec(std::string("refresh -suspend ") + (prevSuspend ? "true" : "false"));
                }
            } refreshGuard;

            // Disconnect focalLength on temp shape before per-frame keying
            // (connectAttr from copyScalarAttr would block setKeyframe).
            bool hadFLConnection = false;
            if (!tmpCamShape.empty()) {
                int isConn = 0;
                MGlobal::executeCommand(
                    utf8ToMString("connectionInfo -isDestination \""
                                  + tmpCamShape + ".focalLength\""), isConn);
                if (isConn) {
                    MString srcPlug;
                    MGlobal::executeCommand(
                        utf8ToMString("connectionInfo -sourceFromDestination \""
                                      + tmpCamShape + ".focalLength\""), srcPlug);
                    if (srcPlug.length() > 0) {
                        melExec("disconnectAttr \"" + toUtf8(srcPlug) + "\" \""
                                + tmpCamShape + ".focalLength\"");
                        hadFLConnection = true;
                        debugInfo("exportCameraFbx: disconnected focalLength connection from "
                                  + toUtf8(srcPlug) + " for per-frame keying");
                    }
                }
            }

            // Remove focalLength from drivenShapePlugs since we handle it
            // directly in the per-frame loop now.
            {
                std::string flPlugName = tmpCamShape + ".focalLength";
                drivenShapePlugs.erase(
                    std::remove(drivenShapePlugs.begin(), drivenShapePlugs.end(), flPlugName),
                    drivenShapePlugs.end());
            }

            double flMin = 1e18, flMax = -1e18;
            for (int f = startFrame; f <= endFrame; ++f) {
                melExec("currentTime -e " + std::to_string(f));
                double m[16] = {0};
                if (!queryWorldMatrix(cameraTransform, m)) {
                    cleanupTmp();
                    double duration = std::difftime(std::time(nullptr), startTime);
                    return makeResult(false, outputPath, 0, duration, warnings,
                                      {"Failed to query source camera world matrix at frame " + std::to_string(f)});
                }
                if (!setWorldMatrix(tmpCamXform, m)) {
                    cleanupTmp();
                    double duration = std::difftime(std::time(nullptr), startTime);
                    return makeResult(false, outputPath, 0, duration, warnings,
                                      {"Failed to set temp camera world matrix at frame " + std::to_string(f)});
                }
                if (!keyTransformAtFrame(tmpCamXform, f)) {
                    cleanupTmp();
                    double duration = std::difftime(std::time(nullptr), startTime);
                    return makeResult(false, outputPath, 0, duration, warnings,
                                      {"Failed to key temp camera at frame " + std::to_string(f)});
                }

                // Sample focalLength from source camera at this frame and key on temp shape.
                if (!srcShape.empty() && !tmpCamShape.empty()) {
                    double fl = 0.0;
                    MGlobal::executeCommand(
                        utf8ToMString("getAttr \"" + srcShape + ".focalLength\""), fl);
                    {
                        std::ostringstream flCmd;
                        flCmd << std::setprecision(15)
                              << "setAttr \"" << tmpCamShape << ".focalLength\" " << fl;
                        melExec(flCmd.str());
                    }
                    {
                        std::ostringstream flCmd;
                        flCmd << "setKeyframe -attribute \"focalLength\""
                              << " -time " << f
                              << " -value " << std::setprecision(15) << fl
                              << " \"" << tmpCamShape << "\"";
                        melExec(flCmd.str());
                    }
                    if (fl < flMin) flMin = fl;
                    if (fl > flMax) flMax = fl;
                }
            }
            {
                std::ostringstream cmd;
                cmd << "currentTime -e " << std::setprecision(15) << prevTime;
                melExec(cmd.str());
            }

            // Log focalLength sampling result
            {
                int flKeyCount = 0;
                if (!tmpCamShape.empty()) {
                    MGlobal::executeCommand(
                        utf8ToMString("keyframe -q -keyframeCount \""
                                      + tmpCamShape + ".focalLength\""), flKeyCount);
                }
                std::ostringstream dbg;
                dbg << "exportCameraFbx: focalLengthSampled{keys=" << flKeyCount
                    << ", min=" << std::setprecision(6) << flMin
                    << ", max=" << flMax
                    << ", isAnimated=" << (std::abs(flMax - flMin) > 1e-6 ? "true" : "false")
                    << ", hadConnection=" << (hadFLConnection ? "true" : "false") << "}";
                debugInfo(dbg.str());
            }

            // Bake remaining driven camera shape plugs (FStop/aperture/etc) if any.
            // focalLength is already keyed per-frame above, so it's excluded here.
            if (!drivenShapePlugs.empty()) {
                {
                    std::ostringstream dbg;
                    dbg << "exportCameraFbx: baking " << drivenShapePlugs.size()
                        << " remaining shape plugs: [";
                    for (size_t pi = 0; pi < drivenShapePlugs.size(); ++pi) {
                        if (pi > 0) dbg << ", ";
                        dbg << drivenShapePlugs[pi];
                    }
                    dbg << "]";
                    debugInfo(dbg.str());
                }
                std::ostringstream cmd;
                cmd << "bakeResults -simulation true"
                    << " -time \"" << startFrame << ":" << endFrame << "\""
                    << " -sampleBy 1"
                    << " -oversamplingRate 1"
                    << " -disableImplicitControl true"
                    << " -preserveOutsideKeys false"
                    << " -sparseAnimCurveBake false";
                for (const auto& plug : drivenShapePlugs) {
                    cmd << " \"" << plug << "\"";
                }
                if (!melExec(cmd.str())) {
                    warnings.push_back("Failed to bake temp camera shape attributes");
                    debugWarn("exportCameraFbx: bakeResults FAILED for remaining shape plugs");
                }
            } else {
                debugInfo("exportCameraFbx: no remaining shape plugs to bake (focalLength handled per-frame)");
            }

            // Ensure no stray keys outside range (defensive).
            {
                int cutBefore = startFrame - 1;
                int cutAfter  = endFrame + 1;
                std::ostringstream ck;
                ck << "cutKey -clear -time \":" << cutBefore << "\" \"" << tmpCamXform << "\"";
                melExec(ck.str());
                ck.str("");
                ck << "cutKey -clear -time \"" << cutAfter << ":\" \"" << tmpCamXform << "\"";
                melExec(ck.str());

                // Camera settings are keyed on the shape; clear there too.
                ck.str("");
                ck << "cutKey -clear -time \":" << cutBefore << "\" \"" << tmpCamShape << "\"";
                melExec(ck.str());
                ck.str("");
                ck << "cutKey -clear -time \"" << cutAfter << ":\" \"" << tmpCamShape << "\"";
                melExec(ck.str());
            }

            melExec("select -replace \"" + tmpCamXform + "\"");
        }

        setFbxExportDefaults();
        melExec("FBXExportCameras -v true");
        setFbxBakeRange(startFrame, endFrame);

        // Apply common options
        melExec("FBXExportFileVersion -v " + opts.fileVersion);
        melExec("FBXExportUpAxis " + opts.upAxis);

        std::string fbxPath = melPath(outputPath);
        const bool fbxExportOk = melExec("FBXExport -f \"" + fbxPath + "\" -s");
        if (!fbxExportOk) {
            cleanupTmp();
            double duration = std::difftime(std::time(nullptr), startTime);
            int64_t fileSize = fileExistsOnDisk(outputPath) ? getFileSize(outputPath) : 0;
            return makeResult(false, outputPath, fileSize, duration, warnings,
                              {"FBXExport command failed in camera export"});
        }

        FbxContentStats fbxStats = scanFbxContent(outputPath);
        debugFbxContent("exportCameraFbx", outputPath, fbxStats);

        cleanupTmp();
        double duration = std::difftime(std::time(nullptr), startTime);
        int64_t fileSize = fileExistsOnDisk(outputPath) ? getFileSize(outputPath) : 0;
        {
            std::ostringstream dbg;
            dbg << "exportCameraFbx: exported file size=" << fileSize
                << ", duration=" << duration << "s";
            debugInfo(dbg.str());
        }

        return makeResult(true, outputPath, fileSize, duration, warnings);
    } catch (...) {
        cleanupTmp();
        double duration = std::difftime(std::time(nullptr), startTime);
        return makeResult(false, outputPath, 0, duration, warnings,
                          {"Camera export failed: unknown exception"});
    }
}

ExportResult exportSkeletonFbx(const std::string& skeletonRoot,
                               const std::string& outputPath,
                               int startFrame, int endFrame,
                               const FbxExportOptions& opts) {
    std::vector<std::string> warnings;
    time_t startTime = std::time(nullptr);

    if (!ensureFbxPlugin()) {
        return makeResult(false, outputPath, 0, 0.0, {}, {"fbxmaya plugin load failed"});
    }

    if (!nodeExists(skeletonRoot)) {
        return makeResult(false, outputPath, 0, 0.0, {},
                          {"Skeleton root does not exist: " + skeletonRoot});
    }

    std::string rootJoint = skeletonRoot;   // working copy (path may change)
    std::string originalParent;

    // Track renames so we can always restore even if export fails.
    struct JointRec {
        MObject obj;
        std::string originalName; // includes namespace, e.g. A:B:Joint
    };
    std::vector<JointRec> jointRecs;
    MObject rootObj;
    bool didRename = false;
    bool hadNamespaceOnBones = false;
    bool rootNameNormalized = false;
    bool rootReferenced = false;
    std::string fatalError;

    try {
        {
            std::ostringstream dbg;
            dbg << "exportSkeletonFbx: input=" << skeletonRoot
                << ", output=" << outputPath
                << ", range=" << startFrame << "-" << endFrame
                << ", opts{AnimationOnly=" << (opts.skelAnimationOnly ? "true" : "false")
                << ", AnimationOnlyEffective=false"
                << ", BakeComplex=" << (opts.skelBakeComplex ? "true" : "false")
                << ", SkeletonDefs(UI)=" << (opts.skelSkeletonDefs ? "true" : "false")
                << ", Constraints=" << (opts.skelConstraints ? "true" : "false")
                << ", InputConns=" << (opts.skelInputConns ? "true" : "false")
                << "}";
            debugInfo(dbg.str());
        }

        std::string outDir = getDirname(outputPath);
        if (!outDir.empty()) ensureDir(outDir);

        // Validate that rootJoint is actually a joint. If a group/transform is
        // accidentally passed in, find the most likely root joint under it.
        {
            std::string type = melQueryString("nodeType \"" + rootJoint + "\"");
            if (type != "joint") {
                std::vector<std::string> joints = melQueryStringArray(
                    "listRelatives -allDescendents -type \"joint\" -fullPath \"" + rootJoint + "\"");
                if (joints.empty()) {
                    return makeResult(false, outputPath, 0, 0.0, {},
                                      {"No joints found under: " + rootJoint});
                }

                // Find a joint that has no joint-parent (inside this subtree).
                std::set<std::string> jointSet(joints.begin(), joints.end());
                std::vector<std::string> candidates;
                for (const auto& j : joints) {
                    std::vector<std::string> parents = melQueryStringArray(
                        "listRelatives -parent -type \"joint\" -fullPath \"" + j + "\"");
                    if (parents.empty() || jointSet.count(parents[0]) == 0) {
                        candidates.push_back(j);
                    }
                }
                if (!candidates.empty()) {
                    // Pick the one with the most descendants.
                    std::string best = candidates[0];
                    int bestCount = -1;
                    for (const auto& c : candidates) {
                        std::vector<std::string> desc = melQueryStringArray(
                            "listRelatives -allDescendents -type \"joint\" -fullPath \"" + c + "\"");
                        if ((int)desc.size() > bestCount) {
                            bestCount = (int)desc.size();
                            best = c;
                        }
                    }
                    warnings.push_back("Input node is not a joint; using joint root: " + best);
                    rootJoint = best;
                } else {
                    warnings.push_back("Input node is not a joint; using first joint found: " + joints[0]);
                    rootJoint = joints[0];
                }
            }
        }

        // Resolve root joint object handle once. The MObject survives reparent/rename
        // and avoids ambiguous path re-query (which can accidentally pick group nodes).
        {
            MSelectionList rootSel;
            rootSel.add(MString(rootJoint.c_str()));
            rootSel.getDependNode(0, rootObj);
            MFnDagNode rootFn(rootObj);
            rootJoint = toUtf8(rootFn.fullPathName());
            rootReferenced = isReferencedNode(rootObj);
            {
                std::ostringstream dbg;
                dbg << "exportSkeletonFbx: rootReferenced=" << (rootReferenced ? "true" : "false");
                debugInfo(dbg.str());
            }
        }


        if (rootReferenced && !opts.skelAnimationOnly) {
            warnings.push_back("Referenced skeleton + AnimationOnly=false: export in-place to preserve skinned meshes");
            warnings.push_back("Referenced export keeps original bone names; namespaces may remain");
            debugWarn("exportSkeletonFbx: referenced skeleton + AnimationOnly=false, using in-place export path");

            std::vector<std::string> allJoints = melQueryStringArray(
                "listRelatives -allDescendents -type \"joint\" -fullPath \"" + rootJoint + "\"");
            allJoints.push_back(rootJoint);
            if (allJoints.empty()) {
                return makeResult(false, outputPath, 0, 0.0, warnings,
                                  {"No joints collected for referenced in-place export"});
            }

            int skinClusters = 0;
            int skinnedMeshShapes = 0;
            std::vector<std::string> meshTransforms =
                collectSkinnedMeshTransformsForJoints(allJoints, &skinClusters, &skinnedMeshShapes);
            if (meshTransforms.empty()) {
                meshTransforms = collectMeshTransformsByNamespace(rootJoint);
                if (meshTransforms.empty()) {
                    meshTransforms = collectMeshTransformsUnderNode(rootJoint);
                }
            }

            {
                std::ostringstream dbg;
                dbg << "exportSkeletonFbx: referencedMeshCollection{skinClusters=" << skinClusters
                    << ", skinnedMeshShapes=" << skinnedMeshShapes
                    << ", meshTransforms=" << meshTransforms.size() << "}";
                debugInfo(dbg.str());
            }

            MSelectionList sel;
            for (const auto& j : allJoints) {
                sel.add(MString(j.c_str()));
            }
            for (const auto& m : meshTransforms) {
                sel.add(MString(m.c_str()));
            }
            MGlobal::setActiveSelectionList(sel, MGlobal::kReplaceList);
            debugSelectionSnapshot("exportSkeletonFbx: referencedInPlaceSelection");

            bool warnedSkelDefs = false;
            auto exportReferencedInPlace = [&](bool useInputConnections,
                                               const char* debugTag,
                                               bool* outExportOk) -> FbxContentStats {
                if (outExportOk) *outExportOk = false;

                setFbxExportDefaults();
                melExec(std::string("FBXExportSkeletonDefinitions -v ") + (opts.skelSkeletonDefs ? "true" : "false"));
                if (!opts.skelSkeletonDefs && !warnedSkelDefs) {
                    warnings.push_back("SkeletonDefs(UI)=false: FBX skeleton hierarchy metadata may be incomplete in some DCC/engines");
                    warnedSkelDefs = true;
                }
                melExec("FBXExportAnimationOnly -v false");
                melExec(std::string("FBXExportBakeComplexAnimation -v ") + (opts.skelBakeComplex ? "true" : "false"));
                melExec(std::string("FBXExportConstraints -v ") + (opts.skelConstraints ? "true" : "false"));
                melExec(std::string("FBXExportInputConnections -v ") + (useInputConnections ? "true" : "false"));
                melExec("FBXExportSkins -v true");
                setFbxBakeRange(startFrame, endFrame);
                melExec("FBXExportFileVersion -v " + opts.fileVersion);
                melExec("FBXExportUpAxis " + opts.upAxis);

                std::string fbxPath = melPath(outputPath);
                const bool fbxExportOk = melExec("FBXExport -f \"" + fbxPath + "\" -s");
                if (!fbxExportOk) {
                    debugWarn(std::string(debugTag) + ": FBXExport command failed");
                    return FbxContentStats();
                }

                FbxContentStats stats = scanFbxContent(outputPath);
                debugFbxContent(debugTag, outputPath, stats);
                if (outExportOk) *outExportOk = true;
                return stats;
            };

            const bool initialInputConnections = opts.skelInputConns;
            {
                std::ostringstream dbg;
                dbg << "exportSkeletonFbx: referencedInPlaceFbxSettings{InputConnections="
                    << (initialInputConnections ? "true" : "false")
                    << ", Skins=true, AnimationOnly=false}";
                debugInfo(dbg.str());
            }

            bool inPlaceExportOk = false;
            FbxContentStats fbxStats = exportReferencedInPlace(initialInputConnections,
                                                               "exportSkeletonFbx(referencedInPlace)",
                                                               &inPlaceExportOk);
            if (!inPlaceExportOk) {
                double duration = std::difftime(std::time(nullptr), startTime);
                int64_t fileSize = fileExistsOnDisk(outputPath) ? getFileSize(outputPath) : 0;
                return makeResult(false, outputPath, fileSize, duration, warnings,
                                  {"FBXExport command failed in referenced in-place skeleton export"});
            }

            if (!initialInputConnections && fbxStats.skins <= 0 && fbxStats.deformers <= 0) {
                warnings.push_back("Referenced in-place export missing skin/deformer data; retry with InputConnections=true");
                debugWarn("exportSkeletonFbx: referenced in-place retry with InputConnections=true");
                bool retryExportOk = false;
                fbxStats = exportReferencedInPlace(true,
                                                   "exportSkeletonFbx(referencedInPlaceRetry)",
                                                   &retryExportOk);
                if (!retryExportOk) {
                    double duration = std::difftime(std::time(nullptr), startTime);
                    int64_t fileSize = fileExistsOnDisk(outputPath) ? getFileSize(outputPath) : 0;
                    return makeResult(false, outputPath, fileSize, duration, warnings,
                                      {"FBXExport command failed in referenced in-place retry"});
                }
            }

            if (fbxStats.limbNodes > static_cast<int>(allJoints.size()) * 2) {
                std::ostringstream warn;
                warn << "Referenced in-place export pulled extra bones: selectedJoints=" << allJoints.size()
                     << ", exportedLimbNodes=" << fbxStats.limbNodes
                     << " (likely from connected assets/namespaces)";
                warnings.push_back(warn.str());
                debugWarn("exportSkeletonFbx: " + warn.str());
            }

            double duration = std::difftime(std::time(nullptr), startTime);
            int64_t fileSize = fileExistsOnDisk(outputPath) ? getFileSize(outputPath) : 0;
            {
                std::ostringstream dbg;
                dbg << "exportSkeletonFbx(referencedInPlace): exported file size=" << fileSize
                    << ", duration=" << duration << "s";
                debugInfo(dbg.str());
            }
            if (fileSize <= 0) {
                return makeResult(false, outputPath, fileSize, duration, warnings,
                                  {"Referenced in-place skeleton export produced empty file"});
            }
            if (fbxStats.limbNodes <= 0) {
                return makeResult(false, outputPath, fileSize, duration, warnings,
                                  {"Referenced in-place export contains no LimbNode bones"});
            }
            if (fbxStats.skins <= 0 && fbxStats.deformers <= 0) {
                return makeResult(false, outputPath, fileSize, duration, warnings,
                                  {"Referenced in-place export contains no skin/deformer data"});
            }

            return makeResult(true, outputPath, fileSize, duration, warnings);
        }

        if (rootReferenced) {
            warnings.push_back("Skeleton root is referenced/read-only; exporting via temporary duplicate skeleton");
            debugWarn("exportSkeletonFbx: referenced skeleton detected; using duplicate export path");
            ExportResult dupRes = exportSkeletonFbxViaDuplicate(rootJoint, outputPath, startFrame, endFrame, opts);
            dupRes.warnings.insert(dupRes.warnings.begin(), warnings.begin(), warnings.end());
            return dupRes;
        }

        // Query the root joint's parent for mesh-collection fallback (AnimationOnly=false).
        // We intentionally do NOT reparent the root joint to world: keeping the original
        // hierarchy preserves the root bone's local-space animation, which is what UE
        // expects when importing via "ImportFromAnimationRoot". DAG parents will appear
        // as Null nodes in the FBX (same as manual File > Export Selection), and UE
        // matches bones by name, ignoring those Nulls.
        {
            std::vector<std::string> parents = melQueryStringArray(
                "listRelatives -parent -fullPath \"" + rootJoint + "\"");
            if (!parents.empty()) {
                originalParent = parents[0];
            }
        }

        if (melQueryString("nodeType \"" + rootJoint + "\"") != "joint") {
            return makeResult(false, outputPath, 0, 0.0, warnings,
                              {"Resolved skeleton root is not a joint: " + rootJoint});
        }

        // Collect all joints under rootJoint (full paths).
        std::vector<std::string> jointPaths = melQueryStringArray(
            "listRelatives -allDescendents -type \"joint\" -fullPath \"" + rootJoint + "\"");
        jointPaths.push_back(rootJoint);

        {
            std::ostringstream dbg;
            dbg << "exportSkeletonFbx: rootAfterResolve=" << rootJoint
                << ", originalParent=" << (originalParent.empty() ? "<world>" : originalParent)
                << ", jointPaths=" << jointPaths.size();
            debugInfo(dbg.str());
        }

        if (jointPaths.empty()) {
            return makeResult(false, outputPath, 0, 0.0, warnings,
                              {"No joints collected for export under: " + rootJoint});
        }

        debugJointMotionSample("exportSkeletonFbx: rootMotionBeforeCleanup",
                               rootJoint, startFrame, endFrame);

        // Build records with MObjects so we can restore names reliably.
        {
            MSelectionList sel;
            for (const auto& p : jointPaths) {
                sel.add(MString(p.c_str()));
            }
            jointRecs.reserve(jointPaths.size());

            for (unsigned int i = 0; i < sel.length() && i < jointPaths.size(); ++i) {
                MObject obj;
                sel.getDependNode(i, obj);
                MFnDependencyNode fn(obj);

                JointRec rec;
                rec.obj = obj;
                rec.originalName = toUtf8(fn.name());
                if (jointPaths[i] == rootJoint) rootObj = obj;
                jointRecs.push_back(rec);
            }
        }

        // Strip namespaces from joints for export only.
        // Also normalize the top bone to Root/root when applicable.
        {
            struct WorkItem {
                std::string fullPath;
                int depth;
                bool isRoot;
                std::string leaf;
                std::string desiredBare;
                bool needsRename;
            };
            std::vector<WorkItem> work;
            work.reserve(jointPaths.size());

            for (const auto& p : jointPaths) {
                WorkItem wi;
                wi.fullPath = p;
                wi.depth = dagDepth(p);
                wi.isRoot = (p == rootJoint);
                wi.leaf = dagLeafName(p);

                std::string bare = stripAllNamespaces(wi.leaf);
                if (wi.isRoot) {
                    bare = normalizeRootBoneName(bare);
                }
                wi.desiredBare = bare;

                bool hasNs = (wi.leaf.find(':') != std::string::npos);
                bool rootNeedsNorm = false;
                if (wi.isRoot) {
                    std::string bare0 = stripAllNamespaces(wi.leaf);
                    rootNeedsNorm = (normalizeRootBoneName(bare0) != bare0);
                }
                wi.needsRename = hasNs || rootNeedsNorm;
                if (hasNs) hadNamespaceOnBones = true;
                if (rootNeedsNorm) rootNameNormalized = true;
                if (wi.needsRename) didRename = true;
                work.push_back(wi);
            }

            if (didRename) {
                // Rename deepest first so parent renames don't invalidate child paths.
                std::sort(work.begin(), work.end(),
                          [](const WorkItem& a, const WorkItem& b) {
                              return a.depth > b.depth;
                          });

                for (const auto& wi : work) {
                    if (!wi.needsRename) continue;
                    std::string target = ":" + wi.desiredBare; // force root namespace
                    std::string cmd = "rename \"" + wi.fullPath + "\" \"" + target + "\"";
                    melQueryString(cmd);
                }

                // Update rootJoint full path after rename using root object handle.
                if (!rootObj.isNull()) {
                    MFnDagNode fn(rootObj);
                    rootJoint = toUtf8(fn.fullPathName());
                }
            }
        }
        // Re-select all joints by querying from the (possibly renamed) rootJoint.
        {
            std::vector<std::string> allJoints = melQueryStringArray(
                "listRelatives -allDescendents -type \"joint\" -fullPath \"" + rootJoint + "\"");
            allJoints.push_back(rootJoint);

            // Safety check: exported skeleton joints must not keep namespace prefixes.
            int namespacedCount = 0;
            for (const auto& j : allJoints) {
                std::string leaf = dagLeafName(j);
                // A leading ":" in Maya means the root namespace (not a real namespace segment).
                if (!leaf.empty() && leaf[0] == ':') leaf = leaf.substr(1);
                if (leaf.find(':') != std::string::npos) {
                    ++namespacedCount;
                }
            }
            {
                std::ostringstream dbg;
                dbg << "exportSkeletonFbx: allJointsForExport=" << allJoints.size()
                    << ", namespacedAfterCleanup=" << namespacedCount
                    << ", didRename=" << (didRename ? "true" : "false");
                debugInfo(dbg.str());
            }
            if (namespacedCount > 0) {
                debugWarn("exportSkeletonFbx: namespace cleanup on source skeleton failed, switching to duplicate fallback");

                // Restore source scene state before fallback export.
                if (didRename) {
                    for (auto& r : jointRecs) {
                        MFnDependencyNode fn(r.obj);
                        fn.setName(MString(r.originalName.c_str()));
                    }
                    didRename = false;
                }
                if (!rootObj.isNull()) {
                    MFnDagNode fn(rootObj);
                    rootJoint = toUtf8(fn.fullPathName());
                }

                ExportResult dupRes = exportSkeletonFbxViaDuplicate(rootJoint, outputPath, startFrame, endFrame, opts);
                dupRes.warnings.insert(dupRes.warnings.begin(), warnings.begin(), warnings.end());
                return dupRes;
            }

            std::vector<std::string> meshTransforms;
            int skinClusters = 0;
            int skinnedMeshShapes = 0;

            if (!opts.skelAnimationOnly) {
                meshTransforms = collectSkinnedMeshTransformsForJoints(allJoints, &skinClusters, &skinnedMeshShapes);
                if (meshTransforms.empty()) {
                    std::string anchor = originalParent.empty() ? rootJoint : originalParent;
                    meshTransforms = collectMeshTransformsUnderNode(anchor);
                    warnings.push_back("No skinned meshes found from joint connections; fallback to meshes under rig hierarchy");
                }

                {
                    std::ostringstream dbg;
                    dbg << "exportSkeletonFbx: meshCollection{skinClusters=" << skinClusters
                        << ", skinnedMeshShapes=" << skinnedMeshShapes
                        << ", meshTransforms=" << meshTransforms.size()
                        << ", mode=Animation+Mesh}";
                    debugInfo(dbg.str());
                }

                if (meshTransforms.empty()) {
                    warnings.push_back("AnimationOnly=false but no mesh transforms were found; FBX will contain skeleton only");
                    debugWarn("exportSkeletonFbx: AnimationOnly=false but no mesh transforms found");
                }
            }

            MSelectionList sel;
            for (const auto& j : allJoints) {
                sel.add(MString(j.c_str()));
            }
            for (const auto& m : meshTransforms) {
                sel.add(MString(m.c_str()));
            }

            MGlobal::setActiveSelectionList(sel, MGlobal::kReplaceList);

            {
                std::vector<std::string> selJoints = melQueryStringArray("ls -sl -type \"joint\"");
                std::vector<std::string> selMeshShapes = melQueryStringArray("ls -sl -type \"mesh\"");
                std::ostringstream dbg2;
                dbg2 << "exportSkeletonFbx: selectionSummary{joints=" << selJoints.size()
                     << ", meshShapes=" << selMeshShapes.size()
                     << ", animationOnly=" << (opts.skelAnimationOnly ? "true" : "false") << "}";
                debugInfo(dbg2.str());
            }
            debugSelectionSnapshot("exportSkeletonFbx: preExportSelection");
        }

        if (hadNamespaceOnBones) {
            warnings.push_back("Detected namespaces in skeleton joints; stripped during export");
        }
        if (rootNameNormalized) {
            warnings.push_back("Top skeleton bone normalized to Root during export");
        }
        if (opts.skelAnimationOnly) {
            warnings.push_back("Skeleton AnimationOnly(UI)=true: exporting joints-only while FBXAnimationOnly is forced false");
            debugWarn("Skeleton export uses joints-only mode (FBXAnimationOnly forced false)");
        }

        debugJointMotionSample("exportSkeletonFbx: rootMotionBeforeExport",
                               rootJoint, startFrame, endFrame);
        debugWorldSpacePosition("exportSkeletonFbx: rootWorldPosBeforeExport",
                                rootJoint, startFrame);

        setFbxExportDefaults();

        // Skeleton definitions are generally required for correct bone hierarchy in engines,
        // but we still honor the UI flag so advanced users can disable it when needed.
        melExec(std::string("FBXExportSkeletonDefinitions -v ") + (opts.skelSkeletonDefs ? "true" : "false"));
        if (!opts.skelSkeletonDefs) {
            warnings.push_back("SkeletonDefs(UI)=false: FBX skeleton hierarchy metadata may be incomplete in some DCC/engines");
        }
        melExec("FBXExportAnimationOnly -v false");
        if (opts.skelAnimationOnly) {
            warnings.push_back("AnimationOnly(UI)=true: force FBXExportAnimationOnly=false to keep skeleton hierarchy");
            debugWarn("exportSkeletonFbx: override FBXExportAnimationOnly=false to preserve skeleton hierarchy");
        }
        melExec(std::string("FBXExportBakeComplexAnimation -v ") + (opts.skelBakeComplex ? "true" : "false"));
        melExec(std::string("FBXExportConstraints -v ") + (opts.skelConstraints ? "true" : "false"));

        const bool effectiveInputConns = opts.skelInputConns;
        melExec(std::string("FBXExportInputConnections -v ") + (effectiveInputConns ? "true" : "false"));
        melExec(std::string("FBXExportSkins -v ") + (!opts.skelAnimationOnly ? "true" : "false"));
        melExec(std::string("FBXExportShapes -v ") + (!opts.skelAnimationOnly ? "true" : "false"));

        {
            std::ostringstream dbg;
            dbg << "exportSkeletonFbx: effectiveFbxSettings{AnimationOnlyRequested=" << (opts.skelAnimationOnly ? "true" : "false")
                << ", AnimationOnlyEffective=false"
                << ", BakeComplex=" << (opts.skelBakeComplex ? "true" : "false")
                << ", Constraints=" << (opts.skelConstraints ? "true" : "false")
                << ", InputConnections=" << (effectiveInputConns ? "true" : "false")
                << ", Skins=" << (!opts.skelAnimationOnly ? "true" : "false") << "}";
            debugInfo(dbg.str());
        }

        setFbxBakeRange(startFrame, endFrame);

        // Apply common options
        melExec("FBXExportFileVersion -v " + opts.fileVersion);
        melExec("FBXExportUpAxis " + opts.upAxis);

        std::string fbxPath = melPath(outputPath);
        const bool fbxExportOk = melExec("FBXExport -f \"" + fbxPath + "\" -s");

        FbxContentStats fbxStats;
        if (fbxExportOk) {
            fbxStats = scanFbxContent(outputPath);
            debugFbxContent("exportSkeletonFbx", outputPath, fbxStats);
        }

        // Restore joint names (namespaces + original root name)
        if (didRename) {
            for (auto& r : jointRecs) {
                MFnDependencyNode fn(r.obj);
                fn.setName(MString(r.originalName.c_str()));
            }
        }

        double duration = std::difftime(std::time(nullptr), startTime);
        int64_t fileSize = fileExistsOnDisk(outputPath) ? getFileSize(outputPath) : 0;
        {
            std::ostringstream dbg;
            dbg << "exportSkeletonFbx: exported file size=" << fileSize
                << ", duration=" << duration << "s";
            debugInfo(dbg.str());
        }

        if (!fbxExportOk) {
            return makeResult(false, outputPath, fileSize, duration, warnings,
                              {"FBXExport command failed in skeleton export"});
        }

        if (fileSize <= 0) {
            return makeResult(false, outputPath, fileSize, duration, warnings,
                              {"Skeleton export produced empty file"});
        }

        if (fbxStats.limbNodes <= 0) {
            return makeResult(false, outputPath, fileSize, duration, warnings,
                              {"Skeleton export did not contain LimbNode bones"});
        }

        if (!opts.skelAnimationOnly && fbxStats.skins <= 0 && fbxStats.deformers <= 0) {
            return makeResult(false, outputPath, fileSize, duration, warnings,
                              {"AnimationOnly=false but exported FBX contains no skin/deformer data"});
        }

        return makeResult(true, outputPath, fileSize, duration, warnings);
    } catch (...) {
        // Best-effort restore names
        try {
            if (didRename) {
                for (auto& r : jointRecs) {
                    MFnDependencyNode fn(r.obj);
                    fn.setName(MString(r.originalName.c_str()));
                }
            }
        } catch (...) {
        }

        double duration = std::difftime(std::time(nullptr), startTime);
        return makeResult(false, outputPath, 0, duration, warnings,
                          {fatalError.empty() ? "Skeleton export failed: unknown exception" : fatalError});
    }
}

ExportResult exportBlendShapeFbx(const std::string& meshNode,
                                 const std::string& outputPath,
                                 int startFrame, int endFrame,
                                 const FbxExportOptions& opts) {
    std::vector<std::string> warnings;
    time_t startTime = std::time(nullptr);
    std::string dupMesh;
    MObject dupMeshObj;
    bool undoChunkOpen = false;
    bool anyMergeSucceeded = false;

    if (!ensureFbxPlugin()) {
        return makeResult(false, outputPath, 0, 0.0, {}, {"fbxmaya plugin load failed"});
    }

    if (!nodeExists(meshNode)) {
        return makeResult(false, outputPath, 0, 0.0, {},
                          {"Mesh node does not exist: " + meshNode});
    }

    try {
        {
            std::ostringstream dbg;
            dbg << "exportBlendShapeFbx: mesh=" << meshNode
                << ", output=" << outputPath
                << ", range=" << startFrame << "-" << endFrame
                << ", opts{Shapes=" << (opts.bsShapes ? "true" : "false")
                << ", IncludeSkeleton=" << (opts.bsIncludeSkeleton ? "true" : "false")
                << ", SmoothMesh=" << (opts.bsSmoothMesh ? "true" : "false")
                << ", fileVersion=" << opts.fileVersion
                << ", upAxis=" << opts.upAxis
                << "}";
            debugInfo(dbg.str());
        }

        std::string outDir = getDirname(outputPath);
        if (!outDir.empty()) ensureDir(outDir);

        // Verify blendShape deformers exist on this mesh before export.
        std::vector<std::string> history;
        std::string histCmd = "listHistory -pruneDagObjects true \"" + meshNode + "\"";
        if (!melQueryStringArrayChecked(histCmd, history)) {
            return makeResult(false, outputPath, 0, 0.0, warnings,
                              {"Failed to query mesh history: " + meshNode});
        }

        std::vector<std::string> blendShapeNodes;
        std::vector<std::string> skinClusterNodes;
        for (const auto& n : history) {
            std::string t = melQueryString("nodeType \"" + n + "\"");
            if (t == "blendShape") blendShapeNodes.push_back(n);
            if (t == "skinCluster") skinClusterNodes.push_back(n);
        }

        // Collect skin influence joints + their non-joint parent transforms
        std::vector<std::string> bsSkelJoints;
        std::vector<std::string> bsSkelTransforms;
        if (opts.bsIncludeSkeleton && !skinClusterNodes.empty()) {
            std::set<std::string> seen;
            for (const auto& sc : skinClusterNodes) {
                auto influences = melQueryStringArray(
                    "skinCluster -q -influence \"" + sc + "\"");
                for (const auto& inf : influences) {
                    auto full = melQueryStringArray("ls -long \"" + inf + "\"");
                    std::string fp = full.empty() ? inf : full[0];
                    if (seen.insert(fp).second)
                        bsSkelJoints.push_back(fp);
                    // Include non-joint parent (e.g. Face_Root transform)
                    auto parents = melQueryStringArray(
                        "listRelatives -parent -fullPath \"" + fp + "\"");
                    if (!parents.empty()) {
                        std::string pt = melQueryString("nodeType \"" + parents[0] + "\"");
                        if (pt == "transform" && seen.insert(parents[0]).second)
                            bsSkelTransforms.push_back(parents[0]);
                    }
                }
            }
        }
        bool includeSkeleton = !bsSkelJoints.empty();
        if (!opts.bsShapes) {
            warnings.push_back("BlendShape Shapes(UI)=false: morph target geometry will not be exported");
        }
        if (includeSkeleton && !opts.skelSkeletonDefs) {
            warnings.push_back("SkeletonDefs(UI)=false: FBX skeleton hierarchy metadata may be incomplete in some DCC/engines");
        }

        {
            std::ostringstream dbg;
            dbg << "exportBlendShapeFbx: historyNodes=" << history.size()
                << ", blendShapeNodes=" << blendShapeNodes.size()
                << ", skinClusterNodes=" << skinClusterNodes.size()
                << ", includeSkeleton=" << (includeSkeleton ? "true" : "false")
                << ", skelJoints=" << bsSkelJoints.size()
                << ", skelTransforms=" << bsSkelTransforms.size();
            debugInfo(dbg.str());
        }

        if (blendShapeNodes.empty()) {
            return makeResult(false, outputPath, 0, 0.0, warnings,
                              {"No blendShape deformer found in mesh history: " + meshNode});
        }

        // Duplicate the mesh to break the reference and get a clean, exportable copy.
        // Use -un (upstream nodes) to preserve blendShape deformers on the duplicate.
        // -rc (return construction) strips DG nodes from referenced meshes, losing blendShapes.
        // The duplicate inherits blendShape deformers + baked animation keyframes.
        auto cleanupDupMesh = [&]() {
            try {
                if (!dupMeshObj.isNull()) {
                    MFnDagNode fn(dupMeshObj);
                    std::string p = toUtf8(fn.fullPathName());
                    if (!p.empty()) melExec("delete \"" + p + "\"");
                } else if (!dupMesh.empty()) {
                    melExec("delete \"" + dupMesh + "\"");
                }
            } catch (...) {}
        };

        {
            std::vector<std::string> dupResult;
            if (!melQueryStringArrayChecked("duplicate -un \"" + meshNode + "\"", dupResult) || dupResult.empty()) {
                melQueryStringArrayChecked("duplicate \"" + meshNode + "\"", dupResult);
            }
            if (dupResult.empty()) {
                return makeResult(false, outputPath, 0, 0.0, warnings,
                                  {"Failed to duplicate mesh for blendShape export: " + meshNode});
            }
            dupMesh = dupResult[0];

            MSelectionList sel;
            sel.add(MString(dupMesh.c_str()));
            sel.getDependNode(0, dupMeshObj);
        }

        // Parent duplicate to world for clean hierarchy (no extra Group/Null parents)
        melExec("parent -world \"" + dupMesh + "\"");
        if (!dupMeshObj.isNull()) {
            MFnDagNode fn(dupMeshObj);
            dupMesh = toUtf8(fn.fullPathName());
        }

        // Delete skinCluster(s) on the duplicate — only when NOT including skeleton.
        // When includeSkeleton is true, keep skinCluster so FBX exporter pulls in joints.
        {
            std::vector<std::string> dupHistory;
            std::string dupHistCmd = "listHistory -pruneDagObjects true \"" + dupMesh + "\"";
            melQueryStringArrayChecked(dupHistCmd, dupHistory);

            int deletedSkinClusters = 0;
            int keptSkinClusters = 0;
            int dupBlendShapeCount = 0;
            for (const auto& n : dupHistory) {
                std::string t = melQueryString("nodeType \"" + n + "\"");
                if (t == "skinCluster") {
                    if (!includeSkeleton) {
                        melExec("delete \"" + n + "\"");
                        ++deletedSkinClusters;
                    } else {
                        ++keptSkinClusters;
                    }
                }
                if (t == "blendShape") ++dupBlendShapeCount;
            }

            std::ostringstream dbg;
            dbg << "exportBlendShapeFbx: duplicate{mesh=" << dupMesh
                << ", deletedSkinClusters=" << deletedSkinClusters
                << ", keptSkinClusters=" << keptSkinClusters
                << ", blendShapeNodes=" << dupBlendShapeCount << "}";
            debugInfo(dbg.str());

            if (dupBlendShapeCount <= 0) {
                cleanupDupMesh();
                return makeResult(false, outputPath, 0, 0.0, warnings,
                                  {"Duplicate mesh lost blendShape deformers during duplication"});
            }
        }

        bool fbxExportOk = false;

        if (includeSkeleton) {
            // --- Skeleton-inclusive branch ---
            // Strip namespaces from influence joints so UE sees clean bone names.
            // Use the same rename approach as exportSkeletonFbxViaDuplicate:
            // rename each joint to its bare name (namespace-stripped) in the root namespace.
            // We do NOT use namespace -mergeNamespaceWithRoot because the source joints
            // are referenced; instead we rename the duplicate's skinCluster influences
            // which point to the *source* joints. Since we only need clean names on the
            // skeleton nodes that get exported alongside the duplicate mesh, we collect
            // the influence joints from the *original* mesh's skinCluster (bsSkelJoints)
            // and note their namespaces. But the duplicate mesh's skinCluster references
            // the *same* source joints — so we need to use undoInfo to temporarily merge
            // namespaces, export, then undo.

            // Collect namespaces from skeleton joints
            // Use full qualified namespace paths (e.g. "Outer:Inner") instead of splitting
            // into individual segments, because inner segments don't exist at root level.
            // Merge from innermost to outermost so child namespaces are removed first.
            std::set<std::string> namespacesToMerge;
            for (const auto& jp : bsSkelJoints) {
                std::string leaf = dagLeafName(jp);
                size_t colonPos = leaf.rfind(':');
                if (colonPos != std::string::npos && colonPos > 0) {
                    // Full namespace path: "A:B:Joint" -> "A:B"
                    std::string fullNs = leaf.substr(0, colonPos);
                    namespacesToMerge.insert(fullNs);
                }
            }
            for (const auto& tp : bsSkelTransforms) {
                std::string leaf = dagLeafName(tp);
                size_t colonPos = leaf.rfind(':');
                if (colonPos != std::string::npos && colonPos > 0) {
                    std::string fullNs = leaf.substr(0, colonPos);
                    namespacesToMerge.insert(fullNs);
                }
            }

            // Sort namespaces: merge deepest (most colons) first so inner namespaces
            // are removed before their parents
            std::vector<std::string> sortedNamespaces(namespacesToMerge.begin(), namespacesToMerge.end());
            std::sort(sortedNamespaces.begin(), sortedNamespaces.end(),
                      [](const std::string& a, const std::string& b) {
                          int aDepth = (int)std::count(a.begin(), a.end(), ':');
                          int bDepth = (int)std::count(b.begin(), b.end(), ':');
                          if (aDepth != bDepth) return aDepth > bDepth; // deeper first
                          return a < b;
                      });

            {
                std::ostringstream dbg;
                dbg << "exportBlendShapeFbx: namespacesToMerge=[";
                for (size_t ni = 0; ni < sortedNamespaces.size(); ++ni) {
                    if (ni > 0) dbg << ", ";
                    dbg << sortedNamespaces[ni];
                }
                dbg << "] (deepest first)";
                debugInfo(dbg.str());
            }

            // Merge namespaces inside an undo chunk so we can revert after export.
            // IMPORTANT: Only open the undo chunk if at least one merge succeeds.
            // If all merges fail (e.g. referenced namespaces), opening+closing an
            // empty undo chunk and then calling undo can corrupt Maya's undo queue.

            int nsMergeAttempted = 0;
            int nsMergeSucceeded = 0;
            int nsMergeFailed = 0;
            int nsSkippedNotExist = 0;
            std::vector<std::string> failedNamespaces;

                for (const auto& ns : sortedNamespaces) {
                    int nsExists = 0;
                    MGlobal::executeCommand(
                        utf8ToMString("namespace -exists \"" + ns + "\""), nsExists);
                    if (!nsExists) {
                        ++nsSkippedNotExist;
                        debugInfo("exportBlendShapeFbx: namespace '" + ns + "' does not exist, skipped");
                        continue;
                    }
                ++nsMergeAttempted;

                // Open undo chunk just before the first actual merge attempt.
                if (!undoChunkOpen) {
                    melExec("undoInfo -openChunk");
                    undoChunkOpen = true;
                }

                bool mergeOk = melExec("namespace -mergeNamespaceWithRoot \"" + ns + "\"");
                if (mergeOk) {
                    ++nsMergeSucceeded;
                    debugInfo("exportBlendShapeFbx: namespace '" + ns + "' merged successfully");
                } else {
                    ++nsMergeFailed;
                    failedNamespaces.push_back(ns);
                    debugWarn("exportBlendShapeFbx: namespace '" + ns + "' merge FAILED "
                              "(may contain referenced nodes or name conflicts)");
                }
            }

            {
                std::ostringstream dbg;
                dbg << "exportBlendShapeFbx: namespaceMergeSummary{"
                    << "attempted=" << nsMergeAttempted
                    << ", succeeded=" << nsMergeSucceeded
                    << ", failed=" << nsMergeFailed
                    << ", skippedNotExist=" << nsSkippedNotExist << "}";
                debugInfo(dbg.str());
            }

            anyMergeSucceeded = (nsMergeSucceeded > 0);

            if (nsMergeFailed > 0) {
                std::ostringstream warn;
                warn << "Namespace merge failed for " << nsMergeFailed << " namespace(s): [";
                for (size_t fi = 0; fi < failedNamespaces.size(); ++fi) {
                    if (fi > 0) warn << ", ";
                    warn << failedNamespaces[fi];
                }
                warn << "]; exported bone names may contain namespace prefixes";
                warnings.push_back(warn.str());
            }

            // Re-query skeleton paths after namespace merge (paths changed).
            // Use MObject-based resolution when possible to avoid ambiguity from
            // bare-name queries that can match wrong nodes in multi-character scenes.
            std::vector<std::string> skelSelectNodes;
            int resolvedByObj = 0;
            int resolvedByName = 0;
            int resolvedAmbiguous = 0;
            int resolvedFallback = 0;

            // Helper lambda: resolve a pre-merge full path to its post-merge full path.
            // Strategy: (1) try MObject lookup (survives rename), (2) bare-name ls with
            // DAG-parent disambiguation, (3) original path fallback.
            auto resolvePostMerge = [&](const std::string& preMergePath,
                                        const char* debugTag) -> std::string {
                // Strategy 1: MObject-based lookup (most reliable, survives renames)
                {
                    MSelectionList sel;
                    // After merge, the original namespaced path may still resolve via MObject
                    // if Maya's internal handle tracking updated it.
                    MStatus st = sel.add(MString(preMergePath.c_str()));
                    if (st == MS::kSuccess) {
                        MObject obj;
                        sel.getDependNode(0, obj);
                        if (!obj.isNull()) {
                            MFnDagNode fn(obj);
                            std::string resolved = toUtf8(fn.fullPathName());
                            if (!resolved.empty()) {
                                ++resolvedByObj;
                                debugInfo(std::string(debugTag) + ": '" + preMergePath
                                          + "' -> '" + resolved + "' (via MObject)");
                                return resolved;
                            }
                        }
                    }
                }

                // Strategy 2: bare-name query with DAG-parent disambiguation
                std::string bare = stripAllNamespaces(dagLeafName(preMergePath));
                auto candidates = melQueryStringArray("ls -long \"" + bare + "\"");

                if (candidates.size() == 1) {
                    ++resolvedByName;
                    debugInfo(std::string(debugTag) + ": '" + preMergePath
                              + "' -> '" + candidates[0] + "' (unique bare name)");
                    return candidates[0];
                }

                if (candidates.size() > 1) {
                    // Disambiguate: pick the candidate whose DAG parent bare name matches
                    // the original path's parent bare name.
                    ++resolvedAmbiguous;
                    std::string origParentBare;
                    {
                        // Extract parent path from the original full path
                        size_t pipePos = preMergePath.rfind('|');
                        if (pipePos != std::string::npos && pipePos > 0) {
                            std::string parentPath = preMergePath.substr(0, pipePos);
                            origParentBare = stripAllNamespaces(dagLeafName(parentPath));
                        }
                    }

                    std::string bestMatch;
                    int matchCount = 0;
                    for (const auto& c : candidates) {
                        size_t pipePos = c.rfind('|');
                        if (pipePos != std::string::npos && pipePos > 0) {
                            std::string cParent = c.substr(0, pipePos);
                            std::string cParentBare = stripAllNamespaces(dagLeafName(cParent));
                            if (cParentBare == origParentBare) {
                                bestMatch = c;
                                ++matchCount;
                            }
                        }
                    }

                    if (matchCount == 1) {
                        debugInfo(std::string(debugTag) + ": '" + preMergePath
                                  + "' -> '" + bestMatch
                                  + "' (disambiguated from " + std::to_string(candidates.size())
                                  + " candidates by parent '" + origParentBare + "')");
                        return bestMatch;
                    }

                    // Could not disambiguate — use first candidate and warn
                    debugWarn(std::string(debugTag) + ": '" + preMergePath
                              + "' has " + std::to_string(candidates.size())
                              + " candidates, parentMatch=" + std::to_string(matchCount)
                              + ", using first: '" + candidates[0] + "'");
                    warnings.push_back("Ambiguous node resolution for '" + bare
                                       + "': " + std::to_string(candidates.size())
                                       + " matches found; exported skeleton may reference wrong node");
                    return candidates[0];
                }

                // Strategy 3: no candidates found — use original path as fallback
                ++resolvedFallback;
                debugWarn(std::string(debugTag) + ": '" + preMergePath
                          + "' could not be resolved post-merge, using original path as fallback");
                return preMergePath;
            };

            for (const auto& jp : bsSkelJoints) {
                skelSelectNodes.push_back(
                    resolvePostMerge(jp, "exportBlendShapeFbx(skelJoint)"));
            }
            for (const auto& tp : bsSkelTransforms) {
                skelSelectNodes.push_back(
                    resolvePostMerge(tp, "exportBlendShapeFbx(skelTransform)"));
            }

            {
                std::ostringstream dbg;
                dbg << "exportBlendShapeFbx: nodeResolutionSummary{"
                    << "total=" << skelSelectNodes.size()
                    << ", byMObject=" << resolvedByObj
                    << ", byUniqueName=" << resolvedByName
                    << ", ambiguous=" << resolvedAmbiguous
                    << ", fallback=" << resolvedFallback << "}";
                debugInfo(dbg.str());
            }

            // Re-query duplicate mesh path (namespace merge may have changed it)
            if (!dupMeshObj.isNull()) {
                MFnDagNode fn(dupMeshObj);
                std::string newDupMesh = toUtf8(fn.fullPathName());
                if (newDupMesh != dupMesh) {
                    debugInfo("exportBlendShapeFbx: dupMesh path updated: '"
                              + dupMesh + "' -> '" + newDupMesh + "'");
                    dupMesh = newDupMesh;
                }
            }

            // Select duplicate mesh + skeleton nodes
            {
                MSelectionList sel;
                MStatus addSt = sel.add(MString(dupMesh.c_str()));
                if (addSt != MS::kSuccess) {
                    debugWarn("exportBlendShapeFbx: failed to add dupMesh to selection: " + dupMesh);
                }
                int skelAddOk = 0;
                int skelAddFail = 0;
                for (const auto& sn : skelSelectNodes) {
                    MStatus st = sel.add(MString(sn.c_str()));
                    if (st == MS::kSuccess) {
                        ++skelAddOk;
                    } else {
                        ++skelAddFail;
                        debugWarn("exportBlendShapeFbx: failed to add skeleton node to selection: " + sn);
                    }
                }
                MGlobal::setActiveSelectionList(sel, MGlobal::kReplaceList);

                std::ostringstream dbg;
                dbg << "exportBlendShapeFbx: selectionBuild{skelAddOk=" << skelAddOk
                    << ", skelAddFail=" << skelAddFail << "}";
                debugInfo(dbg.str());
            }
            debugSelectionSnapshot("exportBlendShapeFbx: preExportSelection(withSkeleton)");

            setFbxExportDefaults();
            melExec("FBXExportInputConnections -v false");
            melExec("FBXExportSkins -v true");
            melExec(std::string("FBXExportShapes -v ") + (opts.bsShapes ? "true" : "false"));
            melExec("FBXExportAnimationOnly -v false");
            melExec("FBXExportBakeComplexAnimation -v true");
            melExec(std::string("FBXExportSkeletonDefinitions -v ") + (opts.skelSkeletonDefs ? "true" : "false"));
            melExec(std::string("FBXExportSmoothMesh -v ") + (opts.bsSmoothMesh ? "true" : "false"));
            setFbxBakeRange(startFrame, endFrame);

            melExec("FBXExportFileVersion -v " + opts.fileVersion);
            melExec("FBXExportUpAxis " + opts.upAxis);

            std::string fbxPath = melPath(outputPath);
            fbxExportOk = melExec("FBXExport -f \"" + fbxPath + "\" -s");
            debugInfo(std::string("exportBlendShapeFbx: FBXExport(withSkeleton) ")
                      + (fbxExportOk ? "succeeded" : "FAILED"));

            // Close undo chunk and undo to restore namespaces — but only if
            // the chunk was actually opened (i.e., at least one merge was attempted).
            // When all merges failed/skipped, no chunk was opened, so calling
            // closeChunk+undo here would corrupt Maya's undo queue.
            if (undoChunkOpen) {
                bool closeOk = melExec("undoInfo -closeChunk");
                undoChunkOpen = false;

                if (!closeOk) {
                    debugWarn("exportBlendShapeFbx: undoInfo -closeChunk FAILED; "
                              "scene undo state may be corrupted");
                    warnings.push_back("Undo chunk close failed; scene undo queue may be in an inconsistent state");
                }

                // Only undo if at least one merge actually succeeded.
                // If all merges failed, the chunk is empty — undo would revert
                // an unrelated previous operation.
                if (nsMergeSucceeded > 0) {
                    bool undoOk = melExec("undo");
                    if (!undoOk) {
                        debugWarn("exportBlendShapeFbx: undo FAILED after namespace merge; "
                                  "namespaces may remain merged in the scene");
                        warnings.push_back("Undo failed after namespace merge; scene namespaces may not be restored. "
                                           "Consider reopening the scene if bone names appear changed.");
                    } else {
                        debugInfo("exportBlendShapeFbx: undo chunk closed and namespace merge reverted successfully");
                    }
                } else {
                    debugInfo("exportBlendShapeFbx: undo chunk closed without undo (no successful merges to revert)");
                }
            } else {
                debugInfo("exportBlendShapeFbx: no undo chunk was opened (all merges skipped/not attempted)");
            }

            cleanupDupMesh();
        } else {
            // --- Mesh-only branch (no skeleton) ---
            melExec("select -replace \"" + dupMesh + "\"");
            debugSelectionSnapshot("exportBlendShapeFbx: preExportSelection(meshOnly)");

            setFbxExportDefaults();
            melExec("FBXExportInputConnections -v false");
            melExec("FBXExportSkins -v false");
            melExec(std::string("FBXExportShapes -v ") + (opts.bsShapes ? "true" : "false"));
            melExec("FBXExportAnimationOnly -v false");
            melExec("FBXExportBakeComplexAnimation -v true");
            melExec(std::string("FBXExportSmoothMesh -v ") + (opts.bsSmoothMesh ? "true" : "false"));
            setFbxBakeRange(startFrame, endFrame);

            melExec("FBXExportFileVersion -v " + opts.fileVersion);
            melExec("FBXExportUpAxis " + opts.upAxis);

            std::string fbxPath = melPath(outputPath);
            fbxExportOk = melExec("FBXExport -f \"" + fbxPath + "\" -s");
            cleanupDupMesh();
        }

        if (!fbxExportOk) {
            double duration = std::difftime(std::time(nullptr), startTime);
            int64_t fileSize = fileExistsOnDisk(outputPath) ? getFileSize(outputPath) : 0;
            return makeResult(false, outputPath, fileSize, duration, warnings,
                              {"FBXExport command failed in blendshape export"});
        }

        FbxContentStats fbxStats = scanFbxContent(outputPath);
        debugFbxContent("exportBlendShapeFbx", outputPath, fbxStats);

        double duration = std::difftime(std::time(nullptr), startTime);
        int64_t fileSize = fileExistsOnDisk(outputPath) ? getFileSize(outputPath) : 0;
        {
            std::ostringstream dbg;
            dbg << "exportBlendShapeFbx: exported file size=" << fileSize
                << ", duration=" << duration << "s";
            debugInfo(dbg.str());
        }

        if (fileSize <= 0) {
            return makeResult(false, outputPath, fileSize, duration, warnings,
                              {"BlendShape export produced empty file"});
        }

        if (fbxStats.meshes <= 0 && fbxStats.deformers <= 0) {
            return makeResult(false, outputPath, fileSize, duration, warnings,
                              {"BlendShape export expected mesh data but FBX contains no mesh/deformer markers"});
        }

        return makeResult(true, outputPath, fileSize, duration, warnings);
    } catch (...) {
        // Best-effort: close undo chunk and undo namespace merge if it was left open.
        // Log all recovery actions so failures here are diagnosable from the debug log.
        debugWarn("exportBlendShapeFbx: EXCEPTION caught, entering cleanup");
        try {
            if (undoChunkOpen) {
                debugWarn("exportBlendShapeFbx: undo chunk was open at exception time, attempting close");
                bool closeOk = melExec("undoInfo -closeChunk");
                if (anyMergeSucceeded) {
                    bool undoOk = melExec("undo");
                    if (closeOk && undoOk) {
                        debugInfo("exportBlendShapeFbx: exception recovery undo succeeded");
                    } else {
                        debugWarn("exportBlendShapeFbx: exception recovery undo "
                                  + std::string(closeOk ? "" : "closeChunk-FAILED ")
                                  + std::string(undoOk ? "" : "undo-FAILED")
                                  + "; scene namespaces may remain merged");
                    }
                } else {
                    debugInfo("exportBlendShapeFbx: exception recovery closed empty undo chunk (no merges to revert)");
                }
            }
        } catch (...) {
            debugWarn("exportBlendShapeFbx: nested exception during undo recovery");
        }
        // Best-effort cleanup of duplicate mesh on exception
        try {
            if (!dupMeshObj.isNull()) {
                MFnDagNode fn(dupMeshObj);
                std::string p = toUtf8(fn.fullPathName());
                if (!p.empty()) {
                    melExec("delete \"" + p + "\"");
                    debugInfo("exportBlendShapeFbx: exception cleanup deleted dupMesh: " + p);
                }
            }
        } catch (...) {
            debugWarn("exportBlendShapeFbx: nested exception during dupMesh cleanup");
        }
        double duration = std::difftime(std::time(nullptr), startTime);
        return makeResult(false, outputPath, 0, duration, warnings,
                          {"BlendShape export failed: unknown exception"});
    }
}

// ============================================================================
// exportSkeletonBlendShapeFbx — combined skeleton + blendshape export
// ============================================================================

ExportResult exportSkeletonBlendShapeFbx(
    const std::string& skeletonRoot,
    const std::vector<std::string>& bsMeshes,
    const std::vector<std::string>& bsWeightAttrs,
    const std::string& outputPath,
    int startFrame, int endFrame,
    const FbxExportOptions& opts) {

    std::vector<std::string> warnings;
    time_t startTime = std::time(nullptr);

    {
        std::ostringstream dbg;
        dbg << "exportSkeletonBlendShapeFbx: input=" << skeletonRoot
            << ", bsMeshes=" << bsMeshes.size()
            << ", bsWeightAttrs=" << bsWeightAttrs.size()
            << ", output=" << outputPath
            << ", range=" << startFrame << "-" << endFrame;
        debugInfo(dbg.str());
    }

    if (!ensureFbxPlugin()) {
        return makeResult(false, outputPath, 0, 0.0, {}, {"fbxmaya plugin load failed"});
    }

    if (!nodeExists(skeletonRoot)) {
        return makeResult(false, outputPath, 0, 0.0, {},
                          {"Skeleton root does not exist: " + skeletonRoot});
    }

    std::string outDir = getDirname(outputPath);
    if (!outDir.empty()) ensureDir(outDir);

    // Declare rename-tracking variables outside try so catch can access them for cleanup
    struct NodeRec {
        MObject obj;
        std::string originalName;
    };
    std::vector<NodeRec> jointRenameRecs;
    std::vector<NodeRec> meshRenameRecs;
    bool didRenameJoints = false;
    bool didRenameMeshes = false;

    try {
    // Resolve root joint — same logic as exportSkeletonFbx
    std::string rootJoint = skeletonRoot;
    MObject rootObj;
    bool rootReferenced = false;

    {
        std::string type = melQueryString("nodeType \"" + rootJoint + "\"");
        if (type != "joint") {
            std::vector<std::string> joints = melQueryStringArray(
                "listRelatives -allDescendents -type \"joint\" -fullPath \"" + rootJoint + "\"");
            if (joints.empty()) {
                return makeResult(false, outputPath, 0, 0.0, {},
                                  {"No joints found under: " + rootJoint});
            }
            std::set<std::string> jointSet(joints.begin(), joints.end());
            std::vector<std::string> candidates;
            for (const auto& j : joints) {
                std::vector<std::string> parents = melQueryStringArray(
                    "listRelatives -parent -type \"joint\" -fullPath \"" + j + "\"");
                if (parents.empty() || jointSet.count(parents[0]) == 0) {
                    candidates.push_back(j);
                }
            }
            if (!candidates.empty()) {
                std::string best = candidates[0];
                int bestCount = -1;
                for (const auto& c : candidates) {
                    std::vector<std::string> desc = melQueryStringArray(
                        "listRelatives -allDescendents -type \"joint\" -fullPath \"" + c + "\"");
                    if ((int)desc.size() > bestCount) {
                        bestCount = (int)desc.size();
                        best = c;
                    }
                }
                warnings.push_back("Input node is not a joint; using joint root: " + best);
                rootJoint = best;
            } else {
                warnings.push_back("Input node is not a joint; using first joint found: " + joints[0]);
                rootJoint = joints[0];
            }
        }
    }

    {
        MSelectionList rootSel;
        rootSel.add(MString(rootJoint.c_str()));
        rootSel.getDependNode(0, rootObj);
        MFnDagNode rootFn(rootObj);
        rootJoint = toUtf8(rootFn.fullPathName());
        rootReferenced = isReferencedNode(rootObj);
    }

    {
        std::ostringstream dbg;
        dbg << "exportSkeletonBlendShapeFbx: exportPath="
            << (rootReferenced ? "inPlace(referenced)" : "inPlace(local)")
            << ", rootReferenced=" << (rootReferenced ? "true" : "false");
        debugInfo(dbg.str());
    }

    // Collect all joints
    std::vector<std::string> allJoints = melQueryStringArray(
        "listRelatives -allDescendents -type \"joint\" -fullPath \"" + rootJoint + "\"");
    allJoints.push_back(rootJoint);

    // Collect skinned mesh transforms for these joints
    int skinClusterCount = 0;
    int meshShapeCount = 0;
    std::vector<std::string> skinnedMeshTransforms =
        collectSkinnedMeshTransformsForJoints(allJoints, &skinClusterCount, &meshShapeCount);

    // Ensure all bsMeshes are included in the selection
    std::set<std::string> meshSet(skinnedMeshTransforms.begin(), skinnedMeshTransforms.end());
    for (const auto& bm : bsMeshes) {
        if (meshSet.count(bm) == 0) {
            if (nodeExists(bm)) {
                skinnedMeshTransforms.push_back(bm);
                meshSet.insert(bm);
                warnings.push_back("BS mesh not in skinCluster set, added: " + bm);
            } else {
                warnings.push_back("BS mesh does not exist, skipped: " + bm);
            }
        }
    }

    {
        std::ostringstream dbg;
        dbg << "exportSkeletonBlendShapeFbx: jointCount=" << allJoints.size()
            << ", skinnedMeshCount=" << skinnedMeshTransforms.size()
            << ", bsMeshCount=" << bsMeshes.size();
        debugInfo(dbg.str());
    }

    if (allJoints.empty()) {
        return makeResult(false, outputPath, 0, 0.0, warnings,
                          {"No joints collected for export"});
    }

    // Verify BS weight attributes have keyframes (should have been baked by batchBakeAll)
    {
        int keyed = 0;
        int unkeyed = 0;
        for (const auto& attr : bsWeightAttrs) {
            int keyCount = 0;
            std::string cmd = "keyframe -q -keyframeCount \"" + attr + "\"";
            MGlobal::executeCommand(utf8ToMString(cmd), keyCount);
            if (keyCount > 0) {
                ++keyed;
            } else {
                ++unkeyed;
            }
        }
        std::ostringstream dbg;
        dbg << "exportSkeletonBlendShapeFbx: verifyBsWeightKeys: keyed=" << keyed
            << ", unkeyed=" << unkeyed << ", total=" << bsWeightAttrs.size();
        debugInfo(dbg.str());

        if (keyed == 0 && !bsWeightAttrs.empty()) {
            warnings.push_back("No BS weight attributes have keyframes after bake");
        }
    }

    // --- Namespace stripping via per-node rename (same strategy as exportSkeletonFbx) ---
    // The previous "merge outermost namespace" approach fails for nested namespaces
    // (e.g. ShotNS:RigNS:Joint). Instead, rename each joint/mesh to its bare name
    // using the proven rename-deepest-first strategy, then restore via MObject handles.
    bool hadNamespaceOnBones = false;

    // Build MObject records for joints
    {
        MSelectionList jSel;
        for (const auto& j : allJoints) {
            jSel.add(MString(j.c_str()));
        }
        jointRenameRecs.reserve(allJoints.size());
        for (unsigned int i = 0; i < jSel.length() && i < allJoints.size(); ++i) {
            MObject obj;
            jSel.getDependNode(i, obj);
            MFnDependencyNode fn(obj);
            NodeRec rec;
            rec.obj = obj;
            rec.originalName = toUtf8(fn.name());
            jointRenameRecs.push_back(rec);
        }
    }

    // Build MObject records for skinned mesh transforms
    {
        MSelectionList mSel;
        for (const auto& m : skinnedMeshTransforms) {
            mSel.add(MString(m.c_str()));
        }
        meshRenameRecs.reserve(skinnedMeshTransforms.size());
        for (unsigned int i = 0; i < mSel.length() && i < skinnedMeshTransforms.size(); ++i) {
            MObject obj;
            mSel.getDependNode(i, obj);
            MFnDependencyNode fn(obj);
            NodeRec rec;
            rec.obj = obj;
            rec.originalName = toUtf8(fn.name());
            meshRenameRecs.push_back(rec);
        }
    }

    // Rename joints: strip namespaces + normalize root bone
    {
        struct WorkItem {
            std::string fullPath;
            int depth;
            bool isRoot;
            std::string leaf;
            std::string desiredBare;
            bool needsRename;
        };
        std::vector<WorkItem> work;
        work.reserve(allJoints.size());

        for (const auto& p : allJoints) {
            WorkItem wi;
            wi.fullPath = p;
            wi.depth = dagDepth(p);
            wi.isRoot = (p == rootJoint);
            wi.leaf = dagLeafName(p);

            std::string bare = stripAllNamespaces(wi.leaf);
            if (wi.isRoot) {
                bare = normalizeRootBoneName(bare);
            }
            wi.desiredBare = bare;

            bool hasNs = (wi.leaf.find(':') != std::string::npos);
            bool rootNeedsNorm = false;
            if (wi.isRoot) {
                std::string bare0 = stripAllNamespaces(wi.leaf);
                rootNeedsNorm = (normalizeRootBoneName(bare0) != bare0);
            }
            wi.needsRename = hasNs || rootNeedsNorm;
            if (hasNs) hadNamespaceOnBones = true;
            if (wi.needsRename) didRenameJoints = true;
            work.push_back(wi);
        }

        if (didRenameJoints) {
            // Rename deepest first so parent renames don't invalidate child paths
            std::sort(work.begin(), work.end(),
                      [](const WorkItem& a, const WorkItem& b) {
                          return a.depth > b.depth;
                      });

            int renameOk = 0, renameFail = 0;
            for (const auto& wi : work) {
                if (!wi.needsRename) continue;
                std::string target = ":" + wi.desiredBare;
                std::string cmd = "rename \"" + wi.fullPath + "\" \"" + target + "\"";
                std::string result = melQueryString(cmd);
                if (!result.empty()) {
                    ++renameOk;
                } else {
                    ++renameFail;
                }
            }

            debugInfo("exportSkeletonBlendShapeFbx: jointRename{ok=" + std::to_string(renameOk)
                      + ", fail=" + std::to_string(renameFail) + "}");

            if (renameFail > 0) {
                warnings.push_back("Failed to rename " + std::to_string(renameFail)
                                   + " joint(s) — exported bone names may contain namespace prefixes");
            }

            // Update rootJoint path after rename
            if (!rootObj.isNull()) {
                MFnDagNode fn(rootObj);
                rootJoint = toUtf8(fn.fullPathName());
            }
        }
    }

    // Re-query joints after rename
    allJoints = melQueryStringArray(
        "listRelatives -allDescendents -type \"joint\" -fullPath \"" + rootJoint + "\"");
    allJoints.push_back(rootJoint);

    // Safety check: verify no namespace prefixes remain on joints
    {
        int namespacedCount = 0;
        for (const auto& j : allJoints) {
            std::string leaf = dagLeafName(j);
            if (!leaf.empty() && leaf[0] == ':') leaf = leaf.substr(1);
            if (leaf.find(':') != std::string::npos) {
                ++namespacedCount;
            }
        }
        debugInfo("exportSkeletonBlendShapeFbx: allJointsForExport=" + std::to_string(allJoints.size())
                  + ", namespacedAfterCleanup=" + std::to_string(namespacedCount)
                  + ", didRename=" + std::string(didRenameJoints ? "true" : "false"));
        if (namespacedCount > 0) {
            warnings.push_back(std::to_string(namespacedCount)
                               + " joint(s) still have namespace prefixes after rename");
        }
    }

    // Rename skinned mesh transforms to strip namespaces
    {
        skinnedMeshTransforms =
            collectSkinnedMeshTransformsForJoints(allJoints, nullptr, nullptr);
        // Re-add bsMeshes using MObject-based resolution (safe after joint rename)
        meshSet.clear();
        meshSet.insert(skinnedMeshTransforms.begin(), skinnedMeshTransforms.end());
        for (const auto& bm : bsMeshes) {
            if (meshSet.count(bm) == 0) {
                // Resolve via MObject if the original path is stale
                if (nodeExists(bm)) {
                    skinnedMeshTransforms.push_back(bm);
                    meshSet.insert(bm);
                } else {
                    // Try bare name lookup, but constrain to the skeleton's DAG hierarchy
                    std::string bareMesh = dagLeafName(bm);
                    bareMesh = stripAllNamespaces(bareMesh);
                    std::vector<std::string> resolved = melQueryStringArray(
                        "ls -long \"*|" + bareMesh + "\"");
                    // Filter: only accept nodes under the same top-level hierarchy as rootJoint
                    std::string rootTop = rootJoint;
                    {
                        size_t secondPipe = rootTop.find('|', 1);
                        if (secondPipe != std::string::npos) rootTop = rootTop.substr(0, secondPipe);
                    }
                    for (const auto& r : resolved) {
                        if (r.find(rootTop) != 0 || meshSet.count(r) != 0) continue;

                        // Only accept transform nodes with mesh children.
                        if (melQueryString("nodeType \"" + r + "\"") != "transform") continue;
                        std::vector<std::string> shapes = melQueryStringArray(
                            "listRelatives -children -type \"mesh\" -fullPath \"" + r + "\"");
                        if (shapes.empty()) continue;

                        if (meshSet.count(r) == 0) {
                            skinnedMeshTransforms.push_back(r);
                            meshSet.insert(r);
                            debugInfo("exportSkeletonBlendShapeFbx: re-resolved bsMesh '" + bm + "' -> '" + r + "'");
                        }
                    }
                }
            }
        }

        // Rename mesh transforms that still have namespaces
        for (auto& m : skinnedMeshTransforms) {
            std::string leaf = dagLeafName(m);
            if (leaf.find(':') != std::string::npos) {
                std::string bare = stripAllNamespaces(leaf);
                std::string cmd = "rename \"" + m + "\" \":" + bare + "\"";
                std::string result = melQueryString(cmd);
                if (!result.empty()) {
                    didRenameMeshes = true;
                }
            }
        }

        // Re-query mesh paths after rename
        if (didRenameMeshes) {
            skinnedMeshTransforms =
                collectSkinnedMeshTransformsForJoints(allJoints, nullptr, nullptr);
            meshSet.clear();
            meshSet.insert(skinnedMeshTransforms.begin(), skinnedMeshTransforms.end());
        }
    }

    if (hadNamespaceOnBones) {
        warnings.push_back("Detected namespaces in skeleton joints; stripped during export");
    }

    // Build selection: joints + skinned meshes (including BS meshes)
    MSelectionList sel;
    for (const auto& j : allJoints) {
        sel.add(MString(j.c_str()));
    }
    for (const auto& m : skinnedMeshTransforms) {
        sel.add(MString(m.c_str()));
    }
    MGlobal::setActiveSelectionList(sel, MGlobal::kReplaceList);
    debugSelectionSnapshot("exportSkeletonBlendShapeFbx: preExportSelection");

    // FBX settings — key difference from pure skeleton: Shapes=true, AnimOnly=false
    setFbxExportDefaults();
    if (opts.skelAnimationOnly) {
        warnings.push_back("Skeleton AnimationOnly(UI)=true is ignored for Skeleton+BlendShape export (mesh/skin required)");
        debugWarn("exportSkeletonBlendShapeFbx: override AnimationOnly(UI)=true -> exporting mesh/skin for BlendShape");
    }
    if (!opts.bsShapes) {
        warnings.push_back("BlendShape Shapes(UI)=false: morph target geometry will not be exported");
    }
    if (!opts.skelSkeletonDefs) {
        warnings.push_back("SkeletonDefs(UI)=false: FBX skeleton hierarchy metadata may be incomplete in some DCC/engines");
    }
    melExec(std::string("FBXExportShapes -v ") + (opts.bsShapes ? "true" : "false"));
    melExec("FBXExportSkins -v true");
    melExec("FBXExportAnimationOnly -v false");
    melExec(std::string("FBXExportBakeComplexAnimation -v ") + (opts.skelBakeComplex ? "true" : "false"));
    melExec(std::string("FBXExportSkeletonDefinitions -v ") + (opts.skelSkeletonDefs ? "true" : "false"));
    melExec(std::string("FBXExportConstraints -v ") + (opts.skelConstraints ? "true" : "false"));
    melExec(std::string("FBXExportInputConnections -v ") + (opts.skelInputConns ? "true" : "false"));
    melExec(std::string("FBXExportSmoothMesh -v ") + (opts.bsSmoothMesh ? "true" : "false"));
    setFbxBakeRange(startFrame, endFrame);
    melExec("FBXExportFileVersion -v " + opts.fileVersion);
    melExec("FBXExportUpAxis " + opts.upAxis);

    {
        std::ostringstream dbg;
        dbg << "exportSkeletonBlendShapeFbx: FBX settings: Shapes=true, Skins=true"
            << ", AnimOnly=false, BakeComplex=" << (opts.skelBakeComplex ? "true" : "false")
            << ", SmoothMesh=" << (opts.bsSmoothMesh ? "true" : "false");
        debugInfo(dbg.str());
    }

    // Export
    std::string fbxPath = melPath(outputPath);
    bool fbxExportOk = melExec("FBXExport -f \"" + fbxPath + "\" -s");

    debugInfo(std::string("exportSkeletonBlendShapeFbx: FBXExport result=")
              + (fbxExportOk ? "ok" : "fail"));

    // Restore original names on joints and meshes via MObject handles
    {
        int restoreOk = 0, restoreFail = 0;
        if (didRenameJoints) {
            for (auto& rec : jointRenameRecs) {
                if (rec.obj.isNull()) continue;
                MFnDependencyNode fn(rec.obj);
                std::string currentName = toUtf8(fn.name());
                if (currentName != rec.originalName) {
                    MStatus st;
                    fn.setName(MString(rec.originalName.c_str()), false, &st);
                    if (st == MS::kSuccess) ++restoreOk; else ++restoreFail;
                }
            }
        }
        if (didRenameMeshes) {
            for (auto& rec : meshRenameRecs) {
                if (rec.obj.isNull()) continue;
                MFnDependencyNode fn(rec.obj);
                std::string currentName = toUtf8(fn.name());
                if (currentName != rec.originalName) {
                    MStatus st;
                    fn.setName(MString(rec.originalName.c_str()), false, &st);
                    if (st == MS::kSuccess) ++restoreOk; else ++restoreFail;
                }
            }
        }
        if (didRenameJoints || didRenameMeshes) {
            debugInfo("exportSkeletonBlendShapeFbx: nameRestore{ok=" + std::to_string(restoreOk)
                      + ", fail=" + std::to_string(restoreFail) + "}");
            if (restoreFail > 0) {
                debugWarn("exportSkeletonBlendShapeFbx: failed to restore " + std::to_string(restoreFail)
                          + " node name(s) — scene may have modified names");
            }
        }
    }

    if (!fbxExportOk) {
        double duration = std::difftime(std::time(nullptr), startTime);
        return makeResult(false, outputPath, 0, duration, warnings,
                          {"FBXExport command failed for skeleton+blendshape export"});
    }

    // Verify output
    FbxContentStats fbxStats = scanFbxContent(outputPath);
    debugFbxContent("exportSkeletonBlendShapeFbx", outputPath, fbxStats);

    if (fbxStats.limbNodes <= 0) {
        warnings.push_back("Exported FBX contains no LimbNode bones");
    }
    if (fbxStats.blendShapes <= 0) {
        warnings.push_back("Exported FBX contains no BlendShape/Shape data — "
                           "UE may not import MorphTargets from this file");
    }

    // Verify no namespace colons remain in exported bone names (quick binary scan).
    // We only consider Model entries whose type is LimbNode, to avoid false positives
    // from namespaced non-bone nodes (Nulls, materials, etc.).
    {
#ifdef _WIN32
        std::ifstream fbxCheck(utf8ToWide(outputPath), std::ios::binary);
#else
        std::ifstream fbxCheck(outputPath.c_str(), std::ios::binary);
#endif
        if (fbxCheck.is_open()) {
            std::string fbxData((std::istreambuf_iterator<char>(fbxCheck)),
                                 std::istreambuf_iterator<char>());
            // Search for "Model" entries of type "LimbNode" that contain ':' in the node name.
            // FBX binary format stores node names near "Model" tokens.
            int colonBoneCount = 0;
            const std::string token = "Model";
            size_t searchPos = 0;
            while ((searchPos = fbxData.find(token, searchPos)) != std::string::npos) {
                // Only consider likely bone models (LimbNode appears near Model entries).
                size_t regionEnd = (std::min)(searchPos + 260, fbxData.size());
                std::string_view region(&fbxData[searchPos], regionEnd - searchPos);
                if (region.find("LimbNode") == std::string_view::npos) {
                    searchPos += token.size();
                    continue;
                }

                // Look ahead for a colon within the next bytes until NUL/newline.
                size_t nameStart = searchPos + token.size();
                for (size_t p = nameStart; p < regionEnd; ++p) {
                    char c = fbxData[p];
                    if (c == ':') { ++colonBoneCount; break; }
                    if (c == '\0' || c == '\n') break;
                }
                searchPos += token.size();
            }
            if (colonBoneCount > 0) {
                std::string warnMsg = "FBX file contains " + std::to_string(colonBoneCount)
                    + " LimbNode bone name(s) with ':' — namespace residue detected, UE may see unexpected bone names";
                warnings.push_back(warnMsg);
                debugWarn("exportSkeletonBlendShapeFbx: " + warnMsg);
            } else {
                debugInfo("exportSkeletonBlendShapeFbx: FBX namespace residue check PASSED (no colons in node names)");
            }
        }
    }

    double duration = std::difftime(std::time(nullptr), startTime);
    int64_t fileSize = fileExistsOnDisk(outputPath) ? getFileSize(outputPath) : 0;

    {
        std::ostringstream dbg;
        dbg << "exportSkeletonBlendShapeFbx: exported file size=" << fileSize
            << ", duration=" << duration << "s";
        debugInfo(dbg.str());
    }

    if (fileSize <= 0) {
        return makeResult(false, outputPath, fileSize, duration, warnings,
                          {"Skeleton+BlendShape export produced empty file"});
    }

    return makeResult(true, outputPath, fileSize, duration, warnings);

    } catch (...) {
        debugWarn("exportSkeletonBlendShapeFbx: EXCEPTION caught, entering cleanup");
        try {
            // Restore original names via MObject handles
            if (didRenameJoints) {
                for (auto& rec : jointRenameRecs) {
                    if (rec.obj.isNull()) continue;
                    MFnDependencyNode fn(rec.obj);
                    fn.setName(MString(rec.originalName.c_str()));
                }
                debugInfo("exportSkeletonBlendShapeFbx: exception recovery — joint names restored");
            }
            if (didRenameMeshes) {
                for (auto& rec : meshRenameRecs) {
                    if (rec.obj.isNull()) continue;
                    MFnDependencyNode fn(rec.obj);
                    fn.setName(MString(rec.originalName.c_str()));
                }
                debugInfo("exportSkeletonBlendShapeFbx: exception recovery — mesh names restored");
            }
        } catch (...) {
            debugWarn("exportSkeletonBlendShapeFbx: nested exception during name restore recovery");
        }
        double duration = std::difftime(std::time(nullptr), startTime);
        return makeResult(false, outputPath, 0, duration, warnings,
                          {"Skeleton+BlendShape export failed: unknown exception"});
    }
}

// Helper: query keyframe count on a node/attr, returns true if > 0
static bool hasKeys(const std::string& target) {
    int count = 0;
    std::string cmd = "keyframe -q -keyframeCount \"" + target + "\"";
    MGlobal::executeCommand(utf8ToMString(cmd), count);
    return count > 0;
}

// Helper: query first or last keyframe on a node/attr
// which = "first" or "last"
static double findKey(const std::string& target, const std::string& which) {
    double val = 0.0;
    std::string cmd = "findKeyframe -which " + which + " \"" + target + "\"";
    MGlobal::executeCommand(utf8ToMString(cmd), val);
    return val;
}

FrameRangeInfo queryFrameRange(const ExportItem& item) {
    FrameRangeInfo info;
    info.name     = item.name;
    info.type     = item.type;
    info.filename = item.filename;
    info.firstKey = 0.0;
    info.lastKey  = 0.0;
    info.valid    = false;

    if (!nodeExists(item.node)) return info;

    double globalMin =  1e18;
    double globalMax = -1e18;
    bool found = false;

    if (item.type == "camera") {
        // Camera: query the transform node directly
        if (hasKeys(item.node)) {
            double first = findKey(item.node, "first");
            double last  = findKey(item.node, "last");
            if (first <= last) {
                globalMin = (std::min)(globalMin, first);
                globalMax = (std::max)(globalMax, last);
                found = true;
            }
        }

    } else if (item.type == "skeleton" || item.type == "skeleton+blendshape") {
        // Skeleton: root joint + all descendant joints.
        // 1) Try explicit keyframe queries first.
        // 2) If no explicit keys exist (common for constraint-driven rigs),
        //    sample transform deltas across playback range as fallback.
        std::string listCmd = "listRelatives -allDescendents -type \"joint\" -fullPath \""
                              + item.node + "\"";
        std::vector<std::string> allJoints = melQueryStringArray(listCmd);
        allJoints.push_back(item.node);

        for (const auto& jnt : allJoints) {
            if (!hasKeys(jnt)) continue;
            double first = findKey(jnt, "first");
            double last  = findKey(jnt, "last");
            if (first <= last) {
                globalMin = (std::min)(globalMin, first);
                globalMax = (std::max)(globalMax, last);
                found = true;
            }
        }

        // If skeleton has BS weight attrs attached, also query their keyframe ranges
        if (!item.bsWeightAttrs.empty()) {
            for (const auto& attr : item.bsWeightAttrs) {
                std::string bsNode = attr;
                size_t dotPos = bsNode.find('.');
                if (dotPos != std::string::npos) {
                    bsNode = bsNode.substr(0, dotPos);
                }
                if (!hasKeys(bsNode)) continue;
                double first = findKey(bsNode, "first");
                double last  = findKey(bsNode, "last");
                if (first <= last) {
                    globalMin = (std::min)(globalMin, first);
                    globalMax = (std::max)(globalMax, last);
                    found = true;
                }
            }
        }

        if (!found && !allJoints.empty()) {
            double playMin = 0.0;
            double playMax = 0.0;
            MGlobal::executeCommand("playbackOptions -q -minTime", playMin);
            MGlobal::executeCommand("playbackOptions -q -maxTime", playMax);
            if (playMax < playMin) std::swap(playMin, playMax);

            int sampleStart = static_cast<int>(playMin);
            int sampleEnd = static_cast<int>(playMax);

            // If export-range env vars are present, use exported range directly
            // so frame-range log stays consistent with actual FBX output.
            // (playbackOptions may not cover negative frames that were actually exported.)
            const char* envStart = std::getenv("MAYA_REF_EXPORT_RANGE_START");
            const char* envEnd = std::getenv("MAYA_REF_EXPORT_RANGE_END");
            if (envStart && envEnd) {
                int exportStart = std::atoi(envStart);
                int exportEnd = std::atoi(envEnd);
                if (exportEnd < exportStart) std::swap(exportStart, exportEnd);
                sampleStart = exportStart;
                sampleEnd = exportEnd;
            }

            // Sample up to 24 joints to keep log generation responsive on big rigs.
            size_t maxSamples = 24;
            size_t stride = allJoints.size() > maxSamples ? (allJoints.size() / maxSamples) : 1;
            if (stride == 0) stride = 1;

            double bestDelta = 0.0;
            std::string bestJoint;

            for (size_t i = 0; i < allJoints.size(); i += stride) {
                const std::string& j = allJoints[i];
                const char* attrs[] = {"tx", "ty", "tz", "rx", "ry", "rz"};
                double totalDelta = 0.0;
                int okAttrs = 0;

                for (const char* attr : attrs) {
                    double v0 = 0.0;
                    double v1 = 0.0;
                    if (!queryAttrAtTime(j, attr, sampleStart, v0)) continue;
                    if (!queryAttrAtTime(j, attr, sampleEnd, v1)) continue;
                    totalDelta += std::abs(v1 - v0);
                    ++okAttrs;
                }

                if (okAttrs > 0 && totalDelta > bestDelta) {
                    bestDelta = totalDelta;
                    bestJoint = j;
                }
            }

            {
                std::ostringstream dbg;
                dbg << "queryFrameRange(skeleton): root=" << item.node
                    << ", sampledJoints=" << ((allJoints.size() + stride - 1) / stride)
                    << ", playbackRange=" << sampleStart << "-" << sampleEnd
                    << ", bestDelta=" << bestDelta
                    << ", bestJoint=" << (bestJoint.empty() ? "<none>" : bestJoint);
                debugInfo(dbg.str());
            }

            if (bestDelta > 1e-4) {
                globalMin = sampleStart;
                globalMax = sampleEnd;
                found = true;
            }
        }

    } else if (item.type == "blendshape") {
        // BlendShape: query mesh history and filter blendShape deformers
        std::vector<std::string> history = melQueryStringArray(
            "listHistory -pruneDagObjects true \"" + item.node + "\"");

        for (const auto& histNode : history) {
            if (melQueryString("nodeType \"" + histNode + "\"") != "blendShape") continue;
            if (!hasKeys(histNode)) continue;

            double first = findKey(histNode, "first");
            double last  = findKey(histNode, "last");
            if (first <= last) {
                globalMin = (std::min)(globalMin, first);
                globalMax = (std::max)(globalMax, last);
                found = true;
            }
        }

    }

    if (found) {
        info.firstKey = globalMin;
        info.lastKey  = globalMax;
        info.valid    = true;
    }
    return info;
}

std::string writeFrameRangeLog(const std::string& outputDir,
                               const std::vector<FrameRangeInfo>& ranges,
                               int userStartFrame, int userEndFrame,
                               double fps) {
    // Build timestamp string
    std::time_t now = std::time(nullptr);
    std::tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char timeFmt[64];
    std::strftime(timeFmt, sizeof(timeFmt), "%Y%m%d_%H%M%S", &tmBuf);
    char dateDisplay[64];
    std::strftime(dateDisplay, sizeof(dateDisplay), "%Y-%m-%d %H:%M:%S", &tmBuf);

    // Build output path — use frame range in filename for easy identification
    // Filename: "导出区间 {start} - {end}.txt"  (no colon — illegal on Windows)
    std::string dirNorm = outputDir;
    for (auto& c : dirNorm) { if (c == '\\') c = '/'; }
    if (!dirNorm.empty() && dirNorm.back() != '/') dirNorm += '/';
    std::string logPath = dirNorm + u8"\u5bfc\u51fa\u533a\u95f4 "
        + std::to_string(userStartFrame) + " - " + std::to_string(userEndFrame) + ".txt";

    // Append to existing export_log file (header was written at export start by BatchExporterUI).
    // If file doesn't exist yet, write fresh with BOM.
#ifdef _WIN32
    std::wstring wLogPath = utf8ToWide(logPath);
    bool fileExists = false;
    {
        struct _stat st;
        fileExists = (_wstat(wLogPath.c_str(), &st) == 0);
    }
    std::ofstream ofs(wLogPath, std::ios::binary | (fileExists ? std::ios::app : std::ios::openmode(0)));
#else
    struct stat st;
    bool fileExists = (stat(logPath.c_str(), &st) == 0);
    std::ofstream ofs(logPath, std::ios::binary | (fileExists ? std::ios::app : std::ios::openmode(0)));
#endif
    if (!ofs.is_open()) return std::string();
    if (!fileExists) {
        ofs << "\xEF\xBB\xBF";
    }

    if (fps <= 0.0) fps = 30.0;
    if (userEndFrame < userStartFrame) std::swap(userStartFrame, userEndFrame);

    auto typeLabelCn = [](const std::string& type) -> std::string {
        if (type == "camera") return u8"\u76f8\u673a";
        if (type == "skeleton") return u8"\u9aa8\u9abc";
        if (type == "blendshape") return u8"\u8868\u60c5(BlendShape)";
        if (type == "skeleton+blendshape") return u8"\u9aa8\u9abc+\u8868\u60c5(Skel+BS)";
        return type;
    };


    ofs << "==============================\n";
    ofs << u8"\u5bfc\u51fa\u65e5\u5fd7\n"; // 导出日志
    ofs << "==============================\n";
    ofs << u8"\u65f6\u95f4: " << dateDisplay << "\n";
    ofs << u8"\u5bfc\u51fa\u533a\u95f4: \u3010" << userStartFrame << " - " << userEndFrame << u8"\u3011\n";
    ofs << u8"\u603b\u5e27\u6570: " << (userEndFrame - userStartFrame) << "f\n";
    ofs << "FPS: " << static_cast<int>(fps) << "\n";
    ofs << "\n";

    for (size_t i = 0; i < ranges.size(); ++i) {
        const FrameRangeInfo& r = ranges[i];
        ofs << "[" << (i + 1) << "] " << typeLabelCn(r.type) << " | ";
        ofs << r.name << " | ";
        ofs << u8"\u6587\u4ef6: " << r.filename << " | ";
        ofs << u8"\u5bfc\u51fa\u533a\u95f4: \u3010" << userStartFrame << " - " << userEndFrame << u8"\u3011\n";
    }

    ofs << "\n";
    ofs << "------------------------------\n";
    ofs << u8"\u7edf\u8ba1: \u5171 " << ranges.size() << u8" \u9879\n";
    ofs << "------------------------------\n";

    ofs.close();
    return logPath;
}

} // namespace AnimExporter

