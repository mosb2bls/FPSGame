#pragma once

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "Maths.h"
#include <vector>
#include <cmath>

// ============================================================================
// LAKE BOTTOM - Curved bowl shape using sphere segment
// ============================================================================

class LakeBottom
{
public:
    std::string shaderName = "LakeBottom";
    std::string psoName = "LakeBottomPSO";

    void init(Core* core, Shaders* shaders, PSOManager* psos,
        const std::string& texturePath,
        const Vec3& center, float radius, float waterY, float bowlDepth = 8.0f)
    {
        this->lakeCenter = center;
        this->lakeRadius = radius;
        this->waterLevel = waterY;
        this->depth = bowlDepth;

        std::cout << "[LakeBottom] Initializing...\n";
        std::cout << "  Center: (" << center.x << ", " << center.y << ", " << center.z << ")\n";
        std::cout << "  Radius: " << radius << ", Depth: " << bowlDepth << "\n";

        // Load texture
        bottomTexture = core->loadTexture(texturePath);

        // Generate bowl mesh using standard STATIC_VERTEX and Mesh class
        std::vector<STATIC_VERTEX> vertices;
        std::vector<unsigned int> indices;
        generateBowlMesh(vertices, indices);

        mesh.init(core, vertices, indices);

        // Load shaders
        shaders->load(core, shaderName, "Shaders/VSLakeBottom.txt", "Shaders/PSLakeBottom.txt");

        // Create PSO using standard method (same as terrain)
        psos->createPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getStaticLayout());

        initialized = true;
        std::cout << "[LakeBottom] Ready! Vertices: " << vertices.size()
            << ", Triangles: " << indices.size() / 3 << "\n";
    }

    void draw(Core* core, PSOManager* psos, Shaders* shaders, const Matrix& vp)
    {
        if (!initialized) return;

        // Update shader constants
        Matrix world;  // Identity - mesh is already in world space
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "VP", (void*)&vp);
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "W", (void*)&world);

        shaders->apply(core, shaderName);
        psos->bind(core, psoName);

        // Bind texture
        core->getCommandList()->SetGraphicsRootDescriptorTable(2, bottomTexture.srvHandle);

        // Draw
        mesh.draw(core);
    }

private:
    Mesh mesh;
    Texture bottomTexture;
    bool initialized = false;

    Vec3 lakeCenter = Vec3(0, 0, 0);
    float lakeRadius = 25.0f;
    float depth = 8.0f;
    float waterLevel = 0.0f;
    int segments = 64;

    void generateBowlMesh(std::vector<STATIC_VERTEX>& vertices, std::vector<unsigned int>& indices)
    {
        vertices.clear();
        indices.clear();

        // Calculate sphere parameters
        // sphereRadius = (lakeRadius^2 + depth^2) / (2 * depth)
        float sphereRadius = (lakeRadius * lakeRadius + depth * depth) / (2.0f * depth);

        // Sphere center is above the bowl bottom
        Vec3 sphereCenter = Vec3(lakeCenter.x, waterLevel - depth + sphereRadius, lakeCenter.z);

        int rings = segments / 2;
        int slices = segments;

        // Calculate the angle where sphere intersects water level
        float cosMaxTheta = (sphereRadius - depth) / sphereRadius;
        float maxTheta = acosf(cosMaxTheta);

        for (int ring = 0; ring <= rings; ring++)
        {
            float t = (float)ring / (float)rings;
            float theta = 3.14159265f - t * (3.14159265f - maxTheta);

            float sinTheta = sinf(theta);
            float cosTheta = cosf(theta);

            for (int slice = 0; slice <= slices; slice++)
            {
                float phi = (float)slice / (float)slices * 2.0f * 3.14159265f;
                float sinPhi = sinf(phi);
                float cosPhi = cosf(phi);

                // Position on sphere
                Vec3 pos;
                pos.x = sphereCenter.x + sphereRadius * sinTheta * cosPhi;
                pos.y = sphereCenter.y + sphereRadius * cosTheta;
                pos.z = sphereCenter.z + sphereRadius * sinTheta * sinPhi;

                // Normal points inward (we're inside the bowl)
                Vec3 normal;
                normal.x = -sinTheta * cosPhi;
                normal.y = -cosTheta;
                normal.z = -sinTheta * sinPhi;

                // UV mapping
                float u = (pos.x - lakeCenter.x) / (lakeRadius * 2.0f) + 0.5f;
                float v = (pos.z - lakeCenter.z) / (lakeRadius * 2.0f) + 0.5f;

                // Create STATIC_VERTEX (matches your Mesh.h format)
                STATIC_VERTEX vert;
                vert.pos = pos;
                vert.normal = normal;
                vert.tangent = Vec3(1, 0, 0);  // Simple tangent
                vert.tu = u;
                vert.tv = v;

                vertices.push_back(vert);
            }
        }

        // Generate indices
        for (int ring = 0; ring < rings; ring++)
        {
            for (int slice = 0; slice < slices; slice++)
            {
                int current = ring * (slices + 1) + slice;
                int next = current + slices + 1;

                indices.push_back(current);
                indices.push_back(current + 1);
                indices.push_back(next);

                indices.push_back(current + 1);
                indices.push_back(next + 1);
                indices.push_back(next);
            }
        }
    }
};