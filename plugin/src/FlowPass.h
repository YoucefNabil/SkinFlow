#pragma once

#include "ShaderFile.h"
#include "SurfacePool.h"

namespace skinflow
{
	// The sweat simulation (Flow.hlsl): one compute step per surface and frame, in the surface's UV
	// space, driven by its gravity field. Sweat comes out of the pores, beads grow until gravity
	// pulls them loose, run downhill gathering the water in their path, leave a wet trail, fall off
	// skin that faces down, and dry. Per texel: x = water (1 = a drop heavy enough to run down
	// vertical skin), y = trail wetness.
	class FlowPass
	{
	public:
		static FlowPass& Get();

		// Render thread, at a surface's first skin draw of the frame: steps last frame's water with
		// last frame's gravity into the water write buffer (published by SurfacePool::BeginFrame).
		// Runs at most once per surface and frame; does nothing until the surface's scale is known.
		void Step(ID3D11DeviceContext* a_context, Surface& a_surface);

		// Longest simulated step: after a pause drops move on, they do not jump.
		static constexpr float kMaxStepSeconds = 0.1f;

	private:
		// Flow.hlsl FlowConstants (b0).
		struct Constants
		{
			float dt;
			std::uint32_t step;
			std::uint32_t size;
			float texelWorld;  // world units per texel
			float amount;      // sweat drive 0..1
			float sweatRate;
			float runSpeed;
			float evaporation;
			float residual;
			float poreShare;  // share of texels that sweat
			float pad[2];
		};
		static_assert(sizeof(Constants) % 16 == 0);

		static constexpr UINT kGroupSize = 8;  // [numthreads] in Flow.hlsl

		bool Init(ID3D11Device* a_device);
		bool CompileShader(ID3D11Device* a_device);
		void ReloadShaderIfChanged(ID3D11DeviceContext* a_context, std::uint64_t a_frame);

		ShaderFile source{ L"Flow.hlsl" };
		ComPtr<ID3D11ComputeShader> shader;
		ComPtr<ID3D11Buffer> constants;
		bool ready = false;
		bool failed = false;
	};
}
