#define NOMINMAX
#include <windows.h>

#include <d3d9.h>
#include <Dxva2api.h>
#include <evr.h>

#include <mfapi.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <wmcodecdsp.h>

#include <wlanapi.h>

#include <wrl.h>
using Microsoft::WRL::ComPtr;

#include <iostream>
#include <string_view>

#include <ReadBarcode.h> // zxing-cpp

#define VIDEO_WIDTH 640
#define VIDEO_HEIGHT 480
#define VIDEO_FRAME_RATE 30
#define WEBCAM_DEVICE_INDEX 0 // Adjust according to desired video capture device.

#define CHECK(v, msg)     if (v == NULL)                          { printf("%s\n", msg); abort(); }
#define CHECK_DW(op)      if (DWORD dw = op; dw != ERROR_SUCCESS) { printf(#op); printf(" Error: %.2X.\n", dw); abort(); }
#define CHECK_HR(op, msg) if (HRESULT __hr = op; __hr != S_OK)    { printf(msg); printf(" Error: %.2X.\n", __hr); goto done; }

HWND _hwnd;
DWORD InitializeWindow(LPVOID lpThreadParameter);
HRESULT GetVideoSourceAndReaderFromDevice(UINT nDevice, IMFMediaSource** ppVideoSource, IMFSourceReader** ppVideoReader);
HRESULT CopyAttribute(IMFAttributes* pDest, IMFAttributes* pSrc, const GUID& key);
HRESULT QRRead(BYTE* qrImgData, wchar_t* qrText);
HRESULT QRConnect(std::wstring_view qrText);

