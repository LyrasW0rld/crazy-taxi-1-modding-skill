#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <gl/gl.h>
#include <gl/glu.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <memory>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "glu32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

// Modern Windows Common Controls v6
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------------------------
// Data Structures
// ---------------------------------------------------------------------------------

struct Vertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

struct Triangle {
    int v0, v1, v2;
};

struct Submesh {
    std::string materialName;
    std::string textureFilename;
    std::string groupName;       // OBJ "g"/"o" group (assembly part_name for assembled models)
    std::string partLabel;       // display label (assembly part_label / part_name / group name)
    bool textureMissing = false; // texture file could not be located on disk
    GLuint textureId = 0;
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
};

struct Model3D {
    std::string name;
    std::string filePath;
    std::vector<Submesh> submeshes;
    float minX = 1e9f, minY = 1e9f, minZ = 1e9f;
    float maxX = -1e9f, maxY = -1e9f, maxZ = -1e9f;
    float centerX = 0.0f, centerY = 0.0f, centerZ = 0.0f;
    float boundingRadius = 1.0f;
    int totalVertices = 0;
    int totalTriangles = 0;
    std::string assemblyPath;                  // assembly JSON used for part labels ("" if none)
    std::vector<std::string> missingTextures;  // distinct basenames of textures not found on disk
    int partCount = 0;                         // distinct part labels
    int distinctTextureCount = 0;              // distinct texture basenames used by submeshes
};

// One row of the bottom-left "Applied textures" panel
struct AppliedTexRow {
    std::string basename;   // texture filename (as shown in the main texture list)
    std::string fullPath;   // resolved path (may not exist when missing)
    std::string partLabel;  // joined part label(s) using this texture
    bool missing = false;
};

struct TextureMappingEntry {
    std::string textureFilename;
    std::string modelName;
    std::string modelFile;
    std::string category;
};

struct TextureEntry {
    std::string filename;
    std::string fullPath;
    std::string relativePath;
    std::string category;
    int width = 0;
    int height = 0;
    std::string origFormat;
    int chunkIndex = -1;
    std::string mappedModel;
    std::string mappedModelFile;
};

// ---------------------------------------------------------------------------------
// Global Application State
// ---------------------------------------------------------------------------------

HINSTANCE g_hInst = NULL;
HWND g_hWndMain = NULL;
HWND g_hWndGL = NULL;
HWND g_hWndList = NULL;
HWND g_hWndSearch = NULL;
HWND g_hWndCategoryCombo = NULL;
HWND g_hWndModelCombo = NULL;
HWND g_hWndStatus = NULL;
HWND g_hWndInfoText = NULL;
HWND g_hWndTexApplied = NULL;     // bottom-left panel: textures applied to the current model
HWND g_hWndAppliedCaption = NULL; // "Applied textures (N)" caption above the panel

HDC g_hGLDC = NULL;
HGLRC g_hGLRC = NULL;

std::string g_ModDir;
std::string g_TexturesDir;
std::string g_ModelsDir;

std::vector<TextureEntry> g_AllTextures;
std::vector<int> g_FilteredIndices;

// Reverse index: texture filename -> ALL candidate models from the index's
// models[] array (deduped by model name, index order preserved).
std::unordered_map<std::string, std::vector<TextureMappingEntry>> g_TextureToModelIndex;
std::vector<std::pair<std::string, std::string>> g_AvailableModels; // (displayName, relativePath)
std::unordered_map<std::string, std::string> g_ModelNameToRelPath;

std::unordered_map<std::string, std::shared_ptr<Model3D>> g_LoadedModels;
std::shared_ptr<Model3D> g_CurrentModel = nullptr;

void OnTextureSelected(int row);
void SelectModelByName(const std::string& modelName, const std::string& explicitRelPath = "");
void PopulateAppliedTexturesPanel();
std::string BuildModelStatsString(const std::shared_ptr<Model3D>& model);

// Bottom-left applied-textures panel state
std::vector<AppliedTexRow> g_AppliedRows;
bool g_SuppressAppliedNotify = false;

// Texture OpenGL Cache
std::unordered_map<std::string, GLuint> g_TextureCache;
std::unordered_set<std::string> g_FailedTexturePaths; // remember decode failures (grey fallback)
GLuint g_CheckerboardTexId = 0;
GLuint g_ActiveSelectedTexId = 0;
std::string g_ActiveTexturePath = "";
int g_SelectedTexIndex = -1;

// Camera State
float g_CamRotX = 20.0f;
float g_CamRotY = -35.0f;
float g_CamDist = 45.0f;
float g_CamPanX = 0.0f;
float g_CamPanY = -2.0f;

POINT g_LastMousePos = { 0, 0 };
bool g_IsLMouseDown = false;
bool g_IsRMouseDown = false;
bool g_Wireframe = false;
bool g_AutoRotate = false;
bool g_Lighting = true;

// ---------------------------------------------------------------------------------
// Helper: Load Texture with GDI+ to OpenGL
// ---------------------------------------------------------------------------------

GLuint LoadTextureToOpenGL(const std::wstring& path, int* outW = nullptr, int* outH = nullptr)
{
    Gdiplus::Bitmap bmp(path.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return 0;

    int w = bmp.GetWidth();
    int h = bmp.GetHeight();
    if (outW) *outW = w;
    if (outH) *outH = h;

    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData bmpData;
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData) != Gdiplus::Ok) {
        return 0;
    }

    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // UV CONVENTION: game UV v=0 = texture TOP row (D3D semantics).
    // Textures are uploaded UNFLIPPED (GL v=0 = first row of the PNG = top row) and
    // the extractor writes RAW game UVs, so no vertical row flip is applied here.
    // (Compact-copy only guards against a padded GDI+ stride.)
    std::vector<uint32_t> pixels(w * h);
    const uint32_t* srcRows = (const uint32_t*)bmpData.Scan0;
    int srcStride = bmpData.Stride / 4;
    for (int y = 0; y < h; ++y) {
        memcpy(pixels.data() + ((size_t)y * w), srcRows + ((size_t)y * srcStride), w * sizeof(uint32_t));
    }

    // Upload with mipmaps via gluBuild2DMipmaps
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGBA, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels.data());

    bmp.UnlockBits(&bmpData);
    return texId;
}

GLuint GetOrCreateTexture(const std::string& fullPath, int* outW = nullptr, int* outH = nullptr)
{
    if (g_hGLDC && g_hGLRC) {
        wglMakeCurrent(g_hGLDC, g_hGLRC);
    }

    auto it = g_TextureCache.find(fullPath);
    if (it != g_TextureCache.end() && glIsTexture(it->second)) {
        return it->second;
    }
    // Do not retry known-bad paths every frame; the submesh renders flat grey instead.
    if (g_FailedTexturePaths.count(fullPath)) return 0;

    std::wstring wpath(fullPath.begin(), fullPath.end());
    GLuint tid = LoadTextureToOpenGL(wpath, outW, outH);
    if (tid) {
        g_TextureCache[fullPath] = tid;
    } else {
        g_FailedTexturePaths.insert(fullPath);
    }
    return tid;
}

GLuint CreateCheckerboardTexture()
{
    const int sz = 64;
    DWORD pixels[sz * sz];
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            bool check = ((x / 8) + (y / 8)) % 2 == 0;
            pixels[y * sz + x] = check ? 0xFF353A45 : 0xFF282C35;
        }
    }

    GLuint tid = 0;
    glGenTextures(1, &tid);
    glBindTexture(GL_TEXTURE_2D, tid);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sz, sz, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels);
    return tid;
}

// ---------------------------------------------------------------------------------
// Index & Model Discovery
// ---------------------------------------------------------------------------------

static std::string JsonGetString(const std::string& json, size_t start, size_t end, const char* key)
{
    std::string token = std::string("\"") + key + "\"";
    size_t kp = json.find(token, start);
    if (kp == std::string::npos || kp >= end) return "";
    size_t colon = json.find(':', kp);
    if (colon == std::string::npos || colon >= end) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos || q1 >= end) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 >= end) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

static bool IEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return tolower((unsigned char)x) == tolower((unsigned char)y);
    });
}

