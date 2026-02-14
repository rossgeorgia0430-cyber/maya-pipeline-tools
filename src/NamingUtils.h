#pragma once
#ifndef NAMINGUTILS_H
#define NAMINGUTILS_H

#include <string>
#include <vector>
#include <map>

// Forward declaration for export items used by deduplication
struct ExportItem {
    std::string type;       // "camera", "skeleton", "blendshape"
    std::string node;       // Maya node full path
    std::string name;       // display name
    std::string nsOrName;   // namespace or bare name
    std::string filename;   // output filename
    bool selected;
    std::string status;     // "pending", "exporting", "done", "error"
    std::string message;

    // BlendShape fields (attached to skeleton items when BS detected on skinned meshes)
    std::vector<std::string> bsMeshes;      // BS mesh transform list
    std::vector<std::string> bsNodes;       // blendShape deformer node list
    std::vector<std::string> bsWeightAttrs; // BS weight attributes (for bake)
};

struct SceneTokens {
    std::string project;
    std::string scene;
    std::string shot;
    std::string basename;
};

namespace NamingUtils {

    // Parse scene tokens from the current Maya scene filename.
    // Expected pattern: <Project>_<SceneXX>_<ShotNN>[_extra].ma
    SceneTokens parseSceneTokens();

    // Clean character name: strip SK_ prefix, _Skin_Rig/_PV_Rig/_Rig suffixes
    std::string cleanCharacterName(const std::string& rawName);

    // Extract rig copy-number from namespace/node name
    std::string extractRigNumber(const std::string& rawName);

    // Build camera FBX filename
    std::string buildCameraFilename(const std::string& cameraTransform,
                                    const SceneTokens& tokens);

    // Build skeleton FBX filename
    std::string buildSkeletonFilename(const std::string& nsOrName,
                                      const SceneTokens& tokens,
                                      const std::string& rigSuffix = "");

    // Build blendshape FBX filename
    std::string buildBlendShapeFilename(const std::string& nsOrName,
                                        const SceneTokens& tokens,
                                        const std::string& rigSuffix = "");

    // Build skeleton+blendshape combined FBX filename
    std::string buildSkeletonBlendShapeFilename(const std::string& nsOrName,
                                                 const SceneTokens& tokens,
                                                 const std::string& rigSuffix = "");

    // Deduplicate filenames by appending rig numbers where needed
    void deduplicateFilenames(std::vector<ExportItem>& items,
                              const SceneTokens& tokens);

} // namespace NamingUtils

#endif // NAMINGUTILS_H
