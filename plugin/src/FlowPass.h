#pragma once

#include "Settings.h"
#include "ShaderFile.h"
#include "SurfacePool.h"

namespace skinflow
{
	// The sweat simulation (Flow.hlsl): one compute step per surface and frame, in the surface's UV
	// space, driven by its gravity field. Sweat comes out of the pores, beads grow until gravity
	// pulls them loose, run downhill gathering the water in their path, leave a wet trail, fall off
	// skin that faces down, and dry. Per texel: x = water (1 = a drop heavy enough to run down
	// vertical skin), y = trail wetness. With seams built (SeamBuilder), water also crosses the cuts
	// of the UV layout and the joins between body parts, and the gutter around each UV island shows
	// the water across the seam.
	class FlowPass
	{
	public:
		static FlowPass& Get();

		// Render thread, at a surface's first skin draw of the frame: steps last frame's water with
		// last frame's gravity into the water write buffer (published by SurfacePool::BeginFrame).
		// Runs at most once per surface and frame; does nothing until the surface's scale and gravity
		// are known. A new or resized surface first warms up: several steps a frame until it has
		// simulated kWarmUpSeconds, so an actor who comes into view is already as sweaty as the
		// sweat amount makes them, rather than starting dry in front of the player.
		void Step(ID3D11DeviceContext* a_context, Surface& a_surface);

		// Longest simulated step: after a pause drops move on, they do not jump.
		static constexpr float kMaxStepSeconds = 0.1f;
		// Warm-up: simulated time a surface starts with, steps per frame while warming (odd, so the
		// result lands in the write buffer), and the most warm-up steps all surfaces take in one frame.
		static constexpr float kWarmUpSeconds = 30.0f;
		static constexpr std::uint32_t kWarmUpStepsPerFrame = 9;
		static constexpr std::uint32_t kWarmUpBudget = 36;
		static_assert(kWarmUpStepsPerFrame % 2 == 1);

	private:
		// Flow.hlsl FlowConstants (b0).
		struct Constants
		{
			float dt;
			std::uint32_t step;
			std::uint32_t size;
			float amount;  // sweat drive 0..1
			float sweatRate;
			float runSpeed;
			float evaporation;
			float residual;
			float poreShare;  // share of texels that sweat
			std::uint32_t seams;  // 1 when the surface's Links are bound
			float pad[2];
		};
		static_assert(sizeof(Constants) % 16 == 0);

		// Flow.hlsl SeamConstants (b1).
		struct SeamConstants
		{
			std::uint32_t offset;
			std::uint32_t count;
			std::uint32_t pad[2];
		};
		static_assert(sizeof(SeamConstants) % 16 == 0);

		static constexpr UINT kGroupSize = 8;       // [numthreads] of CSMain
		static constexpr UINT kSeamGroupSize = 64;  // [numthreads] of CSSend and CSFill

		bool Init(ID3D11Device* a_device);
		// One step from water buffer a_from into the other, then across the seams. Expects b0-b1 bound.
		bool StepOnce(ID3D11DeviceContext* a_context, Surface& a_surface, const Settings::Live& a_live, bool a_seams, float a_dt, std::uint32_t a_from);
		// Steps this surface takes this frame: kWarmUpStepsPerFrame while warming up and the frame's
		// warm-up budget allows, else 1.
		std::uint32_t StepsThisFrame(const Surface& a_surface, std::uint64_t a_frame);
		bool CompileShaders(ID3D11Device* a_device);
		void ReloadShadersIfChanged(ID3D11DeviceContext* a_context, std::uint64_t a_frame);
		// CSSend, then CSFill, over the surface's seam batches whose other surface is still the one
		// they were built for. Runs after the step, with its t0-t1 and b0 still bound.
		void CrossSeams(ID3D11DeviceContext* a_context, Surface& a_surface, ID3D11UnorderedAccessView* a_waterOut);
		bool SetBatch(ID3D11DeviceContext* a_context, const SeamBatch& a_batch);

		ShaderFile source{ L"Flow.hlsl" };
		ComPtr<ID3D11ComputeShader> shader;
		ComPtr<ID3D11ComputeShader> sendShader;
		ComPtr<ID3D11ComputeShader> fillShader;
		ComPtr<ID3D11Buffer> constants;
		ComPtr<ID3D11Buffer> seamConstants;
		bool ready = false;
		bool failed = false;
		std::uint64_t warmUpFrame = 0;     // frame warmUpSteps counts
		std::uint32_t warmUpSteps = 0;     // warm-up steps taken in that frame
	};
}
