#pragma once

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "GEMLoader.h"
#include "HeightmapTerrain.h"
#include "Maths.h"
#include <vector>
#include <random>
#include <string>

// ============================================================================
// CONFIGURATION STRUCTURES
// ============================================================================

// Grass type configuration
struct GrassTypeConfig
{
    std::string modelPath;
    std::string texturePath;
    float weight;           // Probability weight (0.0 - 1.0)
    std::string name;
};

// Grass group configuration
struct GrassGroupConfig
{
    std::string groupName;
    float groupWeight;      // Probability of this group appearing (0.0 - 1.0)
    std::vector<GrassTypeConfig> types;
};

// ============================================================================
// RUNTIME DATA STRUCTURES
// ============================================================================

// Runtime grass type data
struct GrassType
{
    Mesh* mesh = nullptr;
    Texture texture;
    std::string name;
    int groupIndex = 0;
    int typeIndex = 0;
};

// GPU Instance data - MUST match shader input layout!
struct GrassInstanceGPU
{
    Vec3 position;      // 12 bytes
    float rotationY;    // 4 bytes
    float scale;        // 4 bytes
    float windPhase;    // 4 bytes
    // Total: 24 bytes
};

// CPU Instance data - Contains extra fields for organization
struct GrassInstance
{
    Vec3 position;
    float rotationY;
    float scale;
    float windPhase;
    int groupIndex;
    int typeIndex;
};

// Grass group (short, tall, flowers)
struct GrassGroup
{
    std::string name;
    std::vector<GrassType> types;
    std::vector<std::vector<GrassInstance>> instancesByType;
    std::vector<std::vector<GrassInstance>> visibleInstancesByType;
    std::vector<ID3D12Resource*> instanceBuffers;
    std::vector<D3D12_VERTEX_BUFFER_VIEW> instanceBufferViews;
};

// ============================================================================
// HYBRID GRASS FIELD CLASS
// ============================================================================

class HybridGrassField
{
public:
    std::string shaderName = "GrassInstanced";
    std::string psoName = "GrassInstancedPSO";

    // Terrain size (configurable)
    float terrainSizeX = 300.0f;
    float terrainSizeZ = 300.0f;

    // ========================================================================
    // INIT METHOD 1: Original - Random generation (density-based)
    // ========================================================================
    void init(Core* core, PSOManager* psos, Shaders* shaders,
        HeightmapTerrain* terrain,
        const std::vector<GrassGroupConfig>& groupConfigs,
        float density = 3.0f,
        float minDistance = 0.5f,
        float viewDistance = 50.0f,
        float chunkSize = 16.0f)
    {
        this->terrain = terrain;
        this->viewDistance = viewDistance;
        this->density = density;
        this->chunkSize = chunkSize;

        // 1. Load all groups and their types
        loadGrassGroups(core, groupConfigs);

        if (groups.empty())
        {
            std::cout << "[HybridGrassField] Error: No grass groups loaded!\n";
            return;
        }

        // 2. Normalize weights
        normalizeWeights(groupConfigs);

        // 3. Generate grass instances with weighted random selection
        generateWeightedGrassChunks(minDistance);

        // 4. Finish initialization
        finishInit(core, psos, shaders);
    }

