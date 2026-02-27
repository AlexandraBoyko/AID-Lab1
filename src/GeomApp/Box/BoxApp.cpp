

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <chrono>
#include <array>
#include <memory>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;


class Logger {
public:
    enum Level { Info, Warning, Error };

    static void Log(Level level, const std::string& message) {
        std::ofstream logFile("engine.log", std::ios::app);
        if (!logFile.is_open()) return;

        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);
        char timeStr[26];
        ctime_s(timeStr, sizeof(timeStr), &now_time);
        timeStr[24] = '\0';

        std::string levelStr;
        switch (level) {
        case Info:  levelStr = "INFO"; break;
        case Warning: levelStr = "WARN"; break;
        case Error: levelStr = "ERROR"; break;
        }

        logFile << "[" << timeStr << "][" << levelStr << "] " << message << std::endl;
        logFile.close();


        std::cout << "[" << levelStr << "] " << message << std::endl;
    }
};

class IRenderAdapter {
public:
    virtual ~IRenderAdapter() = default;
    virtual bool Initialize(HWND hwnd, int width, int height) = 0;
    virtual void Resize(int width, int height) = 0;
    virtual void BeginFrame() = 0;
    virtual void DrawTriangle(const XMFLOAT4X4& worldViewProj, const XMFLOAT4& color) = 0;
    virtual void EndFrame() = 0;
    virtual void Cleanup() = 0;
};


struct Vertex {
    XMFLOAT3 Pos;
    XMFLOAT4 Color;
};

struct ObjectConstants {
    XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
};

class DX12RenderAdapter : public IRenderAdapter {
public:
    DX12RenderAdapter(ID3D12Device* device, ID3D12CommandQueue* cmdQueue, ID3D12GraphicsCommandList* cmdList)
        : md3dDevice(device), mCommandQueue(cmdQueue), mCommandList(cmdList) {
        Logger::Log(Logger::Info, "DX12RenderAdapter created");
    }

    virtual bool Initialize(HWND hwnd, int width, int height) override {
        Logger::Log(Logger::Info, "Initializing DX12RenderAdapter");

        mClientWidth = width;
        mClientHeight = height;

        D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc = {};
        cbvHeapDesc.NumDescriptors = 1;
        cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        cbvHeapDesc.NodeMask = 0;
        HRESULT hr = md3dDevice->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&mCbvHeap));
        if (FAILED(hr)) {
            Logger::Log(Logger::Error, "Failed to create CBV descriptor heap");
            return false;
        }

        mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(md3dDevice.Get(), 1, true);

        UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
        D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mObjectCB->Resource()->GetGPUVirtualAddress();
        cbAddress += 0 * objCBByteSize;

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = cbAddress;
        cbvDesc.SizeInBytes = objCBByteSize;
        md3dDevice->CreateConstantBufferView(&cbvDesc, mCbvHeap->GetCPUDescriptorHandleForHeapStart());

        BuildRootSignature();
        BuildShadersAndInputLayout();
        BuildTriangleGeometry();
        BuildPSO();

        Logger::Log(Logger::Info, "DX12RenderAdapter initialized successfully");
        return true;
    }

    virtual void Resize(int width, int height) override {
        mClientWidth = width;
        mClientHeight = height;
    }

    virtual void BeginFrame() override {

    }

    virtual void DrawTriangle(const XMFLOAT4X4& worldViewProj, const XMFLOAT4& color) override {
        mCommandList->SetPipelineState(mPSO.Get());


        ObjectConstants objConstants;
        XMStoreFloat4x4(&objConstants.WorldViewProj, XMMatrixTranspose(XMLoadFloat4x4(&worldViewProj)));
        mObjectCB->CopyData(0, objConstants);


        ID3D12DescriptorHeap* heaps[] = { mCbvHeap.Get() };
        mCommandList->SetDescriptorHeaps(1, heaps);

        mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
        mCommandList->SetGraphicsRootDescriptorTable(0, mCbvHeap->GetGPUDescriptorHandleForHeapStart());


        mCommandList->IASetVertexBuffers(0, 1, &mTriangleGeo->VertexBufferView());
        mCommandList->IASetIndexBuffer(&mTriangleGeo->IndexBufferView());
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);


        mCommandList->DrawIndexedInstanced(mTriangleGeo->DrawArgs["triangle"].IndexCount, 1, 0, 0, 0);
    }

    virtual void EndFrame() override {

    }

    virtual void Cleanup() override {
        Logger::Log(Logger::Info, "Cleaning up DX12RenderAdapter");

    }

