#include "mf-video-decoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <iostream>

#include <d3d10_1.h>
#include<d3d11.h>

#include<mferror.h>
#include<limits>

#include<cstring>

#include<vector>

extern "C"
{
#include<libavutil/pixfmt.h>
#include<libswscale/swscale.h>
}

#include<mfobjects.h>



using Microsoft::WRL::ComPtr;

struct MfVideoDecoder::Impl
{
    ComPtr<IMFTransform> transform;

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<IMFDXGIDeviceManager> deviceManager;

    ComPtr<IMFMediaType> inputMediaType;
    ComPtr<IMFMediaType> outputMediaType;

    DWORD inputStreamId = 0;
    DWORD outputStreamId = 0;

    MFT_OUTPUT_STREAM_INFO outputStreamInfo = {};

    VideoTimeBase timeBase;

    bool streamingStarted = false;

    UINT deviceResetToken = 0;

    bool mfStarted = false;
    bool comInitialized = false;

    GUID outputSubtype = GUID_NULL;
    UINT32 outputWidth = 0;
    UINT32 outputHeight = 0;

    UINT32 displayWidth = 0;
    UINT32 displayHeight = 0;

    ComPtr<ID3D11Texture2D>stagingTexture;

    SwsContext* scaleContext = nullptr;
    std::vector<unsigned char >bgraBuffer;

};

static const GUID* getMfInputSubtype(VideoCodec codec)
{
    switch (codec)
    {
    case VideoCodec::H264:
        return &MFVideoFormat_H264;

    case VideoCodec::Hevc:
        return &MFVideoFormat_HEVC;

    default:
        return nullptr;
    }
}

static void releaseActivations(IMFActivate** activations,UINT32 count)
{
    if (!activations)
        return;

    for (UINT32 i = 0; i < count; ++i)
    {
        if (activations[i])
            activations[i]->Release();
    }

    CoTaskMemFree(activations);
}

static void printMfError(const char* operation,HRESULT result)
{
    std::cerr
        << operation
        << " failed, HRESULT=0x"
        << std::hex
        << static_cast<unsigned long>(result)
        << std::dec
        << std::endl;
}

//ʱ���ת������
static LONGLONG toMfTime(std::int64_t value, const VideoTimeBase& timeBase)
{
    //MFʱ�䵥λΪ100ns
    const long double converted =
        static_cast<long double>(value) *
        timeBase.numerator *
        10000000.0L /
        timeBase.denominator;
    if (converted >
        std::numeric_limits<LONGLONG>::max())
    {
        return
            std::numeric_limits<LONGLONG>::max();
    }
    if (converted <
        std::numeric_limits<LONGLONG>::min())
    {
        return
            std::numeric_limits<LONGLONG>::min();
    }
    return static_cast<LONGLONG>(converted);
}

MfVideoDecoder::MfVideoDecoder() : impl_(std::make_unique<Impl>())
{
}

MfVideoDecoder::~MfVideoDecoder()
{
    close();
}

