#pragma once

#include "Core.h"
#include "Mesh.h"
#include "GEMLoader.h"
#include "HybridGrassField.h"  // Include for GrassGroupConfig and GrassTypeConfig
#include <map>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>

// Asset configuration structures
struct ModelAsset
{
    std::string name;
    std::string path;
    Mesh* mesh = nullptr;
};

struct TextureAsset
{
    std::string name;
    std::string path;
    Texture texture;
};

struct GrassGroupAsset
{
    std::string groupName;
    float groupWeight;
    std::vector<std::string> typeNames;
    std::vector<std::string> modelPaths;
    std::vector<std::string> texturePaths;
    std::vector<float> typeWeights;
};

struct RockSetAsset
{
    std::vector<std::string> names;
    std::vector<std::string> modelPaths;      // Single model path
    std::vector<std::string> texturePaths;
};


class AssetManager
{
public:
    // Initialize and load all assets from config files
    bool loadFromConfig(Core* core, const std::string& configFilePath)
    {
        std::cout << "[AssetManager] Loading assets from: " << configFilePath << "\n";

        std::ifstream file(configFilePath);
        if (!file.is_open())
        {
            std::cout << "[AssetManager] ERROR: Could not open config file!\n";
            return false;
        }

        std::string line;
        std::string currentSection = "";

        while (std::getline(file, line))
        {
            // Remove comments and trim whitespace
            size_t commentPos = line.find("//");
            if (commentPos != std::string::npos)
                line = line.substr(0, commentPos);

            line = trim(line);
            if (line.empty()) continue;

            // Check for section headers
            if (line[0] == '[' && line[line.length() - 1] == ']')
            {
                currentSection = line.substr(1, line.length() - 2);
                continue;
            }

            // Parse based on current section
            if (currentSection == "MODELS")
                parseModel(line);
            else if (currentSection == "TEXTURES")
                parseTexture(line);
            else if (currentSection == "GRASS_GROUPS")
                parseGrassGroup(file, line);
            else if (currentSection == "ROCKS")
                parseRockSet(file, line);
        }

        file.close();

        // Load all models
        std::cout << "[AssetManager] Loading " << modelConfigs.size() << " models...\n";
        for (auto& config : modelConfigs)
        {
            config.mesh = loadMesh(core, config.path);
            if (config.mesh)
                loadedModels[config.name] = config.mesh;
        }

        // Load all textures
        std::cout << "[AssetManager] Loading " << textureConfigs.size() << " textures...\n";
        for (auto& config : textureConfigs)
        {
            config.texture = core->loadTexture(config.path);
            loadedTextures[config.name] = config.texture;
        }

        std::cout << "[AssetManager] Assets loaded successfully!\n";
        std::cout << "  Models: " << loadedModels.size() << "\n";
        std::cout << "  Textures: " << loadedTextures.size() << "\n";
        std::cout << "  Grass Groups: " << grassGroups.size() << "\n";
        std::cout << "  Rock Sets: " << rockSets.size() << "\n";

        return true;
    }

    // Get loaded assets
    Mesh* getModel(const std::string& name)
    {
        auto it = loadedModels.find(name);
        return (it != loadedModels.end()) ? it->second : nullptr;
    }

    Texture getTexture(const std::string& name)
    {
        auto it = loadedTextures.find(name);
        return (it != loadedTextures.end()) ? it->second : Texture();
    }

    std::vector<GrassGroupAsset>& getGrassGroups() { return grassGroups; }
    std::vector<RockSetAsset>& getRockSets() { return rockSets; }

    // Helper: Convert grass groups to GrassGroupConfig format
    std::vector<GrassGroupConfig> getGrassGroupConfigs()
    {
        std::vector<GrassGroupConfig> configs;

        for (auto& group : grassGroups)
        {
            GrassGroupConfig config;
            config.groupName = group.groupName;
            config.groupWeight = group.groupWeight;

            for (size_t i = 0; i < group.typeNames.size(); i++)
            {
                GrassTypeConfig typeConfig;
                typeConfig.name = group.typeNames[i];
                typeConfig.modelPath = group.modelPaths[i];
                typeConfig.texturePath = group.texturePaths[i];
                typeConfig.weight = group.typeWeights[i];

                config.types.push_back(typeConfig);
            }

            configs.push_back(config);
        }

        return configs;
    }

