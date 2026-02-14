#pragma once
#ifndef ANIMEXPORTER_H
#define ANIMEXPORTER_H

#include <string>
#include <vector>
#include <map>
#include <set>

// Forward declaration — full definition in NamingUtils.h
struct ExportItem;

struct ExportResult {
    bool success;
    std::string filePath;
    int64_t fileSize;
    double duration;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

// FBX export options configurable from the UI
struct FbxExportOptions {
    // Skeleton options
    bool skelAnimationOnly  = false;
    bool skelBakeComplex    = true;
    bool skelSkeletonDefs   = true;
    bool skelConstraints    = false;
    bool skelInputConns     = false;
    bool skelBlendShape     = true;    // export BS curves with skeleton if detected

    // BlendShape options
    bool bsShapes           = true;
    bool bsSmoothMesh       = false;
    bool bsIncludeSkeleton  = true;   // export skin joints with BS mesh

    // Common options
    std::string fileVersion = "FBX202000";  // "FBX202000" or "FBX201800"
    std::string upAxis      = "y";          // "y" or "z"
};

struct FrameRangeInfo {
    std::string name;       // display name
    std::string type;       // "camera" / "skeleton" / "blendshape"
    std::string filename;   // output .fbx filename
    double firstKey;        // actual first keyframe
    double lastKey;         // actual last keyframe
    bool valid;             // whether query succeeded
};

namespace AnimExporter {

    // Ensure the fbxmaya plugin is loaded
    bool ensureFbxPlugin();

    // Set FBX export defaults
    void setFbxExportDefaults();

    // Set FBX bake frame range
    void setFbxBakeRange(int start, int end);

    // Single-pass batch bake: collects ALL transform nodes and BS attrs
    // from selectedItems, then calls bakeResults once for transforms and
    // once for BS attrs.  Returns set of failed item indices.
    std::set<int> batchBakeAll(const std::vector<ExportItem>& selectedItems,
                               int startFrame, int endFrame);

    // Export camera FBX (no baking, assumes already baked)
    ExportResult exportCameraFbx(const std::string& cameraTransform,
                                 const std::string& outputPath,
                                 int startFrame, int endFrame,
                                 const FbxExportOptions& opts = FbxExportOptions());

    // Export skeleton FBX (no baking, assumes already baked)
    ExportResult exportSkeletonFbx(const std::string& skeletonRoot,
                                   const std::string& outputPath,
                                   int startFrame, int endFrame,
                                   const FbxExportOptions& opts = FbxExportOptions());

    // Export blendshape FBX (no baking, assumes already baked)
    ExportResult exportBlendShapeFbx(const std::string& meshNode,
                                     const std::string& outputPath,
                                     int startFrame, int endFrame,
                                     const FbxExportOptions& opts = FbxExportOptions());

    // Export skeleton+blendshape combined FBX (no baking, assumes already baked)
    ExportResult exportSkeletonBlendShapeFbx(
        const std::string& skeletonRoot,
        const std::vector<std::string>& bsMeshes,
        const std::vector<std::string>& bsWeightAttrs,
        const std::string& outputPath,
        int startFrame, int endFrame,
        const FbxExportOptions& opts = FbxExportOptions());

    // Query Maya current scene frame rate (returns fps value, e.g. 24.0, 30.0)
    double querySceneFps();

    // Set Maya scene time unit to the specified fps (maps common fps to Maya time unit)
    // Returns the previous time unit string, for restoring after export
    std::string setSceneTimeUnit(double fps);

    // Restore Maya scene time unit
    void restoreSceneTimeUnit(const std::string& previousUnit);

    // Query actual keyframe range for an export item (after baking)
    FrameRangeInfo queryFrameRange(const ExportItem& item);
    // Write a frame-range log file; returns the output file path
    std::string writeFrameRangeLog(const std::string& outputDir,
                                   const std::vector<FrameRangeInfo>& ranges,
                                   int userStartFrame, int userEndFrame,
                                   double fps = 30.0);

} // namespace AnimExporter

#endif // ANIMEXPORTER_H
