#include "FlowPass.h"

#include "D3DStateBackup.h"
#include "Settings.h"

namespace skinflow
{
	FlowPass& FlowPass::Get()
	{
		static FlowPass pass;
		return pass;
	}

	bool FlowPass::CompileShader(ID3D11Device* a_device)
	{
		const auto groupSize = std::to_string(kGroupSize);
		const D3D_SHADER_MACRO defines[]{ { "GROUP_SIZE", groupSize.c_str() }, { nullptr, nullptr } };
		const auto code = source.Compile("CSMain", "cs_5_0", defines);
		ComPtr<ID3D11ComputeShader> next;
		if (!code || FAILED(a_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, next.GetAddressOf()))) {
			return false;
		}
		shader = std::move(next);
		SetDebugName(shader.Get(), "SkinFlow::FlowCS");
		return true;
	}

	void FlowPass::ReloadShaderIfChanged(ID3D11DeviceContext* a_context, std::uint64_t a_frame)
	{
		if (!source.Changed(a_frame)) {
			return;
		}
		ComPtr<ID3D11Device> device;
		a_context->GetDevice(device.GetAddressOf());
		if (CompileShader(device.Get())) {
			logger::info("{} changed: shader reloaded", source.Name());
		} else {
			logger::error("{} changed but does not compile: kept the previous shader", source.Name());
		}
	}

	bool FlowPass::Init(ID3D11Device* a_device)
	{
		source.MarkCompiled();
		if (!CompileShader(a_device)) {
			return false;
		}
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = sizeof(Constants);
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(a_device->CreateBuffer(&desc, nullptr, constants.GetAddressOf()))) {
			return false;
		}
		SetDebugName(constants.Get(), "SkinFlow::FlowConstants");
		logger::info("flow pass ready");
		return true;
	}

	void FlowPass::Step(ID3D11DeviceContext* a_context, Surface& a_surface)
	{
		const auto frame = SurfacePool::Get().Frame();
		if (failed || a_surface.waterFrame == frame) {
			return;
		}
		if (!ready) {
			ComPtr<ID3D11Device> device;
			a_context->GetDevice(device.GetAddressOf());
			ready = Init(device.Get());
			if (!ready) {
				failed = true;
				logger::error("flow pass disabled: shader or buffer creation failed");
				return;
			}
		}
		ReloadShaderIfChanged(a_context, frame);

		const auto now = std::chrono::steady_clock::now();
		const bool first = a_surface.waterTime == std::chrono::steady_clock::time_point{};
		const float dt = first ? 0.0f : std::min(std::chrono::duration<float>(now - a_surface.waterTime).count(), kMaxStepSeconds);
		a_surface.waterTime = now;
		if (dt <= 0.0f || a_surface.uvScale <= 0.0f) {
			return;  // nothing written: the read buffer stays current
		}

		const auto& settings = Settings::Get();
		const auto live = settings.GetLive();
		const Constants values{
			dt,
			++a_surface.waterSteps,
			a_surface.size,
			a_surface.uvScale / static_cast<float>(a_surface.size),
			live.amount,
			live.simulation.sweatRate,
			live.simulation.runSpeed,
			live.simulation.evaporation,
			live.simulation.residual,
			live.simulation.poreShare,
			{}
		};
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		std::memcpy(mapped.pData, &values, sizeof(values));
		a_context->Unmap(constants.Get(), 0);

		ComputeStateBackup saved(a_context);
		ID3D11ShaderResourceView* inputs[2]{ a_surface.srv[a_surface.read].Get(), a_surface.waterSrv[a_surface.waterRead].Get() };
		ID3D11UnorderedAccessView* output = a_surface.waterUav[a_surface.WaterWriteIndex()].Get();
		a_context->CSSetShader(shader.Get(), nullptr, 0);
		a_context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
		a_context->CSSetShaderResources(0, 2, inputs);
		a_context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		const UINT groups = (a_surface.size + kGroupSize - 1) / kGroupSize;
		a_context->Dispatch(groups, groups, 1);
		saved.Restore(a_context);
		a_surface.waterFrame = frame;
	}
}
