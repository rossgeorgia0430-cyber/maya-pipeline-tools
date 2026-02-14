#include "SceneScanner.h"
#include "PluginLog.h"

#include <maya/MGlobal.h>
#include <maya/MString.h>
#include <maya/MStringArray.h>
#include <maya/MSelectionList.h>
#include <maya/MDagPath.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnCamera.h>
#include <maya/MFnReference.h>
#include <maya/MItDependencyNodes.h>
#include <maya/MItDag.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MPlug.h>
#include <maya/MObjectArray.h>

#include <regex>
#include <algorithm>
#include <set>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <cctype>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

// Convert MString to UTF-8 std::string safely on Windows
static std::string toUtf8(const MString& ms) {
#ifdef _WIN32
    const wchar_t* wstr = ms.asWChar();
    if (!wstr || !*wstr) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string(ms.asChar());  // fallback
    std::string result(len, '\0');
    int ret = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    if (ret <= 0) return std::string(ms.asChar());
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
#else
    return std::string(ms.asChar());
#endif
}

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

// Helper: execute MEL and return string result
static std::string melQueryString(const std::string& cmd) {
    MString result;
    MGlobal::executeCommand(utf8ToMString(cmd), result);
    return toUtf8(result);
}

// Helper: execute MEL and return string array result
static std::vector<std::string> melQueryStringArray(const std::string& cmd) {
    MStringArray result;
    MGlobal::executeCommand(utf8ToMString(cmd), result);
    std::vector<std::string> vec;
    for (unsigned int i = 0; i < result.length(); ++i) {
        vec.push_back(toUtf8(result[i]));
    }
    return vec;
}

// Helper: execute MEL and return int result
static int melQueryInt(const std::string& cmd) {
    int result = 0;
    MGlobal::executeCommand(utf8ToMString(cmd), result);
    return result;
}

// Helper: check if file exists (UTF-8 safe on Windows)
static bool fileExistsOnDisk(const std::string& utf8Path) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wpath(wlen, L'\0');
    int ret = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, &wpath[0], wlen);
    if (ret <= 0) return false;
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
    struct _stat st;
    return (_wstat(wpath.c_str(), &st) == 0 && (st.st_mode & S_IFREG));
#else
    struct stat st;
    return (stat(utf8Path.c_str(), &st) == 0 && (st.st_mode & S_IFREG));
#endif
}

// Helper: get short name from full DAG path
static std::string shortName(const std::string& fullPath) {
    size_t pos = fullPath.rfind('|');
    return (pos != std::string::npos) ? fullPath.substr(pos + 1) : fullPath;
}

// Helper: get bare name (strip namespace)
static std::string bareName(const std::string& name) {
    size_t pos = name.rfind(':');
    return (pos != std::string::npos) ? name.substr(pos + 1) : name;
}

// Helper: get innermost namespace segment from short name
// For "A:B:NodeName" returns "B", for "B:NodeName" returns "B", for "NodeName" returns ""
static std::string getNamespace(const std::string& shortName) {
    size_t lastColon = shortName.rfind(':');
    if (lastColon == std::string::npos) return "";
    std::string ns = shortName.substr(0, lastColon);
    size_t prevColon = ns.rfind(':');
    return (prevColon != std::string::npos) ? ns.substr(prevColon + 1) : ns;
}

// Helper: normalize path
static std::string normPath(const std::string& path) {
    std::string p = path;
    for (auto& c : p) {
        if (c == '\\') c = '/';
    }
    return p;
}

static std::string expandEnvVars(const std::string& input) {
    std::string value = input;

#ifdef _WIN32
    if (value.find('%') != std::string::npos) {
        // Convert UTF-8 to wide, expand, convert back to UTF-8
        int wlen = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            std::wstring wvalue(wlen, L'\0');
            int ret = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &wvalue[0], wlen);
            if (ret > 0) {
                if (!wvalue.empty() && wvalue.back() == L'\0') wvalue.pop_back();
                wchar_t wexpanded[32768];
                DWORD elen = ExpandEnvironmentStringsW(wvalue.c_str(), wexpanded, 32768);
                if (elen > 0 && elen < 32768) {
                    int u8len = WideCharToMultiByte(CP_UTF8, 0, wexpanded, -1, nullptr, 0, nullptr, nullptr);
                    if (u8len > 0) {
                        std::string utf8result(u8len, '\0');
                        int ret2 = WideCharToMultiByte(CP_UTF8, 0, wexpanded, -1, &utf8result[0], u8len, nullptr, nullptr);
                        if (ret2 > 0 && !utf8result.empty() && utf8result.back() == '\0') utf8result.pop_back();
                        value = utf8result;
                    }
                }
            }
        }
    }
