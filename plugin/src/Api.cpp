#include "RenderHooks.h"

extern "C" __declspec(dllexport) std::uint32_t SkinFlow_GetApiVersion()
{
	return SKINFLOW_API_VERSION;
}

extern "C" __declspec(dllexport) bool SkinFlow_GetSurface(RE::BSRenderPass* a_pass, SkinFlowSurface* a_out)
{
	return skinflow::RenderHooks::OnSetupGeometry(a_pass, a_out);
}