bool MfVideoDecoder::initialize(const VideoDecoderConfig& config)
{
    close();

    const GUID* inputSubtype =getMfInputSubtype(config.codec);

    if (!inputSubtype)
    {
        std::cerr<< "Unsupported MF video codec."<< std::endl;

        return false;
    }

    HRESULT result = CoInitializeEx(nullptr,COINIT_MULTITHREADED);

    if (SUCCEEDED(result))
    {
        impl_->comInitialized = true;
    }
    else if (result != RPC_E_CHANGED_MODE)
    {
        printMfError("CoInitializeEx", result);
        return false;
    }

    result = MFStartup(MF_VERSION,MFSTARTUP_FULL);

    if (FAILED(result))
    {
        printMfError("MFStartup", result);
        close();
        return false;
    }

    impl_->mfStarted = true;

    MFT_REGISTER_TYPE_INFO inputType = {};
    inputType.guidMajorType =MFMediaType_Video;
    inputType.guidSubtype =*inputSubtype;

    IMFActivate** activations = nullptr;
    UINT32 activationCount = 0;

    result = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT |
        MFT_ENUM_FLAG_ASYNCMFT |
        MFT_ENUM_FLAG_HARDWARE |
        MFT_ENUM_FLAG_SORTANDFILTER,
        &inputType,
        nullptr,
        &activations,
        &activationCount);

    if (FAILED(result))
    {
        printMfError("MFTEnumEx", result);
        close();
        return false;
    }

    std::cerr<< "Video decoder MFT count: "<< activationCount<< std::endl;

    for (UINT32 i = 0;i < activationCount;++i)
    {
        WCHAR* friendlyName = nullptr;
        UINT32 friendlyNameLength = 0;

        if (SUCCEEDED(
            activations[i]->
            GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,&friendlyName,&friendlyNameLength)))
        {
            std::wcout<< L"Found video decoder MFT: "<< friendlyName<< std::endl;

            CoTaskMemFree(friendlyName);
        }

        IMFTransform* transform = nullptr;

        result =activations[i]->ActivateObject(IID_PPV_ARGS(&transform));

        if (SUCCEEDED(result) && transform)
        {
            impl_->transform.Attach(transform);
            break;
        }
    }

    releaseActivations(activations,activationCount);

    if (!impl_->transform)
    {
        std::cerr<< "No video decoder MFT could be activated."<< std::endl;

        close();
        return false;
    }
    // Create a D3D11 device with video support.
    D3D_FEATURE_LEVEL featureLevel = {};
    result = D3D11CreateDevice(nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        impl_->d3dDevice.GetAddressOf(),
        &featureLevel,
        impl_->d3dContext.GetAddressOf());
        if (FAILED(result))
        {
            printMfError("D3D11CreateDevice", result);
            close();
            return false;
    }

        // The MFT and decoder thread can access this device concurrently.
        ComPtr<ID3D10Multithread> multithread;
        result = impl_->d3dDevice.As(&multithread);

        if (FAILED(result))
        {
            printMfError(
                "Query ID3D10Multithread",
                result);
            close();
            return false;
        }

        multithread->SetMultithreadProtected(TRUE);

        // Verify that the selected MFT supports D3D11.
        ComPtr<IMFAttributes> transformAttributes;

        result = impl_->transform->GetAttributes(
            transformAttributes.GetAddressOf());

        if (FAILED(result))
        {
            printMfError(
                "IMFTransform::GetAttributes",
                result);

            close();
            return false;
        }

        UINT32 d3d11Aware = FALSE;

        result = transformAttributes->GetUINT32(
            MF_SA_D3D11_AWARE,
            &d3d11Aware);

        if (FAILED(result) || !d3d11Aware)
        {
            std::cerr
                << "Selected MFT is not D3D11 aware."
                << std::endl;

            close();
            return false;
        }

        UINT32 asynchronous = FALSE;
        if (SUCCEEDED(transformAttributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous)) && asynchronous)
        {
            result = transformAttributes->SetUINT32(
                MF_TRANSFORM_ASYNC_UNLOCK,
                TRUE);
            if (FAILED(result))
            {
                printMfError(
                    "MF_TRANSFORM_ASYNC_UNLOCK",
                    result);
                close();
                return false;
            }
        }

        // Create and bind the DXGI device manager.
        result = MFCreateDXGIDeviceManager(
            &impl_->deviceResetToken,
            impl_->deviceManager.GetAddressOf());

        if (FAILED(result))
        {
            printMfError(
                "MFCreateDXGIDeviceManager",
                result);

            close();
            return false;
        }

        result = impl_->deviceManager->ResetDevice(
            impl_->d3dDevice.Get(),
            impl_->deviceResetToken);

        if (FAILED(result))
        {
            printMfError(
                "IMFDXGIDeviceManager::ResetDevice",
                result);

            close();
            return false;
        }

        result = impl_->transform->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(
                impl_->deviceManager.Get()));

        if (FAILED(result))
        {
            printMfError(
                "MFT_MESSAGE_SET_D3D_MANAGER",
                result);

            close();
            return false;
        }

        std::cerr
            << "D3D11-aware video MFT initialized."
            << std::endl;

        DWORD inputStreamCount = 0;
        DWORD outputStreamCount = 0;

        result = impl_->transform->GetStreamCount(
            &inputStreamCount,
            &outputStreamCount);

        if (FAILED(result) ||
            inputStreamCount != 1 ||
            outputStreamCount != 1)
        {
            printMfError(
                "IMFTransform::GetStreamCount",
                result);

            close();
            return false;
        }

        // Most decoder MFTs use default stream ID 0.
        result = impl_->transform->GetStreamIDs(
            1,
            &impl_->inputStreamId,
            1,
            &impl_->outputStreamId);

        if (result == E_NOTIMPL)
        {
            impl_->inputStreamId = 0;
            impl_->outputStreamId = 0;
        }
        else if (FAILED(result))
        {
            printMfError(
                "IMFTransform::GetStreamIDs",
                result);

            close();
            return false;
        }

        // Create the H.264 or H.265 input media type.
        result = MFCreateMediaType(
            impl_->inputMediaType.GetAddressOf());

        if (FAILED(result))
        {
            printMfError("MFCreateMediaType input", result);
            close();
            return false;
        }

        result = impl_->inputMediaType->SetGUID(
            MF_MT_MAJOR_TYPE,
            MFMediaType_Video);

        if (SUCCEEDED(result))
        {
            result = impl_->inputMediaType->SetGUID(
                MF_MT_SUBTYPE,
                *inputSubtype);
        }

        if (SUCCEEDED(result))
        {
            result = MFSetAttributeSize(
                impl_->inputMediaType.Get(),
                MF_MT_FRAME_SIZE,
                static_cast<UINT32>(config.width),
                static_cast<UINT32>(config.height));
        }

        if (SUCCEEDED(result))
        {
            result = impl_->inputMediaType->SetUINT32(
                MF_MT_INTERLACE_MODE,
                MFVideoInterlace_Progressive);
        }

        if (FAILED(result))
        {
            printMfError(
                "Configure MF input media type",
                result);

            close();
            return false;
        }

        // Pass H.264 SPS/PPS or H.265 VPS/SPS/PPS.
        if (!config.extraData.empty())
        {
            if (config.extraData.size() >
                std::numeric_limits<UINT32>::max())
            {
                std::cerr
                    << "MF codec extra data is too large."
                    << std::endl;

                close();
                return false;
            }

            result = impl_->inputMediaType->SetBlob(
                MF_MT_MPEG_SEQUENCE_HEADER,
                config.extraData.data(),
                static_cast<UINT32>(
                    config.extraData.size()));

            if (FAILED(result))
            {
                printMfError(
                    "MF_MT_MPEG_SEQUENCE_HEADER",
                    result);

                close();
                return false;
            }
        }

        result = impl_->transform->SetInputType(
            impl_->inputStreamId,
            impl_->inputMediaType.Get(),
            0);

        if (FAILED(result))
        {
            printMfError(
                "IMFTransform::SetInputType",
                result);

            close();
            return false;
        }

        std::cerr
            << "MF input type configured."
            << std::endl;

        // Prefer NV12 output and use P010 as a fallback.
        ComPtr<IMFMediaType> nv12Type;
        ComPtr<IMFMediaType> p010Type;

        for (DWORD typeIndex = 0;; ++typeIndex)
        {
            ComPtr<IMFMediaType> availableType;

            result =
                impl_->transform->GetOutputAvailableType(
                    impl_->outputStreamId,
                    typeIndex,
                    availableType.GetAddressOf());

            if (result == MF_E_NO_MORE_TYPES)
                break;

            if (FAILED(result))
            {
                printMfError(
                    "GetOutputAvailableType",
                    result);

                close();
                return false;
            }

            GUID subtype = GUID_NULL;

            if (FAILED(
                availableType->GetGUID(
                    MF_MT_SUBTYPE,
                    &subtype)))
            {
                continue;
            }

            if (subtype == MFVideoFormat_NV12 &&
                !nv12Type)
            {
                nv12Type = availableType;
            }
            else if (subtype == MFVideoFormat_P010 &&
                !p010Type)
            {
                p010Type = availableType;
            }
        }

        if (nv12Type)
        {
            impl_->outputMediaType = nv12Type;
        }
        else if (p010Type)
        {
            impl_->outputMediaType = p010Type;
        }
        else
        {
            std::cerr
                << "MF decoder has no NV12 or P010 output."
                << std::endl;

            close();
            return false;
        }

        result = impl_->transform->SetOutputType(
            impl_->outputStreamId,
            impl_->outputMediaType.Get(),
            0);

        if (FAILED(result))
        {
            printMfError(
                "IMFTransform::SetOutputType",
                result);

            close();
            return false;
        }

        //��ȡ�������Ϣ
        result = impl_->transform->GetOutputStreamInfo(
            impl_->outputStreamId,
            &impl_->outputStreamInfo);

        if (FAILED(result))
        {
            printMfError(
                "IMFTransform::GetOutputStreamInfo",
                result);

            close();
            return false;
        }

        const bool providesSamples =
            (impl_->outputStreamInfo.dwFlags &
                MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

        const bool canProvideSamples =
            (impl_->outputStreamInfo.dwFlags &
                MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES) != 0;

        std::cerr
            << "MF output stream: flags=0x"
            << std::hex
            << impl_->outputStreamInfo.dwFlags
            << std::dec
            << ", size="
            << impl_->outputStreamInfo.cbSize
            << ", providesSamples="
            << providesSamples
            << ", canProvideSamples="
            << canProvideSamples
            << std::endl;

        GUID selectedOutputSubtype = GUID_NULL;

        impl_->outputMediaType->GetGUID(
            MF_MT_SUBTYPE,
            &selectedOutputSubtype);

        //���������ʽ
        impl_->outputSubtype = selectedOutputSubtype;

        result = MFGetAttributeSize(
            impl_->outputMediaType.Get(),
            MF_MT_FRAME_SIZE,
            &impl_->outputWidth,
            &impl_->outputHeight);

        if (FAILED(result))
        {
            impl_->outputWidth =
                static_cast<UINT32>(config.width);
            impl_->outputHeight =
                static_cast<UINT32>(config.height);
        }

        std::cerr
            << "MF output type configured: "
            << (selectedOutputSubtype ==
                MFVideoFormat_NV12
                ? "NV12"
                : "P010")
            << std::endl;

        result = impl_->transform->ProcessMessage(
            MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,
            0);

        if (SUCCEEDED(result))
        {
            result = impl_->transform->ProcessMessage(
                MFT_MESSAGE_NOTIFY_START_OF_STREAM,
                0);
        }

        if (FAILED(result))
        {
            printMfError(
                "Start MF decoder streaming",
                result);

            close();
            return false;
        }
        impl_->displayWidth =
            static_cast<UINT32>(config.width);

        impl_->displayHeight =
            static_cast<UINT32>(config.height);

        impl_->streamingStarted = true;
        impl_->timeBase = config.timeBase;
    return true;
}

