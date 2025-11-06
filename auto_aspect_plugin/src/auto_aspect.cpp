#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "aviutl2_sdk/plugin2.h"

using Microsoft::WRL::ComPtr;

namespace {

std::atomic<bool> g_stop_requested{false};
std::atomic<bool> g_pending_reset{true};
std::atomic<bool> g_seen_first_object{false};
std::atomic<bool> g_adjusted{false};
std::atomic<bool> g_mf_started{false};

HOST_APP_TABLE* g_host = nullptr;
EDIT_HANDLE* g_edit_handle = nullptr;
std::thread g_worker;
HINSTANCE g_instance = nullptr;

std::wstring utf8_to_wide(const char* text) {
	if (!text) {
		return {};
	}
	const int required = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	if (required <= 1) {
		return {};
	}
	std::wstring wide(static_cast<size_t>(required) - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), required);
	return wide;
}

void output_log(EDIT_SECTION* edit, const std::wstring& message) {
	if (edit && edit->output_log && !message.empty()) {
		edit->output_log(message.c_str());
	}
}

class ScopedCoInitialize {
public:
	ScopedCoInitialize() {
		HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		if (SUCCEEDED(hr)) {
			result_ = hr;
			need_uninitialize_ = true;
			return;
		}
		if (hr == RPC_E_CHANGED_MODE) {
			hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			if (SUCCEEDED(hr)) {
				result_ = hr;
				need_uninitialize_ = true;
				return;
			}
			if (hr == RPC_E_CHANGED_MODE) {
				result_ = S_OK;
				return;
			}
		}
		result_ = hr;
	}

	ScopedCoInitialize(const ScopedCoInitialize&) = delete;
	ScopedCoInitialize& operator=(const ScopedCoInitialize&) = delete;

	~ScopedCoInitialize() {
		if (need_uninitialize_) {
			CoUninitialize();
		}
	}

	bool ok() const {
		return SUCCEEDED(result_);
	}

private:
	HRESULT result_ = E_FAIL;
	bool need_uninitialize_ = false;
};

bool ensure_media_foundation_started() {
	static std::once_flag once_flag;
	static HRESULT init_result = E_FAIL;
	std::call_once(once_flag, []() {
		HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
		if (FAILED(hr)) {
			hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
		}
		init_result = hr;
		if (SUCCEEDED(hr)) {
			g_mf_started.store(true, std::memory_order_relaxed);
		}
	});
	return SUCCEEDED(init_result);
}

bool get_image_dimensions(const std::wstring& path, int& width, int& height) {
	ScopedCoInitialize com;
	if (!com.ok()) {
		return false;
	}

	ComPtr<IWICImagingFactory> factory;
	HRESULT hr = CoCreateInstance(
		CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	if (FAILED(hr)) {
		hr = CoCreateInstance(
			CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		if (FAILED(hr)) {
			return false;
		}
	}

	ComPtr<IWICBitmapDecoder> decoder;
	hr = factory->CreateDecoderFromFilename(
		path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
	if (FAILED(hr)) {
		return false;
	}

	ComPtr<IWICBitmapFrameDecode> frame;
	hr = decoder->GetFrame(0, &frame);
	if (FAILED(hr)) {
		return false;
	}

	UINT w = 0;
	UINT h = 0;
	hr = frame->GetSize(&w, &h);
	if (FAILED(hr) || w == 0 || h == 0) {
		return false;
	}
	width = static_cast<int>(w);
	height = static_cast<int>(h);
	return true;
}

bool get_video_dimensions(const std::wstring& path, int& width, int& height) {
	if (!ensure_media_foundation_started()) {
		return false;
	}

	ScopedCoInitialize com;
	if (!com.ok()) {
		return false;
	}

	ComPtr<IMFAttributes> attributes;
	HRESULT hr = MFCreateAttributes(&attributes, 1);
	if (FAILED(hr)) {
		return false;
	}
	attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);

	ComPtr<IMFSourceReader> reader;
	hr = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);
	if (FAILED(hr)) {
		return false;
	}

	ComPtr<IMFMediaType> media_type;
	hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &media_type);
	if (FAILED(hr)) {
		hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &media_type);
		if (FAILED(hr)) {
			return false;
		}
	}

	UINT32 w = 0;
	UINT32 h = 0;
	hr = MFGetAttributeSize(media_type.Get(), MF_MT_FRAME_SIZE, &w, &h);
	if (FAILED(hr) || w == 0 || h == 0) {
		return false;
	}
	width = static_cast<int>(w);
	height = static_cast<int>(h);
	return true;
}

enum class MediaKind {
	None,
	Image,
	Video
};

struct MediaInfo {
	MediaKind kind = MediaKind::None;
	std::wstring path;
};

MediaInfo extract_media_info(EDIT_SECTION* edit, OBJECT_HANDLE object) {
	if (!edit) {
		return {};
	}

	struct Candidate {
		const wchar_t* effect;
		const wchar_t* item;
		MediaKind kind;
	};
	const Candidate candidates[] = {
		{L"動画ファイル", L"ファイル", MediaKind::Video},
		{L"VideoFile", L"File", MediaKind::Video},
		{L"画像ファイル", L"ファイル", MediaKind::Image},
		{L"ImageFile", L"File", MediaKind::Image},
	};

	for (const auto& candidate : candidates) {
		const char* value = edit->get_object_item_value(object, candidate.effect, candidate.item);
		if (!value) {
			continue;
		}
		std::wstring path = utf8_to_wide(value);
		if (!path.empty()) {
			return {candidate.kind, std::move(path)};
		}
	}

	return {};
}

