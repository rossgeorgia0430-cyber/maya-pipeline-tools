#pragma once
#ifndef SCENESCANNER_H
#define SCENESCANNER_H

#include <string>
#include <vector>
#include <map>

struct CameraInfo {
    std::string transform; // full DAG path
    std::string display;   // short display name
};

struct CharacterInfo {
    std::string rootJoint;  // full DAG path of main root joint
    std::string nsOrName;   // namespace or bare joint name
    std::string display;    // human-readable label
};

struct BlendShapeGroupInfo {
    std::string mesh;       // first mesh transform with blendShape
    std::string nsOrName;   // namespace or bare mesh name
    std::string display;    // human-readable label
};

struct SkeletonBlendShapeInfo {
    std::string rootJoint;              // skeleton root joint full DAG path
    std::string nsOrName;               // namespace or bare name
    std::string display;                // human-readable label
    std::vector<std::string> bsMeshes;  // skinned meshes that also have blendShape
    std::vector<std::string> bsNodes;   // blendShape deformer node names
    std::vector<std::string> bsWeightAttrs; // all BS weight attributes (for bake)
};

struct DependencyInfo {
    std::string type;           // "reference", "texture", "cache", "audio"
    std::string typeLabel;      // display label
    std::string node;           // Maya node name
    std::string path;           // resolved path
    std::string unresolvedPath; // unresolved/original path
    bool exists;
    bool isLoaded;          // references: queried from Maya; textures/caches/audio: always true
    bool selected;
    std::string matchedPath;    // auto-matched replacement path
};

namespace SceneScanner {

    // Find all non-default cameras in the scene
    std::vector<CameraInfo> findNonDefaultCameras();

    // Find character skeletons grouped by namespace
    std::vector<CharacterInfo> findCharacters();

    // Find blendshape meshes grouped by namespace
    std::vector<BlendShapeGroupInfo> findBlendShapeGroups();

    // Find characters whose skinned meshes also have blendShape deformers
    std::vector<SkeletonBlendShapeInfo> findSkeletonBlendShapeCombos();

    // Scan scene dependencies
    std::vector<DependencyInfo> scanReferences();
    std::vector<DependencyInfo> scanTextures();
    std::vector<DependencyInfo> scanCaches();
    std::vector<DependencyInfo> scanAudio();

    // Utility: get scene directory
    std::string getSceneDir();

    // Utility: resolve path relative to scene
    std::string resolveSceneRelative(const std::string& rawPath);

    // Utility: check if path exists (handles scene-relative)
    bool pathExists(const std::string& rawPath);

} // namespace SceneScanner

#endif // SCENESCANNER_H
