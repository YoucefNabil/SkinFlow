#include "ShaderFile.h"

#include <d3dcompiler.h>

namespace skinflow
{
	ShaderFile::ShaderFile(std::wstring_view a_fileName) :
		path(std::filesystem::path(L"Data/SKSE/Plugins/SkinFlow") / a_fileName),
		name(path.filename().string())
	{}

	ComPtr<ID3DBlob> ShaderFile::Compile(const char* a_entry, const char* a_target, const D3D_SHADER_MACRO* a_defines) const
	{
		ComPtr<ID3DBlob> code, errors;
		const HRESULT hr = D3DCompileFromFile(path.c_str(), a_defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, a_entry, a_target,
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf());
		if (FAILED(hr)) {
			logger::error("{} {} ({}): {}", name, a_entry, a_target,
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : std::format("hr {:#x}", static_cast<std::uint32_t>(hr)));
			return nullptr;
		}
		return code;
	}

	void ShaderFile::MarkCompiled()
	{
		compiledTime = Time();
	}

	bool ShaderFile::Changed(std::uint64_t a_frame)
	{
		if (a_frame % 60 != 0 || a_frame == lastCheck) {
			return false;
		}
		lastCheck = a_frame;
		const auto time = Time();
		if (time == std::filesystem::file_time_type{} || time == compiledTime) {
			return false;
		}
		compiledTime = time;
		return true;
	}

	std::filesystem::file_time_type ShaderFile::Time() const
	{
		std::error_code error;
		const auto time = std::filesystem::last_write_time(path, error);
		return error ? std::filesystem::file_time_type{} : time;
	}
}