// Load 3D_texture_to_model_index.json. Each entry carries a "models" array with
// ALL models using the texture (model_name/model_file/category per element).
// The old parser stopped at the first '}' — i.e. it read models[0] only, so the
// extractors's "first registrant wins" order decided which model a shared
// texture (e.g. wheels shared by 15 vehicles, character textures registered as
// {Char}_Character before {Char}_Complete) pointed to. Here the full array is
// read and deduped; PickPrimaryModel() then resolves the display target.
void LoadTextureToModelIndex()
{
    g_TextureToModelIndex.clear();
    std::string jsonPath = g_ModelsDir + "\\3D_texture_to_model_index.json";
    if (!PathFileExistsA(jsonPath.c_str())) {
        jsonPath = g_ModDir + "\\3D_texture_to_model_index.json";
    }
    if (!PathFileExistsA(jsonPath.c_str())) return;

    std::ifstream f(jsonPath);
    if (!f.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    const std::string token = "\"texture_filename\":";
    while (pos < content.size()) {
        size_t keyStart = content.find(token, pos);
        if (keyStart == std::string::npos) break;

        // Bound the WHOLE entry object (each entry starts with texture_filename).
        // Generated JSON contains no braces inside strings, so brace matching is safe.
        size_t objStart = content.rfind('{', keyStart);
        if (objStart == std::string::npos) break;
        int depth = 0;
        size_t objEnd = std::string::npos;
        for (size_t p = objStart; p < content.size(); ++p) {
            if (content[p] == '{') {
                depth++;
            } else if (content[p] == '}') {
                depth--;
                if (depth == 0) { objEnd = p; break; }
            }
        }
        if (objEnd == std::string::npos) objEnd = content.size();

        std::vector<TextureMappingEntry> models;

        // Preferred: every element of the entry's models[] array
        size_t modelsKey = content.find("\"models\"", objStart);
        if (modelsKey != std::string::npos && modelsKey < objEnd) {
            size_t arrStart = content.find('[', modelsKey);
            if (arrStart != std::string::npos && arrStart < objEnd) {
                int arrDepth = 0;
                size_t elemStart = std::string::npos;
                for (size_t p = arrStart; p < objEnd; ++p) {
                    char c = content[p];
                    if (c == '[') {
                        arrDepth++;
                    } else if (c == ']') {
                        if (arrDepth == 1) break; // end of models array
                        arrDepth--;
                    } else if (c == '{' && arrDepth == 1) {
                        elemStart = p;
                    } else if (c == '}' && arrDepth == 1 && elemStart != std::string::npos) {
                        TextureMappingEntry e;
                        e.modelName = JsonGetString(content, elemStart, p + 1, "model_name");
                        e.modelFile = JsonGetString(content, elemStart, p + 1, "model_file");
                        e.category  = JsonGetString(content, elemStart, p + 1, "category");
                        if (!e.modelName.empty() || !e.modelFile.empty()) models.push_back(e);
                        elemStart = std::string::npos;
                    }
                }
            }
        }

        // Legacy fallback: no (or empty) models[] — top-level entry members
        if (models.empty()) {
            TextureMappingEntry e;
            e.textureFilename = JsonGetString(content, objStart, objEnd, "texture_filename");
            e.modelName = JsonGetString(content, objStart, objEnd, "model_name");
            e.modelFile = JsonGetString(content, objStart, objEnd, "model_file");
            e.category  = JsonGetString(content, objStart, objEnd, "category");
            if (!e.modelName.empty() || !e.modelFile.empty()) models.push_back(e);
        }

        std::string texFn = JsonGetString(content, objStart, objEnd, "texture_filename");
        if (!texFn.empty() && !models.empty()) {
            // Dedupe by model name: the index registers one element per (model, part)
            // pair, so a model using the texture on 3 parts appears 3 times.
            std::vector<TextureMappingEntry> deduped;
            for (const auto& m : models) {
                bool dup = false;
                for (const auto& d : deduped) {
                    if ((!m.modelName.empty() && IEquals(d.modelName, m.modelName)) ||
                        (m.modelName.empty() && IEquals(d.modelFile, m.modelFile))) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    TextureMappingEntry c = m;
                    c.textureFilename = texFn;
                    deduped.push_back(c);
                }
            }
            if (!deduped.empty()) g_TextureToModelIndex[texFn] = std::move(deduped);
        }

        pos = objEnd + 1;
    }
}

// True if an index entry's model_file exists under the 3D models directory.
static bool IndexModelFileExists(const TextureMappingEntry& m)
{
    if (m.modelFile.empty()) return false;
    std::string rel = m.modelFile;
    std::replace(rel.begin(), rel.end(), '/', '\\');
    return PathFileExistsA((g_ModelsDir + "\\" + rel).c_str()) != FALSE;
}

static bool ModelNameEndsWithComplete(const std::string& n)
{
    const std::string suffix = "_Complete";
    return n.size() > suffix.size() &&
        IEquals(n.substr(n.size() - suffix.size()), suffix);
}

// Resolve which model a texture should point to from its full models[] list:
// 1. first *_Complete model whose OBJ exists on disk (the assembled full model —
//    character textures register the raw {Char}_Character dump before
//    {Char}_Complete, and only the Complete model carries the assembly labels),
// 2. else the first candidate that exists on disk,
// 3. else the first candidate (SelectModelByName has directory/assembly fallbacks).
static bool PickPrimaryModel(const std::vector<TextureMappingEntry>& models, TextureMappingEntry& out)
{
    if (models.empty()) return false;
    const TextureMappingEntry* pick = nullptr;
    for (const auto& m : models) {
        if (ModelNameEndsWithComplete(m.modelName) && IndexModelFileExists(m)) { pick = &m; break; }
    }
    if (!pick) {
        for (const auto& m : models) {
            if (IndexModelFileExists(m)) { pick = &m; break; }
        }
    }
    if (!pick) pick = &models[0];
    out = *pick;
    return true;
}

// ---------------------------------------------------------------------------------
// Wavefront OBJ Parser
// ---------------------------------------------------------------------------------

static int SafeParseInt(const std::string& s) {
    try {
        if (s.empty()) return 0;
        return std::stoi(s);
    } catch (...) {
        return 0;
    }
}

// ---------------------------------------------------------------------------------
// Assembly JSON support (part labels / offsets / rotations / per-record textures)
// Handles BOTH schemas:
//   legacy:    parts[].part_name / file / offset / textures[]
//   extended:  parts[].part_label / rotation_deg / matrix / texture_indices[]
// ---------------------------------------------------------------------------------

struct AssemblyPart {
    std::string partName;
    std::string partLabel;
    std::string file;                        // relative part OBJ path
    float offset[3] = { 0.0f, 0.0f, 0.0f };
    bool hasRotation = false;
    float rotationDeg[3] = { 0.0f, 0.0f, 0.0f };
    bool hasMatrix = false;
    float matrix[16] = { 0.0f };             // row-major 4x4
    std::vector<std::string> textureFiles;   // one entry per mesh record (order matches texture_indices)
    std::vector<int> textureIndices;         // chunk-local texture index per mesh record (-1 = untextured)
};

struct AssemblyData {
    bool loaded = false;
    std::string path;
    std::string name;
    std::string category;
    std::string completeModel;
    std::vector<AssemblyPart> parts;
};

static bool JsonGetNumbers(const std::string& json, size_t start, size_t end, const char* key, std::vector<float>& out)
{
    out.clear();
    std::string token = std::string("\"") + key + "\"";
    size_t kp = json.find(token, start);
    if (kp == std::string::npos || kp >= end) return false;
    size_t lb = json.find('[', kp);
    if (lb == std::string::npos || lb >= end) return false;
    size_t rb = json.find(']', lb);
    if (rb == std::string::npos || rb > end) rb = end;
    std::string body = json.substr(lb + 1, rb - lb - 1);
    std::istringstream ss(body);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        try { out.push_back(std::stof(tok)); } catch (...) { out.push_back(0.0f); }
    }
    return true;
}

static void ParseAssemblyPartObject(const std::string& json, size_t start, size_t end, AssemblyPart& part)
{
    part.partName = JsonGetString(json, start, end, "part_name");
    part.partLabel = JsonGetString(json, start, end, "part_label");
    part.file = JsonGetString(json, start, end, "file");

    std::vector<float> nums;
    if (JsonGetNumbers(json, start, end, "offset", nums) && nums.size() >= 3) {
        part.offset[0] = nums[0]; part.offset[1] = nums[1]; part.offset[2] = nums[2];
    }
    if (JsonGetNumbers(json, start, end, "rotation_deg", nums) && nums.size() >= 3) {
        part.hasRotation = true;
        part.rotationDeg[0] = nums[0]; part.rotationDeg[1] = nums[1]; part.rotationDeg[2] = nums[2];
    }
    if (JsonGetNumbers(json, start, end, "matrix", nums) && nums.size() >= 16) {
        part.hasMatrix = true;
        for (int m = 0; m < 16; ++m) part.matrix[m] = nums[m];
    }

    // textures: array of strings
    size_t texKey = json.find("\"textures\"", start);
    if (texKey != std::string::npos && texKey < end) {
        size_t lb = json.find('[', texKey);
        if (lb != std::string::npos && lb < end) {
            size_t rb = json.find(']', lb);
            if (rb == std::string::npos || rb > end) rb = end;
            size_t p = lb + 1;
            while (p < rb) {
                size_t q1 = json.find('"', p);
                if (q1 == std::string::npos || q1 >= rb) break;
                size_t q2 = json.find('"', q1 + 1);
                if (q2 == std::string::npos || q2 >= rb) break;
                part.textureFiles.push_back(json.substr(q1 + 1, q2 - q1 - 1));
                p = q2 + 1;
            }
        }
    }

    // texture_indices: array of ints
    std::vector<float> idxNums;
    if (JsonGetNumbers(json, start, end, "texture_indices", idxNums)) {
        for (float n : idxNums) part.textureIndices.push_back((int)n);
    }

    if (part.partLabel.empty()) part.partLabel = part.partName;
}

static bool LoadAssemblyJson(const std::string& path, AssemblyData& out)
{
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.empty()) return false;

    out = AssemblyData();
    out.path = path;
    out.name = JsonGetString(content, 0, content.size(), "name");
    out.category = JsonGetString(content, 0, content.size(), "category");
    out.completeModel = JsonGetString(content, 0, content.size(), "complete_model");

    size_t partsKey = content.find("\"parts\"");
    if (partsKey == std::string::npos) return false;
    size_t arrStart = content.find('[', partsKey);
    if (arrStart == std::string::npos) return false;

    // Walk each top-level object inside the parts array (brace matched; generated
    // JSON contains no braces inside strings, so this is safe for our files).
    int depth = 0;
    size_t objStart = std::string::npos;
    for (size_t p = arrStart + 1; p < content.size(); ++p) {
        char c = content[p];
        if (c == '{') {
            if (depth == 0) objStart = p;
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && objStart != std::string::npos) {
                AssemblyPart part;
                ParseAssemblyPartObject(content, objStart, p + 1, part);
                out.parts.push_back(part);
                objStart = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break; // end of parts array
        }
    }

    out.loaded = !out.parts.empty();
    return out.loaded;
}

