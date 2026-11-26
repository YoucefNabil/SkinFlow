#pragma once

#include "D3DStateBackup.h"
#include "ShaderFile.h"
#include "SurfacePool.h"

namespace skinflow
{
	// Draws a skin geometry a second time with our geometry + pixel shader, which put each triangle
	// at its UV position and write the downhill direction there. The game's own vertex shader does
	// the skinning (bones, facegen morphs), so nothing is re-implemented here.
	class GravityPass
	{
	public:
		static GravityPass& Get();

		// Render thread, while the game's pass for a_geometry is still set up (its vertex shader,
		// buffers, bone constants and normal map bound). a_draw issues the game's draw again; it runs at
		// most once per geometry and frame, with the pipeline pointed at the surface's write buffer.
		// a_modelSpaceNormals: the material's normal map is model space (skin), else tangent space.
		template <class Draw>
		void Capture(ID3D11DeviceContext* a_context, Surface& a_surface, const void* a_geometry, bool a_modelSpaceNormals, Draw&& a_draw)
		{
			if (!Prepare(a_context, a_surface, a_geometry)) {
				return;
			}
			D3DStateBackup saved(a_context);
			Bind(a_context, a_surface, a_modelSpaceNormals);
			a_draw();
			saved.Restore(a_context);
			captures.fetch_add(1, std::memory_order_relaxed);
		}

		bool Failed() const { return failed; }

		// Fixed-point factor of the world-units-per-UV sums (UV_SCALE_FIXED in Gravity.hlsl).
		static constexpr std::uint32_t kUvScaleFixed = 4;
		// Frames between uvScale read-backs once a surface has one, and the wait before mapping.
		static constexpr std::uint64_t kUvScaleInterval = 60;
		static constexpr std::uint64_t kReadbackDelayFrames = 3;
		std::uint32_t TakeCaptureCount() { return captures.exchange(0); }

	private:
		bool Init(ID3D11Device* a_device);
		// Lazy init, first-write-of-the-frame clear (of the seam snapshot too when one is asked for this
		// frame), and the once-per-geometry check.
		bool Prepare(ID3D11DeviceContext* a_context, Surface& a_surface, const void* a_geometry);
		void Bind(ID3D11DeviceContext* a_context, Surface& a_surface, bool a_modelSpaceNormals);
		static bool CapturingSeams(const Surface& a_surface, std::uint64_t a_frame);
		// At a surface's first write of a frame: reads back an earlier copy of the uvScale sums, or
		// copies the last frame's sums (before they are cleared) when a new value is due.
		void MeasureUvScale(ID3D11DeviceContext* a_context, Surface& a_surface, std::uint64_t a_frame);
		// Compiles Gravity.hlsl's shaders; replaces the current ones only when all succeed.
		bool CompileShaders(ID3D11Device* a_device);
		// Recompiles when Gravity.hlsl was saved (keeps the old shaders when the new file does not compile).
		void ReloadShadersIfChanged(ID3D11DeviceContext* a_context, std::uint64_t a_frame);

		ComPtr<ID3D11GeometryShader> gs;
		// [seam snapshot][model-space normals]
		std::array<std::array<ComPtr<ID3D11PixelShader>, 2>, 2> ps;
		ShaderFile source{ L"Gravity.hlsl" };
		ComPtr<ID3D11RasterizerState> raster;
		ComPtr<ID3D11DepthStencilState> noDepth;
		ComPtr<ID3D11BlendState> opaque;
		bool ready = false;
		bool failed = false;

		std::atomic<std::uint32_t> captures{ 0 };
	};
}