private:
    void BuildRootSignature() {
        CD3DX12_ROOT_PARAMETER slotRootParameter[1];
        CD3DX12_DESCRIPTOR_RANGE cbvTable;
        cbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        slotRootParameter[0].InitAsDescriptorTable(1, &cbvTable);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(1, slotRootParameter, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serializedRootSig = nullptr;
        ComPtr<ID3DBlob> errorBlob = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

        if (errorBlob != nullptr) {
            std::string errStr = (char*)errorBlob->GetBufferPointer();
            Logger::Log(Logger::Error, "Root signature error: " + errStr);
        }
        ThrowIfFailed(hr);

        ThrowIfFailed(md3dDevice->CreateRootSignature(0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(&mRootSignature)));
    }

    void BuildShadersAndInputLayout() {
        mvsByteCode = d3dUtil::CompileShader(L"Shaders\\color.hlsl", nullptr, "VS", "vs_5_0");
        mpsByteCode = d3dUtil::CompileShader(L"Shaders\\color.hlsl", nullptr, "PS", "ps_5_0");

        mInputLayout = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
    }

    void BuildTriangleGeometry() {
        float size = 200.0f;
        float centerX = mClientWidth * 0.5f;
        float centerY = mClientHeight * 0.5f;

        std::array<Vertex, 3> vertices = {
            Vertex({ XMFLOAT3(centerX, centerY - size / 2, 0.0f), XMFLOAT4(Colors::Red) }),
            Vertex({ XMFLOAT3(centerX - size / 2, centerY + size / 2, 0.0f), XMFLOAT4(Colors::Red) }),
            Vertex({ XMFLOAT3(centerX + size / 2, centerY + size / 2, 0.0f), XMFLOAT4(Colors::Red) })
        };

        std::array<std::uint16_t, 3> indices = { 0, 1, 2 };

        const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
        const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

        mTriangleGeo = std::make_unique<MeshGeometry>();
        mTriangleGeo->Name = "triangleGeo";

        ThrowIfFailed(D3DCreateBlob(vbByteSize, &mTriangleGeo->VertexBufferCPU));
        CopyMemory(mTriangleGeo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

        ThrowIfFailed(D3DCreateBlob(ibByteSize, &mTriangleGeo->IndexBufferCPU));
        CopyMemory(mTriangleGeo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

        mTriangleGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
            mCommandList.Get(), vertices.data(), vbByteSize, mTriangleGeo->VertexBufferUploader);

        mTriangleGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
            mCommandList.Get(), indices.data(), ibByteSize, mTriangleGeo->IndexBufferUploader);

        mTriangleGeo->VertexByteStride = sizeof(Vertex);
        mTriangleGeo->VertexBufferByteSize = vbByteSize;
        mTriangleGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
        mTriangleGeo->IndexBufferByteSize = ibByteSize;

        SubmeshGeometry submesh;
        submesh.IndexCount = (UINT)indices.size();
        submesh.StartIndexLocation = 0;
        submesh.BaseVertexLocation = 0;

        mTriangleGeo->DrawArgs["triangle"] = submesh;
    }

    void BuildPSO() {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

        psoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
        psoDesc.pRootSignature = mRootSignature.Get();
        psoDesc.VS = { reinterpret_cast<BYTE*>(mvsByteCode->GetBufferPointer()), mvsByteCode->GetBufferSize() };
        psoDesc.PS = { reinterpret_cast<BYTE*>(mpsByteCode->GetBufferPointer()), mpsByteCode->GetBufferSize() };


        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;


        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // Предполагаем такой формат
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleDesc.Quality = 0;
        psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT; // Типичный формат глубины

        ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));
    }