static std::string FileBaseName(const std::string& path)
{
    const char* b = PathFindFileNameA(path.c_str());
    std::string s = b ? b : "";
    // strip extension for robust comparisons
    size_t dot = s.rfind('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return s;
}

// Locate the assembly JSON belonging to an OBJ: direct basename candidates first,
// then scan all *_assembly.json files for one whose parts[] reference this OBJ.
static std::string FindAssemblyJsonFor(const std::string& objPath)
{
    char drive[_MAX_DRIVE], dir[_MAX_DIR], fname[_MAX_FNAME];
    _splitpath_s(objPath.c_str(), drive, _MAX_DRIVE, dir, _MAX_DIR, fname, _MAX_FNAME, NULL, 0);
    std::string objDir = std::string(drive) + dir;
    std::string base = fname;
    std::string stripped = base;
    const std::string suffix = "_Complete";
    if (stripped.size() > suffix.size() &&
        stripped.compare(stripped.size() - suffix.size(), suffix.size(), suffix) == 0) {
        stripped = stripped.substr(0, stripped.size() - suffix.size());
    }

    const std::string dirs[2] = { objDir, g_ModelsDir + "\\" };
    for (const std::string& d : dirs) {
        const std::string names[2] = { base, stripped };
        for (const std::string& b : names) {
            if (b.empty()) continue;
            std::string cand = d + b + "_assembly.json";
            if (PathFileExistsA(cand.c_str())) return cand;
        }
    }

    // Fallback: part OBJs are referenced by parts[].file inside some assembly JSON
    std::string objBase = base;
    if (objBase.empty()) return "";
    for (const std::string& d : dirs) {
        std::string pattern = d + "*_assembly.json";
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                AssemblyData probe;
                if (LoadAssemblyJson(d + fd.cFileName, probe)) {
                    for (const auto& p : probe.parts) {
                        if (IEquals(FileBaseName(p.file), objBase)) {
                            FindClose(hFind);
                            return probe.path;
                        }
                    }
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }
    return "";
}

// Search the textures subfolders for a bare texture filename.
static std::string FindTextureInTexturesDirs(const std::string& basename)
{
    if (basename.empty()) return "";
    const char* sfList[] = { "3D_Objects", "3D_Stages", "2D_Sprites", "2D_UI", "Fonts", "" };
    for (const char* sf : sfList) {
        std::string candidate = g_TexturesDir + "\\" + (sf[0] ? (std::string(sf) + "\\") : "") + basename;
        if (PathFileExistsA(candidate.c_str())) return candidate;
    }
    return "";
}

// ---------------------------------------------------------------------------------
// Model Discovery (placed after the assembly helpers: the assembled/ prefix is
// derived from the assembly JSON category so that {Char}_Complete character
// models are no longer labeled as vehicles)
// ---------------------------------------------------------------------------------

void DiscoverAllModels()
{
    g_AvailableModels.clear();
    g_ModelNameToRelPath.clear();

    g_AvailableModels.push_back({ "2D Billboard Quad Preview", "" });

    // Category of the assembly JSON belonging to an OBJ (direct basename
    // candidates only); "" when no assembly resolves.
    auto assemblyCategoryFor = [](const std::string& objDir, const std::string& mname) -> std::string {
        std::string stripped = mname;
        const std::string suffix = "_Complete";
        if (stripped.size() > suffix.size() &&
            _stricmp(stripped.substr(stripped.size() - suffix.size()).c_str(), suffix.c_str()) == 0) {
            stripped = stripped.substr(0, stripped.size() - suffix.size());
        }
        const std::string dirs[2] = { objDir + "\\", g_ModelsDir + "\\" };
        for (const std::string& d : dirs) {
            const std::string names[2] = { mname, stripped };
            for (const std::string& b : names) {
                if (b.empty()) continue;
                std::string cand = d + b + "_assembly.json";
                if (PathFileExistsA(cand.c_str())) {
                    AssemblyData probe;
                    if (LoadAssemblyJson(cand, probe) && !probe.category.empty()) {
                        return probe.category;
                    }
                }
            }
        }
        return "";
    };

    auto scanDir = [&](const std::string& subDir, const std::string& prefix, bool useAssemblyCategory) {
        std::string searchDir = g_ModelsDir + "\\" + subDir;
        std::string pattern = searchDir + "\\*.obj";
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string fname = fd.cFileName;
                if (fname.size() >= 4 && fname.substr(fname.size() - 4) == ".obj") {
                    std::string mname = fname.substr(0, fname.size() - 4);
                    std::string relPath = subDir + "/" + fname;
                    std::string usedPrefix = prefix;
                    if (useAssemblyCategory) {
                        std::string cat = assemblyCategoryFor(searchDir, mname);
                        std::string catLower = cat;
                        std::transform(catLower.begin(), catLower.end(), catLower.begin(), ::tolower);
                        if (catLower.find("character") != std::string::npos) {
                            usedPrefix = "[Character]";
                        }
                    }
                    std::string disp = usedPrefix + ": " + mname;
                    g_AvailableModels.push_back({ disp, relPath });
                    g_ModelNameToRelPath[mname] = relPath;
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    };

    scanDir("assembled", "[Vehicle/Bike/Traffic]", true);
    scanDir("characters", "[Pedestrian/Prop]", false);
    scanDir("parts", "[Part]", false);

    // Stages have nested subdirectories: stages/Stage1_Arcade/*.obj
    std::string stagesDir = g_ModelsDir + "\\stages";
    WIN32_FIND_DATAA fdStage;
    HANDLE hStage = FindFirstFileA((stagesDir + "\\*").c_str(), &fdStage);
    if (hStage != INVALID_HANDLE_VALUE) {
        do {
            if (fdStage.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (strcmp(fdStage.cFileName, ".") != 0 && strcmp(fdStage.cFileName, "..") != 0) {
                    std::string stageSub = std::string("stages/") + fdStage.cFileName;
                    std::string stgPattern = stagesDir + "\\" + fdStage.cFileName + "\\*.obj";
                    WIN32_FIND_DATAA fdM;
                    HANDLE hM = FindFirstFileA(stgPattern.c_str(), &fdM);
                    if (hM != INVALID_HANDLE_VALUE) {
                        do {
                            if (!(fdM.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                                std::string fname = fdM.cFileName;
                                if (fname.size() >= 4 && fname.substr(fname.size() - 4) == ".obj") {
                                    std::string mname = fname.substr(0, fname.size() - 4);
                                    std::string relPath = stageSub + "/" + fname;
                                    std::string disp = std::string("[") + fdStage.cFileName + "]: " + mname;
                                    g_AvailableModels.push_back({ disp, relPath });
                                    g_ModelNameToRelPath[mname] = relPath;
                                }
                            }
                        } while (FindNextFileA(hM, &fdM));
                        FindClose(hM);
                    }
                }
            }
        } while (FindNextFileA(hStage, &fdStage));
        FindClose(hStage);
    }
}

void PopulateModelCombo()
{
    if (!g_hWndModelCombo) return;
    SendMessage(g_hWndModelCombo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_AvailableModels.size(); ++i) {
        SendMessageA(g_hWndModelCombo, CB_ADDSTRING, 0, (LPARAM)g_AvailableModels[i].first.c_str());
    }
    int selIdx = 0;
    for (size_t i = 0; i < g_AvailableModels.size(); ++i) {
        if (g_AvailableModels[i].first.find("Axel_Taxi_Complete") != std::string::npos) {
            selIdx = (int)i;
            break;
        }
    }
    SendMessage(g_hWndModelCombo, CB_SETCURSEL, selIdx, 0);
}

// Resolve display labels + missing texture-index fallbacks from an assembly JSON.
static void ApplyAssemblyToModel(const std::shared_ptr<Model3D>& model, const AssemblyData& asmData)
{
    if (!asmData.loaded || model->submeshes.empty()) return;
    model->assemblyPath = asmData.path;

    // Pass 1: resolve each submesh's assembly part (group name first, then texture match)
    std::vector<const AssemblyPart*> match(model->submeshes.size(), nullptr);
    for (size_t si = 0; si < model->submeshes.size(); ++si) {
        Submesh& sub = model->submeshes[si];
        const AssemblyPart* matched = nullptr;

        if (!sub.groupName.empty()) {
            for (const auto& p : asmData.parts) {
                if ((!p.partName.empty() && IEquals(p.partName, sub.groupName)) ||
                    (!p.partLabel.empty() && IEquals(p.partLabel, sub.groupName)) ||
                    (!p.file.empty() && IEquals(FileBaseName(p.file), sub.groupName))) {
                    matched = &p;
                    break;
                }
            }
        }
        // Fallback: unique texture-filename match against part texture lists
        if (!matched && !sub.textureFilename.empty()) {
            std::string texBase = FileBaseName(sub.textureFilename);
            const AssemblyPart* hit = nullptr;
            int hits = 0;
            for (const auto& p : asmData.parts) {
                for (const auto& t : p.textureFiles) {
                    if (IEquals(FileBaseName(t), texBase)) { hit = &p; hits++; break; }
                }
            }
            if (hits == 1) matched = hit; // ambiguous matches keep the group-derived label
        }

        match[si] = matched;
        sub.partLabel = matched ? matched->partLabel : sub.groupName;
    }

    // Pass 2: submeshes without a texture get one from the part's per-record lists.
    // Only the extended schema's texture_indices is authoritative (it encodes -1 =
    // untextured records, matching the engine's +0x20 == 0xFFFFFFFF case); the legacy
    // textures[] array has no -1 marker, so it is never used to guess textures.
    for (const auto& p : asmData.parts) {
        if (p.textureIndices.empty()) continue;
        int ordinal = 0;
        for (size_t si = 0; si < model->submeshes.size(); ++si) {
            if (match[si] != &p) continue;
            Submesh& sub = model->submeshes[si];
            if (sub.textureFilename.empty() && ordinal < (int)p.textureIndices.size()) {
                int tidx = p.textureIndices[ordinal];
                if (tidx >= 0 && tidx < (int)p.textureFiles.size() && !p.textureFiles[tidx].empty()) {
                    std::string base = PathFindFileNameA(p.textureFiles[tidx].c_str());
                    std::string full = FindTextureInTexturesDirs(base);
                    sub.textureFilename = full.empty() ? base : full; // FinalizeModelTextures flags missing
                }
            }
            ordinal++;
        }
    }
}

// Mark textures that cannot be found on disk (they render flat grey, never abort)
// and compute the per-model stats used by the info card / status bar.
static void FinalizeModelTextures(const std::shared_ptr<Model3D>& model)
{
    model->missingTextures.clear();
    model->partCount = 0;
    model->distinctTextureCount = 0;

    std::unordered_map<std::string, int> distinctTex;
    std::unordered_map<std::string, int> labelSeen;

    for (auto& sub : model->submeshes) {
        sub.textureMissing = false;
        if (!sub.textureFilename.empty()) {
            std::string base = PathFindFileNameA(sub.textureFilename.c_str());
            if (!PathFileExistsA(sub.textureFilename.c_str())) {
                std::string alt = FindTextureInTexturesDirs(base);
                if (!alt.empty()) {
                    sub.textureFilename = alt;
                } else {
                    sub.textureMissing = true;
                    if (distinctTex.find(base) == distinctTex.end()) {
                        model->missingTextures.push_back(base);
                    }
                }
            }
            distinctTex[base]++;
        }
        if (sub.partLabel.empty()) sub.partLabel = sub.groupName;
        if (!sub.partLabel.empty() && labelSeen.find(sub.partLabel) == labelSeen.end()) {
            labelSeen[sub.partLabel] = 1;
            model->partCount++;
        }
    }
    model->distinctTextureCount = (int)distinctTex.size();
}

std::shared_ptr<Model3D> LoadWavefrontOBJ(const std::string& objPath)
{
    std::ifstream file(objPath);
    if (!file.is_open()) return nullptr;

    auto model = std::make_shared<Model3D>();
    model->filePath = objPath;
    char fname[_MAX_FNAME];
    _splitpath_s(objPath.c_str(), NULL, 0, NULL, 0, fname, _MAX_FNAME, NULL, 0);
    model->name = fname;

    std::string mtlLibFile = "";
    std::unordered_map<std::string, std::string> materialTextures;

    std::vector<float> posList;  // x, y, z
    std::vector<float> normList; // nx, ny, nz
    std::vector<float> uvList;   // u, v

    std::string currentMtl = "default";
    std::string currentGroup = ""; // OBJ "g"/"o" group = assembly part_name for assembled models
    // Submeshes are keyed by (group, material) and kept in file order of first
    // appearance, so every usemtl group gets its own texture binding.
    std::vector<Submesh> subOrder;
    std::unordered_map<std::string, size_t> subLookup;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string prefix;
        ss >> prefix;

        if (prefix == "mtllib") {
            ss >> mtlLibFile;
            std::string objDir = objPath.substr(0, objPath.find_last_of("\\/"));
            std::string mtlPath = objDir + "\\" + mtlLibFile;
            std::ifstream mtlStream(mtlPath);
            if (mtlStream.is_open()) {
                std::string mline;
                std::string curMat = "";
                while (std::getline(mtlStream, mline)) {
                    if (mline.empty() || mline[0] == '#') continue;
                    std::istringstream mss(mline);
                    std::string mprefix;
                    mss >> mprefix;
                    if (mprefix == "newmtl") {
                        mss >> curMat;
                    } else if (mprefix == "map_Kd") {
                        std::string texRel;
                        mss >> texRel;
                        std::string nativeRel = texRel;
                        std::replace(nativeRel.begin(), nativeRel.end(), '/', '\\');
                        char fullTexPath[MAX_PATH];
                        PathCombineA(fullTexPath, objDir.c_str(), nativeRel.c_str());

                        // Robust fallback across textures directories
                        if (!PathFileExistsA(fullTexPath)) {
                            const char* bname = PathFindFileNameA(nativeRel.c_str());
                            const char* sfList[] = { "3D_Objects", "3D_Stages", "2D_Sprites", "2D_UI", "Fonts", "" };
                            for (const char* sf : sfList) {
                                std::string candidate = g_TexturesDir + "\\" + (sf[0] ? (std::string(sf) + "\\") : "") + bname;
                                if (PathFileExistsA(candidate.c_str())) {
                                    strcpy_s(fullTexPath, candidate.c_str());
                                    break;
                                }
                            }
                        }
                        materialTextures[curMat] = fullTexPath;
                    }
                }
            }
        }
        else if (prefix == "usemtl") {
            ss >> currentMtl;
        }
        else if (prefix == "g" || prefix == "o") {
            std::string rest;
            std::getline(ss, rest);
            size_t b = rest.find_first_not_of(" \t\r");
            size_t e = rest.find_last_not_of(" \t\r");
            currentGroup = (b == std::string::npos) ? "" : rest.substr(b, e - b + 1);
        }
        else if (prefix == "v") {
            float x, y, z;
            ss >> x >> y >> z;
            if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && abs(x) < 1e7f && abs(y) < 1e7f && abs(z) < 1e7f) {
                posList.push_back(x);
                posList.push_back(y);
                posList.push_back(z);

                model->minX = (std::min)(model->minX, x);
                model->minY = (std::min)(model->minY, y);
                model->minZ = (std::min)(model->minZ, z);
                model->maxX = (std::max)(model->maxX, x);
                model->maxY = (std::max)(model->maxY, y);
                model->maxZ = (std::max)(model->maxZ, z);
            }
        }
        else if (prefix == "vn") {
            float nx, ny, nz;
            ss >> nx >> ny >> nz;
            normList.push_back(nx);
            normList.push_back(ny);
            normList.push_back(nz);
        }
        else if (prefix == "vt") {
            float u, v;
            ss >> u >> v;
            uvList.push_back(u);
            uvList.push_back(v);
        }
        else if (prefix == "f") {
            std::string key = currentGroup + "\x01" + currentMtl;
            size_t si;
            auto kit = subLookup.find(key);
            if (kit == subLookup.end()) {
                si = subOrder.size();
                subOrder.emplace_back();
                subOrder[si].materialName = currentMtl;
                subOrder[si].groupName = currentGroup;
                subLookup[key] = si;
            } else {
                si = kit->second;
            }
            Submesh& sub = subOrder[si];
            if (materialTextures.find(currentMtl) != materialTextures.end()) {
                sub.textureFilename = materialTextures[currentMtl];
            }

            std::vector<int> faceVerts;
            std::string vertToken;
            while (ss >> vertToken) {
                int pIdx = 0, tIdx = 0, nIdx = 0;
                size_t slash1 = vertToken.find('/');
                if (slash1 == std::string::npos) {
                    pIdx = SafeParseInt(vertToken);
                } else {
                    pIdx = SafeParseInt(vertToken.substr(0, slash1));
                    size_t slash2 = vertToken.find('/', slash1 + 1);
                    if (slash2 == std::string::npos) {
                        tIdx = SafeParseInt(vertToken.substr(slash1 + 1));
                    } else {
                        if (slash2 > slash1 + 1) {
                            tIdx = SafeParseInt(vertToken.substr(slash1 + 1, slash2 - slash1 - 1));
                        }
                        nIdx = SafeParseInt(vertToken.substr(slash2 + 1));
                    }
                }

                Vertex v;
                if (pIdx > 0 && (size_t)(pIdx * 3) <= posList.size()) {
                    v.x = posList[(pIdx - 1) * 3 + 0];
                    v.y = posList[(pIdx - 1) * 3 + 1];
                    v.z = posList[(pIdx - 1) * 3 + 2];
                } else {
                    v.x = v.y = v.z = 0;
                }

                if (tIdx > 0 && (size_t)(tIdx * 2) <= uvList.size()) {
                    v.u = uvList[(tIdx - 1) * 2 + 0];
                    v.v = uvList[(tIdx - 1) * 2 + 1];
                } else {
                    v.u = v.v = 0;
                }

                if (nIdx > 0 && (size_t)(nIdx * 3) <= normList.size()) {
                    v.nx = normList[(nIdx - 1) * 3 + 0];
                    v.ny = normList[(nIdx - 1) * 3 + 1];
                    v.nz = normList[(nIdx - 1) * 3 + 2];
                } else {
                    v.nx = 0; v.ny = 1; v.nz = 0;
                }

                int newVIdx = (int)sub.vertices.size();
                sub.vertices.push_back(v);
                faceVerts.push_back(newVIdx);
            }

            // Fan triangulation for polygons
            for (size_t i = 1; i + 1 < faceVerts.size(); ++i) {
                Triangle tri;
                tri.v0 = faceVerts[0];
                tri.v1 = faceVerts[i];
                tri.v2 = faceVerts[i + 1];
                sub.triangles.push_back(tri);
            }
        }
    }

    if (posList.empty()) {
        model->minX = model->maxX = 0.0f;
        model->minY = model->maxY = 0.0f;
        model->minZ = model->maxZ = 0.0f;
        model->centerX = model->centerY = model->centerZ = 0.0f;
        model->boundingRadius = 1.0f;
    } else {
        model->centerX = (model->minX + model->maxX) * 0.5f;
        model->centerY = (model->minY + model->maxY) * 0.5f;
        model->centerZ = (model->minZ + model->maxZ) * 0.5f;

        float dx = model->maxX - model->minX;
        float dy = model->maxY - model->minY;
        float dz = model->maxZ - model->minZ;
        if (!std::isfinite(dx) || dx < 0.0f) dx = 1.0f;
        if (!std::isfinite(dy) || dy < 0.0f) dy = 1.0f;
        if (!std::isfinite(dz) || dz < 0.0f) dz = 1.0f;
        model->boundingRadius = (std::max)(1.0f, (std::max)(dx, (std::max)(dy, dz)) * 0.6f);
    }

    for (auto& sub : subOrder) {
        if (!sub.vertices.empty()) {
            model->totalVertices += (int)sub.vertices.size();
            model->totalTriangles += (int)sub.triangles.size();
            model->submeshes.push_back(std::move(sub));
        }
    }

    // Part labels / per-record texture fallbacks from the assembly JSON (if any)
    std::string asmPath = FindAssemblyJsonFor(objPath);
    if (!asmPath.empty()) {
        AssemblyData asmData;
        if (LoadAssemblyJson(asmPath, asmData)) {
            ApplyAssemblyToModel(model, asmData);
        }
    }
    FinalizeModelTextures(model);

    return model;
}

// ---------------------------------------------------------------------------------
// Assembly-based model building (fallback when no assembled OBJ exists).
// Applies per-part offset / rotation_deg (about the part origin) / matrix.
// ---------------------------------------------------------------------------------

static void RotateVecEulerXYZ(float& x, float& y, float& z, const float rotDeg[3])
{
    const float pi = 3.14159265358979f;
    float rx = rotDeg[0] * pi / 180.0f;
    float ry = rotDeg[1] * pi / 180.0f;
    float rz = rotDeg[2] * pi / 180.0f;
    // X axis
    float ny = y * cosf(rx) - z * sinf(rx);
    float nz = y * sinf(rx) + z * cosf(rx);
    y = ny; z = nz;
    // Y axis
    float nx = x * cosf(ry) + z * sinf(ry);
    nz = -x * sinf(ry) + z * cosf(ry);
    x = nx; z = nz;
    // Z axis
    nx = x * cosf(rz) - y * sinf(rz);
    ny = x * sinf(rz) + y * cosf(rz);
    x = nx; y = ny;
}

static void RecomputeModelBounds(const std::shared_ptr<Model3D>& model)
{
    model->minX = model->minY = model->minZ = 1e9f;
    model->maxX = model->maxY = model->maxZ = -1e9f;
    model->totalVertices = 0;
    model->totalTriangles = 0;
    for (const auto& sub : model->submeshes) {
        model->totalVertices += (int)sub.vertices.size();
        model->totalTriangles += (int)sub.triangles.size();
        for (const auto& v : sub.vertices) {
            model->minX = (std::min)(model->minX, v.x);
            model->minY = (std::min)(model->minY, v.y);
            model->minZ = (std::min)(model->minZ, v.z);
            model->maxX = (std::max)(model->maxX, v.x);
            model->maxY = (std::max)(model->maxY, v.y);
            model->maxZ = (std::max)(model->maxZ, v.z);
        }
    }
    if (model->totalVertices == 0) {
        model->minX = model->maxX = 0.0f;
        model->minY = model->maxY = 0.0f;
        model->minZ = model->maxZ = 0.0f;
        model->centerX = model->centerY = model->centerZ = 0.0f;
        model->boundingRadius = 1.0f;
        return;
    }
    model->centerX = (model->minX + model->maxX) * 0.5f;
    model->centerY = (model->minY + model->maxY) * 0.5f;
    model->centerZ = (model->minZ + model->maxZ) * 0.5f;
    float dx = model->maxX - model->minX;
    float dy = model->maxY - model->minY;
    float dz = model->maxZ - model->minZ;
    if (!std::isfinite(dx) || dx < 0.0f) dx = 1.0f;
    if (!std::isfinite(dy) || dy < 0.0f) dy = 1.0f;
    if (!std::isfinite(dz) || dz < 0.0f) dz = 1.0f;
    model->boundingRadius = (std::max)(1.0f, (std::max)(dx, (std::max)(dy, dz)) * 0.6f);
}

std::shared_ptr<Model3D> LoadModelFromAssembly(const AssemblyData& asmData)
{
    auto model = std::make_shared<Model3D>();
    model->name = !asmData.name.empty() ? asmData.name : FileBaseName(asmData.path);
    model->assemblyPath = asmData.path;

    for (const auto& part : asmData.parts) {
        if (part.file.empty()) continue;
        std::string partPath = g_ModelsDir + "\\" + part.file;
        std::replace(partPath.begin(), partPath.end(), '/', '\\');
        auto partModel = LoadWavefrontOBJ(partPath);
        if (!partModel) continue;

        for (auto& sub : partModel->submeshes) {
            // Part OBJs parse with their own "o <file>" group; relabel to this part.
            sub.groupName = !part.partName.empty() ? part.partName : sub.groupName;
            sub.partLabel = !part.partLabel.empty() ? part.partLabel : sub.groupName;

            for (auto& v : sub.vertices) {
                if (part.hasMatrix) {
                    // Row-major 4x4: out = M * [v,1] (assumed to carry the full transform)
                    float x = v.x, y = v.y, z = v.z;
                    v.x = part.matrix[0] * x + part.matrix[1] * y + part.matrix[2] * z + part.matrix[3];
                    v.y = part.matrix[4] * x + part.matrix[5] * y + part.matrix[6] * z + part.matrix[7];
                    v.z = part.matrix[8] * x + part.matrix[9] * y + part.matrix[10] * z + part.matrix[11];
                    float nx = v.nx, ny = v.ny, nz = v.nz;
                    v.nx = part.matrix[0] * nx + part.matrix[1] * ny + part.matrix[2] * nz;
                    v.ny = part.matrix[4] * nx + part.matrix[5] * ny + part.matrix[6] * nz;
                    v.nz = part.matrix[8] * nx + part.matrix[9] * ny + part.matrix[10] * nz;
                } else {
                    if (part.hasRotation) {
                        // Rotation about the part's own origin, then translate by offset
                        RotateVecEulerXYZ(v.x, v.y, v.z, part.rotationDeg);
                        RotateVecEulerXYZ(v.nx, v.ny, v.nz, part.rotationDeg);
                    }
                    v.x += part.offset[0];
                    v.y += part.offset[1];
                    v.z += part.offset[2];
                }
            }
            model->submeshes.push_back(std::move(sub));
        }
    }

    RecomputeModelBounds(model);
    FinalizeModelTextures(model);
    return model;
}

// ---------------------------------------------------------------------------------
// 2D Quad Fallback Model for Sprites / Fonts / UI
// ---------------------------------------------------------------------------------

std::shared_ptr<Model3D> Create2DQuadModel(float aspect = 1.0f)
{
    auto model = std::make_shared<Model3D>();
    model->name = "2D Billboard Quad Preview";
    model->boundingRadius = 15.0f;

    float halfW = 10.0f * aspect;
    float halfH = 10.0f;

    Submesh sub;
    sub.materialName = "mat_quad";
    sub.groupName = "2D Preview";
    sub.partLabel = "2D Preview";
    // UV convention: v=0 = texture TOP row (textures uploaded unflipped).
    // The quad's lower vertices (negative Y) therefore sample v=1 (image bottom).
    if (!g_ActiveTexturePath.empty()) {
        sub.textureFilename = g_ActiveTexturePath;
    }
    sub.vertices = {
        { -halfW, -halfH, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f },
        {  halfW, -halfH, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 1.0f },
        {  halfW,  halfH, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f },
        { -halfW,  halfH, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f },
    };
    sub.triangles = {
        { 0, 1, 2 },
        { 0, 2, 3 }
    };

    model->minX = -halfW; model->maxX = halfW;
    model->minY = -halfH; model->maxY = halfH;
    model->minZ = 0.0f;   model->maxZ = 0.0f;
    model->centerX = 0.0f; model->centerY = 0.0f; model->centerZ = 0.0f;
    model->totalVertices = 4;
    model->totalTriangles = 2;
    model->submeshes.push_back(sub);

    return model;
}

static bool FastReadPngDimensions(const std::string& filePath, int& outW, int& outH)
{
    // 1. Fast parse from filename: e.g. "..._FMT_VQ_ARGB1555_128x128__..." or "..._256x256_..."
    size_t xPos = filePath.rfind('x');
    while (xPos != std::string::npos && xPos > 0 && xPos + 1 < filePath.size()) {
        if (isdigit((unsigned char)filePath[xPos - 1]) && isdigit((unsigned char)filePath[xPos + 1])) {
            size_t startW = xPos - 1;
            while (startW > 0 && isdigit((unsigned char)filePath[startW - 1])) startW--;
            size_t endH = xPos + 1;
            while (endH < filePath.size() && isdigit((unsigned char)filePath[endH])) endH++;
            int w = atoi(filePath.substr(startW, xPos - startW).c_str());
            int h = atoi(filePath.substr(xPos + 1, endH - (xPos + 1)).c_str());
            if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
                outW = w;
                outH = h;
                return true;
            }
        }
        xPos = (xPos > 0) ? filePath.rfind('x', xPos - 1) : std::string::npos;
    }

    // 2. Fallback: Read 24-byte PNG IHDR header
    FILE* f = nullptr;
    if (fopen_s(&f, filePath.c_str(), "rb") == 0 && f) {
        uint8_t hdr[24];
        if (fread(hdr, 1, 24, f) == 24) {
            if (hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G') {
                outW = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
                outH = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
                fclose(f);
                return (outW > 0 && outH > 0);
            }
        }
        fclose(f);
    }
    return false;
}

// ---------------------------------------------------------------------------------
// Scan Mod Textures & Manifest
// ---------------------------------------------------------------------------------

static void ScanDirectoryForViewer(const std::string& baseDir, const std::string& currentSub, const std::string& cat, std::vector<TextureEntry>& outEntries)
{
    std::string dir = currentSub.empty() ? baseDir : (baseDir + "\\" + currentSub);
    std::string pattern = dir + "\\*";

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        std::string fullPath = dir + "\\" + fd.cFileName;
        std::string relItem = currentSub.empty() ? fd.cFileName : (currentSub + "/" + fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDirectoryForViewer(baseDir, relItem, cat, outEntries);
        } else {
            const char* ext = strrchr(fd.cFileName, '.');
            if (ext && (_stricmp(ext, ".png") == 0 || _stricmp(ext, ".jpg") == 0 || _stricmp(ext, ".jpeg") == 0)) {
                TextureEntry entry;
                entry.filename = fd.cFileName;
                entry.fullPath = fullPath;
                entry.relativePath = relItem;
                entry.category = cat;

                FastReadPngDimensions(entry.fullPath, entry.width, entry.height);

                auto it = g_TextureToModelIndex.find(entry.filename);
                TextureMappingEntry picked;
                if (it != g_TextureToModelIndex.end() && PickPrimaryModel(it->second, picked)) {
                    entry.mappedModel = picked.modelName;
                    entry.mappedModelFile = picked.modelFile;
                } else if (entry.category == "3D_OBJECT") {
                    entry.mappedModel = "(unmapped)";
                    entry.mappedModelFile = "";
                } else if (entry.category == "3D_STAGE") {
                    entry.mappedModel = "Stage1_Arcade_Mesh_000";
                    entry.mappedModelFile = "stages/Stage1_Arcade/Stage1_Arcade_Mesh_000.obj";
                } else {
                    entry.mappedModel = "2D Billboard Quad Preview";
                    entry.mappedModelFile = "";
                }

                outEntries.push_back(entry);
            }
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}

void ScanModTextures()
{
    g_AllTextures.clear();

    const std::vector<std::pair<std::string, std::string>> subfolderCategories = {
        { "3D_Objects", "3D_OBJECT" },
        { "3D_Stages", "3D_STAGE" },
        { "2D_Sprites", "2D_SPRITE" },
        { "2D_UI", "2D_UI" },
        { "Fonts", "2D_FONT" }
    };

    for (const auto& sf : subfolderCategories) {
        ScanDirectoryForViewer(g_TexturesDir, sf.first, sf.second, g_AllTextures);
    }

    // Sort textures by category and filename
    std::sort(g_AllTextures.begin(), g_AllTextures.end(), [](const TextureEntry& a, const TextureEntry& b) {
        if (a.category != b.category) return a.category < b.category;
        return a.filename < b.filename;
    });
}

// ---------------------------------------------------------------------------------
// OpenGL Rendering
// ---------------------------------------------------------------------------------

void InitOpenGL(HWND hWnd)
{
    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR),
        1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA,
        32,
        0, 0, 0, 0, 0, 0,
        8,
        0,
        0,
        0, 0, 0, 0,
        24,
        8,
        0,
        PFD_MAIN_PLANE,
        0,
        0, 0, 0
    };

    g_hGLDC = GetDC(hWnd);
    int pixelFormat = ChoosePixelFormat(g_hGLDC, &pfd);
    SetPixelFormat(g_hGLDC, pixelFormat, &pfd);
    g_hGLRC = wglCreateContext(g_hGLDC);
    wglMakeCurrent(g_hGLDC, g_hGLRC);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_TEXTURE_2D);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.05f);

    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);

    GLfloat lightPos[] = { 30.0f, 50.0f, 40.0f, 0.0f };
    GLfloat lightAmbient[] = { 0.45f, 0.45f, 0.48f, 1.0f };
    GLfloat lightDiffuse[] = { 0.85f, 0.85f, 0.82f, 1.0f };
    GLfloat lightSpecular[] = { 0.30f, 0.30f, 0.30f, 1.0f };

    glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
    glLightfv(GL_LIGHT0, GL_AMBIENT, lightAmbient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, lightDiffuse);
    glLightfv(GL_LIGHT0, GL_SPECULAR, lightSpecular);

    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    glClearColor(0.12f, 0.13f, 0.16f, 1.0f); // Sleek modern dark backdrop

    g_CheckerboardTexId = CreateCheckerboardTexture();
}

