#include "SeamBuilder.h"

#include "Settings.h"
#include "ShaderFile.h"

namespace skinflow
{
	namespace
	{
		constexpr RE::FormID kPlayer = 0x14;

		bool CreateEntries(ID3D11Device* a_device, const std::vector<SeamMap::Entry>& a_entries, std::string_view a_name, ComPtr<ID3D11ShaderResourceView>& a_out)
		{
			a_out.Reset();
			if (a_entries.empty()) {
				return true;
			}
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = static_cast<UINT>(a_entries.size() * sizeof(SeamMap::Entry));
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(SeamMap::Entry);
			const D3D11_SUBRESOURCE_DATA data{ a_entries.data(), 0, 0 };
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_UNKNOWN;
			view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			view.Buffer.FirstElement = 0;
			view.Buffer.NumElements = static_cast<UINT>(a_entries.size());
			ComPtr<ID3D11Buffer> buffer;
			if (FAILED(a_device->CreateBuffer(&desc, &data, buffer.GetAddressOf())) ||
				FAILED(a_device->CreateShaderResourceView(buffer.Get(), &view, a_out.GetAddressOf()))) {
				return false;
			}
			SetDebugName(buffer.Get(), a_name);
			return true;
		}
	}

	static_assert(sizeof(SeamMap::Entry) == 8, "Flow.hlsl reads the entries as uint2");

	SeamBuilder& SeamBuilder::Get()
	{
		static SeamBuilder builder;
		return builder;
	}

	void SeamBuilder::Tick(ID3D11DeviceContext* a_context)
	{
		const auto frame = SurfacePool::Get().Frame();
		if (frame == lastFrame) {
			return;
		}
		lastFrame = frame;
		const bool on = Settings::Get().GetLive().simulation.connectSeams;

		switch (job.stage) {
		case Stage::Idle:
			if (on && (lastBuildFrame == 0 || frame >= lastBuildFrame + kMinFramesBetweenBuilds)) {
				if (auto next = NextActor(frame)) {
					Start(a_context, next->first, std::move(next->second), frame);
				}
			}
			break;
		case Stage::Capturing:
		case Stage::Reading:
			if (!on) {
				Stop(frame, {});
			} else if (!MembersValid()) {
				Stop(frame, "its parts changed");
			} else if (job.stage == Stage::Capturing) {
				Capture(a_context, frame);
			} else {
				Read(a_context, frame);
			}
			break;
		case Stage::Building:
			Finish(a_context, frame);
			break;
		}
	}

	std::optional<std::pair<RE::FormID, SeamBuilder::Members>> SeamBuilder::NextActor(std::uint64_t a_frame)
	{
		std::map<RE::FormID, Members> active;
		std::set<RE::FormID> held;
		SurfacePool::Get().ForEach([&](Surface& a_surface) {
			if (!a_surface.inUse) {
				return;
			}
			held.insert(a_surface.key.actor);
			if (a_surface.uvScale > 0.0f && a_surface.coveredTexels > 0 && a_surface.size <= SeamMap::kMaxSize &&
				a_frame - a_surface.lastRequestFrame <= kActiveFrames) {
				active[a_surface.key.actor].push_back({ &a_surface, a_surface.generation, a_surface.coveredTexels });
			}
		});
		std::erase_if(built, [&](const auto& a_entry) { return !held.contains(a_entry.first); });

		std::optional<std::pair<RE::FormID, Members>> next;
		for (auto& [actor, members] : active) {
			const auto done = built.find(actor);
			if (done != built.end() && Unchanged(done->second, members)) {
				continue;
			}
			if (!next || actor == kPlayer) {
				next.emplace(actor, std::move(members));
			}
		}
		return next;
	}

	bool SeamBuilder::Unchanged(const Members& a_built, const Members& a_now)
	{
		if (a_built.size() != a_now.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a_built.size(); ++i) {
			const auto& was = a_built[i];
			const auto& is = a_now[i];
			if (was.surface != is.surface || was.generation != is.generation) {
				return false;
			}
			const float change = std::abs(static_cast<float>(is.coveredTexels) - static_cast<float>(was.coveredTexels));
			if (change > kCoverageChange * static_cast<float>(was.coveredTexels)) {
				return false;
			}
		}
		return true;
	}

