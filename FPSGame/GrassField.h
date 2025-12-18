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

// Instance data for each grass blade (minimal memory footprint)
struct GrassInstance
{
    Vec3 position;        // World position
    float rotationY;      // Random rotation around Y axis
    float scale;          // Slight scale variation (0.8 - 1.2)
    float windPhase;      // Random phase offset for wind
    Vec2 padding;         // Align to 32 bytes for GPU efficiency
};

// Grass chunk for spatial partitioning
struct GrassChunk
{
    Vec3 centerPos;
    std::vector<GrassInstance> instances;
    bool isVisible = false;
};

class GrassField
{
public:
    std::string shaderName = "GrassInstanced";
    std::string psoName = "GrassInstancedPSO";

    void init(Core* core, PSOManager* psos, Shaders* shaders,
        HeightmapTerrain* terrain,
        const std::string& grassModelPath,
        const std::string& grassTexturePath,
        float density = 2.5f,          // Density: blades per square meter
        float minDistance = 0.6f,      // Min spacing between blades
        float viewDistance = 50.0f,    // Max render distance
        float chunkSize = 16.0f)       // Chunk size for spatial partitioning
    {
        this->terrain = terrain;
        this->viewDistance = viewDistance;
        this->density = density;
        this->chunkSize = chunkSize;

        // 1. Load grass model
        loadGrassModel(core, grassModelPath);

        // 2. Load grass texture
        grassTexture = core->loadTexture(grassTexturePath);

        // 3. Generate grass instances in chunks
        generateGrassChunks(minDistance);

        // 4. Create instance buffer
        createInstanceBuffer(core);

        // 5. Load shaders
        shaders->load(core, shaderName, "Shaders/VSGrass.txt", "Shaders/PSGrass.txt");

        // 6. Create PSO with GRASS INSTANCED layout
        psos->createPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getGrassInstancedLayout());

        int totalBlades = 0;
        for (auto& chunk : chunks) totalBlades += (int)chunk.instances.size();

        std::cout << "[GrassField] Initialized with " << totalBlades
            << " grass blades in " << chunks.size() << " chunks\n";
    }

    void update(float deltaTime)
    {
        windTime += deltaTime;
    }

    void draw(Core* core, PSOManager* psos, Shaders* shaders,
        const Matrix& vp, const Vec3& cameraPos)
    {
        if (chunks.empty()) return;

        // Perform chunk-based culling
        int visibleCount = performChunkCulling(cameraPos);
        if (visibleCount == 0) return;

        // Update shader constants
        Matrix world;

        shaders->updateConstantVS(shaderName, "grassBuffer", "VP", (void*)&vp);
        shaders->updateConstantVS(shaderName, "grassBuffer", "W", (void*)&world);

        Vec4 windData(windDirection.x, windDirection.y, windStrength, windTime);
        shaders->updateConstantVS(shaderName, "grassBuffer", "windParams", &windData);

        Vec4 cameraData(cameraPos.x, cameraPos.y, cameraPos.z, viewDistance);
        shaders->updateConstantVS(shaderName, "grassBuffer", "cameraPos", &cameraData);

        Vec4 lightDir(0.5f, 1.0f, -0.5f, 0.3f);
        shaders->updateConstantPS(shaderName, "grassPSBuffer", "lightDir_ambient", &lightDir);

        Vec4 grassColorTop(0.4f, 0.7f, 0.3f, 1.0f);
        Vec4 grassColorBottom(0.15f, 0.3f, 0.1f, 1.0f);
        shaders->updateConstantPS(shaderName, "grassPSBuffer", "grassColorTop", &grassColorTop);
        shaders->updateConstantPS(shaderName, "grassPSBuffer", "grassColorBottom", &grassColorBottom);

        shaders->apply(core, shaderName);
        psos->bind(core, psoName);
        core->getCommandList()->SetGraphicsRootDescriptorTable(2, grassTexture.srvHandle);

        // Set buffers
        D3D12_VERTEX_BUFFER_VIEW views[2];
        views[0] = grassMesh->getVertexBufferView();
        views[1] = instanceBufferView;
        core->getCommandList()->IASetVertexBuffers(0, 2, views);

        D3D12_INDEX_BUFFER_VIEW ibView = grassMesh->getIndexBufferView();
        core->getCommandList()->IASetIndexBuffer(&ibView);

        // Draw instanced
        core->getCommandList()->DrawIndexedInstanced(
            grassMesh->getIndexCount(),
            visibleCount,
            0, 0, 0
        );
    }

    // Public parameters
    Vec2 windDirection = Vec2(1.0f, 0.5f);
    float windStrength = 1.5f;
    float windSpeed = 1.0f;

    ~GrassField()
    {
        if (grassMesh) delete grassMesh;
        if (instanceBuffer) instanceBuffer->Release();
    }

