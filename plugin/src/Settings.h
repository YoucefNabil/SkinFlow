#pragma once

#include "SkinFlowAPI.h"

namespace skinflow
{
	// SKSE/Plugins/SkinFlow.ini.
	struct Settings
	{
		// Read once at load: a change needs a restart.
		bool enabled = true;
		std::uint32_t maxResolution = 512;  // largest surface side in texels (the body's)
		std::uint32_t maxSurfaces = 48;     // actor+UV layout pairs simulated at once (about 4 per actor)
		std::uint32_t statsInterval = 600;  // frames between log lines, 0 = off

		// [Simulation]: how sweat comes out, runs and dries (FlowPass, Flow.hlsl).
		struct Simulation
		{
			float sweatRate = 0.9f;     // water per square world unit and second at full sweat
			float runSpeed = 0.7f;      // world units per second of a drop running down vertical skin
			float evaporation = 0.05f;  // water per square world unit and second
			float residual = 0.03f;     // water a running drop leaves on each texel it crosses
			float poreShare = 0.1f;     // share of texels that sweat (bead spacing)
			float cellSize = 0.28f;     // world units per texel: the simulated drop size, the same on every body part
			bool connectSeams = true;   // water crosses UV cuts and the joins between body parts (SeamBuilder)
		};

		// Re-read while the game runs whenever the file changes.
		struct Live
		{
			std::uint32_t debugView = 0;  // SkinFlowView
			float amount = 0.3f;           // sweat on every simulated actor (test value until exertion drives it)
			SkinFlowLook look{ .dropSize = 0.8f, .sheen = 0.34f };
			Simulation simulation{};
		};

		static Settings& Get();
		void Load();
		// Main thread: reloads the live part when SkinFlow.ini was saved since the last look.
		void ReloadIfChanged();
		Live GetLive() const;
		// Any thread (the settings menu): applies at once, not saved.
		void SetLive(const Live& a_live);
		// Writes the live part to SkinFlow.ini. False when a key could not be written.
		bool SaveLive();
		// Drops unsaved changes: the live part from SkinFlow.ini again.
		void RevertLive();

	private:
		void LoadLive();

		mutable std::mutex liveLock;
		Live live;
		std::filesystem::file_time_type iniTime{};
	};
}