	bool SeamBuilder::MembersValid() const
	{
		return std::ranges::all_of(job.members, [&](const Member& a_member) {
			const auto& s = *a_member.surface;
			return s.inUse && s.generation == a_member.generation && s.key.actor == job.actor;
		});
	}

	bool SeamBuilder::CreateSnapshot(ID3D11Device* a_device, Surface& a_surface)
	{
		auto& snapshot = a_surface.seamCapture;
		snapshot = {};
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_surface.size;
		desc.Height = a_surface.size;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;  // Gravity.hlsl SV_Target1
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(a_device->CreateTexture2D(&desc, nullptr, snapshot.texture.GetAddressOf())) ||
			FAILED(a_device->CreateRenderTargetView(snapshot.texture.Get(), nullptr, snapshot.rtv.GetAddressOf())) ||
			FAILED(a_device->CreateTexture2D(&stagingDesc, nullptr, snapshot.staging.GetAddressOf()))) {
			snapshot = {};
			return false;
		}
		SetDebugName(snapshot.texture.Get(), "SkinFlow::SeamSnapshot");
		return true;
	}

	void SeamBuilder::Start(ID3D11DeviceContext* a_context, RE::FormID a_actor, Members a_members, std::uint64_t a_frame)
	{
		ComPtr<ID3D11Device> device;
		a_context->GetDevice(device.GetAddressOf());
		job.actor = a_actor;
		job.members = std::move(a_members);
		for (auto& member : job.members) {
			if (!CreateSnapshot(device.Get(), *member.surface)) {
				Stop(a_frame, "snapshot texture creation failed");
				return;
			}
			member.surface->seamCapture.requestFrame = a_frame;  // this frame's captures write it
		}
		job.stage = Stage::Capturing;
		job.stageFrame = a_frame;
	}

	void SeamBuilder::Capture(ID3D11DeviceContext* a_context, std::uint64_t a_frame)
	{
		const bool all = std::ranges::all_of(job.members, [&](const Member& a_member) { return a_member.surface->seamCapture.capturedFrame + 1 == a_frame; });
		if (all) {
			for (const auto& member : job.members) {
				const auto& snapshot = member.surface->seamCapture;
				a_context->CopyResource(snapshot.staging.Get(), snapshot.texture.Get());
			}
			job.stage = Stage::Reading;
			job.stageFrame = a_frame;
			return;
		}
		if (a_frame - job.stageFrame > kCaptureTimeoutFrames) {
			Stop(a_frame, "its parts were not drawn in the same frame");
			return;
		}
		for (const auto& member : job.members) {
			member.surface->seamCapture.requestFrame = a_frame;
		}
	}

	void SeamBuilder::Read(ID3D11DeviceContext* a_context, std::uint64_t a_frame)
	{
		if (a_frame < job.stageFrame + kReadbackDelayFrames) {
			return;
		}
		std::vector<SeamMap::Part> parts;
		parts.reserve(job.members.size());
		for (const auto& member : job.members) {
			const auto& surface = *member.surface;
			D3D11_MAPPED_SUBRESOURCE data{};
			const HRESULT hr = a_context->Map(surface.seamCapture.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &data);
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
				return;  // again next frame
			}
			if (FAILED(hr)) {
				Stop(a_frame, "reading the snapshot failed");
				return;
			}
			SeamMap::Part part{ surface.size, surface.uvScale / static_cast<float>(surface.size), {} };
			part.texels.resize(static_cast<std::size_t>(surface.size) * surface.size);
			const std::size_t rowBytes = surface.size * sizeof(part.texels[0]);
			for (std::uint32_t y = 0; y < surface.size; ++y) {
				std::memcpy(part.texels.data() + static_cast<std::size_t>(y) * surface.size, static_cast<const std::byte*>(data.pData) + y * data.RowPitch, rowBytes);
			}
			a_context->Unmap(surface.seamCapture.staging.Get(), 0);
			parts.push_back(std::move(part));
		}
		for (const auto& member : job.members) {
			member.surface->seamCapture = {};
		}
		job.buildStart = std::chrono::steady_clock::now();
		job.result = std::async(std::launch::async, [parts = std::move(parts)] { return SeamMap::Build(parts); });
		job.stage = Stage::Building;
		job.stageFrame = a_frame;
	}

	void SeamBuilder::Finish(ID3D11DeviceContext* a_context, std::uint64_t a_frame)
	{
		if (job.result.wait_for(0s) != std::future_status::ready) {
			return;
		}
		std::vector<SeamMap::PartSeams> result;
		try {
			result = job.result.get();
		} catch (const std::exception& e) {
			Stop(a_frame, std::format("the build failed ({})", e.what()));
			return;
		}
		if (!MembersValid() || result.size() != job.members.size()) {
			Stop(a_frame, "its parts changed while building");
			return;
		}
		const auto ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - job.buildStart).count();
		ComPtr<ID3D11Device> device;
		a_context->GetDevice(device.GetAddressOf());
		for (std::size_t i = 0; i < result.size(); ++i) {
			auto& surface = *job.members[i].surface;
			SurfaceSeams seams;
			if (!Upload(device.Get(), result[i], job.members, surface.size, seams)) {
				Stop(a_frame, "seam buffer creation failed");
				return;
			}
			surface.seams = std::move(seams);
			const auto& stats = result[i].stats;
			logger::info("seams {:08X} {}: {} edge texels, {} open sides, {} sends ({} to other parts), {} gutter fills",
				job.actor, surface.label, stats.edgeTexels, stats.openSides, stats.sends, stats.sendsToOtherParts, stats.fills);
		}
		logger::info("seams {:08X}: {} parts built in {:.0f} ms", job.actor, result.size(), ms);
		built[job.actor] = job.members;
		Stop(a_frame, {});
	}

	void SeamBuilder::Stop(std::uint64_t a_frame, std::string_view a_why)
	{
		if (!a_why.empty()) {
			logger::info("seams {:08X}: not built, {}; trying again later", job.actor, a_why);
		}
		for (const auto& member : job.members) {
			member.surface->seamCapture = {};
		}
		job = {};
		lastBuildFrame = a_frame;
	}

	bool SeamBuilder::Upload(ID3D11Device* a_device, const SeamMap::PartSeams& a_part, const Members& a_members, std::uint32_t a_size, SurfaceSeams& a_out)
	{
		if (a_part.links.size() != static_cast<std::size_t>(a_size) * a_size) {
			return false;
		}
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_size;
		desc.Height = a_size;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8_UINT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const D3D11_SUBRESOURCE_DATA data{ a_part.links.data(), a_size, 0 };
		ComPtr<ID3D11Texture2D> links;
		if (FAILED(a_device->CreateTexture2D(&desc, &data, links.GetAddressOf())) ||
			FAILED(a_device->CreateShaderResourceView(links.Get(), nullptr, a_out.links.GetAddressOf()))) {
			return false;
		}
		SetDebugName(links.Get(), "SkinFlow::SeamLinks");
		if (!CreateEntries(a_device, a_part.sends, "SkinFlow::SeamSends", a_out.sends) ||
			!CreateEntries(a_device, a_part.fills, "SkinFlow::SeamFills", a_out.fills)) {
			return false;
		}
		const auto batches = [&](const std::vector<SeamMap::Group>& a_groups) {
			std::vector<SeamBatch> out;
			out.reserve(a_groups.size());
			for (const auto& group : a_groups) {
				const auto& other = a_members[group.part];
				out.push_back({ other.surface, other.generation, group.offset, group.count });
			}
			return out;
		};
		a_out.sendBatches = batches(a_part.sendGroups);
		a_out.fillBatches = batches(a_part.fillGroups);
		return true;
	}
}
