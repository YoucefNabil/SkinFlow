#include "D3DStateBackup.h"

namespace skinflow
{
	D3DStateBackup::D3DStateBackup(ID3D11DeviceContext* a_context)
	{
		a_context->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs.data(), &dsv,
			0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, uavs.data());
		a_context->RSGetViewports(&numViewports, viewports.data());
		a_context->RSGetState(&raster);
		a_context->OMGetBlendState(&blend, blendFactor, &sampleMask);
		a_context->OMGetDepthStencilState(&depth, &stencilRef);
		a_context->GSGetShader(&gs, nullptr, nullptr);
		a_context->PSGetShader(&ps, nullptr, nullptr);
	}

	void D3DStateBackup::Restore(ID3D11DeviceContext* a_context)
	{
		UINT numRtvs = 0;
		for (UINT i = 0; i < rtvs.size(); ++i) {
			if (rtvs[i]) {
				numRtvs = i + 1;
			}
		}
		const UINT numUavs = static_cast<UINT>(uavs.size()) - numRtvs;
		std::array<UINT, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> keepCounts;
		keepCounts.fill(static_cast<UINT>(-1));
		a_context->OMSetRenderTargetsAndUnorderedAccessViews(numRtvs, rtvs.data(), dsv, numRtvs, numUavs, uavs.data() + numRtvs, keepCounts.data());
		a_context->RSSetViewports(numViewports, viewports.data());
		a_context->RSSetState(raster);
		a_context->OMSetBlendState(blend, blendFactor, sampleMask);
		a_context->OMSetDepthStencilState(depth, stencilRef);
		a_context->GSSetShader(gs, nullptr, 0);
		a_context->PSSetShader(ps, nullptr, 0);
	}

	D3DStateBackup::~D3DStateBackup()
	{
		const auto release = [](IUnknown* a_object) {
			if (a_object) {
				a_object->Release();
			}
		};
		for (auto* view : rtvs) {
			release(view);
		}
		for (auto* view : uavs) {
			release(view);
		}
		release(dsv);
		release(raster);
		release(blend);
		release(depth);
		release(gs);
		release(ps);
	}

	ComputeStateBackup::ComputeStateBackup(ID3D11DeviceContext* a_context)
	{
		a_context->CSGetShader(&shader, nullptr, nullptr);
		a_context->CSGetConstantBuffers(0, static_cast<UINT>(constants.size()), constants.data());
		a_context->CSGetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
		a_context->CSGetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data());
	}

	void ComputeStateBackup::Restore(ID3D11DeviceContext* a_context)
	{
		a_context->CSSetShader(shader, nullptr, 0);
		a_context->CSSetConstantBuffers(0, static_cast<UINT>(constants.size()), constants.data());
		a_context->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
		a_context->CSSetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data(), nullptr);
	}

	ComputeStateBackup::~ComputeStateBackup()
	{
		const auto release = [](IUnknown* a_object) {
			if (a_object) {
				a_object->Release();
			}
		};
		release(shader);
		for (auto* buffer : constants) {
			release(buffer);
		}
		for (auto* view : srvs) {
			release(view);
		}
		for (auto* view : uavs) {
			release(view);
		}
	}
}
