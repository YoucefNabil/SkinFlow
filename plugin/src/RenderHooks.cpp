#include "RenderHooks.h"

#include "FlowPass.h"
#include "GravityPass.h"
#include "SeamBuilder.h"
#include "Settings.h"
#include "SurfacePool.h"

namespace skinflow::RenderHooks
{
	namespace
	{
		// Engine draw path for a skinned pass (BSBatchRenderer::RenderPassImmediately, AE 107644):
		//   shader->SetupGeometry(pass)      <- Community Shaders calls SkinFlow_GetSurface here: we arm
		//   skinInstance->Draw(params)       <- vfunc 37: per partition, set bones and draw (dismembered
		//                                       partitions skipped); params[1] is the geometry
		//   shader->RestoreGeometry(pass)    <- we disarm
		// The capture runs right after the skin instance's draw by calling it once more, so skinning,
		// face morphs (dynamic vertices uploaded just before) and bone palettes are the game's own, and
		// it works whatever wraps the D3D11 context. (The generic per-pass draw, 107637, is not used for
		// skinned geometry; calling it on a face mesh crashed.)
		struct SkinDrawParams
		{
			void* shaderBones;
			RE::BSGeometry* geometry;
		};

		struct Armed
		{
			RE::BSGeometry* geometry = nullptr;
			Surface* surface = nullptr;
			bool modelSpaceNormals = false;
			bool capture = false;  // draws the part's own skin (not a blended overlay or decal on it)
			bool drawn = false;  // the skin instance drew this pass
		};

		// The armed pass when this skin draw is its draw (once per pass), else null.
		const Armed* TakeArmed(const SkinDrawParams* a_params);
		ID3D11DeviceContext* RendererContext();

		template <std::size_t Variant>
		struct SkinInstanceDraw
		{
			static std::uint64_t thunk(RE::NiSkinInstance* a_this, SkinDrawParams* a_params)
			{
				const auto result = func(a_this, a_params);
				if (const auto* armed = TakeArmed(a_params)) {
					if (auto* context = RendererContext()) {
						SeamBuilder::Get().Tick(context);
						FlowPass::Get().Step(context, *armed->surface);
						if (armed->capture) {
							GravityPass::Get().Capture(context, *armed->surface, a_params->geometry, armed->modelSpaceNormals, [=] { func(a_this, a_params); });
						}
					}
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		ID3D11DeviceContext* RendererContext()
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			return renderer ? reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context) : nullptr;
		}

		thread_local Armed t_armed;

		ComPtr<ID3D11Device> g_device;

		std::atomic<std::uint32_t> g_requests{ 0 };
		std::atomic<std::uint32_t> g_served{ 0 };
		std::atomic<std::uint32_t> g_armedNotDrawn{ 0 };  // armed passes drawn by another path (not captured)

		constexpr std::uint16_t kHeadSlot = 30;
		constexpr std::uint16_t kLastBodySlot = 61;  // 30-61 are body parts; 130+ are head part partitions

		// The lowest body part slot among the skin's dismember partitions (a body mesh that also
		// covers the hands still lands on the body), 0 when it has none.
		std::uint16_t BodySlot(RE::NiSkinInstance* a_skin)
		{
			auto* dismember = netimmerse_cast<RE::BSDismemberSkinInstance*>(a_skin);
			if (!dismember) {
				return 0;
			}
			const auto& data = dismember->GetRuntimeData();
			std::uint16_t lowest = 0;
			for (std::int32_t i = 0; i < data.numPartitions && data.partitions; ++i) {
				const auto slot = data.partitions[i].slot;
				if (slot >= kHeadSlot && slot <= kLastBodySlot && (!lowest || slot < lowest)) {
					lowest = slot;
				}
			}
			return lowest;
		}

		// The player's first-person model (arms and hands) carries the player as its user data and the
		// same body slots and textures as the third-person body, but its world positions are not the
		// body's (they measured 0 to a quarter of the body's scale): it would re-measure and resize the
		// player's surfaces on every camera switch. It keeps Advanced Skin's own sweat.
		bool InFirstPersonModel(RE::TESObjectREFR* a_ref, const RE::NiAVObject* a_geometry)
		{
			if (!a_ref->IsPlayerRef()) {
				return false;
			}
			const auto* firstPerson = a_ref->Get3D(true);
			if (!firstPerson || firstPerson == a_ref->Get3D(false)) {
				return false;
			}
			for (const RE::NiAVObject* node = a_geometry; node; node = node->parent) {
				if (node == firstPerson) {
					return true;
				}
			}
			return false;
		}