    // ========================================================================
    // INIT METHOD 2: NEW - Use pre-generated instances from VegetationGenerator
    // ========================================================================
    void initWithInstances(Core* core, PSOManager* psos, Shaders* shaders,
        HeightmapTerrain* terrain,
        const std::vector<GrassGroupConfig>& groupConfigs,
        const std::vector<GrassInstance>& preGeneratedInstances,
        float viewDistance = 50.0f,
        float chunkSize = 16.0f)
    {
        this->terrain = terrain;
        this->viewDistance = viewDistance;
        this->chunkSize = chunkSize;

        std::cout << "[HybridGrassField] Initializing with " << preGeneratedInstances.size()
            << " pre-generated instances\n";

        // 1. Load all groups and their types
        loadGrassGroups(core, groupConfigs);

        if (groups.empty())
        {
            std::cout << "[HybridGrassField] Error: No grass groups loaded!\n";
            return;
        }

        // 2. Normalize weights (needed for type distribution)
        normalizeWeights(groupConfigs);

        // 3. USE PRE-GENERATED INSTANCES
        allInstances = preGeneratedInstances;

        // Validate and fix group/type indices
        for (auto& inst : allInstances)
        {
            // Clamp groupIndex to valid range
            if (inst.groupIndex < 0 || inst.groupIndex >= (int)groups.size())
            {
                inst.groupIndex = inst.groupIndex % (int)groups.size();
                if (inst.groupIndex < 0) inst.groupIndex = 0;
            }

            // Clamp typeIndex to valid range for this group
            int maxType = (int)groups[inst.groupIndex].types.size();
            if (maxType > 0 && (inst.typeIndex < 0 || inst.typeIndex >= maxType))
            {
                inst.typeIndex = inst.typeIndex % maxType;
                if (inst.typeIndex < 0) inst.typeIndex = 0;
            }
        }

        // 4. Organize into chunks
        organizeIntoChunks();

        // 5. Finish initialization
        finishInit(core, psos, shaders);
    }

    // ========================================================================
    // UPDATE - Call each frame
    // ========================================================================
    void update(float deltaTime)
    {
        windTime += deltaTime;
    }

    // ========================================================================
    // DRAW - Call each frame after update
    // ========================================================================
    void draw(Core* core, PSOManager* psos, Shaders* shaders,
        const Matrix& vp, const Vec3& cameraPos)
    {
        if (groups.empty()) return;

        // Perform chunk culling
        performChunkCulling(cameraPos);

        // Update shader constants (same for all)
        Matrix world;
        shaders->updateConstantVS(shaderName, "grassBuffer", "VP", (void*)&vp);
        shaders->updateConstantVS(shaderName, "grassBuffer", "W", (void*)&world);

        Vec4 windData(windDirection.x, windDirection.y, windStrength, windTime);
        shaders->updateConstantVS(shaderName, "grassBuffer", "windParams", &windData);

        Vec4 cameraData(cameraPos.x, cameraPos.y, cameraPos.z, viewDistance);
        shaders->updateConstantVS(shaderName, "grassBuffer", "cameraPos", &cameraData);

        Vec4 lightDir(0.5f, 1.0f, -0.5f, 0.3f);
        shaders->updateConstantPS(shaderName, "grassPSBuffer", "lightDir_ambient", &lightDir);

        // Use public color parameters
        shaders->updateConstantPS(shaderName, "grassPSBuffer", "grassColorTop", &colorTop);
        shaders->updateConstantPS(shaderName, "grassPSBuffer", "grassColorBottom", &colorBottom);

        shaders->apply(core, shaderName);
        psos->bind(core, psoName);

        // Draw each group
        for (auto& group : groups)
        {
            drawGroup(core, group);
        }
    }

    // ========================================================================
    // PUBLIC PROPERTIES
    // ========================================================================
    Vec2 windDirection = Vec2(1.0f, 0.5f);
    float windStrength = 1.5f;
    float windSpeed = 1.0f;

    // Color parameters (edit these to change grass appearance!)
    Vec4 colorTop = Vec4(0.6f, 0.9f, 0.5f, 1.0f);       // Bright lime green tip
    Vec4 colorBottom = Vec4(0.3f, 0.5f, 0.2f, 1.0f);    // Medium green base

    // Get instance count
    size_t getInstanceCount() const { return allInstances.size(); }

    // ========================================================================
    // DESTRUCTOR
    // ========================================================================
    ~HybridGrassField()
    {
        for (auto& group : groups)
        {
            for (auto& type : group.types)
            {
                if (type.mesh) delete type.mesh;
            }
            for (auto& buffer : group.instanceBuffers)
            {
                if (buffer) buffer->Release();
            }
        }
    }

private:
    HeightmapTerrain* terrain = nullptr;
    std::vector<GrassGroup> groups;

    struct GrassChunk
    {
        Vec3 centerPos;
        std::vector<GrassInstance> instances;
        bool isVisible = false;
    };