private:
    ComPtr<ID3D12Device> md3dDevice;
    ComPtr<ID3D12CommandQueue> mCommandQueue;
    ComPtr<ID3D12GraphicsCommandList> mCommandList;

    int mClientWidth = 0;
    int mClientHeight = 0;

    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12DescriptorHeap> mCbvHeap;
    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB;
    std::unique_ptr<MeshGeometry> mTriangleGeo;
    ComPtr<ID3DBlob> mvsByteCode;
    ComPtr<ID3DBlob> mpsByteCode;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;
    ComPtr<ID3D12PipelineState> mPSO;
};


class InputManager {
public:
    static InputManager& Get() {
        static InputManager instance;
        return instance;
    }

    void Update() {

        for (int key = 0; key < 256; ++key) {
            mKeyState[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
        }

        POINT p;
        GetCursorPos(&p);
        ScreenToClient(GetActiveWindow(), &p);
        mMouseX = p.x;
        mMouseY = p.y;

        mLeftButton = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        mRightButton = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;


    }

    bool IsKeyDown(int vkCode) const {
        if (vkCode < 0 || vkCode >= 256) return false;
        return mKeyState[vkCode];
    }

    bool IsMouseLeftDown() const { return mLeftButton; }
    bool IsMouseRightDown() const { return mRightButton; }
    int GetMouseX() const { return mMouseX; }
    int GetMouseY() const { return mMouseY; }


    int GetMouseDeltaX() const { return 0; }
    int GetMouseDeltaY() const { return 0; }

private:
    InputManager() {
        ZeroMemory(mKeyState, sizeof(mKeyState));
        Logger::Log(Logger::Info, "InputManager created");
    }
    ~InputManager() = default;

    bool mKeyState[256];
    int mMouseX = 0, mMouseY = 0;
    bool mLeftButton = false;
    bool mRightButton = false;
};


class GameState {
public:
    virtual ~GameState() = default;
    virtual void Enter() = 0;
    virtual void Update(float deltaTime) = 0;
    virtual void Draw(IRenderAdapter* adapter) = 0;
    virtual void Exit() = 0;
};

class GameplayState : public GameState {
public:
    GameplayState() {
        Logger::Log(Logger::Info, "GameplayState constructed");
    }

    virtual void Enter() override {
        Logger::Log(Logger::Info, "Entering GameplayState");

        mPosition = XMFLOAT2(0.0f, 0.0f);
        mScale = 1.0f;
        mAngle = 0.0f;
    }

    virtual void Update(float deltaTime) override {
        auto& input = InputManager::Get();

        float moveSpeed = 200.0f;
        if (input.IsKeyDown('W') || input.IsKeyDown(VK_UP)) mPosition.y -= moveSpeed * deltaTime;
        if (input.IsKeyDown('S') || input.IsKeyDown(VK_DOWN)) mPosition.y += moveSpeed * deltaTime;
        if (input.IsKeyDown('A') || input.IsKeyDown(VK_LEFT)) mPosition.x -= moveSpeed * deltaTime;
        if (input.IsKeyDown('D') || input.IsKeyDown(VK_RIGHT)) mPosition.x += moveSpeed * deltaTime;


        if (input.IsMouseLeftDown()) {
            mScale += 0.01f;
        }
        if (input.IsMouseRightDown()) {
            mAngle += 0.02f;
        }
    }

    virtual void Draw(IRenderAdapter* adapter) override {

        XMMATRIX world = XMMatrixTranslation(mPosition.x, mPosition.y, 0.0f) *
            XMMatrixRotationZ(mAngle) *
            XMMatrixScaling(mScale, mScale, 1.0f);
        XMMATRIX view = XMMatrixIdentity();
        XMMATRIX proj = XMMatrixOrthographicOffCenterLH(0.0f, 800.0f, 600.0f, 0.0f, 0.0f, 1.0f);
        XMMATRIX wvp = world * view * proj;
        XMFLOAT4X4 wvpMat;
        XMStoreFloat4x4(&wvpMat, wvp);

        adapter->DrawTriangle(wvpMat, XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f));
    }