    ~AssetManager()
    {
        // Clean up loaded models
        for (auto& pair : loadedModels)
        {
            if (pair.second)
                delete pair.second;
        }
    }

private:
    std::vector<ModelAsset> modelConfigs;
    std::vector<TextureAsset> textureConfigs;
    std::vector<GrassGroupAsset> grassGroups;
    std::vector<RockSetAsset> rockSets;

    std::map<std::string, Mesh*> loadedModels;
    std::map<std::string, Texture> loadedTextures;

    std::string trim(const std::string& str)
    {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, last - first + 1);
    }

    std::vector<std::string> split(const std::string& str, char delimiter)
    {
        std::vector<std::string> tokens;
        std::stringstream ss(str);
        std::string token;
        while (std::getline(ss, token, delimiter))
        {
            tokens.push_back(trim(token));
        }
        return tokens;
    }

    void parseModel(const std::string& line)
    {
        // Format: ModelName = path/to/model.gem
        auto parts = split(line, '=');
        if (parts.size() == 2)
        {
            ModelAsset asset;
            asset.name = parts[0];
            asset.path = parts[1];
            modelConfigs.push_back(asset);
        }
    }

    void parseTexture(const std::string& line)
    {
        // Format: TextureName = path/to/texture.png
        auto parts = split(line, '=');
        if (parts.size() == 2)
        {
            TextureAsset asset;
            asset.name = parts[0];
            asset.path = parts[1];
            textureConfigs.push_back(asset);
        }
    }

    void parseGrassGroup(std::ifstream& file, const std::string& firstLine)
    {
        // Format:
        // GroupName, GroupWeight
        // TypeName1, ModelPath1, TexturePath1, TypeWeight1
        // TypeName2, ModelPath2, TexturePath2, TypeWeight2
        // ...
        // END

        auto parts = split(firstLine, ',');
        if (parts.size() < 2) return;

        GrassGroupAsset group;
        group.groupName = parts[0];
        group.groupWeight = std::stof(parts[1]);

        std::string line;
        while (std::getline(file, line))
        {
            line = trim(line);
            if (line.empty()) continue;
            if (line == "END") break;

            auto typeParts = split(line, ',');
            if (typeParts.size() >= 4)
            {
                group.typeNames.push_back(typeParts[0]);
                group.modelPaths.push_back(typeParts[1]);
                group.texturePaths.push_back(typeParts[2]);
                group.typeWeights.push_back(std::stof(typeParts[3]));
            }
        }

        grassGroups.push_back(group);
    }

    void parseRockSet(std::ifstream& file, const std::string& firstLine)
    {
        // NEW FORMAT: RockName, Model, Texture
        // (Simpler - LOD generated at runtime)

        RockSetAsset rockSet;

        std::string line = firstLine;
        do
        {
            line = trim(line);
            if (line.empty()) continue;
            if (line == "END") break;

            auto parts = split(line, ',');
            if (parts.size() >= 3)
            {
                rockSet.names.push_back(parts[0]);
                rockSet.modelPaths.push_back(parts[1]);
                rockSet.texturePaths.push_back(parts[2]);
            }
        } while (std::getline(file, line));

        if (!rockSet.names.empty())
        {
            rockSets.push_back(rockSet);
        }
    }

    Mesh* loadMesh(Core* core, const std::string& path)
    {
        GEMLoader::GEMModelLoader loader;
        std::vector<GEMLoader::GEMMesh> gemmeshes;
        loader.load(path, gemmeshes);

        if (gemmeshes.empty())
        {
            std::cout << "[AssetManager] WARNING: Failed to load " << path << "\n";
            return nullptr;
        }

        Mesh* mesh = new Mesh();
        std::vector<STATIC_VERTEX> vertices;
        for (auto& v : gemmeshes[0].verticesStatic)
        {
            STATIC_VERTEX vert;
            memcpy(&vert, &v, sizeof(STATIC_VERTEX));
            vertices.push_back(vert);
        }
        mesh->init(core, vertices, gemmeshes[0].indices);
        return mesh;
    }
};