#pragma once

#include "SurfacePool.h"

namespace skinflow
{
	// Diagnostic: every stats interval, reads the player's finished gravity surfaces back to the CPU
	// (copy now, map a few frames later without stalling) and logs what they hold, so the field can
	// be checked from SkinFlow.log: a standing body is mostly steep, lying on the back faces up.
	class SurfaceProbe
	{
	public:
		static SurfaceProbe& Get();

		// Render thread, when a surface starts this frame's write: its read buffer holds last
		// frame's complete field.
		void OnFrameStart(ID3D11DeviceContext* a_context, const Surface& a_surface);

	private:
		struct Pending
		{
			ComPtr<ID3D11Texture2D> staging;
			std::string label;
			std::uint64_t frame = 0;
		};

		void Request(ID3D11DeviceContext* a_context, const Surface& a_surface, std::uint64_t a_frame);
		void Collect(ID3D11DeviceContext* a_context, std::uint64_t a_frame);
		static void Log(const Pending& a_pending, const D3D11_MAPPED_SUBRESOURCE& a_data, std::uint32_t a_size);

		std::mutex lock;
		std::vector<Pending> pending;
		std::vector<const Surface*> requested;  // surfaces copied for the current interval
		std::uint64_t requestFrame = 0;
	};
}
