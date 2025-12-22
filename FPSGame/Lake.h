#pragma once

#include "Core.h"
#include "Maths.h"
#include "Shaders.h"
#include "PSO.h"
#include <d3d12.h>
#include <vector>
#include <iostream>
#include <cmath>

// ============================================================================
// LAKE SYSTEM WITH GERSTNER WAVES AND PLANAR REFLECTIONS
// ============================================================================
// Features:
// - Circular lake mesh
// - Gerstner wave animation
// - Planar reflections
// - Fresnel effect
// - Depth-based coloring
// - Sun specular highlights
// - Shore transparency

// ============================================================================

struct LakeConfig
{
    // Position and size
    Vec3 center = Vec3(0, 0, 0);    // Lake center position
    float radius = 50.0f;           // Lake radius
    float waterLevel = 0.0f;        // Y height of water surface
    
    // Mesh quality
    int radialSegments = 64;        // Segments around circumference
    int ringSegments = 32;          // Segments from center to edge
    
    // Wave parameters (Gerstner)
    float waveSpeed = 1.0f;         // Overall wave animation speed
    float waveScale = 1.0f;         // Overall wave height multiplier
    
    // Water appearance
    Vec3 shallowColor = Vec3(0.1f, 0.4f, 0.5f);   // Color at edges
    Vec3 deepColor = Vec3(0.0f, 0.1f, 0.2f);      // Color at center/deep
    float transparency = 0.6f;       // Base transparency
    float fresnelPower = 4.0f;       // Fresnel falloff (higher = more reflective at edges)
    float fresnelBias = 0.02f;       // Minimum reflectivity
    
    // Reflection
    float reflectionStrength = 0.8f; // How strong reflections appear
    float reflectionDistortion = 0.03f; // Wave distortion of reflection
    
    // Specular (sun)
    Vec3 sunDirection = Vec3(0.4f, 0.7f, -0.5f);
    Vec3 sunColor = Vec3(1.0f, 0.95f, 0.8f);
    float specularPower = 256.0f;    // Sharpness of sun highlight
    float specularIntensity = 2.0f;  // Brightness of sun highlight
};

// Callback type for rendering scene (used for reflections)
typedef void (*SceneRenderCallback)(void* userData, const Matrix& view, const Matrix& proj);

class Lake
{
public:
    LakeConfig config;
    
    void init(Core* core, Shaders* shaders, PSOManager* psos, int screenWidth, int screenHeight)
    {
        this->core = core;
        this->screenWidth = screenWidth;
        this->screenHeight = screenHeight;
        
        std::cout << "\n[Lake] Initializing...\n";
        std::cout << "  Center: (" << config.center.x << ", " << config.center.y << ", " << config.center.z << ")\n";
        std::cout << "  Radius: " << config.radius << "\n";
        std::cout << "  Water level: " << config.waterLevel << "\n";
        
        createDescriptorHeaps();
        createReflectionRenderTarget();
        generateMesh();
        loadShaders(shaders);
        createPSO(psos, shaders);
        createConstantBuffers();
        
        initialized = true;
        std::cout << "[Lake] Ready!\n\n";
    }
    
