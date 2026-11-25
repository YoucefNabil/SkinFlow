#include "RenderHooks.h"
#include "Settings.h"
#include "SettingsMenu.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			skinflow::RenderHooks::Install();
			skinflow::SettingsMenu::Register();
		}
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ 0, 1, 0, 0 },
	.Name = "SkinFlow"sv,
	.Author = "Zorka"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SKSE::AllocTrampoline(64);
	logger::info("SkinFlow {} loaded", SKINFLOW_VERSION);
	skinflow::Settings::Get().Load();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
