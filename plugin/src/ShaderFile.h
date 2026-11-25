#pragma once

namespace skinflow
{
	// A shader source in Data/SKSE/Plugins/SkinFlow, compiled at run time. Passes ask Changed()
	// once a frame and recompile when the file was saved, so shaders can be edited while the game runs.
	class ShaderFile
	{
	public:
		explicit ShaderFile(std::wstring_view a_fileName);

		// Null on failure, with the compiler's message logged. a_defines ends with a null entry.
		ComPtr<ID3DBlob> Compile(const char* a_entry, const char* a_target, const D3D_SHADER_MACRO* a_defines) const;
		// Remembers the file's current time as the compiled version (call before the first compile).
		void MarkCompiled();
		// Every 60th frame: true when the file was saved since the last MarkCompiled or Changed.
		// Reports a save once, so a file that does not compile is not retried until saved again.
		bool Changed(std::uint64_t a_frame);
		const std::string& Name() const { return name; }

	private:
		std::filesystem::file_time_type Time() const;

		std::filesystem::path path;
		std::string name;
		std::filesystem::file_time_type compiledTime{};
		std::uint64_t lastCheck = ~0ull;
	};

	template <class T>
	void SetDebugName(T* a_object, std::string_view a_name)
	{
		if (a_object) {
			a_object->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(a_name.size()), a_name.data());
		}
	}
}