    // Render reflection pass - call this BEFORE rendering your main scene
    // The callback should render: sky, terrain, rocks, grass (NOT the lake itself)
    void beginReflectionPass(const Matrix& view, const Matrix& proj, const Vec3& cameraPos,
                             SceneRenderCallback renderScene, void* userData)
    {
        if (!initialized) return;
        
        auto cmdList = core->getCommandList();
        
        // Calculate reflected camera position and view matrix
        float waterY = config.waterLevel;
        Vec3 reflectedCamPos = cameraPos;
        reflectedCamPos.y = 2.0f * waterY - cameraPos.y;  // Flip camera Y around water plane
        
        // Create reflected view matrix
        // We need to flip the up vector and look direction
        Matrix reflectedView = createReflectedViewMatrix(view, waterY);
        
        // Store for later use
        this->reflectionView = reflectedView;
        this->reflectionProj = proj;
        
        // Transition reflection RT
        transitionResource(reflectionTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
        
        // Set reflection render target
        cmdList->OMSetRenderTargets(1, &reflectionRTV, FALSE, &reflectionDSV);
        
        // Clear with sky color
        float clearColor[4] = { 0.5f, 0.7f, 0.9f, 1.0f };
        cmdList->ClearRenderTargetView(reflectionRTV, clearColor, 0, nullptr);
        cmdList->ClearDepthStencilView(reflectionDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        
        // Set viewport (can use half resolution for performance)
        D3D12_VIEWPORT vp = { 0, 0, (float)reflectionWidth, (float)reflectionHeight, 0, 1 };
        D3D12_RECT scissor = { 0, 0, (LONG)reflectionWidth, (LONG)reflectionHeight };
        cmdList->RSSetViewports(1, &vp);
        cmdList->RSSetScissorRects(1, &scissor);
        
        // Render the scene with reflected camera
        if (renderScene)
        {
            renderScene(userData, reflectedView, proj);
        }
        
        // Transition back to shader resource
        transitionResource(reflectionTexture, D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    
    // Render the water surface - call this AFTER rendering your main scene
    void render(Core* core, PSOManager* psos, Shaders* shaders,
                const Matrix& viewProj, const Vec3& cameraPos, float totalTime)
    {
        if (!initialized || vertexCount == 0) return;
        
        auto cmdList = core->getCommandList();
        
        // Update constant buffer
        updateConstantBuffer(viewProj, cameraPos, totalTime);
        
        // Bind PSO
        psos->bind(core, "LakeWaterPSO");
        
        // Set our descriptor heap for reflection texture
        ID3D12DescriptorHeap* heaps[] = { srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        
        // Bind constant buffer (root parameter 0)
        cmdList->SetGraphicsRootConstantBufferView(0, waterConstantBuffer->GetGPUVirtualAddress());
        
        // Bind reflection texture (root parameter 2 - texture slot)
        cmdList->SetGraphicsRootDescriptorTable(2, reflectionSRV);
        
        // Set vertex/index buffers
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 1, &vertexBufferView);
        cmdList->IASetIndexBuffer(&indexBufferView);
        
        // Draw
        cmdList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    }
    
    // Check if a point is inside the lake (for gameplay)
    bool isPointInLake(float x, float z) const
    {
        float dx = x - config.center.x;
        float dz = z - config.center.z;
        return (dx * dx + dz * dz) <= (config.radius * config.radius);
    }
    
    // Get water height at a point (approximate, doesn't include waves)
    float getWaterLevel() const { return config.waterLevel; }
    
    ~Lake()
    {
        if (vertexBuffer) vertexBuffer->Release();
        if (indexBuffer) indexBuffer->Release();
        if (waterConstantBuffer) waterConstantBuffer->Release();
        if (reflectionTexture) reflectionTexture->Release();
        if (reflectionDepth) reflectionDepth->Release();
        if (rtvHeap) rtvHeap->Release();
        if (dsvHeap) dsvHeap->Release();
        if (srvHeap) srvHeap->Release();
    }
    
private:
    Core* core = nullptr;
    bool initialized = false;
    
    int screenWidth, screenHeight;
    int reflectionWidth, reflectionHeight;
    
    // Mesh
    ID3D12Resource* vertexBuffer = nullptr;
    ID3D12Resource* indexBuffer = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW indexBufferView = {};
    UINT vertexCount = 0;
    UINT indexCount = 0;
    
    // Constant buffer
    ID3D12Resource* waterConstantBuffer = nullptr;
    
    // Reflection render target
    ID3D12Resource* reflectionTexture = nullptr;
    ID3D12Resource* reflectionDepth = nullptr;
    
    // Descriptor heaps
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12DescriptorHeap* dsvHeap = nullptr;
    ID3D12DescriptorHeap* srvHeap = nullptr;
    
    D3D12_CPU_DESCRIPTOR_HANDLE reflectionRTV;
    D3D12_CPU_DESCRIPTOR_HANDLE reflectionDSV;
    D3D12_GPU_DESCRIPTOR_HANDLE reflectionSRV;
    
    // Stored reflection matrices
    Matrix reflectionView;
    Matrix reflectionProj;
    
    // Vertex structure
    struct WaterVertex
    {
        float x, y, z;      // Position
        float u, v;         // UV coordinates (for wave calculation)
        float nx, ny, nz;   // Normal (will be recalculated in shader)
    };
    
    // Constant buffer structure (must match shader)
    struct WaterCB
    {
        Matrix worldViewProj;
        Matrix world;
        Matrix reflectionMatrix;    // For sampling reflection texture
        Vec4 cameraPos;             // xyz = position, w = time
        Vec4 waterParams;           // x = waterLevel, y = radius, z = transparency, w = fresnelPower
        Vec4 shallowColor;          // xyz = color, w = fresnelBias
        Vec4 deepColor;             // xyz = color, w = reflectionStrength
        Vec4 sunDirection;          // xyz = direction, w = specularPower
        Vec4 sunColor;              // xyz = color, w = specularIntensity
        Vec4 waveParams;            // x = speed, y = scale, z = distortion, w = unused
        Vec4 screenParams;          // x = width, y = height, z = 1/width, w = 1/height
        
        // Gerstner wave parameters (4 waves)
        Vec4 waveDirections[4];     // xy = direction, zw = unused
        Vec4 waveParams2[4];        // x = wavelength, y = amplitude, z = steepness, w = speed
    };
    
    void createDescriptorHeaps()
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.NumDescriptors = 1;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        core->device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));
        
        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = 1;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        core->device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap));
        
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
        srvDesc.NumDescriptors = 2;
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        core->device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap));
    }
    
    void createReflectionRenderTarget()
    {
        // Use half resolution for reflection (performance optimization)
        reflectionWidth = screenWidth / 2;
        reflectionHeight = screenHeight / 2;
        
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        
        // Reflection color texture
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = reflectionWidth;
        texDesc.Height = reflectionHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        
        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        clearValue.Color[0] = 0.5f;
        clearValue.Color[1] = 0.7f;
        clearValue.Color[2] = 0.9f;
        clearValue.Color[3] = 1.0f;
        
        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&reflectionTexture));
        
        // RTV
        reflectionRTV = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        core->device->CreateRenderTargetView(reflectionTexture, nullptr, reflectionRTV);
        
        // SRV
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
        reflectionSRV = srvHeap->GetGPUDescriptorHandleForHeapStart();
        
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        core->device->CreateShaderResourceView(reflectionTexture, &srvDesc, srvCpu);
        
        // Reflection depth buffer
        D3D12_RESOURCE_DESC depthDesc = {};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = reflectionWidth;
        depthDesc.Height = reflectionHeight;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        
        D3D12_CLEAR_VALUE depthClear = {};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;
        
        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&reflectionDepth));
        
        // DSV
        reflectionDSV = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvViewDesc = {};
        dsvViewDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        core->device->CreateDepthStencilView(reflectionDepth, &dsvViewDesc, reflectionDSV);
        
        std::cout << "[Lake] Reflection RT created: " << reflectionWidth << "x" << reflectionHeight << "\n";
    }
    
    void generateMesh()
    {
        std::vector<WaterVertex> vertices;
        std::vector<unsigned int> indices;
        
        int radialSegs = config.radialSegments;
        int ringSegs = config.ringSegments;
        float radius = config.radius;
        float centerX = config.center.x;
        float centerZ = config.center.z;
        float waterY = config.waterLevel;
        
        const float PI = 3.14159265359f;
        
        // Center vertex
        WaterVertex center;
        center.x = centerX;
        center.y = waterY;
        center.z = centerZ;
        center.u = 0.5f;
        center.v = 0.5f;
        center.nx = 0;
        center.ny = 1;
        center.nz = 0;
        vertices.push_back(center);
        
        // Generate rings of vertices from center to edge
        for (int ring = 1; ring <= ringSegs; ring++)
        {
            float ringRadius = (float)ring / ringSegs * radius;
            float ringU = (float)ring / ringSegs * 0.5f;
            
            for (int seg = 0; seg < radialSegs; seg++)
            {
                float angle = (float)seg / radialSegs * 2.0f * PI;
                
                WaterVertex v;
                v.x = centerX + cosf(angle) * ringRadius;
                v.y = waterY;
                v.z = centerZ + sinf(angle) * ringRadius;
                v.u = 0.5f + cosf(angle) * ringU;
                v.v = 0.5f + sinf(angle) * ringU;
                v.nx = 0;
                v.ny = 1;
                v.nz = 0;
                
                vertices.push_back(v);
            }
        }
        
        // Generate indices
        // Center triangles (first ring)
        for (int seg = 0; seg < radialSegs; seg++)
        {
            int next = (seg + 1) % radialSegs;
            indices.push_back(0);               // Center
            indices.push_back(1 + next);        // Next vertex in first ring
            indices.push_back(1 + seg);         // Current vertex in first ring
        }
        
        // Ring triangles
        for (int ring = 1; ring < ringSegs; ring++)
        {
            int ringStart = 1 + (ring - 1) * radialSegs;
            int nextRingStart = 1 + ring * radialSegs;
            
            for (int seg = 0; seg < radialSegs; seg++)
            {
                int next = (seg + 1) % radialSegs;
                
                // Two triangles per quad
                indices.push_back(ringStart + seg);
                indices.push_back(nextRingStart + next);
                indices.push_back(nextRingStart + seg);
                
                indices.push_back(ringStart + seg);
                indices.push_back(ringStart + next);
                indices.push_back(nextRingStart + next);
            }
        }
        
        vertexCount = (UINT)vertices.size();
        indexCount = (UINT)indices.size();
        
        std::cout << "[Lake] Generated mesh: " << vertexCount << " vertices, " << indexCount / 3 << " triangles\n";
        
        // Create vertex buffer
        UINT vbSize = vertexCount * sizeof(WaterVertex);
        
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        
        D3D12_RESOURCE_DESC vbDesc = {};
        vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vbDesc.Width = vbSize;
        vbDesc.Height = 1;
        vbDesc.DepthOrArraySize = 1;
        vbDesc.MipLevels = 1;
        vbDesc.SampleDesc.Count = 1;
        vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        
        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &vbDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&vertexBuffer));
        
        // Upload vertex data
        core->uploadResource(vertexBuffer, vertices.data(), vbSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        
        vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexBufferView.SizeInBytes = vbSize;
        vertexBufferView.StrideInBytes = sizeof(WaterVertex);
        
        // Create index buffer
        UINT ibSize = indexCount * sizeof(unsigned int);
        
        D3D12_RESOURCE_DESC ibDesc = vbDesc;
        ibDesc.Width = ibSize;
        
        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &ibDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&indexBuffer));
        
        core->uploadResource(indexBuffer, indices.data(), ibSize, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        
        indexBufferView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        indexBufferView.SizeInBytes = ibSize;
        indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    }
    
    void loadShaders(Shaders* shaders)
    {
        shaders->load(core, "Water", "Shaders/VSWater.txt", "Shaders/PSWater.txt");
        
    }
    
    
    void createPSO(PSOManager* psos, Shaders* shaders)
    {
        std::vector<D3D12_INPUT_ELEMENT_DESC> layout = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        
        // Create PSO with alpha blending enabled
        auto* shader = shaders->find("Water");
        if (!shader)
        {
            std::cout << "[Lake] ERROR: Water shader not found!\n";
            return;
        }
        
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = core->rootSignature;
        psoDesc.VS = { shader->vs->GetBufferPointer(), shader->vs->GetBufferSize() };
        psoDesc.PS = { shader->ps->GetBufferPointer(), shader->ps->GetBufferSize() };
        psoDesc.InputLayout = { layout.data(), (UINT)layout.size() };
        
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // Render both sides
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;
        
        // Alpha blending
        psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count = 1;
        
        ID3D12PipelineState* pso = nullptr;
        HRESULT hr = core->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
        
        if (FAILED(hr))
        {
            std::cout << "[Lake] ERROR: Failed to create water PSO\n";
            return;
        }
        
        psos->add("LakeWaterPSO", pso);
        std::cout << "[Lake] PSO created\n";
    }
    
    void createConstantBuffers()
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 512;  // Enough for our constant buffer
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        
        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&waterConstantBuffer));
    }
    
    void updateConstantBuffer(const Matrix& viewProj, const Vec3& cameraPos, float time)
    {
        // World matrix (identity, lake is already at world position)
        Matrix world;  // Identity
        
        WaterCB cb;
        cb.worldViewProj = world * viewProj;
        cb.world = world;
        cb.reflectionMatrix = reflectionView * reflectionProj;
        cb.cameraPos = Vec4(cameraPos.x, cameraPos.y, cameraPos.z, time);
        cb.waterParams = Vec4(config.waterLevel, config.radius, config.transparency, config.fresnelPower);
        cb.shallowColor = Vec4(config.shallowColor.x, config.shallowColor.y, config.shallowColor.z, config.fresnelBias);
        cb.deepColor = Vec4(config.deepColor.x, config.deepColor.y, config.deepColor.z, config.reflectionStrength);
        cb.sunDirection = Vec4(config.sunDirection.x, config.sunDirection.y, config.sunDirection.z, config.specularPower);
        cb.sunColor = Vec4(config.sunColor.x, config.sunColor.y, config.sunColor.z, config.specularIntensity);
        cb.waveParams = Vec4(config.waveSpeed, config.waveScale, config.reflectionDistortion, 0);
        cb.screenParams = Vec4((float)reflectionWidth, (float)reflectionHeight,
                               1.0f / reflectionWidth, 1.0f / reflectionHeight);
        
        // Gerstner wave parameters (4 waves)
        // Wave 1: Main wave
        cb.waveDirections[0] = Vec4(1.0f, 0.0f, 0, 0);
        cb.waveParams2[0] = Vec4(20.0f, 0.4f, 0.5f, 1.0f);  // wavelength, amplitude, steepness, speed
        
        // Wave 2: Secondary
        cb.waveDirections[1] = Vec4(0.7f, 0.7f, 0, 0);
        cb.waveParams2[1] = Vec4(12.0f, 0.25f, 0.4f, 1.2f);
        
        // Wave 3: Detail
        cb.waveDirections[2] = Vec4(0.2f, 0.9f, 0, 0);
        cb.waveParams2[2] = Vec4(6.0f, 0.1f, 0.3f, 0.8f);
        
        // Wave 4: Fine detail
        cb.waveDirections[3] = Vec4(-0.4f, 0.8f, 0, 0);
        cb.waveParams2[3] = Vec4(3.0f, 0.05f, 0.2f, 1.5f);
        
        void* data;
        waterConstantBuffer->Map(0, nullptr, &data);
        memcpy(data, &cb, sizeof(cb));
        waterConstantBuffer->Unmap(0, nullptr);
    }
    
    Matrix createReflectedViewMatrix(const Matrix& view, float waterY)
    {
        // Reflection matrix for Y plane at waterY
        Matrix reflection;
        reflection.a[1][1] = -1.0f;           // Flip Y
        reflection.a[1][3] = 2.0f * waterY;
        
        // Return view * reflection (reflect the view)
        Matrix viewCopy = view;
        return reflection * viewCopy;
    }
    
    void transitionResource(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = res;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        core->getCommandList()->ResourceBarrier(1, &barrier);
    }
};