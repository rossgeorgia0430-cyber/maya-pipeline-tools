#include "NamingUtils.h"
#include "PluginLog.h"

#include <maya/MGlobal.h>
#include <maya/MString.h>
#include <maya/MStringArray.h>
#include <maya/MStatus.h>

#include <regex>
#include <algorithm>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

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

// Helper: split string by delimiter
static std::vector<std::string> splitString(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, delim)) {
        parts.push_back(token);
    }
    return parts;
}

// Helper: get basename without extension from a path
static std::string getBasenameNoExt(const std::string& path) {
    std::string p = path;
    // Normalize separators
    for (auto& c : p) {
        if (c == '\\') c = '/';
    }
    // Get filename
    size_t pos = p.rfind('/');
    std::string fname = (pos != std::string::npos) ? p.substr(pos + 1) : p;
    // Remove extension
    size_t dotPos = fname.rfind('.');
    if (dotPos != std::string::npos) {
        fname = fname.substr(0, dotPos);
    }
    return fname;
}

// Helper: strip last component after | or :
static std::string shortName(const std::string& fullPath) {
    size_t pos = fullPath.rfind('|');
    std::string s = (pos != std::string::npos) ? fullPath.substr(pos + 1) : fullPath;
    pos = s.rfind(':');
    if (pos != std::string::npos) {
        s = s.substr(pos + 1);
    }
    return s;
}

namespace NamingUtils {

static std::string trimTokenDelimiters(const std::string& s) {
    if (s.empty()) return s;
    size_t start = 0;
    while (start < s.size() && (s[start] == '_' || s[start] == ':' || s[start] == '|')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == '_' || s[end - 1] == ':' || s[end - 1] == '|')) {
        --end;
    }
    return s.substr(start, end - start);
}

static void fillTokensFromText(const std::string& source, SceneTokens& out) {
    if (source.empty()) return;

    std::regex sceneRe("(Scene[A-Za-z0-9]+)", std::regex::icase);
    std::regex shotRe("(Shot[A-Za-z0-9]+)", std::regex::icase);
    std::smatch sceneMatch;
    std::smatch shotMatch;

    if (out.scene.empty() && std::regex_search(source, sceneMatch, sceneRe)) {
        out.scene = sceneMatch[1].str();
    }
    if (out.shot.empty() && std::regex_search(source, shotMatch, shotRe)) {
        out.shot = shotMatch[1].str();
    }

    if (out.project.empty()) {
        size_t firstPos = source.size();
        if (sceneMatch.size() > 0 && sceneMatch.position(0) >= 0 &&
            static_cast<size_t>(sceneMatch.position(0)) < firstPos) {
            firstPos = static_cast<size_t>(sceneMatch.position(0));
        }
        if (shotMatch.size() > 0 && shotMatch.position(0) >= 0 &&
            static_cast<size_t>(shotMatch.position(0)) < firstPos) {
            firstPos = static_cast<size_t>(shotMatch.position(0));
        }

        if (firstPos < source.size()) {
            out.project = trimTokenDelimiters(source.substr(0, firstPos));
        }
    }
}

