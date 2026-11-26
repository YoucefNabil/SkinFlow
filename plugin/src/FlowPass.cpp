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

	bool FlowPass::CompileShaders(ID3D11Device* a_device)
	{
		const auto groupSize = std::to_string(kGroupSize);
		const auto seamGroupSize = std::to_string(kSeamGroupSize);
		const D3D_SHADER_MACRO defines[]{ { "GROUP_SIZE", groupSize.c_str() }, { "SEAM_GROUP_SIZE", seamGroupSize.c_str() }, { nullptr, nullptr } };
		const auto create = [&](const char* a_entry, ComPtr<ID3D11ComputeShader>& a_out) {
			const auto code = source.Compile(a_entry, "cs_5_0", defines);
			return code && SUCCEEDED(a_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, a_out.GetAddressOf()));
		};
		ComPtr<ID3D11ComputeShader> nextStep, nextSend, nextFill;
		if (!create("CSMain", nextStep) || !create("CSSend", nextSend) || !create("CSFill", nextFill)) {
			return false;
		}
		shader = std::move(nextStep);
		sendShader = std::move(nextSend);
		fillShader = std::move(nextFill);
		SetDebugName(shader.Get(), "SkinFlow::FlowCS");
		SetDebugName(sendShader.Get(), "SkinFlow::FlowCS seam send");
		SetDebugName(fillShader.Get(), "SkinFlow::FlowCS seam fill");
		return true;
	}

	void FlowPass::ReloadShadersIfChanged(ID3D11DeviceContext* a_context, std::uint64_t a_frame)
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

	bool FlowPass::Init(ID3D11Device* a_device)
	{
		source.MarkCompiled();
		if (!CompileShaders(a_device)) {
			return false;
		}
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = sizeof(Constants);
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		D3D11_BUFFER_DESC seamDesc = desc;
		seamDesc.ByteWidth = sizeof(SeamConstants);
		if (FAILED(a_device->CreateBuffer(&desc, nullptr, constants.GetAddressOf())) ||
			FAILED(a_device->CreateBuffer(&seamDesc, nullptr, seamConstants.GetAddressOf()))) {
			return false;
		}
		SetDebugName(constants.Get(), "SkinFlow::FlowConstants");
		SetDebugName(seamConstants.Get(), "SkinFlow::SeamConstants");
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
		ReloadShadersIfChanged(a_context, frame);

		const auto now = std::chrono::steady_clock::now();
		const bool first = a_surface.waterTime == std::chrono::steady_clock::time_point{};
		const float elapsed = first ? 0.0f : std::min(std::chrono::duration<float>(now - a_surface.waterTime).count(), kMaxStepSeconds);
		a_surface.waterTime = now;
		if (a_surface.uvScale <= 0.0f || !a_surface.gravityReady) {
			return;  // nothing written: the read buffer stays current
		}
		const auto steps = StepsThisFrame(a_surface, frame);
		const float dt = steps > 1 ? kMaxStepSeconds : elapsed;
		if (dt <= 0.0f) {
			return;
		}

		const auto live = Settings::Get().GetLive();
		const bool seams = live.simulation.connectSeams && a_surface.seams.links;
		ComputeStateBackup saved(a_context);
		ID3D11Buffer* buffers[2]{ constants.Get(), seamConstants.Get() };
		a_context->CSSetConstantBuffers(0, 2, buffers);
		// Ping-pong between the two water buffers: an odd number of steps ends in the write buffer.
		std::uint32_t done = 0;
		while (done < steps && StepOnce(a_context, a_surface, live, seams, dt, a_surface.waterRead ^ (done & 1u))) {
			++done;
		}
		saved.Restore(a_context);
		a_surface.simulatedSeconds += dt * static_cast<float>(done);
		if (done & 1u) {
			a_surface.waterFrame = frame;  // published by BeginFrame (an even count ended in the read buffer: already current)
		}
	}

	std::uint32_t FlowPass::StepsThisFrame(const Surface& a_surface, std::uint64_t a_frame)
	{
		if (a_surface.simulatedSeconds >= kWarmUpSeconds) {
			return 1;
		}
		if (warmUpFrame != a_frame) {
			warmUpFrame = a_frame;
			warmUpSteps = 0;
		}
		if (warmUpSteps + kWarmUpStepsPerFrame > kWarmUpBudget) {
			return 1;  // others are warming up this frame: a normal step, warm up later
		}
		warmUpSteps += kWarmUpStepsPerFrame;
		return kWarmUpStepsPerFrame;
	}

	bool FlowPass::StepOnce(ID3D11DeviceContext* a_context, Surface& a_surface, const Settings::Live& a_live, bool a_seams, float a_dt, std::uint32_t a_from)
	{
		const Constants values{
			a_dt,
			++a_surface.waterSteps,
			a_surface.size,
			a_live.amount,
			a_live.simulation.sweatRate,
			a_live.simulation.runSpeed,
			a_live.simulation.evaporation,
			a_live.simulation.residual,
			a_live.simulation.poreShare,
			a_seams ? 1u : 0u,
			{}
		};
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		std::memcpy(mapped.pData, &values, sizeof(values));
		a_context->Unmap(constants.Get(), 0);

		// Outputs first: binding the new write buffer as a UAV unbinds it as last step's input.
		ID3D11UnorderedAccessView* waterOut = a_surface.waterUav[a_from ^ 1u].Get();
		ID3D11UnorderedAccessView* outputs[3]{ waterOut, a_surface.inflowUav.Get(), nullptr };
		a_context->CSSetUnorderedAccessViews(0, 3, outputs, nullptr);
		ID3D11ShaderResourceView* inputs[5]{ a_surface.srv[a_surface.read].Get(), a_surface.waterSrv[a_from].Get(),
			a_seams ? a_surface.seams.links.Get() : nullptr, nullptr, nullptr };
		a_context->CSSetShaderResources(0, 5, inputs);
		a_context->CSSetShader(shader.Get(), nullptr, 0);
		const UINT groups = (a_surface.size + kGroupSize - 1) / kGroupSize;
		a_context->Dispatch(groups, groups, 1);
		if (a_seams) {
			CrossSeams(a_context, a_surface, waterOut);
		}
		return true;
	}

	bool FlowPass::SetBatch(ID3D11DeviceContext* a_context, const SeamBatch& a_batch)
	{
		const auto* other = a_batch.other;
		if (!other || !other->inUse || other->generation != a_batch.generation || a_batch.count == 0) {
			return false;  // the surface across was reassigned or resized: wait for the rebuild
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(seamConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		const SeamConstants values{ a_batch.offset, a_batch.count, {} };
		std::memcpy(mapped.pData, &values, sizeof(values));
		a_context->Unmap(seamConstants.Get(), 0);
		return true;
	}

	void FlowPass::CrossSeams(ID3D11DeviceContext* a_context, Surface& a_surface, ID3D11UnorderedAccessView* a_waterOut)
	{
		const auto& seams = a_surface.seams;
		a_context->CSSetShader(sendShader.Get(), nullptr, 0);
		a_context->CSSetShaderResources(3, 1, seams.sends.GetAddressOf());
		for (const auto& batch : seams.sendBatches) {
			if (!SetBatch(a_context, batch)) {
				continue;
			}
			ID3D11UnorderedAccessView* outputs[3]{ nullptr, nullptr, batch.other->inflowUav.Get() };
			a_context->CSSetUnorderedAccessViews(0, 3, outputs, nullptr);
			a_context->Dispatch((batch.count + kSeamGroupSize - 1) / kSeamGroupSize, 1, 1);
		}

		a_context->CSSetShader(fillShader.Get(), nullptr, 0);
		ID3D11UnorderedAccessView* outputs[3]{ a_waterOut, nullptr, nullptr };
		a_context->CSSetUnorderedAccessViews(0, 3, outputs, nullptr);
		a_context->CSSetShaderResources(3, 1, seams.fills.GetAddressOf());
		for (const auto& batch : seams.fillBatches) {
			if (!SetBatch(a_context, batch)) {
				continue;
			}
			// The other surface's published water (its write buffer is never bound here).
			ID3D11ShaderResourceView* across = batch.other->waterSrv[batch.other->waterRead].Get();
			a_context->CSSetShaderResources(4, 1, &across);
			a_context->Dispatch((batch.count + kSeamGroupSize - 1) / kSeamGroupSize, 1, 1);
		}
	}
}