void RenderScene()
{
    if (!g_hGLRC) return;
    wglMakeCurrent(g_hGLDC, g_hGLRC);

    RECT rc;
    GetClientRect(g_hWndGL, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    glViewport(0, 0, width, height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)width / (double)height, 0.5, 2000.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Camera Transform
    glTranslatef(g_CamPanX, g_CamPanY, -g_CamDist);
    glRotatef(g_CamRotX, 1.0f, 0.0f, 0.0f);
    glRotatef(g_CamRotY, 0.0f, 1.0f, 0.0f);

    if (g_AutoRotate) {
        g_CamRotY += 0.4f;
        if (g_CamRotY > 360.0f) g_CamRotY -= 360.0f;
    }

    if (g_Lighting) {
        glEnable(GL_LIGHTING);
    } else {
        glDisable(GL_LIGHTING);
    }

    if (g_Wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    } else {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    // Render Ground Grid
    if (g_CurrentModel && g_CurrentModel->name != "2D Billboard Quad Preview") {
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glColor4f(0.20f, 0.22f, 0.26f, 1.0f);
        glBegin(GL_LINES);
        float gridSize = 60.0f;
        float step = 5.0f;
        for (float i = -gridSize; i <= gridSize; i += step) {
            glVertex3f(i, 0.0f, -gridSize);
            glVertex3f(i, 0.0f,  gridSize);
            glVertex3f(-gridSize, 0.0f, i);
            glVertex3f( gridSize, 0.0f, i);
        }
        glEnd();
        if (g_Lighting) glEnable(GL_LIGHTING);
        glEnable(GL_TEXTURE_2D);
    }

    // Render Model
    if (g_CurrentModel) {
        glPushMatrix();
        // Center model
        glTranslatef(-g_CurrentModel->centerX, -g_CurrentModel->centerY + (g_CurrentModel->maxY - g_CurrentModel->minY) * 0.5f, -g_CurrentModel->centerZ);

        for (auto& sub : g_CurrentModel->submeshes) {
            GLuint tid = 0;
            std::string curTexBase = PathFindFileNameA(g_ActiveTexturePath.c_str());
            std::string subTexBase = PathFindFileNameA(sub.textureFilename.c_str());

            if (g_CurrentModel->name == "2D Billboard Quad Preview") {
                tid = g_ActiveSelectedTexId;
            } else if (!g_ActiveTexturePath.empty() && !subTexBase.empty() && _stricmp(curTexBase.c_str(), subTexBase.c_str()) == 0) {
                // Exact match: selected texture applies to this submesh
                tid = g_ActiveSelectedTexId;
            } else if (!sub.textureFilename.empty()) {
                // Multi-part assembly: load this submesh's own texture
                if (sub.textureId == 0) {
                    sub.textureId = GetOrCreateTexture(sub.textureFilename);
                }
                tid = sub.textureId;
            }

            if (tid) {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, tid);
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            } else {
                glDisable(GL_TEXTURE_2D);
                glColor4f(0.75f, 0.75f, 0.78f, 1.0f);
            }

            glBegin(GL_TRIANGLES);
            for (const auto& tri : sub.triangles) {
                if (tri.v0 < 0 || tri.v0 >= (int)sub.vertices.size() ||
                    tri.v1 < 0 || tri.v1 >= (int)sub.vertices.size() ||
                    tri.v2 < 0 || tri.v2 >= (int)sub.vertices.size()) {
                    continue;
                }
                const Vertex& v0 = sub.vertices[tri.v0];
                const Vertex& v1 = sub.vertices[tri.v1];
                const Vertex& v2 = sub.vertices[tri.v2];

                glNormal3f(v0.nx, v0.ny, v0.nz);
                glTexCoord2f(v0.u, v0.v);
                glVertex3f(v0.x, v0.y, v0.z);

                glNormal3f(v1.nx, v1.ny, v1.nz);
                glTexCoord2f(v1.u, v1.v);
                glVertex3f(v1.x, v1.y, v1.z);

                glNormal3f(v2.nx, v2.ny, v2.nz);
                glTexCoord2f(v2.u, v2.v);
                glVertex3f(v2.x, v2.y, v2.z);
            }
            glEnd();
        }

        glPopMatrix();
    }

    SwapBuffers(g_hGLDC);
}

// ---------------------------------------------------------------------------------
// UI Control Handlers
// ---------------------------------------------------------------------------------

void ApplyTextureFilter()
{
    char searchBuf[256] = { 0 };
    GetWindowTextA(g_hWndSearch, searchBuf, sizeof(searchBuf));
    std::string searchStr = searchBuf;
    std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);

    int catSel = (int)SendMessage(g_hWndCategoryCombo, CB_GETCURSEL, 0, 0);

    g_FilteredIndices.clear();
    for (int i = 0; i < (int)g_AllTextures.size(); ++i) {
        const auto& tex = g_AllTextures[i];

        if (catSel == 1 && tex.category != "3D_OBJECT") continue;
        if (catSel == 2 && tex.category != "3D_STAGE") continue;
        if (catSel == 3 && tex.category != "2D_SPRITE") continue;
        if (catSel == 4 && tex.category != "2D_UI") continue;
        if (catSel == 5 && tex.category != "2D_FONT") continue;

        if (!searchStr.empty()) {
            std::string fnLower = tex.filename;
            std::transform(fnLower.begin(), fnLower.end(), fnLower.begin(), ::tolower);
            if (fnLower.find(searchStr) == std::string::npos) continue;
        }

        g_FilteredIndices.push_back(i);
    }

    SendMessage(g_hWndList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hWndList);
    ListView_SetItemCount(g_hWndList, (int)g_FilteredIndices.size());

    for (int row = 0; row < (int)g_FilteredIndices.size(); ++row) {
        int idx = g_FilteredIndices[row];
        const auto& tex = g_AllTextures[idx];

        LVITEMA lvi = { 0 };
        lvi.mask = LVIF_TEXT;
        lvi.iItem = row;
        lvi.iSubItem = 0;
        lvi.pszText = (LPSTR)tex.filename.c_str();
        ListView_InsertItem(g_hWndList, &lvi);

        ListView_SetItemText(g_hWndList, row, 1, (LPSTR)tex.category.c_str());
        char dimBuf[32];
        sprintf_s(dimBuf, "%dx%d", tex.width, tex.height);
        ListView_SetItemText(g_hWndList, row, 2, dimBuf);
        ListView_SetItemText(g_hWndList, row, 3, (LPSTR)tex.mappedModel.c_str());
    }

    SendMessage(g_hWndList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hWndList, NULL, TRUE);

    char statusBuf[128];
    sprintf_s(statusBuf, "Displaying %d / %d textures", (int)g_FilteredIndices.size(), (int)g_AllTextures.size());
    SendMessage(g_hWndStatus, SB_SETTEXT, 0, (LPARAM)statusBuf);

    if (!g_FilteredIndices.empty()) {
        ListView_SetItemState(g_hWndList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        OnTextureSelected(0);
    } else {
        SetWindowTextA(g_hWndInfoText, "No textures match current filter.");
    }
}

std::string BuildModelStatsString(const std::shared_ptr<Model3D>& model)
{
    if (!model) return "";
    char buf[192];
    sprintf_s(buf, "Parts: %d | Textures used: %d (%d missing)",
        model->partCount, model->distinctTextureCount, (int)model->missingTextures.size());
    return buf;
}

// Rebuild the bottom-left "Applied textures (N)" panel from the current model.
void PopulateAppliedTexturesPanel()
{
    g_AppliedRows.clear();
    if (g_CurrentModel) {
        std::unordered_map<std::string, size_t> byBase;
        for (const auto& sub : g_CurrentModel->submeshes) {
            if (sub.textureFilename.empty()) continue;
            std::string base = PathFindFileNameA(sub.textureFilename.c_str());
            std::string lbl = !sub.partLabel.empty() ? sub.partLabel
                              : (!sub.groupName.empty() ? sub.groupName : sub.materialName);
            auto it = byBase.find(base);
            if (it == byBase.end()) {
                AppliedTexRow row;
                row.basename = base;
                row.fullPath = sub.textureFilename;
                row.partLabel = lbl;
                row.missing = sub.textureMissing;
                byBase[base] = g_AppliedRows.size();
                g_AppliedRows.push_back(row);
            } else {
                AppliedTexRow& row = g_AppliedRows[it->second];
                if (row.partLabel.find(lbl) == std::string::npos &&
                    row.partLabel.size() + lbl.size() + 2 < 96) {
                    row.partLabel += ", " + lbl;
                }
            }
        }
    }

    if (g_hWndTexApplied) {
        g_SuppressAppliedNotify = true;
        SendMessage(g_hWndTexApplied, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(g_hWndTexApplied);
        for (size_t r = 0; r < g_AppliedRows.size(); ++r) {
            const AppliedTexRow& row = g_AppliedRows[r];
            std::string col0 = row.missing ? (row.basename + "  [missing]") : row.basename;
            LVITEMA lvi = { 0 };
            lvi.mask = LVIF_TEXT;
            lvi.iItem = (int)r;
            lvi.iSubItem = 0;
            lvi.pszText = (LPSTR)col0.c_str();
            ListView_InsertItem(g_hWndTexApplied, &lvi);
            ListView_SetItemText(g_hWndTexApplied, (int)r, 1, (LPSTR)row.partLabel.c_str());
        }
        SendMessage(g_hWndTexApplied, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_hWndTexApplied, NULL, TRUE);
        g_SuppressAppliedNotify = false;
    }

    if (g_hWndAppliedCaption) {
        char capBuf[128];
        sprintf_s(capBuf, "Applied textures (%d)", (int)g_AppliedRows.size());
        SetWindowTextA(g_hWndAppliedCaption, capBuf);
    }
}

void SelectModelByName(const std::string& modelName, const std::string& explicitRelPath)
{
    if (modelName == "2D Billboard Quad Preview") {
        float aspect = 1.0f;
        if (g_SelectedTexIndex >= 0 && g_SelectedTexIndex < (int)g_AllTextures.size()) {
            const auto& tex = g_AllTextures[g_SelectedTexIndex];
            if (tex.height > 0) aspect = (float)tex.width / (float)tex.height;
        }
        g_CurrentModel = Create2DQuadModel(aspect);
        g_CamDist = 28.0f;
        g_CamRotX = 0.0f;
        g_CamRotY = 0.0f;
        g_CamPanX = 0.0f;
        g_CamPanY = 0.0f;
        FinalizeModelTextures(g_CurrentModel);
        PopulateAppliedTexturesPanel();
        if (g_hWndStatus) {
            std::string s = "Model: 2D Billboard Quad Preview | " + BuildModelStatsString(g_CurrentModel);
            SendMessage(g_hWndStatus, SB_SETTEXT, 0, (LPARAM)s.c_str());
        }
        InvalidateRect(g_hWndGL, NULL, FALSE);
        return;
    }

    auto it = g_LoadedModels.find(modelName);
    if (it != g_LoadedModels.end()) {
        g_CurrentModel = it->second;
        g_CamDist = g_CurrentModel->boundingRadius * 2.2f;
        g_CamPanX = 0.0f;
        g_CamPanY = -g_CurrentModel->centerY * 0.3f;
    } else {
        std::string targetObjPath = "";
        if (!explicitRelPath.empty()) {
            std::string cand = g_ModelsDir + "\\" + explicitRelPath;
            std::replace(cand.begin(), cand.end(), '/', '\\');
            if (PathFileExistsA(cand.c_str())) targetObjPath = cand;
        }

        if (targetObjPath.empty()) {
            auto rit = g_ModelNameToRelPath.find(modelName);
            if (rit != g_ModelNameToRelPath.end()) {
                std::string cand = g_ModelsDir + "\\" + rit->second;
                std::replace(cand.begin(), cand.end(), '/', '\\');
                if (PathFileExistsA(cand.c_str())) targetObjPath = cand;
            }
        }

        if (targetObjPath.empty()) {
            const char* dirs[] = {
                "assembled\\", "parts\\", "characters\\",
                "stages\\Stage1_Arcade\\", "stages\\Stage2_Original\\", "stages\\Stage3_CrazyBox\\"
            };
            for (const char* d : dirs) {
                std::string cand = g_ModelsDir + "\\" + d + modelName + ".obj";
                if (PathFileExistsA(cand.c_str())) { targetObjPath = cand; break; }
            }
        }

        std::shared_ptr<Model3D> m = nullptr;
        if (!targetObjPath.empty()) {
            m = LoadWavefrontOBJ(targetObjPath);
        }

        // Fallback: no assembled OBJ — build the model from the assembly JSON's
        // part OBJs, applying per-part offset / rotation_deg / matrix transforms.
        if (!m) {
            char fname[_MAX_FNAME];
            _splitpath_s(modelName.c_str(), NULL, 0, NULL, 0, fname, _MAX_FNAME, NULL, 0);
            std::string base = fname;
            std::string stripped = base;
            const std::string suffix = "_Complete";
            if (stripped.size() > suffix.size() &&
                stripped.compare(stripped.size() - suffix.size(), suffix.size(), suffix) == 0) {
                stripped = stripped.substr(0, stripped.size() - suffix.size());
            }
            const std::string candPaths[2] = { g_ModelsDir + "\\" + base + "_assembly.json",
                                               g_ModelsDir + "\\" + stripped + "_assembly.json" };
            for (const std::string& cand : candPaths) {
                AssemblyData asmData;
                if (LoadAssemblyJson(cand, asmData)) {
                    m = LoadModelFromAssembly(asmData);
                    break;
                }
            }
        }

        if (m) {
            g_LoadedModels[modelName] = m;
            g_CurrentModel = m;
            g_CamDist = g_CurrentModel->boundingRadius * 2.2f;
            g_CamPanX = 0.0f;
            g_CamPanY = -g_CurrentModel->centerY * 0.3f;
        }
    }

    if (g_CurrentModel && g_hWndInfoText) {
        std::string info = "Active 3D Model:\r\n";
        info += "Model: " + g_CurrentModel->name + "\r\n";
        info += "Vertices: " + std::to_string(g_CurrentModel->totalVertices) + " | Triangles: " + std::to_string(g_CurrentModel->totalTriangles) + "\r\n";
        char boundsBuf[512] = { 0 };
        float w = g_CurrentModel->maxX - g_CurrentModel->minX;
        float h = g_CurrentModel->maxY - g_CurrentModel->minY;
        float d = g_CurrentModel->maxZ - g_CurrentModel->minZ;
        if (g_CurrentModel->totalVertices > 0 && std::isfinite(w) && std::isfinite(h) && std::isfinite(d)) {
            sprintf_s(boundsBuf, "Dimensions: W=%.1f, H=%.1f, D=%.1f\r\n", w, h, d);
        } else {
            sprintf_s(boundsBuf, "Dimensions: N/A\r\n");
        }
        info += boundsBuf;
        info += BuildModelStatsString(g_CurrentModel) + "\r\n";
        if (!g_CurrentModel->assemblyPath.empty()) {
            info += "Part labels: " + std::string(PathFindFileNameA(g_CurrentModel->assemblyPath.c_str())) + "\r\n";
        }

        std::string primTex = "";
        int texW = 0, texH = 0;
        for (const auto& sub : g_CurrentModel->submeshes) {
            if (!sub.textureFilename.empty()) {
                primTex = PathFindFileNameA(sub.textureFilename.c_str());
                FastReadPngDimensions(sub.textureFilename, texW, texH);
                break;
            }
        }
        if (!primTex.empty()) {
            info += "Primary Texture: " + primTex + "\r\n";
            info += "Resolution: " + std::to_string(texW) + "x" + std::to_string(texH) + "\r\n";
        }

        // List textures referenced by the model but missing on disk
        if (!g_CurrentModel->missingTextures.empty()) {
            info += "Missing textures (" + std::to_string(g_CurrentModel->missingTextures.size()) + "):\r\n";
            for (size_t i = 0; i < g_CurrentModel->missingTextures.size() && i < 6; ++i) {
                info += "  - " + g_CurrentModel->missingTextures[i] + "\r\n";
            }
            if (g_CurrentModel->missingTextures.size() > 6) {
                info += "  ... and " + std::to_string(g_CurrentModel->missingTextures.size() - 6) + " more\r\n";
            }
        }
        SetWindowTextA(g_hWndInfoText, info.c_str());
    }

    PopulateAppliedTexturesPanel();

    // Status bar: model load summary
    if (g_hWndStatus && g_CurrentModel) {
        std::string s = "Model: " + g_CurrentModel->name + " | " + BuildModelStatsString(g_CurrentModel);
        SendMessage(g_hWndStatus, SB_SETTEXT, 0, (LPARAM)s.c_str());
    }

    InvalidateRect(g_hWndGL, NULL, FALSE);
}

void OnTextureSelected(int row)
{
    static bool s_inSelection = false;
    if (s_inSelection) return;
    s_inSelection = true;

    if (row < 0 || row >= (int)g_FilteredIndices.size()) {
        s_inSelection = false;
        return;
    }
    int texIdx = g_FilteredIndices[row];
    auto& tex = g_AllTextures[texIdx];
    g_SelectedTexIndex = texIdx;

    g_ActiveTexturePath = tex.fullPath;
    g_ActiveSelectedTexId = GetOrCreateTexture(tex.fullPath, &tex.width, &tex.height);

    // Update list item dimensions
    char dimBuf[32];
    sprintf_s(dimBuf, "%dx%d", tex.width, tex.height);
    ListView_SetItemText(g_hWndList, row, 2, dimBuf);

    if (tex.category == "3D_OBJECT" || tex.category == "3D_STAGE") {
        // A texture shared by several models must not yank the view to the index's
        // first registrant: if the currently shown model is one of the texture's
        // mapped models, keep it; otherwise load the resolved primary model.
        std::string targetModel = tex.mappedModel;
        std::string targetFile = tex.mappedModelFile;
        auto mit = g_TextureToModelIndex.find(tex.filename);
        if (g_CurrentModel && mit != g_TextureToModelIndex.end()) {
            for (const auto& m : mit->second) {
                if (IEquals(m.modelName, g_CurrentModel->name)) {
                    targetModel = m.modelName;
                    targetFile = m.modelFile;
                    break;
                }
            }
        }
        SelectModelByName(targetModel, targetFile);

        // Sync Model combo selection: exact match on the name after the "prefix: " separator
        int count = (int)SendMessage(g_hWndModelCombo, CB_GETCOUNT, 0, 0);
        for (int i = 0; i < count; ++i) {
            char itemBuf[256];
            SendMessageA(g_hWndModelCombo, CB_GETLBTEXT, i, (LPARAM)itemBuf);
            std::string itemStr = itemBuf;
            std::string itemModel = itemStr;
            size_t sep = itemStr.find(": ");
            if (sep != std::string::npos) itemModel = itemStr.substr(sep + 2);
            if (IEquals(itemModel, targetModel)) {
                SendMessage(g_hWndModelCombo, CB_SETCURSEL, i, 0);
                break;
            }
        }
    } else {
        // 2D Quad view with accurate aspect ratio
        float aspect = tex.height > 0 ? (float)tex.width / (float)tex.height : 1.0f;
        g_CurrentModel = Create2DQuadModel(aspect);
        FinalizeModelTextures(g_CurrentModel);
        g_CamDist = 28.0f;
        g_CamRotX = 0.0f;
        g_CamRotY = 0.0f;
        g_CamPanX = 0.0f;
        g_CamPanY = 0.0f;

        // Select "2D Billboard Quad Preview" in combo
        int count = (int)SendMessage(g_hWndModelCombo, CB_GETCOUNT, 0, 0);
        for (int i = 0; i < count; ++i) {
            char itemBuf[256];
            SendMessageA(g_hWndModelCombo, CB_GETLBTEXT, i, (LPARAM)itemBuf);
            if (strstr(itemBuf, "2D Billboard")) {
                SendMessage(g_hWndModelCombo, CB_SETCURSEL, i, 0);
                break;
            }
        }
        PopulateAppliedTexturesPanel();
    }

    // Update Info Text Card
    std::string info = "Selected Texture:\r\n";
    info += "File: " + tex.filename + "\r\n";
    info += "Resolution: " + std::to_string(tex.width) + "x" + std::to_string(tex.height) + "\r\n";
    info += "Category: " + tex.category + "\r\n";
    info += "Mapped 3D Model: " + tex.mappedModel + "\r\n";
    if (g_CurrentModel) {
        info += "Active Mesh: " + g_CurrentModel->name + " (Verts: " + std::to_string(g_CurrentModel->totalVertices) + ", Tris: " + std::to_string(g_CurrentModel->totalTriangles) + ")\r\n";
        info += BuildModelStatsString(g_CurrentModel);
        if (!g_CurrentModel->missingTextures.empty()) {
            info += "\r\nMissing textures (" + std::to_string(g_CurrentModel->missingTextures.size()) + "):";
            for (size_t i = 0; i < g_CurrentModel->missingTextures.size() && i < 4; ++i) {
                info += "\r\n  - " + g_CurrentModel->missingTextures[i];
            }
        }
    }
    SetWindowTextA(g_hWndInfoText, info.c_str());

    InvalidateRect(g_hWndGL, NULL, FALSE);
    s_inSelection = false;
}

// ---------------------------------------------------------------------------------
// Window Procedures
// ---------------------------------------------------------------------------------

LRESULT CALLBACK GLViewProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        RenderScene();
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_SIZE:
        RenderScene();
        return 0;

    case WM_LBUTTONDOWN:
        g_IsLMouseDown = true;
        g_LastMousePos.x = GET_X_LPARAM(lParam);
        g_LastMousePos.y = GET_Y_LPARAM(lParam);
        SetCapture(hWnd);
        return 0;

    case WM_LBUTTONUP:
        g_IsLMouseDown = false;
        ReleaseCapture();
        return 0;

    case WM_RBUTTONDOWN:
        g_IsRMouseDown = true;
        g_LastMousePos.x = GET_X_LPARAM(lParam);
        g_LastMousePos.y = GET_Y_LPARAM(lParam);
        SetCapture(hWnd);
        return 0;

    case WM_RBUTTONUP:
        g_IsRMouseDown = false;
        ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        int dx = x - g_LastMousePos.x;
        int dy = y - g_LastMousePos.y;
        g_LastMousePos.x = x;
        g_LastMousePos.y = y;

        if (g_IsLMouseDown) {
            g_CamRotY += dx * 0.5f;
            g_CamRotX += dy * 0.5f;
            if (g_CamRotX > 89.0f) g_CamRotX = 89.0f;
            if (g_CamRotX < -89.0f) g_CamRotX = -89.0f;
            InvalidateRect(hWnd, NULL, FALSE);
        } else if (g_IsRMouseDown) {
            g_CamPanX += dx * 0.05f;
            g_CamPanY -= dy * 0.05f;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        g_CamDist -= (float)delta * 0.02f;
        if (g_CamDist < 2.0f) g_CamDist = 2.0f;
        if (g_CamDist > 600.0f) g_CamDist = 600.0f;
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }

    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
}

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    {
        // Category Filter Combo
        g_hWndCategoryCombo = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            12, 12, 180, 200, hWnd, (HMENU)101, g_hInst, NULL);
        SendMessageA(g_hWndCategoryCombo, CB_ADDSTRING, 0, (LPARAM)"All Categories");
        SendMessageA(g_hWndCategoryCombo, CB_ADDSTRING, 0, (LPARAM)"3D Objects (Cabs/Wheels)");
        SendMessageA(g_hWndCategoryCombo, CB_ADDSTRING, 0, (LPARAM)"3D Stages (Geometry)");
        SendMessageA(g_hWndCategoryCombo, CB_ADDSTRING, 0, (LPARAM)"2D Sprites (HUD)");
        SendMessageA(g_hWndCategoryCombo, CB_ADDSTRING, 0, (LPARAM)"2D UI (Menus/JPK)");
        SendMessageA(g_hWndCategoryCombo, CB_ADDSTRING, 0, (LPARAM)"Fonts (Subtitles/Texts)");
        SendMessageA(g_hWndCategoryCombo, CB_SETCURSEL, 0, 0);

        // Search Input Box
        g_hWndSearch = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            200, 12, 230, 24, hWnd, (HMENU)102, g_hInst, NULL);

        // 3D Model Selector Combo
        g_hWndModelCombo = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            450, 12, 220, 450, hWnd, (HMENU)103, g_hInst, NULL);

        // Toggles
        CreateWindowA("BUTTON", "Wireframe", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            680, 14, 85, 20, hWnd, (HMENU)104, g_hInst, NULL);
        CreateWindowA("BUTTON", "Rotate", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            770, 14, 65, 20, hWnd, (HMENU)105, g_hInst, NULL);
        CreateWindowA("BUTTON", "Reset View", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            845, 12, 85, 24, hWnd, (HMENU)106, g_hInst, NULL);

        // Texture List View
        g_hWndList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
            12, 45, 418, 520, hWnd, (HMENU)107, g_hInst, NULL);
        ListView_SetExtendedListViewStyle(g_hWndList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNA col;
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 210; col.pszText = (LPSTR)"Filename"; ListView_InsertColumn(g_hWndList, 0, &col);
        col.cx = 85;  col.pszText = (LPSTR)"Category"; ListView_InsertColumn(g_hWndList, 1, &col);
        col.cx = 65;  col.pszText = (LPSTR)"Resolution"; ListView_InsertColumn(g_hWndList, 2, &col);
        col.cx = 120; col.pszText = (LPSTR)"Mapped 3D Model"; ListView_InsertColumn(g_hWndList, 3, &col);

        // Selected Texture Info Text
        g_hWndInfoText = CreateWindowExA(WS_EX_STATICEDGE, "EDIT", "Select a texture to preview in 3D...",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            12, 575, 418, 110, hWnd, (HMENU)108, g_hInst, NULL);

        // Applied-textures panel (bottom-left): all textures used by the current model
        g_hWndAppliedCaption = CreateWindowA("STATIC", "Applied textures (0)",
            WS_CHILD | WS_VISIBLE, 12, 692, 418, 18, hWnd, (HMENU)111, g_hInst, NULL);
        g_hWndTexApplied = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
            12, 712, 418, 120, hWnd, (HMENU)112, g_hInst, NULL);
        ListView_SetExtendedListViewStyle(g_hWndTexApplied, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNA colApplied;
        colApplied.mask = LVCF_TEXT | LVCF_WIDTH;
        colApplied.cx = 210; colApplied.pszText = (LPSTR)"Texture"; ListView_InsertColumn(g_hWndTexApplied, 0, &colApplied);
        colApplied.cx = 180; colApplied.pszText = (LPSTR)"Part";    ListView_InsertColumn(g_hWndTexApplied, 1, &colApplied);

        // OpenGL Viewport Window
        WNDCLASSEXA glwc = { sizeof(WNDCLASSEXA) };
        glwc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        glwc.lpfnWndProc = GLViewProc;
        glwc.hInstance = g_hInst;
        glwc.hCursor = LoadCursor(NULL, IDC_ARROW);
        glwc.lpszClassName = "LyrasTextureMod_GLView";
        RegisterClassExA(&glwc);

        g_hWndGL = CreateWindowExA(WS_EX_CLIENTEDGE, "LyrasTextureMod_GLView", "",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            440, 45, 740, 640, hWnd, (HMENU)109, g_hInst, NULL);

        InitOpenGL(g_hWndGL);

        // Status Bar
        g_hWndStatus = CreateWindowA(STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)110, g_hInst, NULL);

        // Set Timer for Auto-Rotate
        SetTimer(hWnd, 1, 16, NULL);
        return 0;
    }

    case WM_TIMER:
        if (g_AutoRotate) {
            InvalidateRect(g_hWndGL, NULL, FALSE);
        }
        return 0;

    case WM_SIZE:
    {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        SendMessage(g_hWndStatus, WM_SIZE, 0, 0);

        int sidebarW = 420;
        int listH = h - 320;
        if (listH < 120) listH = 120;
        SetWindowPos(g_hWndList, NULL, 12, 45, sidebarW, listH, SWP_NOZORDER);

        // Info card, then applied-textures caption + panel at the bottom-left
        int infoY = 50 + listH;
        int infoH = 88;
        SetWindowPos(g_hWndInfoText, NULL, 12, infoY, sidebarW, infoH, SWP_NOZORDER);
        int capY = infoY + infoH + 6;
        SetWindowPos(g_hWndAppliedCaption, NULL, 12, capY, sidebarW, 18, SWP_NOZORDER);
        int appliedY = capY + 20;
        int appliedH = h - appliedY - 28;
        if (appliedH < 60) appliedH = 60;
        SetWindowPos(g_hWndTexApplied, NULL, 12, appliedY, sidebarW, appliedH, SWP_NOZORDER);

        int glW = w - sidebarW - 35;
        int glH = h - 75;
        SetWindowPos(g_hWndGL, NULL, sidebarW + 22, 45, glW, glH, SWP_NOZORDER);
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == 101 && code == CBN_SELCHANGE) {
            ApplyTextureFilter();
        } else if (id == 102 && code == EN_CHANGE) {
            ApplyTextureFilter();
        } else if (id == 103 && code == CBN_SELCHANGE) {
            int sel = (int)SendMessage(g_hWndModelCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_AvailableModels.size()) {
                std::string relPath = g_AvailableModels[sel].second;
                std::string disp = g_AvailableModels[sel].first;
                size_t colon = disp.find(": ");
                std::string modelName = (colon != std::string::npos) ? disp.substr(colon + 2) : disp;
                SelectModelByName(modelName, relPath);
            }
        } else if (id == 104) {
            g_Wireframe = (IsDlgButtonChecked(hWnd, 104) == BST_CHECKED);
            InvalidateRect(g_hWndGL, NULL, FALSE);
        } else if (id == 105) {
            g_AutoRotate = (IsDlgButtonChecked(hWnd, 105) == BST_CHECKED);
        } else if (id == 106) {
            g_CamRotX = 20.0f;
            g_CamRotY = -35.0f;
            g_CamPanX = 0.0f;
            g_CamPanY = -2.0f;
            if (g_CurrentModel) g_CamDist = g_CurrentModel->boundingRadius * 2.2f;
            InvalidateRect(g_hWndGL, NULL, FALSE);
        }
        return 0;
    }

    case WM_APP + 1: // Deferred selection coming from the applied-textures panel
    {
        int row = (int)wParam;
        if (row < 0 || row >= (int)g_AppliedRows.size()) return 0;
        std::string fn = g_AppliedRows[row].basename;

        // Find the texture in the master list
        int allIdx = -1;
        for (int i = 0; i < (int)g_AllTextures.size(); ++i) {
            if (_stricmp(g_AllTextures[i].filename.c_str(), fn.c_str()) == 0) { allIdx = i; break; }
        }
        if (allIdx < 0) return 0;

        // Find it in the current filter; relax the filter if it is hidden
        int listRow = -1;
        for (int r = 0; r < (int)g_FilteredIndices.size(); ++r) {
            if (g_FilteredIndices[r] == allIdx) { listRow = r; break; }
        }
        if (listRow < 0) {
            SendMessage(g_hWndCategoryCombo, CB_SETCURSEL, 0, 0);
            if (g_hWndSearch) SetWindowTextA(g_hWndSearch, "");
            ApplyTextureFilter();
            for (int r = 0; r < (int)g_FilteredIndices.size(); ++r) {
                if (g_FilteredIndices[r] == allIdx) { listRow = r; break; }
            }
        }
        if (listRow >= 0) {
            ListView_SetItemState(g_hWndList, listRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_hWndList, listRow, FALSE);
        }
        return 0;
    }

    case WM_NOTIFY:
    {
        LPNMHDR nm = (LPNMHDR)lParam;
        if (nm->idFrom == 107) {
            if (nm->code == LVN_ITEMCHANGED) {
                LPNMLISTVIEW nmlv = (LPNMLISTVIEW)lParam;
                if ((nmlv->uChanged & LVIF_STATE) && (nmlv->uNewState & LVIS_SELECTED)) {
                    OnTextureSelected(nmlv->iItem);
                }
            }
        }
        else if (nm->idFrom == 112) {
            if (nm->code == LVN_ITEMCHANGED && !g_SuppressAppliedNotify) {
                LPNMLISTVIEW nmlv = (LPNMLISTVIEW)lParam;
                if ((nmlv->uChanged & LVIF_STATE) && (nmlv->uNewState & LVIS_SELECTED) && nmlv->iItem >= 0) {
                    // Deferred: never mutate lists re-entrantly inside LVN_ITEMCHANGED
                    PostMessage(hWnd, WM_APP + 1, (WPARAM)nmlv->iItem, 0);
                }
            }
        }
        return 0;
    }

    case WM_DESTROY:
        if (g_hGLRC) {
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(g_hGLRC);
        }
        if (g_hGLDC) ReleaseDC(g_hWndGL, g_hGLDC);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------------