    std::vector<GrassChunk> chunks;
    std::vector<GrassInstance> allInstances;

    // Normalized weights for selection
    std::vector<float> normalizedGroupWeights;
    std::vector<std::vector<float>> normalizedTypeWeights;

    float density = 3.0f;
    float viewDistance = 50.0f;
    float chunkSize = 16.0f;
    float windTime = 0.0f;

    // ========================================================================
    // COMMON INITIALIZATION (shared between both init methods)
    // ========================================================================
    void finishInit(Core* core, PSOManager* psos, Shaders* shaders)
    {
        // Separate instances by group and type
        separateInstancesByGroupAndType();

        // Create instance buffers for each type in each group
        createInstanceBuffers(core);

        // Load shaders (shared)
        shaders->load(core, shaderName, "Shaders/VSGrass.txt", "Shaders/PSGrass.txt");

        // Create PSO (shared)
        psos->createPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getGrassInstancedLayout());

        printStatistics();
    }

    // ========================================================================
    // ORGANIZE INSTANCES INTO SPATIAL CHUNKS
    // ========================================================================
    void organizeIntoChunks()
    {
        chunks.clear();

        float halfX = terrainSizeX * 0.5f;
        float halfZ = terrainSizeZ * 0.5f;

        int numChunksX = (int)std::ceil(terrainSizeX / chunkSize);
        int numChunksZ = (int)std::ceil(terrainSizeZ / chunkSize);

        // Create empty chunks
        chunks.resize(numChunksX * numChunksZ);

        for (int cz = 0; cz < numChunksZ; cz++)
        {
            for (int cx = 0; cx < numChunksX; cx++)
            {
                int idx = cz * numChunksX + cx;
                float chunkMinX = cx * chunkSize - halfX;
                float chunkMinZ = cz * chunkSize - halfZ;
                chunks[idx].centerPos = Vec3(
                    chunkMinX + chunkSize * 0.5f,
                    0.0f,
                    chunkMinZ + chunkSize * 0.5f
                );
                chunks[idx].isVisible = false;
            }
        }

        // Assign instances to chunks based on position
        for (const auto& inst : allInstances)
        {
            int cx = (int)((inst.position.x + halfX) / chunkSize);
            int cz = (int)((inst.position.z + halfZ) / chunkSize);

            cx = std::clamp(cx, 0, numChunksX - 1);
            cz = std::clamp(cz, 0, numChunksZ - 1);

            int chunkIdx = cz * numChunksX + cx;
            chunks[chunkIdx].instances.push_back(inst);
        }

        std::cout << "[HybridGrassField] Organized " << allInstances.size()
            << " instances into " << chunks.size() << " chunks\n";
    }

    // ========================================================================
    // LOAD GRASS MODELS AND TEXTURES
    // ========================================================================
    void loadGrassGroups(Core* core, const std::vector<GrassGroupConfig>& configs)
    {
        for (size_t g = 0; g < configs.size(); g++)
        {
            const auto& groupConfig = configs[g];

            GrassGroup group;
            group.name = groupConfig.groupName;

            for (size_t t = 0; t < groupConfig.types.size(); t++)
            {
                const auto& typeConfig = groupConfig.types[t];

                GrassType type;
                type.name = typeConfig.name;
                type.groupIndex = (int)g;
                type.typeIndex = (int)t;

                // Load mesh
                type.mesh = loadGrassModel(core, typeConfig.modelPath);
                if (!type.mesh)
                {
                    std::cout << "[HybridGrassField] Failed to load: "
                        << typeConfig.modelPath << "\n";
                    continue;
                }

                // Load texture
                type.texture = core->loadTexture(typeConfig.texturePath);

                group.types.push_back(type);
            }

            if (!group.types.empty())
            {
                groups.push_back(group);
            }
        }
    }

    Mesh* loadGrassModel(Core* core, const std::string& path)
    {
        GEMLoader::GEMModelLoader loader;
        std::vector<GEMLoader::GEMMesh> gemmeshes;
        loader.load(path, gemmeshes);

        if (gemmeshes.empty()) return nullptr;

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

    // ========================================================================
    // WEIGHT NORMALIZATION
    // ========================================================================
    void normalizeWeights(const std::vector<GrassGroupConfig>& configs)
    {
        // Normalize group weights
        float totalGroupWeight = 0.0f;
        for (const auto& config : configs)
        {
            totalGroupWeight += config.groupWeight;
        }

        normalizedGroupWeights.clear();
        for (const auto& config : configs)
        {
            float normalized = (totalGroupWeight > 0.0f) ? config.groupWeight / totalGroupWeight : 0.0f;
            normalizedGroupWeights.push_back(normalized);
        }

        // Normalize type weights within each group
        normalizedTypeWeights.clear();
        for (const auto& config : configs)
        {
            float totalTypeWeight = 0.0f;
            for (const auto& type : config.types)
            {
                totalTypeWeight += type.weight;
            }

            std::vector<float> typeWeights;
            for (const auto& type : config.types)
            {
                float normalized = (totalTypeWeight > 0.0f) ? type.weight / totalTypeWeight : 0.0f;
                typeWeights.push_back(normalized);
            }
            normalizedTypeWeights.push_back(typeWeights);
        }
    }

    // ========================================================================
    // RANDOM GENERATION (Original method)
    // ========================================================================
    void generateWeightedGrassChunks(float minSpacing)
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> randScale(0.8f, 1.2f);
        std::uniform_real_distribution<float> randRot(0.0f, 6.28318f);
        std::uniform_real_distribution<float> randPhase(0.0f, 6.28318f);
        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
        std::uniform_real_distribution<float> randOffset(-minSpacing * 0.3f, minSpacing * 0.3f);

        float halfX = terrainSizeX * 0.5f;
        float halfZ = terrainSizeZ * 0.5f;

        int numChunksX = (int)(terrainSizeX / chunkSize);
        int numChunksZ = (int)(terrainSizeZ / chunkSize);

        float spacing = 1.0f / std::sqrt(density);

        for (int cz = 0; cz < numChunksZ; cz++)
        {
            for (int cx = 0; cx < numChunksX; cx++)
            {
                GrassChunk chunk;
                float chunkMinX = cx * chunkSize - halfX;
                float chunkMinZ = cz * chunkSize - halfZ;
                chunk.centerPos = Vec3(chunkMinX + chunkSize * 0.5f, 0.0f, chunkMinZ + chunkSize * 0.5f);

                int gridCount = (int)(chunkSize / spacing);

                for (int z = 0; z < gridCount; z++)
                {
                    for (int x = 0; x < gridCount; x++)
                    {
                        float worldX = chunkMinX + (x * spacing) + randOffset(gen);
                        float worldZ = chunkMinZ + (z * spacing) + randOffset(gen);
                        float worldY = terrain->sampleHeightWorld(worldX, worldZ);

                        // Weighted random selection of group
                        int selectedGroup = selectWeightedGroup(rand01(gen));

                        // Weighted random selection of type within group
                        int selectedType = selectWeightedType(selectedGroup, rand01(gen));

                        GrassInstance inst;
                        inst.position = Vec3(worldX, worldY, worldZ);
                        inst.rotationY = randRot(gen);
                        inst.scale = randScale(gen);
                        inst.windPhase = randPhase(gen);
                        inst.groupIndex = selectedGroup;
                        inst.typeIndex = selectedType;

                        chunk.instances.push_back(inst);
                        allInstances.push_back(inst);
                    }
                }

                chunks.push_back(chunk);
            }
        }
    }

    int selectWeightedGroup(float randomValue)
    {
        float cumulative = 0.0f;
        for (size_t i = 0; i < normalizedGroupWeights.size(); i++)
        {
            cumulative += normalizedGroupWeights[i];
            if (randomValue <= cumulative)
            {
                return (int)i;
            }
        }
        return (int)normalizedGroupWeights.size() - 1;
    }

    int selectWeightedType(int groupIndex, float randomValue)
    {
        if (groupIndex < 0 || groupIndex >= (int)normalizedTypeWeights.size())
            return 0;

        const auto& weights = normalizedTypeWeights[groupIndex];
        float cumulative = 0.0f;
        for (size_t i = 0; i < weights.size(); i++)
        {
            cumulative += weights[i];
            if (randomValue <= cumulative)
            {
                return (int)i;
            }
        }
        return (int)weights.size() - 1;
    }

    // ========================================================================
    // SEPARATE INSTANCES BY GROUP AND TYPE
    // ========================================================================
    void separateInstancesByGroupAndType()
    {
        // Initialize storage
        for (auto& group : groups)
        {
            group.instancesByType.resize(group.types.size());
            group.visibleInstancesByType.resize(group.types.size());
        }

        // Distribute instances
        for (const auto& inst : allInstances)
        {
            if (inst.groupIndex >= 0 && inst.groupIndex < (int)groups.size())
            {
                auto& group = groups[inst.groupIndex];
                if (inst.typeIndex >= 0 && inst.typeIndex < (int)group.instancesByType.size())
                {
                    group.instancesByType[inst.typeIndex].push_back(inst);
                }
            }
        }
    }

    // ========================================================================
    // CREATE GPU BUFFERS
    // ========================================================================
    void createInstanceBuffers(Core* core)
    {
        for (auto& group : groups)
        {
            group.instanceBuffers.resize(group.types.size(), nullptr);
            group.instanceBufferViews.resize(group.types.size());

            // Count total instances for this group (for buffer sizing)
            size_t totalGroupInstances = 0;
            for (size_t t = 0; t < group.types.size(); t++)
            {
                totalGroupInstances += group.instancesByType[t].size();
            }

            for (size_t t = 0; t < group.types.size(); t++)
            {
                // Use total group instances for buffer size (instances can be redistributed)
                size_t maxInstances = totalGroupInstances;
                if (maxInstances == 0) maxInstances = 1000; // Default buffer size

                // Use GPU struct size
                UINT bufferSize = (UINT)(maxInstances * sizeof(GrassInstanceGPU));

                D3D12_HEAP_PROPERTIES heapProps = {};
                heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

                D3D12_RESOURCE_DESC bufferDesc = {};
                bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bufferDesc.Width = bufferSize;
                bufferDesc.Height = 1;
                bufferDesc.DepthOrArraySize = 1;
                bufferDesc.MipLevels = 1;
                bufferDesc.SampleDesc.Count = 1;
                bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                HRESULT hr = core->device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&group.instanceBuffers[t])
                );

                if (FAILED(hr) || group.instanceBuffers[t] == nullptr)
                {
                    std::cout << "[HybridGrassField] ERROR: Failed to create buffer for group "
                        << group.name << " type " << t << "\n";
                    continue;
                }

                group.instanceBufferViews[t].BufferLocation =
                    group.instanceBuffers[t]->GetGPUVirtualAddress();
                group.instanceBufferViews[t].StrideInBytes = sizeof(GrassInstanceGPU);
                group.instanceBufferViews[t].SizeInBytes = bufferSize;
            }
        }
    }

    // ========================================================================
    // CHUNK CULLING AND BUFFER UPDATE
    // ========================================================================
    void performChunkCulling(const Vec3& cameraPos)
    {
        // Clear all visible lists
        for (auto& group : groups)
        {
            for (auto& list : group.visibleInstancesByType)
            {
                list.clear();
            }
        }

        float maxDist = viewDistance + chunkSize * 0.5f;
        float maxDistSq = maxDist * maxDist;

        // Process chunks
        for (auto& chunk : chunks)
        {
            float dx = chunk.centerPos.x - cameraPos.x;
            float dz = chunk.centerPos.z - cameraPos.z;
            float distSq = dx * dx + dz * dz;

            chunk.isVisible = (distSq <= maxDistSq);

            if (chunk.isVisible)
            {
                for (const auto& inst : chunk.instances)
                {
                    if (inst.groupIndex >= 0 && inst.groupIndex < (int)groups.size())
                    {
                        auto& group = groups[inst.groupIndex];
                        if (inst.typeIndex >= 0 && inst.typeIndex < (int)group.visibleInstancesByType.size())
                        {
                            group.visibleInstancesByType[inst.typeIndex].push_back(inst);
                        }
                    }
                }
            }
        }

        // Update GPU buffers (converting to GPU format)
        for (auto& group : groups)
        {
            for (size_t t = 0; t < group.types.size(); t++)
            {
                if (group.visibleInstancesByType[t].empty()) continue;
                if (group.instanceBuffers[t] == nullptr) continue;

                // Convert to GPU format
                std::vector<GrassInstanceGPU> gpuInstances;
                gpuInstances.reserve(group.visibleInstancesByType[t].size());

                for (const auto& inst : group.visibleInstancesByType[t])
                {
                    GrassInstanceGPU gpuInst;
                    gpuInst.position = inst.position;
                    gpuInst.rotationY = inst.rotationY;
                    gpuInst.scale = inst.scale;
                    gpuInst.windPhase = inst.windPhase;
                    gpuInstances.push_back(gpuInst);
                }

                void* mappedData = nullptr;
                D3D12_RANGE readRange = { 0, 0 };
                HRESULT hr = group.instanceBuffers[t]->Map(0, &readRange, &mappedData);

                if (SUCCEEDED(hr) && mappedData)
                {
                    size_t copySize = gpuInstances.size() * sizeof(GrassInstanceGPU);
                    memcpy(mappedData, gpuInstances.data(), copySize);
                    group.instanceBuffers[t]->Unmap(0, nullptr);
                }
            }
        }
    }

    // ========================================================================
    // DRAW A GRASS GROUP
    // ========================================================================
    void drawGroup(Core* core, GrassGroup& group)
    {
        for (size_t t = 0; t < group.types.size(); t++)
        {
            int visibleCount = (int)group.visibleInstancesByType[t].size();
            if (visibleCount == 0) continue;

            // Null checks
            if (group.instanceBuffers[t] == nullptr) continue;

            auto& type = group.types[t];
            if (type.mesh == nullptr) continue;

            // Bind texture
            core->getCommandList()->SetGraphicsRootDescriptorTable(2, type.texture.srvHandle);

            // Set vertex buffers
            D3D12_VERTEX_BUFFER_VIEW views[2];
            views[0] = type.mesh->getVertexBufferView();
            views[1] = group.instanceBufferViews[t];
            core->getCommandList()->IASetVertexBuffers(0, 2, views);

            D3D12_INDEX_BUFFER_VIEW ibView = type.mesh->getIndexBufferView();
            core->getCommandList()->IASetIndexBuffer(&ibView);

            // Draw instanced
            core->getCommandList()->DrawIndexedInstanced(
                type.mesh->getIndexCount(),
                visibleCount,
                0, 0, 0
            );
        }
    }

    // ========================================================================
    // STATISTICS
    // ========================================================================
    void printStatistics()
    {
        std::cout << "\n[HybridGrassField] Statistics:\n";
        std::cout << "================================\n";
        std::cout << "Total Instances: " << allInstances.size() << "\n";
        std::cout << "Chunks: " << chunks.size() << "\n\n";

        int totalDrawCalls = 0;
        for (size_t g = 0; g < groups.size(); g++)
        {
            auto& group = groups[g];
            int groupTotal = 0;

            std::cout << "Group: " << group.name << "\n";
            for (size_t t = 0; t < group.types.size(); t++)
            {
                int count = (int)group.instancesByType[t].size();
                groupTotal += count;
                totalDrawCalls++;

                float percentage = allInstances.empty() ? 0.0f :
                    (float)count / (float)allInstances.size() * 100.0f;
                std::cout << "  - " << group.types[t].name << ": "
                    << count << " (" << percentage << "%)\n";
            }

            float groupPercentage = allInstances.empty() ? 0.0f :
                (float)groupTotal / (float)allInstances.size() * 100.0f;
            std::cout << "  Total: " << groupTotal << " (" << groupPercentage << "%)\n\n";
        }

        std::cout << "Total Draw Calls: " << totalDrawCalls << "\n";
        std::cout << "================================\n\n";
    }
};