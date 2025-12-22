#pragma once

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include <d3d12.h>

// ============================================================================
// FULLSCREEN QUAD - Renders a screen-filling triangle for post-processing
// ============================================================================
// Used for fog ray marching pass and final composite pass.
// Uses a single triangle that covers the entire screen (more efficient than quad)
// ============================================================================

class FullscreenQuad
{
public:
    ID3D12Resource* vertexBuffer = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};
    bool initialized = false;

    // Simple vertex for fullscreen pass
    struct FullscreenVertex
    {
        float x, y;     // Position (-1 to 1)
        float u, v;     // UV (0 to 1)
    };

    void init(Core* core)
    {
        // Create a fullscreen triangle (3 vertices)
        // Using a single triangle is more efficient than a quad (no diagonal)
        // The triangle extends beyond the screen edges to cover everything
        FullscreenVertex vertices[3] = {
            { -1.0f,  3.0f, 0.0f, -1.0f },  // Top-left (extended)
            { -1.0f, -1.0f, 0.0f,  1.0f },  // Bottom-left
            {  3.0f, -1.0f, 2.0f,  1.0f }   // Bottom-right (extended)
        };

        // Create vertex buffer
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = sizeof(vertices);
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = core->device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&vertexBuffer)
        );

        if (FAILED(hr))
        {
            std::cout << "[FullscreenQuad] ERROR: Failed to create vertex buffer\n";
            return;
        }

        // Upload vertex data
        void* mappedData = nullptr;
        vertexBuffer->Map(0, nullptr, &mappedData);
        memcpy(mappedData, vertices, sizeof(vertices));
        vertexBuffer->Unmap(0, nullptr);

        // Setup vertex buffer view
        vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexBufferView.SizeInBytes = sizeof(vertices);
        vertexBufferView.StrideInBytes = sizeof(FullscreenVertex);

        initialized = true;
        std::cout << "[FullscreenQuad] Initialized\n";
    }

    void draw(ID3D12GraphicsCommandList* cmdList)
    {
        if (!initialized) return;

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 1, &vertexBufferView);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    // Get input layout for fullscreen shaders
    static std::vector<D3D12_INPUT_ELEMENT_DESC> getInputLayout()
    {
        std::vector<D3D12_INPUT_ELEMENT_DESC> layout = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        return layout;
    }

    ~FullscreenQuad()
    {
        if (vertexBuffer) vertexBuffer->Release();
    }
};