OBJECT_HANDLE find_first_object(EDIT_SECTION* edit) {
	if (!edit || !edit->info) {
		return nullptr;
	}

	const int max_layer = edit->info->layer_max;
	if (max_layer < 0) {
		return nullptr;
	}

	OBJECT_HANDLE result = nullptr;
	OBJECT_LAYER_FRAME result_frame{};
	bool found = false;

	for (int layer = 0; layer <= max_layer; ++layer) {
		OBJECT_HANDLE object = edit->find_object(layer, 0);
		if (!object) {
			continue;
		}
		const OBJECT_LAYER_FRAME frame = edit->get_object_layer_frame(object);
		if (!found || frame.start < result_frame.start ||
			(frame.start == result_frame.start && frame.layer < result_frame.layer)) {
			result = object;
			result_frame = frame;
			found = true;
		}
	}

	return result;
}

bool adjust_scene_if_needed(EDIT_SECTION* edit) {
	if (!edit || !edit->info) {
		return false;
	}

	if (g_pending_reset.exchange(false, std::memory_order_acq_rel)) {
		g_seen_first_object.store(false, std::memory_order_release);
		g_adjusted.store(false, std::memory_order_release);
	}

	if (g_adjusted.load(std::memory_order_acquire)) {
		return false;
	}

	OBJECT_HANDLE first_object = find_first_object(edit);
	if (!first_object) {
		return false;
	}

	const MediaInfo info = extract_media_info(edit, first_object);
	if (info.kind == MediaKind::None || info.path.empty()) {
		return false;
	}

	bool first_media_object = !g_seen_first_object.exchange(true, std::memory_order_acq_rel);
	if (!first_media_object) {
		return false;
	}

	int width = 0;
	int height = 0;
	bool ok = false;
	switch (info.kind) {
		case MediaKind::Image:
			ok = get_image_dimensions(info.path, width, height);
			break;
		case MediaKind::Video:
			ok = get_video_dimensions(info.path, width, height);
			break;
		default:
			break;
	}
	if (!ok || width <= 0 || height <= 0) {
		std::wstring message = L"[auto_aspect] \"";
		message += info.path;
		message += L"\" のサイズ取得に失敗しました";
		output_log(edit, message);
		return false;
	}

	if (edit->info->width != width || edit->info->height != height) {
		edit->info->width = width;
		edit->info->height = height;
		std::wstring message = L"[auto_aspect] シーン解像度を ";
		message += std::to_wstring(width);
		message += L" x ";
		message += std::to_wstring(height);
		message += L" に変更しました";
		output_log(edit, message);
	} else {
		output_log(edit, L"[auto_aspect] シーン解像度は既に読み込みファイルと一致しています");
	}

	g_adjusted.store(true, std::memory_order_release);
	return true;
}

void poll_callback(EDIT_SECTION* edit) {
	adjust_scene_if_needed(edit);
}

void worker_routine() {
	using namespace std::chrono_literals;
	while (!g_stop_requested.load(std::memory_order_acquire)) {
		if (g_edit_handle) {
			g_edit_handle->call_edit_section(poll_callback);
		}
		for (int i = 0; i < 5; ++i) {
			if (g_stop_requested.load(std::memory_order_acquire)) {
				break;
			}
			std::this_thread::sleep_for(100ms);
		}
	}
}

void project_load_handler(PROJECT_FILE*) {
	g_pending_reset.store(true, std::memory_order_release);
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_instance = instance;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}

extern "C" __declspec(dllexport) BOOL InitializePlugin(DWORD) {
	if (!ensure_media_foundation_started()) {
		return FALSE;
	}
	return TRUE;
}

extern "C" __declspec(dllexport) void UninitializePlugin() {
	g_stop_requested.store(true, std::memory_order_release);
	if (g_worker.joinable()) {
		g_worker.join();
	}
	if (g_mf_started.exchange(false, std::memory_order_acq_rel)) {
		MFShutdown();
	}
}

extern "C" __declspec(dllexport) BOOL RegisterPlugin(HOST_APP_TABLE* host) {
	g_host = host;
	if (!g_host) {
		return FALSE;
	}

	g_host->set_plugin_information(L"Auto Aspect Plugin 1.0");

	g_edit_handle = g_host->create_edit_handle();
	if (!g_edit_handle) {
		g_host = nullptr;
		return FALSE;
	}

	g_pending_reset.store(true, std::memory_order_release);
	g_host->register_project_load_handler(project_load_handler);

	g_edit_handle->call_edit_section([](EDIT_SECTION* edit) {
		if (edit && edit->output_log) {
			edit->output_log(L"[auto_aspect] Loaded (RegisterPlugin)");
		}
	});

	g_stop_requested.store(false, std::memory_order_release);
	g_worker = std::thread(worker_routine);

	return TRUE;
}
