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
		const D3D_SHADER_MACRO defines[]{ { "UV_SCALE_FIXED", fixed.c_str() }, { nullptr, nullptr } };
		const D3D_SHADER_MACRO modelSpaceDefines[]{ { "UV_SCALE_FIXED", fixed.c_str() },
			{ "MODELSPACENORMALS", "1" }, { nullptr, nullptr } };
		const auto gsCode = source.Compile("GSMain", "gs_5_0", defines);
		const auto psTangentCode = source.Compile("PSMain", "ps_5_0", defines);
		const auto psModelCode = source.Compile("PSMain", "ps_5_0", modelSpaceDefines);
		ComPtr<ID3D11GeometryShader> newGs;
		ComPtr<ID3D11PixelShader> newTangent, newModel;
		if (!gsCode || !psTangentCode || !psModelCode ||
			FAILED(a_device->CreateGeometryShader(gsCode->GetBufferPointer(), gsCode->GetBufferSize(), nullptr, newGs.GetAddressOf())) ||
			FAILED(a_device->CreatePixelShader(psTangentCode->GetBufferPointer(), psTangentCode->GetBufferSize(), nullptr, newTangent.GetAddressOf())) ||
			FAILED(a_device->CreatePixelShader(psModelCode->GetBufferPointer(), psModelCode->GetBufferSize(), nullptr, newModel.GetAddressOf()))) {
			return false;
		}
		gs = std::move(newGs);
		psTangentSpace = std::move(newTangent);
		psModelSpace = std::move(newModel);
		SetDebugName(gs.Get(), "SkinFlow::GravityGS");
		SetDebugName(psTangentSpace.Get(), "SkinFlow::GravityPS tangent-space normals");
		SetDebugName(psModelSpace.Get(), "SkinFlow::GravityPS model-space normals");
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

	void GravityPass::Bind(ID3D11DeviceContext* a_context, Surface& a_surface, bool a_modelSpaceNormals)
	{
		const auto res = static_cast<float>(a_surface.size);
		const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, res, res, 0.0f, 1.0f };
		ID3D11RenderTargetView* target = a_surface.rtv[a_surface.WriteIndex()].Get();
		ID3D11UnorderedAccessView* metrics = a_surface.metricsUav.Get();
		a_context->OMSetRenderTargetsAndUnorderedAccessViews(1, &target, nullptr, 1, 1, &metrics, nullptr);
		a_context->RSSetViewports(1, &viewport);
		a_context->RSSetState(raster.Get());
		a_context->OMSetBlendState(opaque.Get(), nullptr, 0xFFFFFFFF);
		a_context->OMSetDepthStencilState(noDepth.Get(), 0);
		a_context->GSSetShader(gs.Get(), nullptr, 0);
		a_context->PSSetShader((a_modelSpaceNormals ? psModelSpace : psTangentSpace).Get(), nullptr, 0);
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