int
wmain()
{
	ComPtr<IMFSourceReader> pWebcamVideoSourceReader = NULL;
	ComPtr<IMFMediaType> pWebcamVideoSourceOutputType = NULL;
	ComPtr<IMFMediaSink> pEVRVideoSink = NULL;
	ComPtr<IMFStreamSink> pEVRVideoFirstStreamSink = NULL;
	ComPtr<IMFMediaType> pWebcamSourceType = NULL;
	ComPtr<IMFMediaType> pEVRSinkType = NULL;
	ComPtr<IMFTransform> pColorConvTransform = NULL; // This is colour converter MFT is used to convert between the webcam pixel format and RGB32.
	ComPtr<IMFSample> pD3DVideoSample = NULL;

	CHECK_HR(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE), "COM initialisation failed.");
	CHECK_HR(MFStartup(MF_VERSION), "Media Foundation initialisation failed.");

	// Need the color converter DSP for conversions between YUV, RGB etc.
	CHECK_HR(MFTRegisterLocalByCLSID(__uuidof(CColorConvertDMO), MFT_CATEGORY_VIDEO_PROCESSOR, L"", MFT_ENUM_FLAG_SYNCMFT, 0, NULL, 0, NULL), "Error registering colour converter DSP.");

	// Create a separate Window and thread to host the Video player.
	CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitializeWindow, NULL, 0, NULL);
	Sleep(100);
	if (_hwnd == nullptr)
	{
		printf("Failed to initialize video window.\n");
		goto done;
	}

	// ----- Set up Video sink (Enhanced Video Renderer). -----
	{
		ComPtr<IMFActivate> pActive = NULL;
		CHECK_HR(MFCreateVideoRendererActivate(_hwnd, &pActive), "Failed to created video rendered activation context.");
		CHECK_HR(pActive->ActivateObject(IID_PPV_ARGS(&pEVRVideoSink)), "Failed to activate IMFMediaSink interface on video sink.");

		// Initialize the renderer before doing anything else including querying for other interfaces
		ComPtr<IMFVideoRenderer> pVideoRenderer = NULL;
		CHECK_HR(pEVRVideoSink->QueryInterface(IID_PPV_ARGS(&pVideoRenderer)), "Failed to get video Renderer interface from EVR media sink.");
		CHECK_HR(pVideoRenderer->InitializeRenderer(NULL, NULL), "Failed to initialize the video renderer.");

		ComPtr<IMFVideoDisplayControl> pVideoDisplayControl = NULL;
		CHECK_HR(MFGetService(pEVRVideoSink.Get(), MR_VIDEO_RENDER_SERVICE, IID_PPV_ARGS(&pVideoDisplayControl)), "Failed to get video display control interface from EVR media sink");
		CHECK_HR(pVideoDisplayControl->SetVideoWindow(_hwnd), "Failed to SetVideoWindow.");

		RECT rc = {0, 0, VIDEO_WIDTH, VIDEO_HEIGHT};
		CHECK_HR(pVideoDisplayControl->SetVideoPosition(NULL, &rc), "Failed to SetVideoPosition.");

		CHECK_HR(pEVRVideoSink->GetStreamSinkByIndex(0, &pEVRVideoFirstStreamSink), "Failed to get video renderer stream by index.");
	}

	// ----- Set up webcam video source reader. -----
	{
		ComPtr<IMFMediaSource> pWebcamVideoSource = NULL;
		CHECK_HR(GetVideoSourceAndReaderFromDevice(WEBCAM_DEVICE_INDEX, &pWebcamVideoSource, &pWebcamVideoSourceReader), "Failed to get webcam video source.");
		CHECK_HR(pWebcamVideoSourceReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pWebcamVideoSourceOutputType), "Error retrieving current media type from first video stream.");

		UINT32 uStride;
		if (FAILED(pWebcamVideoSourceOutputType->GetUINT32(MF_MT_DEFAULT_STRIDE, &uStride)))
		{
			GUID subtype{};
			CHECK_HR(pWebcamVideoSourceOutputType->GetGUID(MF_MT_SUBTYPE, &subtype), "Failed to get subtype GUID from the media source.");
			DWORD subtypeFormat = subtype.Data1;
			CHECK_HR(MFGetStrideForBitmapInfoHeader(subtypeFormat, VIDEO_WIDTH, (LONG*)&uStride), "Failed to calculate the stride for the media source subtype format.");
			CHECK_HR(pWebcamVideoSourceOutputType->SetUINT32(MF_MT_DEFAULT_STRIDE, uStride), "Failed to set the default stride for the media source.");
		}

		CHECK_HR(pWebcamVideoSourceReader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE), "Failed to set the first video stream on the source reader.");
	}

	// ----- Create a compatible media type and set on the source and sink. -----

	// Set the video input type on the EVR sink.
	{
		CHECK_HR(MFCreateMediaType(&pEVRSinkType), "Failed to create video output media type.");
		CHECK_HR(pEVRSinkType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Failed to set video output media major type.");
		CHECK_HR(pEVRSinkType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "Failed to set video sub-type attribute on media type.");
		CHECK_HR(pEVRSinkType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Failed to set interlace mode attribute on media type.");
		CHECK_HR(pEVRSinkType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE), "Failed to set independent samples attribute on media type.");
		CHECK_HR(MFSetAttributeRatio(pEVRSinkType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Failed to set pixel aspect ratio attribute on media type.");
		CHECK_HR(MFSetAttributeSize(pEVRSinkType.Get(), MF_MT_FRAME_SIZE, VIDEO_WIDTH, VIDEO_HEIGHT), "Failed to set the frame size attribute on media type.");
		CHECK_HR(MFSetAttributeSize(pEVRSinkType.Get(), MF_MT_FRAME_RATE, VIDEO_FRAME_RATE, 1), "Failed to set the frame rate attribute on media type.");
		CHECK_HR(CopyAttribute(pEVRSinkType.Get(), pWebcamVideoSourceOutputType.Get(), MF_MT_DEFAULT_STRIDE), "Failed to copy default stride attribute.");
		ComPtr<IMFMediaTypeHandler> pSinkMediaTypeHandler = NULL;
		CHECK_HR(pEVRVideoFirstStreamSink->GetMediaTypeHandler(&pSinkMediaTypeHandler), "Failed to get media type handler for stream sink.");
		CHECK_HR(pSinkMediaTypeHandler->SetCurrentMediaType(pEVRSinkType.Get()), "Failed to set input media type on EVR sink.");
	}

	// Combine EVR input type with Webcam output subtype to create the Webcam output type
	{
		CHECK_HR(MFCreateMediaType(&pWebcamSourceType), "Failed to create webcam output media type.");
		CHECK_HR(pEVRSinkType->CopyAllItems(pWebcamSourceType.Get()), "Error copying media type attributes from EVR input to webcam output media type.");
		CHECK_HR(CopyAttribute(pWebcamSourceType.Get(), pWebcamVideoSourceOutputType.Get(), MF_MT_SUBTYPE), "Failed to set video sub-type attribute on webcam media type.");

		CHECK_HR(pWebcamVideoSourceReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pWebcamSourceType.Get()), "Failed to set output media type on source reader.");
	}

	// ----- Create an MFT to convert between webcam pixel format and RGB32. -----
	{
		ComPtr<IUnknown> colorConvTransformUnk = NULL;
		CHECK_HR(CoCreateInstance(CLSID_CColorConvertDMO, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&colorConvTransformUnk)), "Failed to create colour converter MFT.");
		CHECK_HR(colorConvTransformUnk->QueryInterface(IID_PPV_ARGS(&pColorConvTransform)), "Failed to get IMFTransform interface from colour converter MFT object.");
		CHECK_HR(pColorConvTransform->SetInputType(0, pWebcamSourceType.Get(), 0), "Failed to set input media type on colour converter MFT.");
		CHECK_HR(pColorConvTransform->SetOutputType(0, pEVRSinkType.Get(), 0), "Failed to set output media type on colour converter MFT.");

		DWORD mftStatus = 0;
		CHECK_HR(pColorConvTransform->GetInputStatus(0, &mftStatus), "Failed to get input status from colour converter MFT.");
		if (MFT_INPUT_STATUS_ACCEPT_DATA != mftStatus)
		{
			printf("Colour converter MFT is not accepting data.\n");
			goto done;
		}

		CHECK_HR(pColorConvTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL), "Failed to process FLUSH command on colour converter MFT.");
		CHECK_HR(pColorConvTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL), "Failed to process BEGIN_STREAMING command on colour converter MFT.");
		CHECK_HR(pColorConvTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL), "Failed to process START_OF_STREAM command on colour converter MFT.");
	}

	// ----- Source and sink now configured. Set up remaining infrastructure and then start sampling. -----

	// Get Direct3D surface organised.
	{
		ComPtr<IDirect3DDeviceManager9> pD3DManager = NULL;
		ComPtr<IMFVideoSampleAllocator> pVideoSampleAllocator = NULL;
		CHECK_HR(MFGetService(pEVRVideoFirstStreamSink.Get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(&pVideoSampleAllocator)), "Failed to get IMFVideoSampleAllocator.");
		CHECK_HR(MFGetService(pEVRVideoSink.Get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(&pD3DManager)), "Failed to get Direct3D manager from EVR media sink.");
		CHECK_HR(pVideoSampleAllocator->SetDirectXManager(pD3DManager.Get()), "Failed to set D3DManager on video sample allocator.");
		CHECK_HR(pVideoSampleAllocator->InitializeSampleAllocator(1, pEVRSinkType.Get()), "Failed to initialize video sample allocator.");
		CHECK_HR(pVideoSampleAllocator->AllocateSample(&pD3DVideoSample), "Failed to allocate video sample.");
	}

	// Get clocks organised.
	{
		ComPtr<IMFPresentationClock> pClock = NULL;
		ComPtr<IMFPresentationTimeSource> pTimeSource = NULL;
		CHECK_HR(MFCreatePresentationClock(&pClock), "Failed to create presentation clock.");
		CHECK_HR(MFCreateSystemTimeSource(&pTimeSource), "Failed to create system time source.");
		CHECK_HR(pClock->SetTimeSource(pTimeSource.Get()), "Failed to set time source.");
		CHECK_HR(pEVRVideoSink->SetPresentationClock(pClock.Get()), "Failed to set presentation clock on video sink.");
		CHECK_HR(pClock->Start(0), "Error starting presentation clock.");
	}

	// Start the sample read-write loop.
	LONGLONG evrTimestamp = 0;
	while (true)
	{
		DWORD streamIndex, flags;
		LONGLONG llTimeStamp;
		ComPtr<IMFSample> videoSample = NULL;
		CHECK_HR(pWebcamVideoSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &llTimeStamp, &videoSample), "Error reading video sample.");
		if (!videoSample)
			continue;

		// ----- Video source sample. -----
		LONGLONG sampleDuration = 0;
		CHECK_HR(videoSample->GetSampleDuration(&sampleDuration), "Failed to get video sample duration.");

		// ----- Apply colour conversion transfrom. -----
		CHECK_HR(pColorConvTransform->ProcessInput(0, videoSample.Get(), NULL), "The colour conversion decoder ProcessInput call failed.");

		MFT_OUTPUT_STREAM_INFO streamInfo;
		CHECK_HR(pColorConvTransform->GetOutputStreamInfo(0, &streamInfo), "Failed to get output stream info from colour conversion MFT.");

		ComPtr<IMFSample> mftOutSample = NULL;
		CHECK_HR(MFCreateSample(&mftOutSample), "Failed to create MF sample.");

		ComPtr<IMFMediaBuffer> mftOutBuffer = NULL;
		CHECK_HR(MFCreateMemoryBuffer(streamInfo.cbSize, &mftOutBuffer), "Failed to create memory buffer.");
		CHECK_HR(mftOutSample->AddBuffer(mftOutBuffer.Get()), "Failed to add sample to buffer.");

		MFT_OUTPUT_DATA_BUFFER outputDataBuffer{};
		outputDataBuffer.pSample = mftOutSample.Get();
		DWORD processOutputStatus = 0;
		HRESULT mftProcessOutput = pColorConvTransform->ProcessOutput(0, 1, &outputDataBuffer, &processOutputStatus);
		if (FAILED(mftProcessOutput))
		{
			printf("Colour conversion failed with %.2X.\n", mftProcessOutput);
			break;
		}

		// ----- Copy data into D3D Sample. -----
		ComPtr<IMFMediaBuffer> buf = NULL;
		DWORD bufLength = 0;
		CHECK_HR(mftOutSample->ConvertToContiguousBuffer(&buf), "ConvertToContiguousBuffer failed.");
		CHECK_HR(buf->GetCurrentLength(&bufLength), "Get buffer length failed.");

		BYTE* mftOutSampleBuf = NULL;
		DWORD mftOutSampleBufLength = 0;
		CHECK_HR(buf->Lock(&mftOutSampleBuf, NULL, &mftOutSampleBufLength), "Failed to lock sample buffer.");

		wchar_t qrText[256];
		if (HRESULT hr = QRRead(mftOutSampleBuf, qrText); SUCCEEDED(hr))
		{
			if (SUCCEEDED(QRConnect(qrText)))
			{
				PostMessage(_hwnd, WM_CLOSE, 0, 0);
				goto done;
			}
		}

		ComPtr<IMFMediaBuffer> pD3DVideoSampleDstBuffer = NULL;
		CHECK_HR(pD3DVideoSample->GetBufferByIndex(0, &pD3DVideoSampleDstBuffer), "Failed to get destination buffer.");
		ComPtr<IMF2DBuffer> pD3DVideoSampleDst2DBuffer = NULL;
		CHECK_HR(pD3DVideoSampleDstBuffer->QueryInterface(IID_PPV_ARGS(&pD3DVideoSampleDst2DBuffer)), "Failed to get pointer to 2D buffer.");
		CHECK_HR(pD3DVideoSampleDst2DBuffer->ContiguousCopyFrom(mftOutSampleBuf, mftOutSampleBufLength), "Failed to copy D2D buffer.");

		UINT32 uiAttribute = 0;
		CHECK_HR(videoSample->GetUINT32(MFSampleExtension_Discontinuity, &uiAttribute), "Failed to get discontinuity attribute.");
		CHECK_HR(pD3DVideoSample->SetUINT32(MFSampleExtension_Discontinuity, uiAttribute), "Failed to set discontinuity attribute.");
		CHECK_HR(videoSample->GetUINT32(MFSampleExtension_CleanPoint, &uiAttribute), "Failed to get clean point attribute.");
		CHECK_HR(pD3DVideoSample->SetUINT32(MFSampleExtension_CleanPoint, uiAttribute), "Failed to set clean point attribute.");

		CHECK_HR(pD3DVideoSample->SetSampleTime(evrTimestamp), "Failed to set D3D video sample time.");
		CHECK_HR(pD3DVideoSample->SetSampleDuration(sampleDuration), "Failed to set D3D video sample duration.");

		CHECK_HR(pEVRVideoFirstStreamSink->ProcessSample(pD3DVideoSample.Get()), "Streamsink process sample failed.");

		CHECK_HR(buf->Unlock(), "Failed to unlock source buffer.");
		evrTimestamp += sampleDuration;
	}

