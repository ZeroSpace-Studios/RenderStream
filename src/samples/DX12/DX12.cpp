// A simple RenderStream application that sends back a 3D scene using a DX12 texture
//
// Usage: Compile, copy the executable into your RenderStream Projects folder and launch via d3

#include <tchar.h>
#include <d3d12.h>
#include "d3dx12.h"
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <unordered_map>

// auto-generated from hlsl
#include "Generated_Code/VertexShader.h"
#include "Generated_Code/PixelShader.h"

#include "../../include/renderstream.hpp"

#define LOG(streamexpr) std::cerr << streamexpr << std::endl

#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

DXGI_FORMAT toDxgiFormat(RSPixelFormat format)
{
    switch (format)
    {
    case RS_FMT_BGRA8:
    case RS_FMT_BGRX8:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case RS_FMT_RGBA32F:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case RS_FMT_RGBA16:
        return DXGI_FORMAT_R16G16B16A16_UNORM;
    default:
        throw std::runtime_error("Unhandled RS pixel format");
    }
}

static constexpr DirectX::XMFLOAT3 cubeVertices[] =
{
    DirectX::XMFLOAT3(-0.5f, 0.5f,-0.5f),
    DirectX::XMFLOAT3(0.5f, 0.5f,-0.5f),
    DirectX::XMFLOAT3(-0.5f,-0.5f,-0.5f),
    DirectX::XMFLOAT3( 0.5f,-0.5f,-0.5f),

    DirectX::XMFLOAT3(-0.5f, 0.5f, 0.5f),
    DirectX::XMFLOAT3(0.5f, 0.5f, 0.5f),
    DirectX::XMFLOAT3(-0.5f,-0.5f, 0.5f),
    DirectX::XMFLOAT3( 0.5f,-0.5f, 0.5f),
};

static constexpr uint16_t cubeIndices[] =
{
    0, 1, 2,    // side 1
    2, 1, 3,
    4, 0, 6,    // side 2
    6, 0, 2,
    7, 5, 6,    // side 3
    6, 5, 4,
    3, 1, 7,    // side 4
    7, 1, 5,
    4, 5, 0,    // side 5
    0, 5, 1,
    3, 7, 2,    // side 6
    2, 7, 6,
};


static constexpr UINT cubeDrawCalls[] =
{
    36
};

HRESULT CreateDefaultBuffer(ID3D12Device* dev, ID3D12GraphicsCommandList* cmdLst, ID3D12Resource** defaultBuffer, ID3D12Resource** uploadBuffer, const void* initData, UINT64 byteSize)
{
    HRESULT hr;
    CD3DX12_HEAP_PROPERTIES defaultHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
    hr = dev->CreateCommittedResource(
        &defaultHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(defaultBuffer)
    );
    if (FAILED(hr))
        return hr;

    CD3DX12_HEAP_PROPERTIES uploadHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    hr = dev->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(uploadBuffer)
    );
    if (FAILED(hr))
        return hr;

    D3D12_SUBRESOURCE_DATA data = {};
    data.pData = initData;
    data.RowPitch = byteSize;
    data.SlicePitch = data.RowPitch;

    const auto toCopyBarrier = CD3DX12_RESOURCE_BARRIER::Transition(*defaultBuffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdLst->ResourceBarrier(1, &toCopyBarrier);

    UpdateSubresources<1>(cmdLst, *defaultBuffer, *uploadBuffer, 0, 0, 1, &data);

    const auto toGenericBarrier = CD3DX12_RESOURCE_BARRIER::Transition(*defaultBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
    cmdLst->ResourceBarrier(1, &toGenericBarrier);

    return hr;
}

struct ConstantBufferStruct 
{
    DirectX::XMMATRIX worldViewProjection;
};

class Fence
{
public:
    Fence(ID3D12Device* device, ID3D12CommandQueue* _commandQueue)
        : commandQueue(_commandQueue)
    {
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))))
            throw std::runtime_error("Failed to create fence");
    }

    void wait()
    {
        ++fenceValue;
        commandQueue->Signal(fence.Get(), fenceValue);
        if (fence->GetCompletedValue() < fenceValue)
        {
            HANDLE eventHandle = CreateEvent(nullptr, false, false, nullptr);
            if (eventHandle == 0)
                throw std::runtime_error("Unable to create event");
            fence->SetEventOnCompletion(fenceValue, eventHandle);
            WaitForSingleObject(eventHandle, INFINITE);
            CloseHandle(eventHandle);
        }
    }