private:
    HeightmapTerrain* terrain = nullptr;
    Mesh* grassMesh = nullptr;
    Texture grassTexture;

    std::vector<GrassChunk> chunks;
    std::vector<GrassInstance> visibleInstances;

    ID3D12Resource* instanceBuffer = nullptr;
    D3D12_VERTEX_BUFFER_VIEW instanceBufferView = {};

    float density = 2.5f;
    float viewDistance = 50.0f;
    float chunkSize = 16.0f;
    float windTime = 0.0f;

    void loadGrassModel(Core* core, const std::string& modelPath)
    {
        GEMLoader::GEMModelLoader loader;
        std::vector<GEMLoader::GEMMesh> gemmeshes;
        loader.load(modelPath, gemmeshes);

        if (gemmeshes.empty()) return;

        grassMesh = new Mesh();
        std::vector<STATIC_VERTEX> vertices;
        for (auto& v : gemmeshes[0].verticesStatic)
        {
            STATIC_VERTEX vert;
            memcpy(&vert, &v, sizeof(STATIC_VERTEX));
            vertices.push_back(vert);
        }
        grassMesh->init(core, vertices, gemmeshes[0].indices);
    }

    void generateGrassChunks(float minSpacing)
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> randScale(0.8f, 1.2f);
        std::uniform_real_distribution<float> randRot(0.0f, 6.28318f);
        std::uniform_real_distribution<float> randPhase(0.0f, 6.28318f);
        std::uniform_real_distribution<float> randOffset(-minSpacing * 0.5f, minSpacing * 0.5f);

        float terrainSizeX = 300.0f;
        float terrainSizeZ = 300.0f;
        float halfX = terrainSizeX * 0.5f;
        float halfZ = terrainSizeZ * 0.5f;

        // Create chunks
        int numChunksX = (int)(terrainSizeX / chunkSize);
        int numChunksZ = (int)(terrainSizeZ / chunkSize);

        for (int cz = 0; cz < numChunksZ; cz++)
        {
            for (int cx = 0; cx < numChunksX; cx++)
            {
                GrassChunk chunk;

                float chunkMinX = cx * chunkSize - halfX;
                float chunkMinZ = cz * chunkSize - halfZ;

                chunk.centerPos = Vec3(
                    chunkMinX + chunkSize * 0.5f,
                    0.0f,
                    chunkMinZ + chunkSize * 0.5f
                );

                // Generate grass in this chunk
                float spacing = sqrtf(1.0f / density);
                int grassPerChunkX = (int)(chunkSize / spacing);
                int grassPerChunkZ = (int)(chunkSize / spacing);

                for (int z = 0; z < grassPerChunkZ; z++)
                {
                    for (int x = 0; x < grassPerChunkX; x++)
                    {
                        float worldX = chunkMinX + (x * spacing) + randOffset(gen);
                        float worldZ = chunkMinZ + (z * spacing) + randOffset(gen);

                        // Clamp to chunk bounds
                        worldX = std::max(chunkMinX, std::min(chunkMinX + chunkSize, worldX));
                        worldZ = std::max(chunkMinZ, std::min(chunkMinZ + chunkSize, worldZ));

                        float worldY = terrain->sampleHeightWorld(worldX, worldZ);

                        GrassInstance inst;
                        inst.position = Vec3(worldX, worldY, worldZ);
                        inst.rotationY = randRot(gen);
                        inst.scale = randScale(gen);
                        inst.windPhase = randPhase(gen);

                        chunk.instances.push_back(inst);
                    }
                }

                chunks.push_back(chunk);
            }
        }
    }

    void createInstanceBuffer(Core* core)
    {
        // Calculate max possible instances
        size_t maxInstances = 0;
        for (auto& chunk : chunks)
            maxInstances += chunk.instances.size();

        if (maxInstances == 0) return;

        UINT bufferSize = (UINT)(maxInstances * sizeof(GrassInstance));

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

        core->device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&instanceBuffer)
        );

        instanceBufferView.BufferLocation = instanceBuffer->GetGPUVirtualAddress();
        instanceBufferView.StrideInBytes = sizeof(GrassInstance);
        instanceBufferView.SizeInBytes = bufferSize;
    }

    int performChunkCulling(const Vec3& cameraPos)
    {
        visibleInstances.clear();

        float maxDist = viewDistance + chunkSize * 0.5f; // Add chunk radius
        float maxDistSq = maxDist * maxDist;

        for (auto& chunk : chunks)
        {
            // Distance to chunk center
            float dx = chunk.centerPos.x - cameraPos.x;
            float dz = chunk.centerPos.z - cameraPos.z;
            float distSq = dx * dx + dz * dz;

            chunk.isVisible = (distSq <= maxDistSq);

            if (chunk.isVisible)
            {
                // Add all instances from this chunk
                for (const auto& inst : chunk.instances)
                {
                    visibleInstances.push_back(inst);
                }
            }
        }

        // Update instance buffer
        if (!visibleInstances.empty())
        {
            void* mappedData = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            instanceBuffer->Map(0, &readRange, &mappedData);

            size_t copySize = std::min(visibleInstances.size() * sizeof(GrassInstance),
                (size_t)instanceBufferView.SizeInBytes);
            memcpy(mappedData, visibleInstances.data(), copySize);

            instanceBuffer->Unmap(0, nullptr);
        }

        return (int)visibleInstances.size();
    }
};