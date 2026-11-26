#include "Settings.h"

namespace skinflow
{
	namespace
	{
		constexpr auto kPath = L"Data/SKSE/Plugins/SkinFlow.ini";

		std::uint32_t ReadInt(const wchar_t* a_section, const wchar_t* a_key, std::uint32_t a_default)
		{
			return static_cast<std::uint32_t>(GetPrivateProfileIntW(a_section, a_key, static_cast<int>(a_default), kPath));
		}

		float ReadFloat(const wchar_t* a_section, const wchar_t* a_key, float a_default, float a_min, float a_max)
		{
			wchar_t text[32]{};
			GetPrivateProfileStringW(a_section, a_key, L"", text, static_cast<DWORD>(std::size(text)), kPath);
			wchar_t* end = nullptr;
			const float value = std::wcstof(text, &end);
			return end == text ? a_default : std::clamp(value, a_min, a_max);
		}

		bool WriteString(const wchar_t* a_section, const wchar_t* a_key, const std::wstring& a_value)
		{
			return WritePrivateProfileStringW(a_section, a_key, a_value.c_str(), kPath) != 0;
		}

		bool WriteFloat(const wchar_t* a_section, const wchar_t* a_key, float a_value)
		{
			return WriteString(a_section, a_key, std::format(L"{:.4g}", a_value));
		}

		std::filesystem::file_time_type IniTime()
		{
			std::error_code error;
			const auto time = std::filesystem::last_write_time(kPath, error);
			return error ? std::filesystem::file_time_type{} : time;
		}
	}

	Settings& Settings::Get()
	{
		static Settings s;
		return s;
	}

	void Settings::Load()
	{
		enabled = ReadInt(L"General", L"bEnabled", enabled) != 0;
		maxResolution = std::clamp(ReadInt(L"General", L"iMaxResolution", maxResolution), 64u, 2048u);
		maxSurfaces = std::clamp(ReadInt(L"General", L"iMaxSurfaces", maxSurfaces), 1u, 128u);
		statsInterval = ReadInt(L"General", L"iStatsInterval", statsInterval);
		logger::info("settings: enabled={} maxResolution={} maxSurfaces={}", enabled, maxResolution, maxSurfaces);

		iniTime = IniTime();
		LoadLive();
	}

	void Settings::ReloadIfChanged()
	{
		const auto time = IniTime();
		{
			std::scoped_lock guard(liveLock);
			if (time == iniTime) {
				return;
			}
			iniTime = time;
		}
		LoadLive();
	}

	Settings::Live Settings::GetLive() const
	{
		std::scoped_lock guard(liveLock);
		return live;
	}

	void Settings::SetLive(const Live& a_live)
	{
		std::scoped_lock guard(liveLock);
		live = a_live;
	}

	bool Settings::SaveLive()
	{
		const auto l = GetLive();
		bool ok = WriteString(L"General", L"iDebugView", std::to_wstring(l.debugView));
		ok &= WriteFloat(L"Look", L"fAmount", l.amount);
		ok &= WriteFloat(L"Look", L"fDropSize", l.look.dropSize);
		ok &= WriteFloat(L"Look", L"fSheen", l.look.sheen);
		ok &= WriteFloat(L"Simulation", L"fSweatRate", l.simulation.sweatRate);
		ok &= WriteFloat(L"Simulation", L"fPoreShare", l.simulation.poreShare);
		ok &= WriteFloat(L"Simulation", L"fRunSpeed", l.simulation.runSpeed);
		ok &= WriteFloat(L"Simulation", L"fEvaporation", l.simulation.evaporation);
		ok &= WriteFloat(L"Simulation", L"fResidual", l.simulation.residual);
		ok &= WriteFloat(L"Simulation", L"fCellSize", l.simulation.cellSize);
		ok &= WriteString(L"Simulation", L"bConnectSeams", l.simulation.connectSeams ? L"1" : L"0");
		{
			std::scoped_lock guard(liveLock);
			iniTime = IniTime();  // our own write: nothing to reload
		}
		if (ok) {
			logger::info("settings saved from the menu");
		} else {
			logger::error("settings: writing SkinFlow.ini failed (error {})", GetLastError());
		}
		return ok;
	}

	void Settings::RevertLive()
	{
		LoadLive();
	}

	void Settings::LoadLive()
	{
		const Live defaults{};
		Live next;
		next.debugView = ReadInt(L"General", L"iDebugView", defaults.debugView);
		next.amount = ReadFloat(L"Look", L"fAmount", defaults.amount, 0.0f, 1.0f);
		next.look.dropSize = ReadFloat(L"Look", L"fDropSize", defaults.look.dropSize, 0.25f, 4.0f);
		next.look.sheen = ReadFloat(L"Look", L"fSheen", defaults.look.sheen, 0.0f, 1.0f);
		next.simulation.sweatRate = ReadFloat(L"Simulation", L"fSweatRate", defaults.simulation.sweatRate, 0.0f, 20.0f);
		next.simulation.runSpeed = ReadFloat(L"Simulation", L"fRunSpeed", defaults.simulation.runSpeed, 0.0f, 50.0f);
		next.simulation.evaporation = ReadFloat(L"Simulation", L"fEvaporation", defaults.simulation.evaporation, 0.0f, 10.0f);
		next.simulation.residual = ReadFloat(L"Simulation", L"fResidual", defaults.simulation.residual, 0.0f, 0.5f);
		next.simulation.poreShare = ReadFloat(L"Simulation", L"fPoreShare", defaults.simulation.poreShare, 0.02f, 1.0f);
		next.simulation.cellSize = ReadFloat(L"Simulation", L"fCellSize", defaults.simulation.cellSize, 0.05f, 2.0f);
		next.simulation.connectSeams = ReadInt(L"Simulation", L"bConnectSeams", defaults.simulation.connectSeams) != 0;
		logger::info("look: view={} amount={:.2f} dropSize={:.2f} sheen={:.2f}", next.debugView, next.amount, next.look.dropSize, next.look.sheen);
		logger::info("simulation: sweatRate={:.3f} poreShare={:.3f} runSpeed={:.2f} evaporation={:.3f} residual={:.3f} cellSize={:.3f} connectSeams={}",
			next.simulation.sweatRate, next.simulation.poreShare, next.simulation.runSpeed, next.simulation.evaporation, next.simulation.residual,
			next.simulation.cellSize, next.simulation.connectSeams);

		std::scoped_lock guard(liveLock);
		live = next;
	}
}