private:
    ID3D12CommandQueue* commandQueue = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    int fenceValue = 0;
};


int mainImpl()
{
    RenderStream rs;

    rs.initialise();
    LOG("RenderStream initialised - program starting");

#if _DEBUG
    Microsoft::WRL::ComPtr<ID3D12Debug> debugInterface;
    if (!FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(debugInterface.GetAddressOf()))))
        debugInterface->EnableDebugLayer();
#endif

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.GetAddressOf()))))
    {
        LOG("Failed to initialise DirectX 12");
        return 4;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue;
    D3D12_COMMAND_QUEUE_DESC queueDesc;
    ZeroMemory(&queueDesc, sizeof(D3D12_COMMAND_QUEUE_DESC));
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue.GetAddressOf()))))
    {
        LOG("Failed to create command queue");
        return 40;
    }

    std::unique_ptr<Fence> fence;
    try
    {
        fence = std::make_unique<Fence>(device.Get(), commandQueue.Get());
    }
    catch (const std::exception& e)
    {
        LOG(e.what());
        return 41;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(commandAllocator.GetAddressOf()))))
    {
        LOG("Failed to create command allocator");
        return 42;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(commandList.GetAddressOf()))))
    {
        LOG("Failed to create command list");
        return 43;
    }
    commandList->Close();

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    {
        CD3DX12_ROOT_PARAMETER slotRootParameter[1];
        CD3DX12_DESCRIPTOR_RANGE cbvTable;
        cbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        slotRootParameter[0].InitAsDescriptorTable(1, &cbvTable);
        CD3DX12_ROOT_SIGNATURE_DESC rsDesc(1, slotRootParameter, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSigBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, serializedRootSigBlob.GetAddressOf(), errorBlob.GetAddressOf())))
        {
            LOG("Failed to serialize root signature");
            return 44;
        }
        if (FAILED(device->CreateRootSignature(0, serializedRootSigBlob->GetBufferPointer(), serializedRootSigBlob->GetBufferSize(), IID_PPV_ARGS(rootSignature.GetAddressOf()))))
        {
            LOG("Failed to create root signature");
            return 44;
        }
    }

    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
    {
        commandList->Reset(commandAllocator.Get(), nullptr);
        Microsoft::WRL::ComPtr<ID3D12Resource> vertexUploadBuffer;
        size_t byteSize = UINT(std::size(cubeVertices)) * sizeof(DirectX::XMFLOAT3);
        if (FAILED(CreateDefaultBuffer(device.Get(), commandList.Get(), vertexBuffer.GetAddressOf(), vertexUploadBuffer.GetAddressOf(), cubeVertices, byteSize)) ||
            FAILED(commandList->Close()))
        {
            LOG("Failed to initialise vertex buffer");
            return 45;
        }
        ID3D12CommandList* cmdLists[1] = { commandList.Get() };
        commandQueue->ExecuteCommandLists(1, cmdLists);
        fence->wait();
        vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexBufferView.SizeInBytes = UINT(byteSize);
        vertexBufferView.StrideInBytes = sizeof(DirectX::XMFLOAT3);
    }
    D3D12_INDEX_BUFFER_VIEW indexBufferView;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
    {
        commandList->Reset(commandAllocator.Get(), nullptr);
        Microsoft::WRL::ComPtr<ID3D12Resource> indexUploadBuffer;
        size_t byteSize = UINT(std::size(cubeIndices)) * sizeof(uint16_t);
        if (FAILED(CreateDefaultBuffer(device.Get(), commandList.Get(), indexBuffer.GetAddressOf(), indexUploadBuffer.GetAddressOf(), cubeIndices, byteSize)) ||
            FAILED(commandList->Close()))
        {
            LOG("Failed to initialise index buffer");
            return 46;
        }
        ID3D12CommandList* cmdLists[1] = { commandList.Get() };
        commandQueue->ExecuteCommandLists(1, cmdLists);
        fence->wait();
        indexBufferView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        indexBufferView.SizeInBytes = UINT(byteSize);
        indexBufferView.Format = DXGI_FORMAT_R16_UINT;
    }

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cbvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> cbUploadBuffer;
    BYTE* cbUploadBufferPtr;
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc;
        heapDesc.NumDescriptors = 1;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NodeMask = 0;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(cbvHeap.GetAddressOf()))))
        {
            LOG("Failed to create constant buffer descriptor heap");
            return 47;
        }
    }
    {
        UINT64 alignedSize = (sizeof(ConstantBufferStruct) + 255) & ~255;
        CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(alignedSize);
        if (FAILED(device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&cbUploadBuffer))) ||
            FAILED(cbUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&cbUploadBufferPtr))))
        {
            LOG("Failed to create constant buffer");
            return 48;
        }

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbDesc;
        cbDesc.BufferLocation = cbUploadBuffer->GetGPUVirtualAddress();
        cbDesc.SizeInBytes = UINT(alignedSize);
        device->CreateConstantBufferView(&cbDesc, cbvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    std::unordered_map<DXGI_FORMAT, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelineStates;
    for (DXGI_FORMAT format : { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R16G16B16A16_UNORM})
    {
        auto& pipelineState = pipelineStates[format];
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
        ZeroMemory(&desc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

        D3D12_INPUT_ELEMENT_DESC inputElementDesc[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
        desc.InputLayout = { inputElementDesc, 1 };
        desc.pRootSignature = rootSignature.Get();
        desc.VS = { VertexShaderBlob, std::size(VertexShaderBlob) };
        desc.PS = { PixelShaderBlob, std::size(PixelShaderBlob) };
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pipelineState.GetAddressOf()))))
        {
            LOG("Failed to create pipeline state");
            return 49;
        }
    }

    rs.initialiseGpGpuWithDX12DeviceAndQueue(device.Get(), commandQueue.Get());

    std::vector<uint8_t> descMem;
    const StreamDescriptions* header = nullptr;
    struct RenderTarget
    {
        DXGI_FORMAT format;
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        D3D12_CPU_DESCRIPTOR_HANDLE view;
    };
    std::unordered_map<StreamHandle, RenderTarget> renderTargets;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    const float clearColour[4] = { 0.f, 0.2f, 0.f, 0.f };
    while (true)
    {
        // Wait for a frame request
        std::variant<FrameData, RS_ERROR> awaitResult = rs.awaitFrameData(5000);
        if (std::holds_alternative<RS_ERROR>(awaitResult))
        {
            RS_ERROR err = std::get<RS_ERROR>(awaitResult);
            if (err == RS_ERROR_STREAMS_CHANGED)
            {
                header = rs.getStreams();
                // Create render targets for all streams
                const size_t numStreams = header ? header->nStreams : 0;
                D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
                rtvHeapDesc.NumDescriptors = UINT(numStreams);
                rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                rtvHeapDesc.NodeMask = 0;
                if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(rtvHeap.GetAddressOf()))))
                        throw std::runtime_error("Failed to create render target descriptor heap");
                renderTargets.clear(); // Heap handles are now stale
                UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                for (size_t i = 0; i < numStreams; ++i)
                {
                    const StreamDescription& description = header->streams[i];
                    RenderTarget& target = renderTargets[description.handle];

                    target.format = toDxgiFormat(description.format);

                    D3D12_RESOURCE_DESC rtDesc;
                    ZeroMemory(&rtDesc, sizeof(D3D12_RESOURCE_DESC));
                    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    rtDesc.Width = description.width;
                    rtDesc.Height = description.height;
                    rtDesc.MipLevels = 1;
                    rtDesc.DepthOrArraySize = 1;
                    rtDesc.Format = target.format;
                    rtDesc.SampleDesc.Count = 1;
                    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

                    D3D12_CLEAR_VALUE clearValue;
                    clearValue.Format = target.format;
                    clearValue.Color[0] = clearColour[0];
                    clearValue.Color[1] = clearColour[1];
                    clearValue.Color[2] = clearColour[2];
                    clearValue.Color[3] = clearColour[3];

                    CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
                    if (FAILED(device->CreateCommittedResource(
                        &heapProperties,
                        D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES | D3D12_HEAP_FLAG_SHARED,
                        &rtDesc,
                        D3D12_RESOURCE_STATE_GENERIC_READ,
                        &clearValue,
                        IID_PPV_ARGS(target.texture.GetAddressOf()))))
                        throw std::runtime_error("Failed to create render target texture");

                    target.view = CD3DX12_CPU_DESCRIPTOR_HANDLE(rtvHeap->GetCPUDescriptorHandleForHeapStart(), INT(i), rtvDescriptorSize);

                    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;
                    ZeroMemory(&rtvDesc, sizeof(D3D12_RENDER_TARGET_VIEW_DESC));
                    rtvDesc.Format = rtDesc.Format;
                    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    device->CreateRenderTargetView(target.texture.Get(), &rtvDesc, target.view);
                }
                LOG("Found " << (header ? header->nStreams : 0) << " streams");
                continue;
            }
            else if (err == RS_ERROR_TIMEOUT)
            {
                continue;
            }
            else if (err == RS_ERROR_QUIT)
            {
                LOG("Exiting due to quit request.");
                return 0;
            }
            else if (err != RS_ERROR_SUCCESS)
            {
                LOG("rs_awaitFrameData returned " << err);
                break;
            }
        }

        // Respond to frame request
        const FrameData& frameData = std::get<FrameData>(awaitResult);
        const size_t numStreams = header ? header->nStreams : 0;
        for (size_t i = 0; i < numStreams; ++i)
        {
            const StreamDescription& description = header->streams[i];

            CameraResponseData cameraData;
            cameraData.tTracked = frameData.tTracked;
            try
            {
                cameraData.camera = rs.getFrameCamera(description.handle);
            }
            catch (const RenderStreamError& e)
            {
                // It's possible to race here and be processing a request
                // which uses data from before streams changed.
                // TODO: Fix this in the API dll
                if (e.error == RS_ERROR_NOTFOUND)
                    continue;
                throw;
            }

            {
                const RenderTarget& target = renderTargets.at(description.handle);
                const auto& pipelineState = pipelineStates[target.format];

                commandAllocator->Reset();
                commandList->Reset(commandAllocator.Get(), pipelineState.Get());

                const auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(target.texture.Get(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
                commandList->ResourceBarrier(1, &toRenderTarget);

                commandList->OMSetRenderTargets(1, &target.view, false, nullptr);
                commandList->ClearRenderTargetView(target.view, clearColour, 0, nullptr);

                D3D12_VIEWPORT viewport;
                ZeroMemory(&viewport, sizeof(D3D12_VIEWPORT));
                viewport.Width = static_cast<float>(description.width);
                viewport.Height = static_cast<float>(description.height);
                viewport.MinDepth = 0;
                viewport.MaxDepth = 1;
                commandList->RSSetViewports(1, &viewport);

                D3D12_RECT scissorRect = { 0,0, LONG(description.width), LONG(description.height)};
                commandList->RSSetScissorRects(1, &scissorRect);

                ConstantBufferStruct constantBufferData;
                const float angleDeg = float(frameData.localTime * 40);
                const float angleRad = DirectX::XMConvertToRadians(angleDeg);
                const DirectX::XMMATRIX world = DirectX::XMMatrixRotationRollPitchYaw(angleRad, angleRad, angleRad);

                const float pitch = -DirectX::XMConvertToRadians(cameraData.camera.rx);
                const float yaw = DirectX::XMConvertToRadians(cameraData.camera.ry);
                const float roll = -DirectX::XMConvertToRadians(cameraData.camera.rz);

                const DirectX::XMMATRIX cameraTranslation = DirectX::XMMatrixTranslation(cameraData.camera.x, cameraData.camera.y, cameraData.camera.z);
                const DirectX::XMMATRIX cameraRotation = DirectX::XMMatrixRotationRollPitchYaw(pitch, yaw, roll);
                const DirectX::XMMATRIX view = DirectX::XMMatrixInverse(nullptr, cameraTranslation) * DirectX::XMMatrixTranspose(cameraRotation);

                const float throwRatioH = cameraData.camera.focalLength / cameraData.camera.sensorX;
                const float throwRatioV = cameraData.camera.focalLength / cameraData.camera.sensorY;
                const float fovH = 2.0f * atan(0.5f / throwRatioH);
                const float fovV = 2.0f * atan(0.5f / throwRatioV);

                const bool orthographic = cameraData.camera.orthoWidth > 0.0f;
                const float cameraAspect = cameraData.camera.sensorX / cameraData.camera.sensorY;
                float imageHeight, imageWidth;
                if (orthographic)
                {
                    imageHeight = cameraData.camera.orthoWidth / cameraAspect;
                    imageWidth = cameraAspect * imageHeight;
                }
                else
                {
                    imageWidth = 2.0f * tan(0.5f * fovH);
                    imageHeight = 2.0f * tan(0.5f * fovV);
                }

                const DirectX::XMMATRIX overscan = DirectX::XMMatrixTranslation(cameraData.camera.cx, cameraData.camera.cy, 0.f);

                const float nearZ = cameraData.camera.nearZ;
                const float farZ = cameraData.camera.farZ;

                const float l = (-0.5f + description.clipping.left) * imageWidth;
                const float r = (-0.5f + description.clipping.right) * imageWidth;
                const float t = (-0.5f + 1.f - description.clipping.top) * imageHeight;
                const float b = (-0.5f + 1.f - description.clipping.bottom) * imageHeight;

                const DirectX::XMMATRIX projection = orthographic ? DirectX::XMMatrixOrthographicOffCenterLH(l, r, b, t, nearZ, farZ) : DirectX::XMMatrixPerspectiveOffCenterLH(l * nearZ, r * nearZ, b * nearZ, t * nearZ, nearZ, farZ);

                constantBufferData.worldViewProjection = DirectX::XMMatrixTranspose(world * view * projection * overscan);
                memcpy(cbUploadBufferPtr, &constantBufferData, sizeof(ConstantBufferStruct));

                // Draw cube
                UINT stride = sizeof(DirectX::XMFLOAT3);
                UINT offset = 0;
                commandList->SetGraphicsRootSignature(rootSignature.Get());
                commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
                commandList->IASetIndexBuffer(&indexBufferView);
                commandList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D12DescriptorHeap* descHeaps = { cbvHeap.Get() };
                commandList->SetDescriptorHeaps(1, &descHeaps);
                commandList->SetGraphicsRootDescriptorTable(0, cbvHeap->GetGPUDescriptorHandleForHeapStart());
                UINT startIndex = 0;
                for (UINT indexCount : cubeDrawCalls)
                {
                    commandList->DrawIndexedInstanced(indexCount, 1, startIndex, 0, 0);
                    startIndex += indexCount;
                }

                const auto toGenericBarrier = CD3DX12_RESOURCE_BARRIER::Transition(target.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
                commandList->ResourceBarrier(1, &toGenericBarrier);

                commandList->Close();
                ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
                commandQueue->ExecuteCommandLists(1, ppCommandLists);

                SenderFrameTypeData data;
                data.dx12.resource = target.texture.Get();

                FrameResponseData response = {};
                response.cameraData = &cameraData;
                rs.sendFrame(description.handle, RS_FRAMETYPE_DX12_TEXTURE, data, &response);
            }
        }
    }

    cbUploadBuffer->Unmap(0, nullptr);

    return 0;
}

int main()
{
    try
    {
        return mainImpl();
    }
    catch (const std::exception& e)
    {
        LOG("Error: " << e.what());
        return 99;
    }
}
