#pragma once

#include "SkinFlowAPI.h"

namespace skinflow::RenderHooks
{
	// kDataLoaded: BSLightingShader::RestoreGeometry and the per-frame tick.
	void Install();

	// SkinFlow_GetSurface: classifies the pass, assigns its surface and arms the gravity capture
	// for the draw that follows (disarmed again in RestoreGeometry).
	bool OnSetupGeometry(RE::BSRenderPass* a_pass, SkinFlowSurface* a_out);
}