VideoDecoderResult MfVideoDecoder::sendPacket(
    const EncodedVideoPacket& packet)
{
    if (!impl_ ||
        !impl_->transform ||
        !packet.data ||
        packet.size == 0 ||
        packet.size >
            std::numeric_limits<DWORD>::max())
    {
        return VideoDecoderResult::Error;
    }

    ComPtr<IMFSample> sample;
    HRESULT result = MFCreateSample(
        sample.GetAddressOf());

    if (FAILED(result))
    {
        printMfError("MFCreateSample", result);
        return VideoDecoderResult::Error;
    }

    ComPtr<IMFMediaBuffer> buffer;

    result = MFCreateMemoryBuffer(
        static_cast<DWORD>(packet.size),
        buffer.GetAddressOf());

    if (FAILED(result))
    {
        printMfError(
            "MFCreateMemoryBuffer",
            result);

        return VideoDecoderResult::Error;
    }

    BYTE* destination = nullptr;
    DWORD maximumLength = 0;
    DWORD currentLength = 0;

    result = buffer->Lock(
        &destination,
        &maximumLength,
        &currentLength);

    if (FAILED(result))
    {
        printMfError(
            "IMFMediaBuffer::Lock",
            result);

        return VideoDecoderResult::Error;
    }

    std::memcpy(
        destination,
        packet.data,
        packet.size);

    buffer->Unlock();

    result = buffer->SetCurrentLength(
        static_cast<DWORD>(packet.size));

    if (FAILED(result))
    {
        printMfError(
            "SetCurrentLength",
            result);

        return VideoDecoderResult::Error;
    }

    result = sample->AddBuffer(buffer.Get());

    if (FAILED(result))
    {
        printMfError(
            "IMFSample::AddBuffer",
            result);

        return VideoDecoderResult::Error;
    }

    if (packet.hasPts)
    {
        result = sample->SetSampleTime(
            toMfTime(
                packet.pts,
                impl_->timeBase));

        if (FAILED(result))
        {
            printMfError(
                "SetSampleTime",
                result);

            return VideoDecoderResult::Error;
        }
    }

    if (packet.duration > 0)
    {
        result = sample->SetSampleDuration(
            toMfTime(
                packet.duration,
                impl_->timeBase));

        if (FAILED(result))
        {
            printMfError(
                "SetSampleDuration",
                result);

            return VideoDecoderResult::Error;
        }
    }

    if (packet.keyFrame)
    {
        sample->SetUINT32(
            MFSampleExtension_CleanPoint,
            TRUE);
    }

    result = impl_->transform->ProcessInput(
        impl_->inputStreamId,
        sample.Get(),
        0);

    if (result == MF_E_NOTACCEPTING)
    {
        return VideoDecoderResult::TryAgain;
    }

    if (FAILED(result))
    {
        printMfError(
            "IMFTransform::ProcessInput",
            result);

    return VideoDecoderResult::Error;
}

    return VideoDecoderResult::Ok;
}
VideoDecoderResult MfVideoDecoder::receiveFrame(
    DecodedVideoFrame* frame)
{
    if (!impl_ ||
        !impl_->transform ||
        !frame ||
        impl_->outputWidth == 0 ||
        impl_->outputHeight == 0)
    {
        return VideoDecoderResult::Error;
    }

    MFT_OUTPUT_DATA_BUFFER outputData = {};
    outputData.dwStreamID =
        impl_->outputStreamId;

    DWORD outputStatus = 0;

    HRESULT result =
        impl_->transform->ProcessOutput(
            0,
            1,
            &outputData,
            &outputStatus);

    ComPtr<IMFCollection> outputEvents;

    if (outputData.pEvents)
        outputEvents.Attach(outputData.pEvents);
    //提前接管输出Sample
    ComPtr<IMFSample> outputSample;

    if (outputData.pSample)
        outputSample.Attach(outputData.pSample);

    if (result ==
        MF_E_TRANSFORM_NEED_MORE_INPUT)
    {
        return VideoDecoderResult::TryAgain;
    }

    if (result ==
        MF_E_TRANSFORM_STREAM_CHANGE)
    {
        std::cerr
            << "MF output stream changed, "
            "renegotiating."
            << std::endl;

        ComPtr<IMFMediaType> changedType;
        GUID changedSubtype = GUID_NULL;

        for (DWORD typeIndex = 0;; ++typeIndex)
        {
            ComPtr<IMFMediaType> availableType;

            HRESULT typeResult =
                impl_->transform->
                GetOutputAvailableType(
                    impl_->outputStreamId,
                    typeIndex,
                    availableType.GetAddressOf());

            if (typeResult == MF_E_NO_MORE_TYPES)
                break;

            if (FAILED(typeResult))
            {
                printMfError(
                    "GetOutputAvailableType after change",
                    typeResult);

                return VideoDecoderResult::Error;
            }

            GUID subtype = GUID_NULL;

            if (FAILED(
                availableType->GetGUID(
                    MF_MT_SUBTYPE,
                    &subtype)))
            {
                continue;
            }

            if (subtype != MFVideoFormat_NV12 &&
                subtype != MFVideoFormat_P010)
            {
                continue;
            }

            typeResult =
                impl_->transform->SetOutputType(
                    impl_->outputStreamId,
                    availableType.Get(),
                    0);

            if (SUCCEEDED(typeResult))
            {
                changedType = availableType;
                changedSubtype = subtype;
                break;
            }
        }

        if (!changedType)
        {
            std::cerr
                << "Failed to renegotiate MF output type."
                << std::endl;

            return VideoDecoderResult::Error;
        }

        impl_->outputMediaType = changedType;
        impl_->outputSubtype = changedSubtype;

        UINT32 changedWidth = 0;
        UINT32 changedHeight = 0;

        if (SUCCEEDED(
            MFGetAttributeSize(
                changedType.Get(),
                MF_MT_FRAME_SIZE,
                &changedWidth,
                &changedHeight)))
        {
            impl_->outputWidth = changedWidth;
            impl_->outputHeight = changedHeight;
        }

        HRESULT infoResult =
            impl_->transform->GetOutputStreamInfo(
                impl_->outputStreamId,
                &impl_->outputStreamInfo);

        if (FAILED(infoResult))
        {
            printMfError(
                "GetOutputStreamInfo after change",
                infoResult);

            return VideoDecoderResult::Error;
        }

        std::cerr
            << "MF output renegotiated: "
            << (changedSubtype == MFVideoFormat_NV12
                ? "NV12"
                : "P010")
            << ", "
            << impl_->outputWidth
            << "x"
            << impl_->outputHeight
            << std::endl;

        // 下一次receiveFrame再次调用ProcessOutput。
        return VideoDecoderResult::TryAgain;
    }

    if (FAILED(result))
    {
        printMfError(
            "IMFTransform::ProcessOutput",
            result);

        return VideoDecoderResult::Error;
    }

    if (!outputSample)
    {
        std::cerr
            << "MF ProcessOutput returned no sample."
            << std::endl;

        return VideoDecoderResult::Error;
    }


    ComPtr<IMFMediaBuffer> mediaBuffer;

    result = outputSample->GetBufferByIndex(
        0,
        mediaBuffer.GetAddressOf());

    if (FAILED(result))
    {
        printMfError(
            "IMFSample::GetBufferByIndex",
            result);

        return VideoDecoderResult::Error;
    }

    ComPtr<IMFDXGIBuffer> dxgiBuffer;

    result = mediaBuffer.As(&dxgiBuffer);

    if (FAILED(result))
    {
        printMfError(
            "Query IMFDXGIBuffer",
            result);

        return VideoDecoderResult::Error;
    }

    ComPtr<ID3D11Texture2D> sourceTexture;

    result = dxgiBuffer->GetResource(
        IID_PPV_ARGS(
            sourceTexture.GetAddressOf()));

    if (FAILED(result))
    {
        printMfError(
            "IMFDXGIBuffer::GetResource",
            result);

        return VideoDecoderResult::Error;
    }

    UINT sourceSubresource = 0;

    result = dxgiBuffer->GetSubresourceIndex(
        &sourceSubresource);

    if (FAILED(result))
    {
        printMfError(
            "GetSubresourceIndex",
            result);

        return VideoDecoderResult::Error;
    }

    D3D11_TEXTURE2D_DESC sourceDescription = {};
    sourceTexture->GetDesc(&sourceDescription);

    bool recreateStaging =
        !impl_->stagingTexture;

    if (impl_->stagingTexture)
    {
        D3D11_TEXTURE2D_DESC stagingDescription = {};
        impl_->stagingTexture->GetDesc(
            &stagingDescription);

        recreateStaging =
            stagingDescription.Width !=
            sourceDescription.Width ||
            stagingDescription.Height !=
            sourceDescription.Height ||
            stagingDescription.Format !=
            sourceDescription.Format;
    }

    if (recreateStaging)
    {
        impl_->stagingTexture.Reset();

        D3D11_TEXTURE2D_DESC stagingDescription = {};
        stagingDescription.Width =
            sourceDescription.Width;
        stagingDescription.Height =
            sourceDescription.Height;
        stagingDescription.MipLevels = 1;
        stagingDescription.ArraySize = 1;
        stagingDescription.Format =
            sourceDescription.Format;
        stagingDescription.SampleDesc.Count = 1;
        stagingDescription.Usage =
            D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags =
            D3D11_CPU_ACCESS_READ;
        stagingDescription.MiscFlags = 0;

        result = impl_->d3dDevice->CreateTexture2D(
            &stagingDescription,
            nullptr,
            impl_->stagingTexture.GetAddressOf());

        if (FAILED(result))
        {
            printMfError(
                "Create staging texture",
                result);

            return VideoDecoderResult::Error;
        }
    }

    // 立即将当前解码表面复制到独立的STAGING纹理，
    // 防止MFT复用输出表面后读取到旧帧。
    impl_->d3dContext->CopySubresourceRegion(
        impl_->stagingTexture.Get(),
        0,
        0,
        0,
        0,
        sourceTexture.Get(),
        sourceSubresource,
        nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped = {};

    result = impl_->d3dContext->Map(
        impl_->stagingTexture.Get(),
        0,
        D3D11_MAP_READ,
        0,
        &mapped);

    if (FAILED(result))
    {
        printMfError(
            "Map staging texture",
            result);

        return VideoDecoderResult::Error;
    }

    if (!mapped.pData ||
        mapped.RowPitch >
        static_cast<UINT>(
            std::numeric_limits<int>::max()))
    {
        impl_->d3dContext->Unmap(
            impl_->stagingTexture.Get(),
            0);

        return VideoDecoderResult::Error;
    }

    const int width =
        static_cast<int>(
            impl_->displayWidth);

    const int height =
        static_cast<int>(
            impl_->displayHeight);

    const int codedHeight =
        static_cast<int>(
            sourceDescription.Height);

    if (width <= 0 ||
        height <= 0 ||
        width >
        static_cast<int>(
            sourceDescription.Width) ||
        height > codedHeight ||
        width >
        std::numeric_limits<int>::max() / 4)
    {
        impl_->d3dContext->Unmap(
            impl_->stagingTexture.Get(),
            0);

        return VideoDecoderResult::Error;
    }

    AVPixelFormat sourceFormat;

    if (impl_->outputSubtype ==
        MFVideoFormat_NV12)
    {
        sourceFormat = AV_PIX_FMT_NV12;
    }
    else if (impl_->outputSubtype ==
        MFVideoFormat_P010)
    {
        sourceFormat = AV_PIX_FMT_P010LE;
    }
    else
    {
        impl_->d3dContext->Unmap(
            impl_->stagingTexture.Get(),
            0);

        return VideoDecoderResult::Error;
    }

    impl_->scaleContext =
        sws_getCachedContext(
            impl_->scaleContext,
            width,
            height,
            sourceFormat,
            width,
            height,
            AV_PIX_FMT_BGRA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);

    if (!impl_->scaleContext)
    {
        impl_->d3dContext->Unmap(
            impl_->stagingTexture.Get(),
            0);

        return VideoDecoderResult::Error;
    }

    const int sourcePitch =
        static_cast<int>(mapped.RowPitch);

    const int destinationStride =
        width * 4;

    impl_->bgraBuffer.resize(
        static_cast<std::size_t>(
            destinationStride) *
        height);

    // UV平面位于完整编码表面Y平面之后，
    // 因此这里必须使用codedHeight，而不是displayHeight。
    const std::size_t chromaOffset =
        static_cast<std::size_t>(
            mapped.RowPitch) *
        codedHeight;

    const std::uint8_t* mappedBytes =
        static_cast<const std::uint8_t*>(
            mapped.pData);

    const std::uint8_t* sourceData[4] = {
        mappedBytes,
        mappedBytes + chromaOffset,
        nullptr,
        nullptr
    };

    const int sourceStrides[4] = {
        sourcePitch,
        sourcePitch,
        0,
        0
    };

    std::uint8_t* destinationData[4] = {
        impl_->bgraBuffer.data(),
        nullptr,
        nullptr,
        nullptr
    };

    const int destinationStrides[4] = {
        destinationStride,
        0,
        0,
        0
    };

    const int convertedHeight = sws_scale(
        impl_->scaleContext,
        sourceData,
        sourceStrides,
        0,
        height,
        destinationData,
        destinationStrides);

    impl_->d3dContext->Unmap(
        impl_->stagingTexture.Get(),
        0);

    if (convertedHeight != height)
    {
        std::cerr
            << "Failed to convert staging texture."
            << std::endl;

    return VideoDecoderResult::Error;
}

    LONGLONG sampleTime = 0;

    if (SUCCEEDED(
        outputSample->GetSampleTime(
            &sampleTime)))
    {
        frame->ptsSeconds =
            static_cast<double>(sampleTime) /
            10000000.0;
    }
    else
    {
        frame->ptsSeconds = -1.0;
    }

    frame->data = impl_->bgraBuffer.data();
    frame->width = width;
    frame->height = height;
    frame->stride = destinationStride;

    return VideoDecoderResult::FrameReady;
}
VideoDecoderResult MfVideoDecoder::drain()
{
    if (!impl_ || !impl_->transform)
        return VideoDecoderResult::Error;

    HRESULT result =
        impl_->transform->ProcessMessage(
            MFT_MESSAGE_NOTIFY_END_OF_STREAM,
            0);

    if (FAILED(result))
    {
        printMfError(
            "MFT_MESSAGE_NOTIFY_END_OF_STREAM",
            result);
        return VideoDecoderResult::Error;
    }

    result = impl_->transform->ProcessMessage(
        MFT_MESSAGE_COMMAND_DRAIN,
        0);

    if (FAILED(result))
    {
        printMfError(
            "MFT_MESSAGE_COMMAND_DRAIN",
            result);
    return VideoDecoderResult::Error;
    }

    return VideoDecoderResult::Ok;
}

void MfVideoDecoder::flush()
{
    if (impl_ && impl_->transform)
    {
        impl_->transform->ProcessMessage(
            MFT_MESSAGE_COMMAND_FLUSH,
            0);
    }
}

void MfVideoDecoder::close()
{
    if (!impl_)
        return;


    if (impl_->transform && impl_->streamingStarted)
    {
        impl_->transform->ProcessMessage(
            MFT_MESSAGE_NOTIFY_END_OF_STREAM,
            0);

        impl_->transform->ProcessMessage(
            MFT_MESSAGE_NOTIFY_END_STREAMING,
            0);
    }

    if (impl_->scaleContext)
    {
        sws_freeContext(impl_->scaleContext);
        impl_->scaleContext = nullptr;
    }
    impl_->bgraBuffer.clear();
    impl_->outputSubtype = GUID_NULL;
    impl_->outputWidth = 0;
    impl_->outputHeight = 0;

    impl_->stagingTexture.Reset();

    impl_->displayWidth = 0;
    impl_->displayHeight = 0;

    impl_->streamingStarted = false;
    impl_->inputMediaType.Reset();
    impl_->outputMediaType.Reset();
    impl_->inputStreamId = 0;
    impl_->outputStreamId = 0;
    impl_->outputStreamInfo = {};
    impl_->timeBase = {};

    impl_->transform.Reset();
    impl_->deviceManager.Reset();
    impl_->d3dContext.Reset();
    impl_->d3dDevice.Reset();
    impl_->deviceResetToken = 0;

    if (impl_->mfStarted)
    {
        MFShutdown();
        impl_->mfStarted = false;
    }

    if (impl_->comInitialized)
    {
        CoUninitialize();
        impl_->comInitialized = false;
    }
}