		// Actor skin drawn with a skinned mesh: face tint (head) or skin tint (body, hands, feet,
		// and armour pieces that show skin). Keyed by body part, so RaceMenu overlays (copies of
		// the body that keep its partitions) and face decals share the part's surface; by diffuse
		// texture when the part is unknown.
		std::optional<SurfaceKey> Classify(RE::BSRenderPass* a_pass)
		{
			if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty) {
				return std::nullopt;
			}
			auto* geometry = a_pass->geometry;
			if (!geometry->GetGeometryRuntimeData().skinInstance) {
				return std::nullopt;
			}
			auto* ref = geometry->GetUserData();
			if (!ref || ref->GetFormType() != RE::FormType::ActorCharacter || InFirstPersonModel(ref, geometry)) {
				return std::nullopt;
			}
			auto* material = a_pass->shaderProperty->GetBaseMaterial();
			if (!material) {
				return std::nullopt;
			}
			const auto feature = material->GetFeature();
			if (feature != RE::BSShaderMaterial::Feature::kFaceGen && feature != RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
				return std::nullopt;
			}
			const auto* lighting = static_cast<const RE::BSLightingShaderMaterialBase*>(material);
			const auto slot = feature == RE::BSShaderMaterial::Feature::kFaceGen ? kHeadSlot : BodySlot(geometry->GetGeometryRuntimeData().skinInstance.get());
			return SurfaceKey{ ref->GetFormID(), slot, slot ? nullptr : lighting->diffuseTexture.get() };
		}

		// A pass blended over the part's skin (RaceMenu overlay, decal) shares its surface but must
		// not write the gravity field: it may cover only part of the UVs or carry another normal map.
		bool DrawsOwnSkin(RE::BSRenderPass* a_pass)
		{
			const auto* alpha = a_pass->geometry->GetGeometryRuntimeData().alphaProperty.get();
			return !alpha || !alpha->GetAlphaBlending();
		}

		// Names a surface after the first pass drawn on it since its assignment.
		void Label(Surface& a_surface, RE::BSRenderPass* a_pass)
		{
			if (!a_surface.label.empty()) {
				return;
			}
			const auto* lighting = static_cast<const RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->GetBaseMaterial());
			const auto* texture = lighting->diffuseTexture.get();
			const char* name = texture && !texture->name.empty() ? texture->name.c_str() : "?";
			a_surface.label = a_surface.key.bodySlot ? std::format("{} (slot {})", name, a_surface.key.bodySlot) : std::string(name);
		}

		const Armed* TakeArmed(const SkinDrawParams* a_params)
		{
			auto& armed = t_armed;
			if (!armed.surface || armed.drawn || !a_params || a_params->geometry != armed.geometry) {
				return nullptr;
			}
			armed.drawn = true;
			return &armed;
		}

		struct RestoreGeometry
		{
			static void thunk(RE::BSShader* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
			{
				if (t_armed.surface && !t_armed.drawn) {
					g_armedNotDrawn.fetch_add(1, std::memory_order_relaxed);
				}
				t_armed = {};
				func(a_this, a_pass, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Main loop, before the frame is built: publish last frame's gravity and water, log stats.
		struct MainUpdate
		{
			static void thunk(RE::Main* a_this, float a_arg)
			{
				auto& pool = SurfacePool::Get();
				const auto frame = pool.Frame();
				const auto interval = Settings::Get().statsInterval;
				if (interval && frame % interval == 0) {
					logger::info("frame {}: skin passes {} asked, {} served, {} refused (pool full), {} not drawn by the skin instance, {} surfaces active, {} draws captured",
						frame, g_requests.exchange(0), g_served.exchange(0), pool.TakeRefusals(), g_armedNotDrawn.exchange(0), pool.ActiveCount(),
						GravityPass::Get().TakeCaptureCount());
				}
				if (frame % 60 == 0) {
					Settings::Get().ReloadIfChanged();
				}
				pool.BeginFrame();
				func(a_this, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		stl::write_vfunc<7, RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
		// BSDismemberSkinInstance overrides the draw and calls NiSkinInstance's directly, so each
		// skinned draw passes through exactly one of these.
		stl::write_vfunc<37, SkinInstanceDraw<0>>(RE::VTABLE_NiSkinInstance[0]);
		stl::write_vfunc<37, SkinInstanceDraw<1>>(RE::VTABLE_BSDismemberSkinInstance[0]);
		stl::write_thunk_call<MainUpdate>(REL::RelocationID(35551, 36544).address() + REL::Relocate(0x11F, 0x160));
		logger::info("hooks installed");
	}

	bool OnSetupGeometry(RE::BSRenderPass* a_pass, SkinFlowSurface* a_out)
	{
		t_armed = {};
		if (!a_out) {
			return false;
		}
		*a_out = {};

		const auto& settings = Settings::Get();
		if (!settings.enabled || GravityPass::Get().Failed()) {
			return false;
		}
		const auto key = Classify(a_pass);
		if (!key) {
			return false;
		}
		g_requests.fetch_add(1, std::memory_order_relaxed);

		auto* context = RendererContext();
		if (!context) {
			return false;
		}
		if (!g_device) {
			context->GetDevice(g_device.GetAddressOf());
		}
		auto* surface = SurfacePool::Get().Acquire(g_device.Get(), context, *key, key->actor == 0x14);
		if (!surface) {
			return false;
		}
		Label(*surface, a_pass);
		const bool modelSpaceNormals = a_pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals);
		t_armed = { a_pass->geometry, surface, modelSpaceNormals, DrawsOwnSkin(a_pass), false };
		g_served.fetch_add(1, std::memory_order_relaxed);

		const auto live = settings.GetLive();
		a_out->srv = surface->srv[surface->read].Get();
		a_out->water = surface->waterSrv[surface->waterRead].Get();
		a_out->amount = live.amount;
		a_out->uvScale = surface->uvScale;
		a_out->view = static_cast<SkinFlowView>(live.debugView);
		a_out->look = live.look;
		return true;
	}
}
