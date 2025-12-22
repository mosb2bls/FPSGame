#pragma once

#include <vector>
#include <string>
#include <cmath>

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "Maths.h"

class SkyDome
{
    Texture skyTexture;
public:
   
    std::string shaderName = "Sky";
    std::string psoName = "SkyPSO";

    void init(Core* core, PSOManager* psos, Shaders* shaders,
        float radius = 5000.0f, int slices = 64, int stacks = 32)
    {
        this->radius = radius;

        // 1) 生成球体网格
        std::vector<STATIC_VERTEX> vertices;
        std::vector<unsigned int>  indices;
        buildSphere(vertices, indices, slices, stacks);

        mesh.init(core, vertices, indices);

        skyTexture = core->loadTexture("Assets/Sky/sky.png"); 

        
        shaders->load(core, shaderName, "Shaders/VSSky.txt", "Shaders/PSSky.txt");

        // 3. Create PSO (Standard)
        psos->createPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getStaticLayout());
    }

    void draw(Core* core, PSOManager* psos, Shaders* shaders,
        const Matrix& vp, const Vec3& cameraPos)
    {
        // 世界矩阵：缩放到大球，并把球中心移动到摄像机位置
        Matrix W = Matrix::scaling(Vec3(radius, radius, radius))
            * Matrix::translation(cameraPos);

        // VS 常量：W 和 VP
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "W", (void*)&W);
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "VP", (void*)&vp);

        // PS 常量：天空颜色（你可以随便改）
        Vec4 zenith(0.2f, 0.4f, 0.8f, 1.0f); // 顶部颜色
        Vec4 horizon(0.8f, 0.7f, 0.5f, 1.0f); // 地平线颜色

        shaders->updateConstantPS(shaderName, "skyPSBuffer", "zenithColor", &zenith);
        shaders->updateConstantPS(shaderName, "skyPSBuffer", "horizonColor", &horizon);

        // 绑定 shader + PSO + 画 mesh
        shaders->apply(core, shaderName);
        psos->bind(core, psoName);

        core->getCommandList()->SetGraphicsRootDescriptorTable(2, skyTexture.srvHandle);
        mesh.draw(core);
    }

private:
    Mesh  mesh;
    float radius = 5000.0f;

    // 用 STATIC_VERTEX 构造一个顶点，只用 pos/normal/tangent/tu/tv
    STATIC_VERTEX makeVertex(const Vec3& p, const Vec3& n, float u, float v)
    {
        STATIC_VERTEX vert;
        vert.pos = p;
        vert.normal = n;

        Frame frame;
        frame.fromVector(n);      // 根据法线生成一个正交坐标系
        vert.tangent = frame.u;   // 取其中一个方向作为切线

        vert.tu = u;
        vert.tv = v;
        return vert;
    }

    // 生成一个三角形球体（经纬线方式）
    void buildSphere(std::vector<STATIC_VERTEX>& outV,
        std::vector<unsigned int>& outI,
        int slices, int stacks)
    {
        outV.clear();
        outI.clear();

        const float PI = 3.141592654f;

        // 顶点
        for (int stack = 0; stack <= stacks; ++stack)
        {
            float phi = (float)stack / (float)stacks * PI;   // 0..PI
            float y = cosf(phi);
            float r = sinf(phi);

            for (int slice = 0; slice <= slices; ++slice)
            {
                float theta = (float)slice / (float)slices * (2.0f * PI); // 0..2PI
                float x = r * cosf(theta);
                float z = r * sinf(theta);

                Vec3 pos(x, y, z);
                Vec3 normal = pos.normalize();

                // 纹理坐标（如果以后你想用贴图，可以直接用）
                float u = (float)slice / (float)slices;
                float v = (float)stack / (float)stacks;

                outV.push_back(makeVertex(pos, normal, u, v));
            }
        }

        // 索引
        int ring = slices + 1;

        for (int stack = 0; stack < stacks; ++stack)
        {
            for (int slice = 0; slice < slices; ++slice)
            {
                unsigned int i0 = (unsigned int)(stack * ring + slice);
                unsigned int i1 = (unsigned int)(i0 + 1);
                unsigned int i2 = (unsigned int)((stack + 1) * ring + slice);
                unsigned int i3 = (unsigned int)(i2 + 1);

                // 两个三角形
                outI.push_back(i0);
                outI.push_back(i1);
                outI.push_back(i2);

                outI.push_back(i1);
                outI.push_back(i3);
                outI.push_back(i2);
            }
        }
    }
};