#endif

    // Expand $VAR or ${VAR}
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '$') {
            size_t start = i + 1;
            size_t end = start;
            std::string varName;
            bool usedBraces = false;
            if (start < value.size() && value[start] == '{') {
                usedBraces = true;
                start++;
                end = start;
                while (end < value.size() && value[end] != '}') {
                    end++;
                }
                if (end < value.size()) {
                    varName = value.substr(start, end - start);
                    i = end;
                }
            } else {
                while (end < value.size() &&
                       (std::isalnum((unsigned char)value[end]) || value[end] == '_')) {
                    end++;
                }
                varName = value.substr(start, end - start);
                i = end - 1;
            }
            if (!varName.empty()) {
                const char* envVal = std::getenv(varName.c_str());
                if (envVal) {
                    out += envVal;
                    continue;
                }
            }
            out += '$';
            if (usedBraces) {
                out += '{';
                out += varName;
                out += '}';
            } else {
                out += varName;
            }
        } else {
            out += value[i];
        }
    }

    return out;
}

namespace SceneScanner {

std::string getSceneDir() {
    MString scenePath;
    MGlobal::executeCommand("file -q -sceneName", scenePath);
    std::string sp = toUtf8(scenePath);
    if (sp.empty()) return "";
    for (auto& c : sp) {
        if (c == '\\') c = '/';
    }
    size_t pos = sp.rfind('/');
    if (pos != std::string::npos) {
        return sp.substr(0, pos);
    }
    return "";
}

std::string resolveSceneRelative(const std::string& rawPath) {
    if (rawPath.empty()) return "";

    std::string normalized = rawPath;
    // Remove Maya reference copy number suffix
    std::regex copyNum(R"(\{\d+\}$)");
    normalized = std::regex_replace(normalized, copyNum, "");
    normalized = expandEnvVars(normalized);
    for (auto& c : normalized) {
        if (c == '\\') c = '/';
    }

    // Check if absolute
    bool isAbs = false;
    if (!normalized.empty() && (normalized[0] == '/' || normalized[0] == '\\')) isAbs = true;
    if (normalized.size() >= 2 && std::isalpha((unsigned char)normalized[0]) && normalized[1] == ':') isAbs = true;

    if (isAbs) {
        return normalized;
    }

    std::string sceneDir = getSceneDir();
    if (!sceneDir.empty()) {
        return sceneDir + "/" + normalized;
    }
    return normalized;
}

bool pathExists(const std::string& rawPath) {
    if (rawPath.empty()) return false;
    std::string resolved = resolveSceneRelative(rawPath);
    return fileExistsOnDisk(resolved);
}

std::vector<CameraInfo> findNonDefaultCameras() {
    std::vector<CameraInfo> result;

    static const std::set<std::string> defaultCams = {
        "persp", "top", "front", "side", "back", "bottom", "left", "right"
    };
    static const std::set<std::string> defaultShapes = {
        "perspShape", "topShape", "frontShape", "sideShape",
        "backShape", "bottomShape", "leftShape", "rightShape"
    };

    // Get all camera shapes
    std::vector<std::string> camShapes = melQueryStringArray("ls -type \"camera\" -long");

    for (const auto& camShape : camShapes) {
        // Get parent transform
        std::string cmd = "listRelatives -parent -fullPath \"" + camShape + "\"";
        std::vector<std::string> parents = melQueryStringArray(cmd);
        if (parents.empty()) continue;

        std::string transform = parents[0];
        std::string sn = shortName(transform);
        std::string bn = bareName(sn);

        if (defaultCams.count(bn)) continue;

        std::string shapeSn = shortName(camShape);
        std::string shapeBn = bareName(shapeSn);
        if (defaultShapes.count(shapeBn)) continue;

        // Check startup camera
        std::string startupCmd = "camera -q -startupCamera \"" + camShape + "\"";
        int isStartup = 0;
        MGlobal::executeCommand(utf8ToMString(startupCmd), isStartup);
        if (isStartup) continue;

        CameraInfo info;
        info.transform = transform;
        info.display = sn;
        result.push_back(info);
    }

    return result;
}

std::vector<CharacterInfo> findCharacters() {
    std::vector<CharacterInfo> result;

    // Get all joints
    std::vector<std::string> allJoints = melQueryStringArray("ls -type \"joint\" -long");

    // Find root joints (no joint parent)
    std::vector<std::string> rootJoints;
    for (const auto& joint : allJoints) {
        std::string cmd = "listRelatives -parent -type \"joint\" \"" + joint + "\"";
        std::vector<std::string> parents = melQueryStringArray(cmd);
        if (parents.empty()) {
            rootJoints.push_back(joint);
        }
    }

    // Group roots by namespace
    std::map<std::string, std::vector<std::string>> nsMap;
    for (const auto& root : rootJoints) {
        std::string sn = shortName(root);
        std::string ns = getNamespace(sn);
        nsMap[ns].push_back(root);
    }

    for (auto& kv : nsMap) {
        const std::string& ns = kv.first;
        std::vector<std::string>& roots = kv.second;

        // Pick the best root joint for export.
        // Prefer a root whose bare name is "root" (case-insensitive) — this is the
        // standard UE export skeleton in rigs that have multiple skeleton hierarchies
        // (e.g. DeformationSystem/Root_M, FitSkeleton/Root1, and the export skeleton "root").
        // Fall back to the root with the most descendants if none is named "root".
        std::string bestRoot = roots[0];
        int bestCount = 0;
        bool bestIsRootNamed = false;

        for (const auto& r : roots) {
            std::string sn_r = shortName(r);
            std::string bn = bareName(sn_r);
            std::string lower = bn;
            for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            bool isRootNamed = (lower == "root");

            std::string cmd = "listRelatives -allDescendents -type \"joint\" \"" + r + "\"";
            std::vector<std::string> desc = melQueryStringArray(cmd);
            int count = (int)desc.size();

            // Prefer "root"-named joint; among same priority, prefer most descendants
            if ((isRootNamed && !bestIsRootNamed) ||
                (isRootNamed == bestIsRootNamed && count > bestCount)) {
                bestCount = count;
                bestRoot = r;
                bestIsRootNamed = isRootNamed;
            }
        }

        std::string sn = shortName(bestRoot);
        CharacterInfo info;
        info.rootJoint = bestRoot;

        if (!ns.empty()) {
            info.nsOrName = ns;
            info.display = sn;
        } else {
            info.nsOrName = bareName(sn);
            info.display = sn;
        }

        result.push_back(info);
    }

    return result;
}

std::vector<BlendShapeGroupInfo> findBlendShapeGroups() {
    std::vector<BlendShapeGroupInfo> result;

    std::vector<std::string> bsNodes = melQueryStringArray("ls -type \"blendShape\"");

    struct NsEntry {
        std::string mesh;
        int weightCount;
    };
    std::map<std::string, NsEntry> nsMap; // namespace -> best mesh (highest weightCount)

    for (const auto& bsNode : bsNodes) {
        // Get geometry connected to this blendShape
        std::string cmd = "blendShape -q -geometry \"" + bsNode + "\"";
        std::vector<std::string> geometries = melQueryStringArray(cmd);

        int weightCount = 0;
        {
            std::string wcCmd = "blendShape -q -weightCount \"" + bsNode + "\"";
            MGlobal::executeCommand(utf8ToMString(wcCmd), weightCount);
        }

        for (const auto& geo : geometries) {
            std::string parentCmd = "listRelatives -parent -fullPath \"" + geo + "\"";
            std::vector<std::string> parents = melQueryStringArray(parentCmd);
            if (parents.empty()) continue;

            std::string transform = parents[0];
            std::string sn = shortName(transform);
            std::string ns = getNamespace(sn);

            auto it = nsMap.find(ns);
            if (it == nsMap.end() || weightCount > it->second.weightCount) {
                nsMap[ns] = {transform, weightCount};
            }
        }
    }

    for (auto& kv : nsMap) {
        const std::string& ns = kv.first;
        const std::string& mesh = kv.second.mesh;

        std::string sn = shortName(mesh);
        BlendShapeGroupInfo info;
        info.mesh = mesh;

        if (!ns.empty()) {
            info.nsOrName = ns;
            info.display = ns + ":* (BlendShape)";
        } else {
            info.nsOrName = bareName(sn);
            info.display = sn + " (BlendShape)";
        }

        result.push_back(info);
    }

    return result;
}

std::vector<SkeletonBlendShapeInfo> findSkeletonBlendShapeCombos() {
    std::vector<SkeletonBlendShapeInfo> result;

    std::vector<CharacterInfo> characters = findCharacters();

    PluginLog::info("SceneScanner",
        "findSkeletonBlendShapeCombos: scanning " + std::to_string(characters.size()) + " characters for BS deformers");

    for (const auto& ch : characters) {
        // Collect all joints under this character
        std::string listCmd = "listRelatives -allDescendents -type \"joint\" -fullPath \"" + ch.rootJoint + "\"";
        std::vector<std::string> allJoints = melQueryStringArray(listCmd);
        allJoints.push_back(ch.rootJoint);

        // Find skinClusters connected to these joints
        std::set<std::string> skinClusters;
        for (const auto& j : allJoints) {
            std::vector<std::string> clusters = melQueryStringArray(
                "listConnections -source true -destination false -type \"skinCluster\" \"" + j + "\"");
            if (clusters.empty()) {
                clusters = melQueryStringArray(
                    "listConnections -source true -destination true -type \"skinCluster\" \"" + j + "\"");
            }
            skinClusters.insert(clusters.begin(), clusters.end());
        }

        // For each skinCluster, get the output mesh transforms
        std::set<std::string> skinnedMeshTransforms;
        for (const auto& skin : skinClusters) {
            std::vector<std::string> geos = melQueryStringArray("skinCluster -q -g \"" + skin + "\"");
            for (const auto& g : geos) {
                std::vector<std::string> full = melQueryStringArray("ls -long \"" + g + "\"");
                std::string geo = full.empty() ? g : full[0];
                std::string geoType = melQueryString("nodeType \"" + geo + "\"");

                if (geoType == "mesh") {
                    std::vector<std::string> parent = melQueryStringArray(
                        "listRelatives -parent -fullPath \"" + geo + "\"");
                    if (!parent.empty()) {
                        skinnedMeshTransforms.insert(parent[0]);
                    }
                } else if (geoType == "transform") {
                    skinnedMeshTransforms.insert(geo);
                }
            }
        }

        {
            std::ostringstream dbg;
            dbg << "  character '" << ch.display << "': "
                << skinClusters.size() << " skinClusters, "
                << skinnedMeshTransforms.size() << " skinned meshes";
            PluginLog::info("SceneScanner", dbg.str());
        }

        // For each skinned mesh, check if it also has a blendShape deformer
        std::vector<std::string> bsMeshes;
        std::vector<std::string> bsNodes;
        std::vector<std::string> bsWeightAttrs;
        std::set<std::string> seenBsNodes;

        // Lambda: scan a mesh transform for blendShape deformers and collect attrs
        auto scanMeshForBS = [&](const std::string& meshXform) {
            std::string histCmd = "listHistory -pruneDagObjects true \"" + meshXform + "\"";
            std::vector<std::string> history = melQueryStringArray(histCmd);

            bool meshHasBS = false;
            int meshBsCount = 0;
            int meshWeightCount = 0;

            for (const auto& histNode : history) {
                MString nodeType;
                MGlobal::executeCommand(utf8ToMString("nodeType \"" + histNode + "\""), nodeType);
                if (toUtf8(nodeType) != "blendShape") continue;

                meshHasBS = true;
                ++meshBsCount;

                if (seenBsNodes.count(histNode) == 0) {
                    seenBsNodes.insert(histNode);
                    bsNodes.push_back(histNode);
                }

                int wc = 0;
                MGlobal::executeCommand(
                    utf8ToMString("blendShape -q -weightCount \"" + histNode + "\""), wc);

                for (int i = 0; i < wc; ++i) {
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
                    bsWeightAttrs.push_back(attrName);
                    ++meshWeightCount;
                }
            }

            if (meshHasBS) {
                bsMeshes.push_back(meshXform);
                std::ostringstream dbg;
                dbg << "  mesh '" << meshXform << "': "
                    << meshBsCount << " blendShape deformers found, "
                    << meshWeightCount << " weight attrs collected";
                PluginLog::info("SceneScanner", dbg.str());
            }
        };

        // Scan skinned meshes for blendShape
        for (const auto& meshXform : skinnedMeshTransforms) {
            scanMeshForBS(meshXform);
        }

        // Also scan non-skinned meshes under the character hierarchy for blendShape.
        // These may be constrained or parented to the skeleton without a skinCluster.
        {
            std::set<std::string> alreadyScanned(skinnedMeshTransforms.begin(), skinnedMeshTransforms.end());
            // Find all mesh shapes under the character root's top-level parent
            std::string rootTop = ch.rootJoint;
            size_t pipePos = rootTop.find('|', 1);
            if (pipePos != std::string::npos) rootTop = rootTop.substr(0, pipePos);
            if (rootTop.empty()) rootTop = ch.rootJoint;

            std::vector<std::string> allMeshShapes = melQueryStringArray(
                "listRelatives -allDescendents -type \"mesh\" -fullPath \"" + rootTop + "\"");
            std::set<std::string> nonSkinnedTransforms;
            for (const auto& shape : allMeshShapes) {
                std::vector<std::string> parent = melQueryStringArray(
                    "listRelatives -parent -fullPath \"" + shape + "\"");
                if (!parent.empty() && alreadyScanned.count(parent[0]) == 0) {
                    nonSkinnedTransforms.insert(parent[0]);
                }
            }
            if (!nonSkinnedTransforms.empty()) {
                for (const auto& meshXform : nonSkinnedTransforms) {
                    scanMeshForBS(meshXform);
                }
            }
        }

        // Only create a combo entry if at least one skinned mesh has BS
        if (!bsMeshes.empty()) {
            SkeletonBlendShapeInfo info;
            info.rootJoint     = ch.rootJoint;
            info.nsOrName      = ch.nsOrName;
            info.display       = ch.display + " (Skel+BS)";
            info.bsMeshes      = bsMeshes;
            info.bsNodes       = bsNodes;
            info.bsWeightAttrs = bsWeightAttrs;
            result.push_back(info);
        }
    }

    PluginLog::info("SceneScanner",
        "findSkeletonBlendShapeCombos: found " + std::to_string(result.size()) + " skeleton+BS combos total");

    return result;
}

std::vector<DependencyInfo> scanReferences() {
    std::vector<DependencyInfo> deps;

    std::vector<std::string> refs = melQueryStringArray("file -q -reference");

    for (const auto& refPath : refs) {
        std::string refNode = "unknown";
        std::string unresolved = refPath;

        // Try to get reference node and unresolved name
        {
            std::string cmd = "referenceQuery -referenceNode \"" + refPath + "\"";
            MString result;
            MStatus status = MGlobal::executeCommand(utf8ToMString(cmd), result);
            if (status == MS::kSuccess) {
                refNode = toUtf8(result);
            }
        }
        {
            std::string cmd = "referenceQuery -filename -unresolvedName \"" + refPath + "\"";
            MString result;
            MStatus status = MGlobal::executeCommand(utf8ToMString(cmd), result);
            if (status == MS::kSuccess) {
                unresolved = toUtf8(result);
            }
        }

        // Clean copy number suffix
        std::regex copyNum(R"(\{\d+\}$)");
        std::string cleanPath = std::regex_replace(refPath, copyNum, "");
        std::string cleanUnresolved = std::regex_replace(unresolved, copyNum, "");

        bool exists = pathExists(cleanPath);

        bool isLoaded = false;
        {
            int loaded = 0;
            std::string cmd = "referenceQuery -isLoaded \"" + refPath + "\"";
            MGlobal::executeCommand(utf8ToMString(cmd), loaded);
            isLoaded = (loaded != 0);
        }

        DependencyInfo dep;
        dep.type = "reference";
        dep.typeLabel = "Reference";
        dep.node = refNode;
        dep.path = cleanPath;
        dep.unresolvedPath = cleanUnresolved;
        dep.exists = exists;
        dep.isLoaded = isLoaded;
        dep.selected = false;
        dep.matchedPath = "";
        deps.push_back(dep);
    }

    return deps;
}

std::vector<DependencyInfo> scanTextures() {
    std::vector<DependencyInfo> deps;

    // File texture nodes
    std::vector<std::string> fileNodes = melQueryStringArray("ls -type \"file\"");
    for (const auto& node : fileNodes) {
        std::string cmd = "getAttr \"" + node + ".fileTextureName\"";
        MString result;
        MStatus status = MGlobal::executeCommand(utf8ToMString(cmd), result);
        if (status != MS::kSuccess) continue;

        std::string path = toUtf8(result);
        if (path.empty()) continue;

        bool exists = pathExists(path);

        DependencyInfo dep;
        dep.type = "texture";
        dep.typeLabel = "Texture";
        dep.node = node;
        dep.path = path;
        dep.unresolvedPath = path;
        dep.exists = exists;
        dep.isLoaded = true;
        dep.selected = false;
        dep.matchedPath = "";
        deps.push_back(dep);
    }

    // Arnold aiImage nodes
    std::vector<std::string> aiNodes = melQueryStringArray("ls -type \"aiImage\"");
    for (const auto& node : aiNodes) {
        std::string cmd = "getAttr \"" + node + ".filename\"";
        MString result;
        MStatus status = MGlobal::executeCommand(utf8ToMString(cmd), result);
        if (status != MS::kSuccess) continue;

        std::string path = toUtf8(result);
        if (path.empty()) continue;

        bool exists = pathExists(path);

        DependencyInfo dep;
        dep.type = "texture";
        dep.typeLabel = "Texture";
        dep.node = node;
        dep.path = path;
        dep.unresolvedPath = path;
        dep.exists = exists;
        dep.isLoaded = true;
        dep.selected = false;
        dep.matchedPath = "";
        deps.push_back(dep);
    }

    return deps;
}

std::vector<DependencyInfo> scanCaches() {
    std::vector<DependencyInfo> deps;

    // AlembicNode
    std::vector<std::string> alembicNodes = melQueryStringArray("ls -type \"AlembicNode\"");
    for (const auto& node : alembicNodes) {
        std::string cmd = "getAttr \"" + node + ".abc_File\"";
        MString result;
        MStatus status = MGlobal::executeCommand(utf8ToMString(cmd), result);
        if (status != MS::kSuccess) continue;

        std::string path = toUtf8(result);
        if (path.empty()) continue;

        bool exists = pathExists(path);

        DependencyInfo dep;
        dep.type = "cache";
        dep.typeLabel = "Cache";
        dep.node = node;
        dep.path = path;
        dep.unresolvedPath = path;
        dep.exists = exists;
        dep.isLoaded = true;
        dep.selected = false;
        dep.matchedPath = "";
        deps.push_back(dep);
    }

    // gpuCache
    std::vector<std::string> gpuNodes = melQueryStringArray("ls -type \"gpuCache\"");
    for (const auto& node : gpuNodes) {
        std::string cmd = "getAttr \"" + node + ".cacheFileName\"";
        MString result;
        MStatus status = MGlobal::executeCommand(utf8ToMString(cmd), result);
        if (status != MS::kSuccess) continue;

        std::string path = toUtf8(result);
        if (path.empty()) continue;

        bool exists = pathExists(path);

        DependencyInfo dep;
        dep.type = "cache";
        dep.typeLabel = "Cache";
        dep.node = node;
        dep.path = path;
        dep.unresolvedPath = path;
        dep.exists = exists;
        dep.isLoaded = true;
        dep.selected = false;
        dep.matchedPath = "";
        deps.push_back(dep);
    }

    return deps;
}

std::vector<DependencyInfo> scanAudio() {
    std::vector<DependencyInfo> deps;

    std::vector<std::string> audioNodes = melQueryStringArray("ls -type \"audio\"");
    for (const auto& node : audioNodes) {
        std::string cmd = "getAttr \"" + node + ".filename\"";
        MString result;
        MStatus status = MGlobal::executeCommand(utf8ToMString(cmd), result);
        if (status != MS::kSuccess) continue;

        std::string path = toUtf8(result);
        if (path.empty()) continue;

        bool exists = pathExists(path);

        DependencyInfo dep;
        dep.type = "audio";
        dep.typeLabel = "Audio";
        dep.node = node;
        dep.path = path;
        dep.unresolvedPath = path;
        dep.exists = exists;
        dep.isLoaded = true;
        dep.selected = false;
        dep.matchedPath = "";
        deps.push_back(dep);
    }

    return deps;
}

} // namespace SceneScanner
