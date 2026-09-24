/**
 * @file snapshot.cpp
 * @brief Off-screen WIC target, session settle loop, PNG encode.
 */

#include "snapshot.h"

#include "config.h"
#include "picker.h"
#include "render.h"
#include "session.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <thread>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace wbshterm {

	using Microsoft::WRL::ComPtr;

	static bool createImagingFactory(ComPtr<IWICImagingFactory>& out_factory,
			std::string& out_error) {
		const HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(out_factory.GetAddressOf()));
		if (FAILED(hr)) {
			out_error = "WIC imaging factory unavailable";
			return false;
		}

		return true;
	}

	static bool savePng(IWICImagingFactory* factory, IWICBitmap* bitmap,
			const std::wstring& path, UINT width, UINT height, std::string& out_error) {
		ComPtr<IWICStream> stream;
		if (FAILED(factory->CreateStream(stream.GetAddressOf()))
			|| FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
			out_error = "cannot open the snapshot file for writing";
			return false;
		}

		ComPtr<IWICBitmapEncoder> encoder;
		ComPtr<IWICBitmapFrameEncode> frame;
		if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
				encoder.GetAddressOf()))
			|| FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
			|| FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr))
			|| FAILED(frame->Initialize(nullptr))
			|| FAILED(frame->SetSize(width, height))) {
			out_error = "PNG encoder setup failed";
			return false;
		}

		WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
		if (FAILED(frame->SetPixelFormat(&format))
			|| FAILED(frame->WriteSource(bitmap, nullptr))
			|| FAILED(frame->Commit())
			|| FAILED(encoder->Commit())) {
			out_error = "writing the PNG failed";
			return false;
		}

		return true;
	}

	class SnapshotPicker : public PickHandler {
	public:
		void pickBegin(const std::string& prompt) override { picker.begin(prompt); }
		void pickItem(const std::string& text) override { picker.addItem(text); }
		void pickEnd() override { picker.finish(); }
		void pickCancel() override { picker.cancel(); }

		Picker picker;
	};

	static void runUntilQuiet(Session& session, const SnapshotRequest& request) {
		const auto started = std::chrono::steady_clock::now();
		auto last_change = started;

		std::this_thread::sleep_for(std::chrono::milliseconds(request.delay_ms));
		if (!request.feed.empty()) session.writeInput(request.feed.data(), request.feed.size());

		for (;;) {
			const auto now = std::chrono::steady_clock::now();
			const auto since_start = std::chrono::duration_cast<std::chrono::milliseconds>(
				now - started).count();
			const auto since_change = std::chrono::duration_cast<std::chrono::milliseconds>(
				now - last_change).count();

			if (since_start > request.timeout_ms) return;
			if (since_change > request.settle_ms) return;

			if (session.drainOutput()) last_change = std::chrono::steady_clock::now();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}

	static bool paintToBitmap(Renderer& renderer, const Screen& screen, const TerminalView& view,
			const Picker& picker, IWICBitmap* bitmap, std::string& out_error) {
		ComPtr<ID2D1RenderTarget> target;
		const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_DEFAULT,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
			96.0f, 96.0f);

		if (FAILED(renderer.factory()->CreateWicBitmapRenderTarget(bitmap, properties,
				target.GetAddressOf()))) {
			out_error = "CreateWicBitmapRenderTarget failed";
			return false;
		}

		const D2D1_SIZE_F size = target->GetSize();
		PaneCanvas canvas;
		canvas.target = target.Get();
		canvas.bounds = D2D1::RectF(0.0f, 0.0f, size.width, size.height);
		canvas.screen = &screen;
		canvas.view   = &view;

		target->BeginDraw();
		renderer.draw(canvas);
		renderer.drawPicker(canvas, picker);
		if (FAILED(target->EndDraw())) {
			out_error = "off-screen drawing failed";
			return false;
		}

		return true;
	}

	static bool createRenderer(Renderer& renderer, Config config, std::string& out_error) {
		if (renderer.create(config, out_error)) return true;

		config.font.family = L"Consolas";
		return renderer.create(config, out_error);
	}

	static Config snapshotConfig(const Config& wanted) {
		Config config = wanted;
		config.window.padding = 0;
		return config;
	}

	static bool paintGridToFile(Renderer& renderer, const Screen& screen,
			const TerminalView& view, const Picker& picker, const std::wstring& path,
			std::string& out_error) {
		const CellMetrics& cell = renderer.metrics();
		const UINT width  = static_cast<UINT>(cell.width * static_cast<float>(screen.columns()));
		const UINT height = static_cast<UINT>(cell.height * static_cast<float>(screen.rows()));

		ComPtr<IWICImagingFactory> factory;
		if (!createImagingFactory(factory, out_error)) return false;

		ComPtr<IWICBitmap> bitmap;
		if (FAILED(factory->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
				WICBitmapCacheOnLoad, bitmap.GetAddressOf()))) {
			out_error = "cannot create the off-screen bitmap";
			return false;
		}

		if (!paintToBitmap(renderer, screen, view, picker, bitmap.Get(), out_error)) {
			return false;
		}
		return savePng(factory.Get(), bitmap.Get(), path, width, height, out_error);
	}

	bool renderScreenToPng(const Screen& screen, const std::wstring& path, std::string& out_error) {
		::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

		Config config;
		findBuiltInTheme(config.theme_name, config.palette);

		Renderer renderer;
		if (!createRenderer(renderer, snapshotConfig(config), out_error)) return false;

		const TerminalView view;
		const Picker picker;
		return paintGridToFile(renderer, screen, view, picker, path, out_error);
	}

	bool renderSnapshot(const SnapshotRequest& request, std::string& out_error) {
		Renderer renderer;
		if (!createRenderer(renderer, snapshotConfig(request.config), out_error)) return false;

		Session session;
		SnapshotPicker picker;
		session.screen().setPickHandler(&picker);
		if (!session.start({ request.command_line }, request.columns, request.rows, out_error)) {
			return false;
		}

		if (!request.record_path.empty()) session.recordTo(request.record_path);

		runUntilQuiet(session, request);

		TerminalView view;
		view.followOutput(session.screen());
		if (request.scroll_lines != 0) view.scrollBy(request.scroll_lines, session.screen());
		if (request.select) {
			view.beginSelection({ request.select_row, request.select_column });
			view.extendSelection({ request.select_to_row, request.select_to_col });
		}

		const bool painted = paintGridToFile(renderer, session.screen(), view, picker.picker,
			request.output_path, out_error);
		session.stop();
		return painted;
	}

} /* namespace wbshterm */
