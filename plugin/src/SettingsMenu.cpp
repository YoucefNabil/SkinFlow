#include "SettingsMenu.h"

#include "Settings.h"

#pragma warning(push)
#pragma warning(disable : 4099 4100 4189 4244 4267 4456 4458 5054)
#include "SKSEMenuFramework.h"
#pragma warning(pop)

namespace skinflow::SettingsMenu
{
	namespace
	{
		// Render thread only (the framework draws the page).
		bool unsaved = false;
		std::string status;

		bool Slider(const char* a_label, float& a_value, float a_min, float a_max, const char* a_format, const char* a_help)
		{
			const bool changed = ImGui::SliderFloat(a_label, &a_value, a_min, a_max, a_format, ImGuiSliderFlags_AlwaysClamp);
			ImGui::SetItemTooltip("%s\n\nCtrl+click to type a value.", a_help);
			return changed;
		}

		void Toolbar()
		{
			auto& settings = Settings::Get();
			if (ImGui::Button("Save")) {
				status = settings.SaveLive() ? "Saved to SKSE\\Plugins\\SkinFlow.ini." : "Saving FAILED (see SkinFlow.log).";
				unsaved = false;
			}
			ImGui::SetItemTooltip("Writes these settings to SkinFlow.ini so they are kept after you quit the game.");
			ImGui::SameLine();
			if (ImGui::Button("Revert to saved")) {
				settings.RevertLive();
				unsaved = false;
				status = "Back to the settings saved in SkinFlow.ini.";
			}
			ImGui::SetItemTooltip("Throws away the changes made since the last Save.");
			ImGui::SameLine();
			if (ImGui::Button("Defaults")) {
				settings.SetLive(Settings::Live{});
				unsaved = true;
				status = "Default settings restored (not saved yet).";
			}
			ImGui::SetItemTooltip("Puts every setting on this page back to SkinFlow's defaults. Press Save to keep them.");
			if (unsaved) {
				ImGui::TextColored(ImVec4{ 1.0f, 0.8f, 0.3f, 1.0f }, "Unsaved changes: they apply now but are lost when you quit unless you Save.");
			} else {
				ImGui::TextDisabled("Changes apply immediately. Hover a setting for what it does.");
			}
			if (!status.empty()) {
				ImGui::TextDisabled("%s", status.c_str());
			}
			ImGui::Separator();
		}

		void __stdcall Render()
		{
			Toolbar();
			auto live = Settings::Get().GetLive();
			auto& sim = live.simulation;
			bool changed = false;

			changed |= Slider("Sweat amount", live.amount, 0.0f, 1.0f, "%.2f",
				"How much everyone sweats right now: 0 = dry, 1 = drenched.\n"
				"For now this is the same for every actor; later it will follow how hard each one is working.\n"
				"Community Shaders Advanced Skin's own exertion sweat is added on top.");

			ImGui::SeparatorText("Drops");
			changed |= Slider("Sweat rate", sim.sweatRate, 0.0f, 2.0f, "%.2f",
				"How fast sweat comes out of the skin.\n"
				"Higher: beads fill up and run more often (a drop every few seconds).\n"
				"Lower: beads grow slowly and run now and then.");
			changed |= Slider("Sweat spots", sim.poreShare, 0.02f, 0.6f, "%.2f",
				"Share of the skin that sweats (0.15 = 15% of spots).\n"
				"Lower: fewer beads, further apart; each fills faster, so they run sooner.\n"
				"Higher: many small beads close together.\n"
				"The total amount of sweat stays the same.");
			changed |= Slider("Run speed", sim.runSpeed, 0.0f, 20.0f, "%.1f",
				"How fast a drop runs down vertical skin, in game units per second (1 unit = 1.43 cm).\n"
				"On gentler slopes drops run slower; on flat skin they stay put.");
			changed |= Slider("Water left behind", sim.residual, 0.0f, 0.1f, "%.3f",
				"How much water a running drop leaves on each spot it passes (its wet trail).\n"
				"Lower: drops run much further before they stop.\n"
				"Higher: short runs that end as beads, wetter trails.");
			changed |= Slider("Drying", sim.evaporation, 0.0f, 0.5f, "%.3f",
				"How fast water dries off the skin.\n"
				"Higher: small beads vanish before they can run, and trails dry sooner.\n"
				"If it is higher than the sweat rate, sweat dries before beads ever form.");

			ImGui::SeparatorText("Look");
			changed |= Slider("Drop size", live.look.dropSize, 0.25f, 4.0f, "%.2f",
				"How big each drop is drawn.\n"
				"Only changes how they look: where drops are and when they run comes from the simulation.");
			changed |= Slider("Sheen", live.look.sheen, 0.0f, 1.0f, "%.2f",
				"Shine of the thin wet film along drop trails and where sweat has not gathered into beads yet.\n"
				"0 = only the drops themselves shine.");

			if (ImGui::CollapsingHeader("Advanced")) {
				changed |= Slider("Simulation detail", sim.cellSize, 0.1f, 1.0f, "%.2f",
					"Size of one simulated spot on the skin, in game units (0.25 = about 3.6 mm). It sets the smallest drop.\n"
					"Smaller: finer, more numerous drops and more GPU work. Larger: coarser and cheaper.\n"
					"Changing it starts the sweat over on every body.");
				const char* views[] = { "Sweat (normal)", "Gravity colours", "Water heat map" };
				int view = static_cast<int>(std::min(live.debugView, 2u));
				if (ImGui::Combo("Debug view", &view, views, 3)) {
					live.debugView = static_cast<std::uint32_t>(view);
					changed = true;
				}
				ImGui::SetItemTooltip(
					"What the skin shows.\n"
					"Sweat: the normal look.\n"
					"Gravity colours: which way is downhill on the skin, for checking the simulation sees the body right.\n"
					"Water heat map: how much water the simulation holds where (blue dry, red a drop about to run).");
				ImGui::TextDisabled("Texture size, actor count and on/off are in SkinFlow.ini [General] and need a restart.");
			}

			if (changed) {
				Settings::Get().SetLive(live);
				unsaved = true;
				status.clear();
			}
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			logger::info("menu: SKSE Menu Framework not installed, settings only in SkinFlow.ini");
			return;
		}
		SKSEMenuFramework::SetSection("SkinFlow");
		SKSEMenuFramework::AddSectionItem("Sweat", Render);
		logger::info("menu: registered with SKSE Menu Framework");
	}
}