done:
	printf("Finished.\n");
	auto c = getchar();
	return 0;
}

HRESULT
CopyAttribute(IMFAttributes* pDest, IMFAttributes* pSrc, const GUID& key)
{
	PROPVARIANT var;
	PropVariantInit(&var);

	HRESULT hr = S_OK;

	hr = pSrc->GetItem(key, &var);
	if (SUCCEEDED(hr))
		hr = pDest->SetItem(key, var);

	PropVariantClear(&var);
	return hr;
}

HRESULT
GetVideoSourceAndReaderFromDevice(UINT nDevice, IMFMediaSource** ppVideoSource, IMFSourceReader** ppVideoReader)
{
	UINT32 videoDeviceCount = 0;
	ComPtr<IMFAttributes> videoConfig = NULL;
	IMFActivate** videoDevices = NULL;
	HRESULT hr = S_OK;

	// Get the first available webcam.
	hr = MFCreateAttributes(&videoConfig, 1);
	CHECK_HR(hr, "Error creating video configuation.");

	// Request video capture devices.
	hr = videoConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	CHECK_HR(hr, "Error initialising video configuration object.");

	hr = MFEnumDeviceSources(videoConfig.Get(), &videoDevices, &videoDeviceCount);
	CHECK_HR(hr, "Error enumerating video devices.");

	hr = videoDevices[nDevice]->ActivateObject(IID_PPV_ARGS(ppVideoSource));
	CHECK_HR(hr, "Error activating video device.");

	// Create a source reader.
	hr = MFCreateSourceReaderFromMediaSource(*ppVideoSource, videoConfig.Get(), ppVideoReader);
	CHECK_HR(hr, "Error creating video source reader.");

done:
	if (videoDevices)
	{
		for (DWORD i = 0; i < videoDeviceCount; ++i)
			videoDevices[i]->Release();
		CoTaskMemFree(videoDevices);
	}
	return hr;
}

