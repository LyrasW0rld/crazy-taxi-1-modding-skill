#pragma once
#include <string>

class AssetExtractor
{
public:
    // Checks if 3D, splines, originals, textures exist and are populated.
    // If any is missing or empty, runs the full native extraction pipeline.
    static bool CheckAndRunAutoExtraction(const std::string& modDir, const std::string& gameDir);

    // Forces a full extraction of textures, 3D models, and splines.
    static bool ExtractAllAssets(const std::string& modDir, const std::string& gameDir);

    // Returns true if asset extraction is actively running in background
    static bool IsExtracting();
private:
    static bool s_IsExtracting;
};