SceneTokens parseSceneTokens() {
    SceneTokens result;
    result.project = "";
    result.scene = "";
    result.shot = "";
    result.basename = "";

    MString scenePath;
    MGlobal::executeCommand("file -q -sceneName", scenePath);
    std::string scenePathStr = toUtf8(scenePath);

    result.basename = getBasenameNoExt(scenePathStr);
    fillTokensFromText(result.basename, result);

    {
        std::ostringstream dbg;
        dbg << "parseSceneTokens: scenePath='" << scenePathStr
            << "', basename='" << result.basename << "'"
            << ", fromSceneName{project='" << result.project
            << "', scene='" << result.scene
            << "', shot='" << result.shot << "'}";
        PluginLog::info("NamingUtils", dbg.str());
    }

    // Fallback for imported/unsaved scenes:
    // when sceneName is empty, recover tokens from namespace names.
    if (result.scene.empty() || result.shot.empty() || result.project.empty()) {
        MStringArray namespaces;

        auto queryNamespaces = [&](const char* cmd) -> bool {
            namespaces.setLength(0);
            MStatus st = MGlobal::executeCommand(MString(cmd), namespaces);
            return st == MS::kSuccess;
        };

        // Query from root namespace for deterministic results.
        MString currentNs;
        MGlobal::executeCommand("namespaceInfo -cur", currentNs);
        MGlobal::executeCommand("namespace -set \":\";");

        bool ok = queryNamespaces("namespaceInfo -listOnlyNamespaces -recurse true");
        if (!ok || namespaces.length() == 0) ok = queryNamespaces("namespaceInfo -listOnlyNamespaces");
        if (!ok || namespaces.length() == 0) ok = queryNamespaces("namespaceInfo -lon -r");

        if (currentNs.length() > 0) {
            std::string restore = "namespace -set \"" + toUtf8(currentNs) + "\";";
            MGlobal::executeCommand(utf8ToMString(restore));
        }

        {
            std::ostringstream dbg;
            dbg << "parseSceneTokens: namespace query ok=" << (ok ? "true" : "false")
                << ", count=" << namespaces.length();
            if (namespaces.length() > 0) {
                dbg << ", sample='" << toUtf8(namespaces[0]) << "'";
            }
            PluginLog::info("NamingUtils", dbg.str());
        }

        for (unsigned int i = 0; i < namespaces.length(); ++i) {
            std::string ns = toUtf8(namespaces[i]);
            if (ns.empty()) continue;
            if (ns == "UI" || ns == "shared") continue;

            SceneTokens candidate = result;
            fillTokensFromText(ns, candidate);

            bool matched = !candidate.scene.empty() && !candidate.shot.empty();
            if (!matched) continue;

            if (result.project.empty()) result.project = candidate.project;
            if (result.scene.empty()) result.scene = candidate.scene;
            if (result.shot.empty()) result.shot = candidate.shot;

            std::ostringstream dbg;
            dbg << "parseSceneTokens: matched namespace='" << ns
                << "' -> project='" << result.project
                << "', scene='" << result.scene
                << "', shot='" << result.shot << "'";
            PluginLog::info("NamingUtils", dbg.str());
            break;
        }
    }

    return result;
}

std::string cleanCharacterName(const std::string& rawName) {
    std::string name = rawName;

    // Strip any namespace prefix (everything up to and including the last colon)
    size_t colonPos = name.rfind(':');
    if (colonPos != std::string::npos) {
        name = name.substr(colonPos + 1);
    }

    // Strip SK_ prefix (case-insensitive)
    std::regex skPrefix("^SK_", std::regex::icase);
    name = std::regex_replace(name, skPrefix, "");

    // Strip _Skin_Rig / _PV_Rig / _Rig (with optional trailing digits)
    std::regex rigSuffix("(_Skin_Rig|_PV_Rig|_Rig)\\d*$", std::regex::icase);
    name = std::regex_replace(name, rigSuffix, "");

    // Strip trailing _digits (copy-number like _2, _3)
    std::regex trailingDigits("_\\d+$");
    name = std::regex_replace(name, trailingDigits, "");

    return name.empty() ? rawName : name;
}

std::string extractRigNumber(const std::string& rawName) {
    std::regex re("(?:_Skin_Rig|_PV_Rig|_Rig)(\\d+)$", std::regex::icase);
    std::smatch match;
    if (std::regex_search(rawName, match, re)) {
        return match[1].str();
    }
    return "";
}

std::string buildCameraFilename(const std::string& cameraTransform,
                                const SceneTokens& tokens) {
    std::vector<std::string> parts;
    parts.push_back("Cam");

    if (!tokens.project.empty()) parts.push_back(tokens.project);
    if (!tokens.scene.empty()) parts.push_back(tokens.scene);
    if (!tokens.shot.empty()) parts.push_back(tokens.shot);

    if (parts.size() == 1) {
        // Fallback: use camera name
        std::string sn = shortName(cameraTransform);
        parts.push_back(sn);
    }

    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "_";
        result += parts[i];
    }
    result += ".fbx";
    return result;
}

std::string buildSkeletonFilename(const std::string& nsOrName,
                                  const SceneTokens& tokens,
                                  const std::string& rigSuffix) {
    std::string charName = cleanCharacterName(nsOrName);

    std::vector<std::string> parts;
    parts.push_back("A");
    if (!tokens.project.empty()) parts.push_back(tokens.project);
    parts.push_back(charName);
    if (!rigSuffix.empty()) parts.push_back(rigSuffix);
    if (!tokens.scene.empty()) parts.push_back(tokens.scene);
    if (!tokens.shot.empty()) parts.push_back(tokens.shot);

    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "_";
        result += parts[i];
    }
    result += ".fbx";
    return result;
}