DWORD
InitializeWindow(LPVOID lpThreadParameter)
{
	WNDCLASS wc = {0};

	wc.lpfnWndProc = DefWindowProc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = L"QRConnect Class";

	if (RegisterClass(&wc))
	{
		_hwnd = CreateWindow(wc.lpszClassName, L"Hold the QR code in front of the camera", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, VIDEO_WIDTH, VIDEO_HEIGHT, NULL, NULL, GetModuleHandle(NULL), NULL);

		if (_hwnd)
		{
			ShowWindow(_hwnd, SW_SHOWDEFAULT);
			MSG msg{};
			while (GetMessage(&msg, NULL, 0, 0))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
	}

	return 0;
}

class WlanManager
{
public:
	static std::pair<std::wstring_view, std::wstring_view>
	GetCredentialsFromQrText(std::wstring_view qr)
	{
		// qr = WIFI:S:<SSID>;T:<WEP|WPA|blank>;P:<PASSWORD>;H:<true|false|blank>;;
		size_t ssid_begin = qr.find(L"WIFI:S:");
		if (ssid_begin != 0)
			return {};
		ssid_begin += 7;

		size_t ssid_end = qr.find(';', ssid_begin);
		if (ssid_end == qr.npos)
			return {};

		size_t password_begin = qr.find(L";P:", ssid_end);
		if (password_begin == qr.npos)
			return {};
		password_begin += 3;

		size_t password_end = qr.find(';', password_begin);
		if (password_end == qr.npos)
			return {};

		return {qr.substr(ssid_begin, ssid_end - ssid_begin), qr.substr(password_begin, password_end - password_begin)};
	}

	WlanManager()
	{
		DWORD current_version;
		CHECK_DW(WlanOpenHandle(2, NULL, &current_version, &m_hClient));

		WLAN_INTERFACE_INFO_LIST* if_info_list;
		CHECK_DW(WlanEnumInterfaces(m_hClient, NULL, &if_info_list));
		m_guidIF = if_info_list->InterfaceInfo[0].InterfaceGuid;
		WlanFreeMemory(if_info_list);
	}

	~WlanManager()
	{
		WlanCloseHandle(m_hClient, NULL);
	}

	void
	AddNetworkProfile(std::wstring_view ssid, std::wstring_view password)
	{
		std::wstring profile_xml = LR"xml(<?xml version="1.0"?>
		<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
			<name>{{SSID}}</name>
			<SSIDConfig>
				<SSID>
					<name>{{SSID}}</name>
				</SSID>
			</SSIDConfig>
			<connectionType>ESS</connectionType>
			<connectionMode>auto</connectionMode>
			<MSM>
				<security>
					<authEncryption>
						<authentication>WPA2PSK</authentication>
						<encryption>AES</encryption>
						<useOneX>false</useOneX>
					</authEncryption>
					<sharedKey>
						<keyType>passPhrase</keyType>
						<protected>false</protected>
						<keyMaterial>{{PASSWORD}}</keyMaterial>
					</sharedKey>
				</security>
			</MSM>
		</WLANProfile>)xml";
		{
			size_t i = 0;
			while ((i = profile_xml.find(L"{{SSID}}", i)) != profile_xml.npos)
				profile_xml.replace(i, wcslen(L"{{SSID}}"), ssid);
		}
		{
			size_t i = 0;
			while ((i = profile_xml.find(L"{{PASSWORD}}", i)) != profile_xml.npos)
				profile_xml.replace(i, wcslen(L"{{PASSWORD}}"), password);
		}
		WLAN_REASON_CODE reason_code{};
		CHECK_DW(WlanSetProfile(m_hClient, &m_guidIF, 0, profile_xml.c_str(), NULL, TRUE, NULL, &reason_code));
	}

	bool
	IsDown()
	{
		WLAN_RADIO_STATE* radio_state_info{};
		DWORD radio_state_info_size = sizeof(WLAN_RADIO_STATE);
		CHECK_DW(WlanQueryInterface(m_hClient, &m_guidIF, wlan_intf_opcode_radio_state, NULL, &radio_state_info_size, (PVOID*)&radio_state_info, NULL));

		bool software_is_down = radio_state_info->PhyRadioState[0].dot11SoftwareRadioState == dot11_radio_state_off;
		bool hardware_is_down = radio_state_info->PhyRadioState[0].dot11HardwareRadioState == dot11_radio_state_off;

		WlanFreeMemory(radio_state_info);
		return software_is_down || hardware_is_down;
	}

	void
	SetUp()
	{
		WLAN_PHY_RADIO_STATE radio_state{};
		radio_state.dwPhyIndex = 0;
		radio_state.dot11HardwareRadioState = dot11_radio_state_on;
		radio_state.dot11SoftwareRadioState = dot11_radio_state_on;
		CHECK_DW(WlanSetInterface(m_hClient, &m_guidIF, wlan_intf_opcode_radio_state, sizeof(radio_state), (const PVOID)&radio_state, NULL));
	}

	void
	ConnectSsid(std::wstring_view ssid)
	{
		WLAN_AVAILABLE_NETWORK_LIST* nw_list{};
		CHECK_DW(WlanGetAvailableNetworkList(m_hClient, &m_guidIF, WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_MANUAL_HIDDEN_PROFILES, NULL, &nw_list));

		WLAN_CONNECTION_PARAMETERS cp{};
		cp.wlanConnectionMode = wlan_connection_mode_profile;
		cp.dot11BssType = dot11_BSS_type_infrastructure;
		cp.dwFlags = WLAN_CONNECTION_HIDDEN_NETWORK;
		for (DWORD i = 0; i < nw_list->dwNumberOfItems; ++i)
		{
			if (ssid == nw_list->Network[i].strProfileName)
			{
				cp.strProfile = nw_list->Network[i].strProfileName;
				break;
			}
		}
		WCHAR cp_profile_name[WLAN_MAX_NAME_LENGTH]{};
		if (cp.strProfile == NULL)
		{
			wcsncpy_s(cp_profile_name, ssid.data(), ssid.length());
			cp.strProfile = cp_profile_name;
		}
		CHECK(cp.strProfile, "WLAN profile not found");

		CHECK_DW(WlanDisconnect(m_hClient, &m_guidIF, NULL));
		CHECK_DW(WlanConnect(m_hClient, &m_guidIF, &cp, NULL));
		WlanFreeMemory(nw_list);
	}

	WLAN_INTERFACE_STATE
	GetState()
	{
		WLAN_INTERFACE_STATE* state{};
		DWORD sz = sizeof(WLAN_INTERFACE_STATE);
		CHECK_DW(WlanQueryInterface(m_hClient, &m_guidIF, wlan_intf_opcode_interface_state, NULL, &sz, (PVOID*)&state, NULL));

		WLAN_INTERFACE_STATE ret = *state;
		WlanFreeMemory(state);
		return ret;
	}

private:
	HANDLE m_hClient;
	GUID m_guidIF;
};

