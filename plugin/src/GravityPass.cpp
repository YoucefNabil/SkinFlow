#include "GravityPass.h"

#include "SurfaceProbe.h"

namespace skinflow
{
	GravityPass& GravityPass::Get()
	{
		static GravityPass pass;
		return pass;
	}

	bool GravityPass::CompileShaders(ID3D11Device* a_device)
	{
		const auto fixed = std::to_string(kUvScaleFixed);
		const D3D_SHADER_MACRO gsDefines[]{ { "UV_SCALE_FIXED", fixed.c_str() }, { nullptr, nullptr } };
		const auto gsCode = source.Compile("GSMain", "gs_5_0", gsDefines);
		ComPtr<ID3D11GeometryShader> newGs;
		if (!gsCode || FAILED(a_device->CreateGeometryShader(gsCode->GetBufferPointer(), gsCode->GetBufferSize(), nullptr, newGs.GetAddressOf()))) {
			return false;
		}
		std::array<std::array<ComPtr<ID3D11PixelShader>, 2>, 2> newPs;
		for (std::size_t seams = 0; seams < 2; ++seams) {
			for (std::size_t modelSpace = 0; modelSpace < 2; ++modelSpace) {
				std::vector<D3D_SHADER_MACRO> defines{ { "UV_SCALE_FIXED", fixed.c_str() } };
				if (modelSpace) {
					defines.push_back({ "MODELSPACENORMALS", "1" });
				}
				if (seams) {
					defines.push_back({ "SEAM_CAPTURE", "1" });
				}
				defines.push_back({ nullptr, nullptr });
				const auto code = source.Compile("PSMain", "ps_5_0", defines.data());
				auto& shader = newPs[seams][modelSpace];
				if (!code || FAILED(a_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf()))) {
					return false;
				}
				SetDebugName(shader.Get(), std::format("SkinFlow::GravityPS {}-space normals{}", modelSpace ? "model" : "tangent", seams ? " + seam snapshot" : ""));
			}
		}
		gs = std::move(newGs);
		ps = std::move(newPs);
		SetDebugName(gs.Get(), "SkinFlow::GravityGS");
		return true;
	}

	void GravityPass::ReloadShadersIfChanged(ID3D11DeviceContext* a_context, std::uint64_t a_frame)
	{
		if (!source.Changed(a_frame)) {
			return;
		}
		ComPtr<ID3D11Device> device;
		a_context->GetDevice(device.GetAddressOf());
		if (CompileShaders(device.Get())) {
			logger::info("{} changed: shaders reloaded", source.Name());
		} else {
			logger::error("{} changed but does not compile: kept the previous shaders", source.Name());
		}
	}

	bool GravityPass::Init(ID3D11Device* a_device)
	{
		source.MarkCompiled();
		if (!CompileShaders(a_device)) {
			return false;
		}

		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;  // UV islands can be mirrored
		rd.DepthClipEnable = FALSE;
		D3D11_DEPTH_STENCIL_DESC dd{};
		dd.DepthEnable = FALSE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
		D3D11_BLEND_DESC bd{};
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(a_device->CreateRasterizerState(&rd, raster.GetAddressOf())) ||
			FAILED(a_device->CreateDepthStencilState(&dd, noDepth.GetAddressOf())) ||
			FAILED(a_device->CreateBlendState(&bd, opaque.GetAddressOf()))) {
			return false;
		}

		logger::info("gravity pass ready");
		return true;
	}

	bool GravityPass::Prepare(ID3D11DeviceContext* a_context, Surface& a_surface, const void* a_geometry)
	{
		if (failed) {
			return false;
		}
		if (!ready) {
			ComPtr<ID3D11Device> device;
			a_context->GetDevice(device.GetAddressOf());
			ready = Init(device.Get());
			if (!ready) {
				failed = true;
				logger::error("gravity pass disabled: shader or state creation failed");
				return false;
			}
		}

		const auto frame = SurfacePool::Get().Frame();
		ReloadShadersIfChanged(a_context, frame);
		if (a_surface.writeFrame != frame) {
			SurfaceProbe::Get().OnFrameStart(a_context, a_surface);
			MeasureUvScale(a_context, a_surface, frame);
			constexpr float zero[4]{};
			constexpr UINT zeros[4]{};
			a_context->ClearRenderTargetView(a_surface.rtv[a_surface.WriteIndex()].Get(), zero);
			a_context->ClearUnorderedAccessViewUint(a_surface.metricsUav.Get(), zeros);
			auto& snapshot = a_surface.seamCapture;
			if (snapshot.rtv && snapshot.requestFrame == frame) {
				a_context->ClearRenderTargetView(snapshot.rtv.Get(), zero);
				snapshot.capturedFrame = frame;
			}
			a_surface.writeFrame = frame;
			a_surface.capturedGeometry.clear();
		}
		// The same geometry can be drawn more than once a frame (reflections, several passes).
		if (std::ranges::find(a_surface.capturedGeometry, a_geometry) != a_surface.capturedGeometry.end()) {
			return false;
		}
		a_surface.capturedGeometry.push_back(a_geometry);
		return true;
	}

	bool GravityPass::CapturingSeams(const Surface& a_surface, std::uint64_t a_frame)
	{
		return a_surface.seamCapture.rtv && a_surface.seamCapture.capturedFrame == a_frame;
	}

	void GravityPass::Bind(ID3D11DeviceContext* a_context, Surface& a_surface, bool a_modelSpaceNormals)
	{
		const auto res = static_cast<float>(a_surface.size);
		const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, res, res, 0.0f, 1.0f };
		const bool seams = CapturingSeams(a_surface, SurfacePool::Get().Frame());
		ID3D11RenderTargetView* targets[2]{ a_surface.rtv[a_surface.WriteIndex()].Get(), a_surface.seamCapture.rtv.Get() };
		ID3D11UnorderedAccessView* metrics = a_surface.metricsUav.Get();
		// Metrics at u2 (Gravity.hlsl), after the two render targets of the seam snapshot.
		a_context->OMSetRenderTargetsAndUnorderedAccessViews(seams ? 2 : 1, targets, nullptr, 2, 1, &metrics, nullptr);
		a_context->RSSetViewports(1, &viewport);
		a_context->RSSetState(raster.Get());
		a_context->OMSetBlendState(opaque.Get(), nullptr, 0xFFFFFFFF);
		a_context->OMSetDepthStencilState(noDepth.Get(), 0);
		a_context->GSSetShader(gs.Get(), nullptr, 0);
		a_context->PSSetShader(ps[seams ? 1 : 0][a_modelSpaceNormals ? 1 : 0].Get(), nullptr, 0);
	}

	void GravityPass::MeasureUvScale(ID3D11DeviceContext* a_context, Surface& a_surface, std::uint64_t a_frame)
	{
		if (a_surface.metricsCopyFrame) {
			if (a_frame < a_surface.metricsCopyFrame + kReadbackDelayFrames) {
				return;
			}
			D3D11_MAPPED_SUBRESOURCE data{};
			const HRESULT hr = a_context->Map(a_surface.metricsReadback.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &data);
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
				return;
			}
			if (SUCCEEDED(hr)) {
				const auto* sums = static_cast<const std::uint32_t*>(data.pData);
				if (sums[1] > 0) {
					const float scale = static_cast<float>(sums[0]) / kUvScaleFixed / static_cast<float>(sums[1]);
					if (a_surface.key.actor == 0x14 && std::abs(scale - a_surface.uvScale) > 0.1f * scale) {
						logger::info("player {}: {:.1f} world units per UV unit", a_surface.label, scale);
					}
					a_surface.uvScale = scale;
					a_surface.coveredTexels = sums[1];
				}
				a_context->Unmap(a_surface.metricsReadback.Get(), 0);
			}
			a_surface.metricsCopyFrame = 0;
			return;
		}
		// The sums still hold the last frame this surface was drawn; Prepare clears them next.
		if (a_surface.writeFrame != 0 && (a_surface.uvScale == 0.0f || a_frame % kUvScaleInterval == 0)) {
			a_context->CopyResource(a_surface.metricsReadback.Get(), a_surface.metrics.Get());
			a_surface.metricsCopyFrame = a_frame;
		}
	}
}