    virtual void Exit() override {
        Logger::Log(Logger::Info, "Exiting GameplayState");
    }

private:
    XMFLOAT2 mPosition;
    float mScale;
    float mAngle;
};


class MenuState : public GameState {
public:
    virtual void Enter() override {
        Logger::Log(Logger::Info, "Entering MenuState");
    }
    virtual void Update(float deltaTime) override {

        if (GetAsyncKeyState(VK_RETURN) & 0x8000) {

        }
    }
    virtual void Draw(IRenderAdapter* adapter) override {

    }
    virtual void Exit() override {
        Logger::Log(Logger::Info, "Exiting MenuState");
    }
};


class BoxApp : public D3DApp {
public:
    BoxApp(HINSTANCE hInstance);
    BoxApp(const BoxApp& rhs) = delete;
    BoxApp& operator=(const BoxApp& rhs) = delete;
    ~BoxApp();

    virtual bool Initialize() override;

private:
    virtual void OnResize() override;
    virtual void Update(const GameTimer& gt) override;
    virtual void Draw(const GameTimer& gt) override;


    virtual void OnMouseDown(WPARAM btnState, int x, int y) override {

    }
    virtual void OnMouseUp(WPARAM btnState, int x, int y) override {}
    virtual void OnMouseMove(WPARAM btnState, int x, int y) override {}

private:
    std::unique_ptr<IRenderAdapter> mRenderAdapter;
    std::unique_ptr<GameState> mCurrentState;
    XMFLOAT4X4 mView;
    XMFLOAT4X4 mProj;
};


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance, PSTR cmdLine, int showCmd) {
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try {
        BoxApp theApp(hInstance);
        if (!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch (DxException& e) {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

BoxApp::BoxApp(HINSTANCE hInstance) : D3DApp(hInstance) {
    Logger::Log(Logger::Info, "BoxApp created");
}

BoxApp::~BoxApp() {
    if (mRenderAdapter) mRenderAdapter->Cleanup();
    Logger::Log(Logger::Info, "BoxApp destroyed");
}

bool BoxApp::Initialize() {
    Logger::Log(Logger::Info, "BoxApp initializing");

    if (!D3DApp::Initialize())
        return false;
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));


    mRenderAdapter = std::make_unique<DX12RenderAdapter>(md3dDevice.Get(), mCommandQueue.Get(), mCommandList.Get());
    if (!mRenderAdapter->Initialize(mhMainWnd, mClientWidth, mClientHeight)) {
        Logger::Log(Logger::Error, "Failed to initialize render adapter");
        return false;
    }

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    FlushCommandQueue();


    mCurrentState = std::make_unique<GameplayState>();
    mCurrentState->Enter();


    XMStoreFloat4x4(&mView, XMMatrixIdentity());
    XMStoreFloat4x4(&mProj, XMMatrixOrthographicOffCenterLH(0.0f, (float)mClientWidth,
        (float)mClientHeight, 0.0f, 0.0f, 1.0f));

    Logger::Log(Logger::Info, "BoxApp initialized successfully");
    return true;
}

void BoxApp::OnResize() {
    D3DApp::OnResize();


    XMMATRIX P = XMMatrixOrthographicOffCenterLH(0.0f, (float)mClientWidth,
        (float)mClientHeight, 0.0f, 0.0f, 1.0f);
    XMStoreFloat4x4(&mProj, P);

    if (mRenderAdapter) {
        mRenderAdapter->Resize(mClientWidth, mClientHeight);
    }
}

void BoxApp::Update(const GameTimer& gt) {

    InputManager::Get().Update();


    if (mCurrentState) {
        mCurrentState->Update(gt.DeltaTime());
    }
}

void BoxApp::Draw(const GameTimer& gt) {

    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));


    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);


    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());


    if (mCurrentState) {
        mCurrentState->Draw(mRenderAdapter.get());
    }


    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));


    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    FlushCommandQueue();
}