HRESULT
QRRead(BYTE* data, wchar_t* qrText)
{
	auto imgView = ZXing::ImageView{data, VIDEO_WIDTH, VIDEO_HEIGHT, ZXing::ImageFormat::BGRX};
	auto r = ZXing::ReadBarcode(imgView, ZXing::DecodeHints{}.setFormats(ZXing::BarcodeFormat::QRCode));
	if (r.isValid() == false)
		return E_FAIL;

	mbstowcs(qrText, r.text().data(), r.text().length());
	qrText[r.text().length()] = 0;

	auto [ssid, password] = WlanManager::GetCredentialsFromQrText(qrText);
	return ssid.empty() ? E_FAIL : S_OK;
}

HRESULT
QRConnect(std::wstring_view qrText)
{
	WlanManager wlan{};
	auto [ssid, password] = wlan.GetCredentialsFromQrText(qrText);
	wlan.AddNetworkProfile(ssid, password);
	if (wlan.IsDown())
		wlan.SetUp();
	wlan.ConnectSsid(ssid);
	while (true)
	{
		Sleep(100);
		WLAN_INTERFACE_STATE state = wlan.GetState();
		switch (state)
		{
		case wlan_interface_state_connected:
			std::wcout << "Connected to " << ssid << std::endl;
			return S_OK;

		case wlan_interface_state_disconnected:
			return E_FAIL;

		case wlan_interface_state_discovering:
		case wlan_interface_state_associating:
		case wlan_interface_state_authenticating:
			std::wcout << "Connecting to " << ssid << "..." << std::endl;
			break;

		case wlan_interface_state_not_ready:
		case wlan_interface_state_ad_hoc_network_formed:
		case wlan_interface_state_disconnecting:
			break;
		}
	}
}