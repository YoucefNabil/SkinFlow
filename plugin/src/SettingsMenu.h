#pragma once

namespace skinflow::SettingsMenu
{
	// Adds the SkinFlow page to SKSE Menu Framework's menu (F1 by default) when it is installed.
	// Every change applies at once through Settings::SetLive; Save writes SkinFlow.ini.
	void Register();
}
