#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace logger = SKSE::log;
using namespace std::literals;
using Microsoft::WRL::ComPtr;

namespace stl
{
	// Replace a CALL at a_src with our thunk; T::func receives the original target.
	template <class T>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		T::func = trampoline.write_call<5>(a_src, T::thunk);
	}

	// Replace virtual slot N of a vtable; T::func receives the original.
	template <std::size_t N, class T>
	void write_vfunc(REL::VariantID a_vtable)
	{
		REL::Relocation<std::uintptr_t> vtbl{ a_vtable };
		T::func = vtbl.write_vfunc(N, T::thunk);
	}
}