// Hidden CLI self-test mode:  ModelTextureViewer.exe --selftest <model.obj>
// Loads an OBJ with the in-app parser and prints submesh/texture/assembly stats.
// ---------------------------------------------------------------------------------

static std::string RunSelfTest(const std::string& objPath)
{
    std::ostringstream out;
    out << "=== ModelTextureViewer parser selftest ===\n";
    out << "OBJ: " << objPath << "\n";
    if (!PathFileExistsA(objPath.c_str())) {
        out << "ERROR: OBJ file not found\n";
        return out.str();
    }

    auto model = LoadWavefrontOBJ(objPath);
    if (!model) {
        out << "ERROR: failed to parse OBJ\n";
        return out.str();
    }

    out << "Assembly JSON: " << (model->assemblyPath.empty() ? "(not found)" : model->assemblyPath) << "\n";
    out << "Submeshes: " << model->submeshes.size() << "\n";
    int si = 0;
    for (const auto& sub : model->submeshes) {
        std::string texBase = sub.textureFilename.empty() ? "(untextured)" : PathFindFileNameA(sub.textureFilename.c_str());
        out << "  [" << si << "] part='" << (sub.partLabel.empty() ? sub.groupName : sub.partLabel)
            << "' mat='" << sub.materialName << "' tex='" << texBase
            << (sub.textureMissing ? "' MISSING" : "'")
            << " verts=" << sub.vertices.size() << " tris=" << sub.triangles.size() << "\n";
        si++;
    }
    out << "Distinct part labels: " << model->partCount << "\n";
    out << "Textures used: " << model->distinctTextureCount
        << " (missing: " << model->missingTextures.size() << ")\n";
    for (const auto& m : model->missingTextures) {
        out << "  missing: " << m << "\n";
    }

    // Reverse-index cross-check: how many indexed textures reference this model
    // and how many resolve to it as the primary model (Complete-first rule).
    LoadTextureToModelIndex();
    int idxRefs = 0, idxPrimary = 0;
    for (const auto& kv : g_TextureToModelIndex) {
        bool refs = false;
        for (const auto& m : kv.second) {
            if (IEquals(m.modelName, model->name)) { refs = true; break; }
        }
        if (!refs) continue;
        idxRefs++;
        TextureMappingEntry picked;
        if (PickPrimaryModel(kv.second, picked) && IEquals(picked.modelName, model->name)) {
            idxPrimary++;
        }
    }
    out << "Reverse index: " << idxRefs << " texture(s) reference '" << model->name
        << "'; " << idxPrimary << " resolve to it as primary\n";

    // Dropdown discovery check: assembled/ entries must carry a category-correct
    // prefix (character completes labeled [Character], not [Vehicle/Bike/Traffic]).
    DiscoverAllModels();
    int vehCount = 0, charCount = 0;
    out << "Dropdown (assembled/):\n";
    for (const auto& am : g_AvailableModels) {
        if (am.second.rfind("assembled/", 0) == 0) {
            out << "  " << am.first << "\n";
            if (am.first.rfind("[Vehicle/Bike/Traffic]:", 0) == 0) vehCount++;
            else if (am.first.rfind("[Character]:", 0) == 0) charCount++;
        }
    }
    out << "Dropdown totals: " << g_AvailableModels.size() << " entries; assembled: "
        << vehCount << " [Vehicle/Bike/Traffic] + " << charCount << " [Character]\n";

    // Report the first raw vt line to verify the raw-UV convention (v=0 = texture top)
    std::ifstream f(objPath);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("vt ", 0) == 0) { out << "First raw vt: " << line << "\n"; break; }
    }

    out << "Totals: verts=" << model->totalVertices << " tris=" << model->totalTriangles << "\n";
    out << "Selftest OK\n";
    return out.str();
}