std::string buildBlendShapeFilename(const std::string& nsOrName,
                                    const SceneTokens& tokens,
                                    const std::string& rigSuffix) {
    std::string charName = cleanCharacterName(nsOrName);

    std::vector<std::string> parts;
    parts.push_back("A");
    if (!tokens.project.empty()) parts.push_back(tokens.project);
    parts.push_back(charName);
    if (!rigSuffix.empty()) parts.push_back(rigSuffix);
    if (!tokens.scene.empty()) parts.push_back(tokens.scene);
    if (!tokens.shot.empty()) parts.push_back(tokens.shot);
    parts.push_back("Face");

    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "_";
        result += parts[i];
    }
    result += ".fbx";
    return result;
}

std::string buildSkeletonBlendShapeFilename(const std::string& nsOrName,
                                              const SceneTokens& tokens,
                                              const std::string& rigSuffix) {
    std::string charName = cleanCharacterName(nsOrName);

    std::vector<std::string> parts;
    parts.push_back("A");
    if (!tokens.project.empty()) parts.push_back(tokens.project);
    parts.push_back(charName);
    if (!rigSuffix.empty()) parts.push_back(rigSuffix);
    if (!tokens.scene.empty()) parts.push_back(tokens.scene);
    if (!tokens.shot.empty()) parts.push_back(tokens.shot);
    // NOTE: For type="skeleton+blendshape", we intentionally do NOT append a "_SkelBS"
    // suffix. The combined export is treated as the character's main animation FBX.

    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "_";
        result += parts[i];
    }
    result += ".fbx";
    return result;
}

void deduplicateFilenames(std::vector<ExportItem>& items,
                          const SceneTokens& tokens) {
    // Group by (type_category, cleaned_name) to find duplicates
    // type_category: "skel" or "bs"
    struct GroupKey {
        std::string category;
        std::string cleanedName;
        bool operator<(const GroupKey& o) const {
            if (category != o.category) return category < o.category;
            return cleanedName < o.cleanedName;
        }
    };

    std::map<GroupKey, std::vector<size_t>> groups;

    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].type == "skeleton") {
            GroupKey key;
            key.category = "skel";
            key.cleanedName = cleanCharacterName(items[i].nsOrName);
            groups[key].push_back(i);
        } else if (items[i].type == "blendshape") {
            GroupKey key;
            key.category = "bs";
            key.cleanedName = cleanCharacterName(items[i].nsOrName);
            groups[key].push_back(i);
        } else if (items[i].type == "skeleton+blendshape") {
            GroupKey key;
            key.category = "skelbs";
            key.cleanedName = cleanCharacterName(items[i].nsOrName);
            groups[key].push_back(i);
        }
    }

    for (auto& kv : groups) {
        if (kv.second.size() < 2) continue;

        const std::string& cat = kv.first.category;
        for (size_t idx : kv.second) {
            std::string rigNum = extractRigNumber(items[idx].nsOrName);
            if (cat == "skel") {
                items[idx].filename = buildSkeletonFilename(
                    items[idx].nsOrName, tokens, rigNum);
            } else if (cat == "skelbs") {
                items[idx].filename = buildSkeletonBlendShapeFilename(
                    items[idx].nsOrName, tokens, rigNum);
            } else {
                items[idx].filename = buildBlendShapeFilename(
                    items[idx].nsOrName, tokens, rigNum);
            }
        }
    }

    // Final safety: if any filenames still collide, append _2, _3, etc.
    std::map<std::string, int> seen;
    for (auto& item : items) {
        std::string fn = item.filename;
        if (seen.find(fn) != seen.end()) {
            seen[fn]++;
            size_t dotPos = fn.rfind('.');
            if (dotPos != std::string::npos) {
                item.filename = fn.substr(0, dotPos) + "_" +
                    std::to_string(seen[fn]) + fn.substr(dotPos);
            } else {
                item.filename = fn + "_" + std::to_string(seen[fn]);
            }
        } else {
            seen[fn] = 1;
        }
    }
}

} // namespace NamingUtils