static void EmitSelfTestOutput(const std::string& text)
{
    // When launched with redirected stdout (pipe/file), write straight to the
    // inherited handle; otherwise allocate a console window and wait for Enter.
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut && hOut != INVALID_HANDLE_VALUE && GetFileType(hOut) != FILE_TYPE_UNKNOWN) {
        DWORD written = 0;
        WriteFile(hOut, text.c_str(), (DWORD)text.size(), &written, NULL);
        FlushFileBuffers(hOut);
        return;
    }
    if (AllocConsole()) {
        FILE* fpOut = nullptr;
        FILE* fpIn = nullptr;
        freopen_s(&fpOut, "CONOUT$", "w", stdout);
        fwrite(text.c_str(), 1, text.size(), stdout);
        fflush(stdout);
        freopen_s(&fpIn, "CONIN$", "r", stdin);
        printf("\nPress Enter to close...");
        fflush(stdout);
        if (fpIn) fgetc(fpIn);
    }
}

static void InitPaths()
{
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string runDir = exePath;
    runDir = runDir.substr(0, runDir.find_last_of("\\/"));

    // Check if running from mods/LyrasTextureMod/ or TextureMod_SRC/
    if (PathFileExistsA((runDir + "\\textures").c_str())) {
        g_ModDir = runDir;
    } else if (PathFileExistsA((runDir + "\\mods\\LyrasTextureMod\\textures").c_str())) {
        g_ModDir = runDir + "\\mods\\LyrasTextureMod";
    } else if (PathFileExistsA((runDir + "\\..\\mods\\LyrasTextureMod\\textures").c_str())) {
        g_ModDir = runDir + "\\..\\mods\\LyrasTextureMod";
    } else {
        g_ModDir = runDir;
    }

    g_TexturesDir = g_ModDir + "\\textures";
    g_ModelsDir = g_ModDir + "\\3D";
}

// ---------------------------------------------------------------------------------
// WinMain Application Entry Point
// ---------------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int nCmdShow)
{
    g_hInst = hInstance;

    // Hidden CLI mode: --selftest <model.obj> (runs headless, exit code 0)
    {
        std::string cmd = lpCmdLine ? lpCmdLine : "";
        size_t st = cmd.find("--selftest");
        if (st != std::string::npos) {
            std::string path = cmd.substr(st + 10);
            size_t b = path.find_first_not_of(" \t\"'");
            size_t e = path.find_last_not_of(" \t\"'");
            path = (b == std::string::npos) ? "" : path.substr(b, e - b + 1);
            char full[MAX_PATH];
            if (!path.empty() && GetFullPathNameA(path.c_str(), MAX_PATH, full, NULL)) {
                path = full;
            }
            InitPaths();
            std::string report = RunSelfTest(path);
            EmitSelfTestOutput(report);
            return 0;
        }
    }

    // Initialize GDI+
    Gdiplus::GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, NULL);

    InitCommonControls();

    // Determine Paths
    InitPaths();

    // Register Window Class
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "LyrasTextureMod_ViewerClass";
    RegisterClassExA(&wc);

    g_hWndMain = CreateWindowExA(
        0,
        "LyrasTextureMod_ViewerClass",
        "Lyra's Texture Mod - 3D Model & HD Texture Preview Tool",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1220, 780,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hWndMain) return 1;

    ShowWindow(g_hWndMain, nCmdShow);
    UpdateWindow(g_hWndMain);

    // Initial Discovery & Index Loading
    LoadTextureToModelIndex();
    DiscoverAllModels();
    PopulateModelCombo();

    // Scan & Filter Textures
    ScanModTextures();
    ApplyTextureFilter();
    SelectModelByName("Axel_Taxi_Complete", "assembled/Axel_Taxi_Complete.obj");

    // Select first texture if available
    if (!g_FilteredIndices.empty()) {
        ListView_SetItemState(g_hWndList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        OnTextureSelected(0);
    }

    // Message Loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Gdiplus::GdiplusShutdown(gdiToken);
    return (int)msg.wParam;
}
