#include "SurfaceProbe.h"

#include "Settings.h"

#include <DirectXPackedVector.h>

namespace skinflow
{
	namespace
	{
		constexpr RE::FormID kPlayer = 0x14;
		constexpr std::uint64_t kMapDelayFrames = 3;  // the GPU has finished the copy by then
	}

	SurfaceProbe& SurfaceProbe::Get()
	{
		static SurfaceProbe probe;
		return probe;
	}

	void SurfaceProbe::OnFrameStart(ID3D11DeviceContext* a_context, const Surface& a_surface)
	{
		const auto interval = Settings::Get().statsInterval;
		if (!interval) {
			return;
		}
		const auto frame = SurfacePool::Get().Frame();
		std::scoped_lock guard(lock);
		if (a_surface.key.actor == kPlayer && frame % interval == 0) {
			Request(a_context, a_surface, frame);
		}
		Collect(a_context, frame);
	}

	void SurfaceProbe::Request(ID3D11DeviceContext* a_context, const Surface& a_surface, std::uint64_t a_frame)
	{
		if (requestFrame != a_frame) {
			requestFrame = a_frame;
			requested.clear();
		}
		if (std::ranges::find(requested, &a_surface) != requested.end()) {
			return;
		}
		requested.push_back(&a_surface);

		auto* source = a_surface.texture[a_surface.read].Get();
		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;
		ComPtr<ID3D11Device> device;
		a_context->GetDevice(device.GetAddressOf());
		Pending entry{ nullptr, a_surface.label, a_frame };
		if (FAILED(device->CreateTexture2D(&desc, nullptr, entry.staging.GetAddressOf()))) {
			return;
		}
		a_context->CopyResource(entry.staging.Get(), source);
		pending.push_back(std::move(entry));
	}

	void SurfaceProbe::Collect(ID3D11DeviceContext* a_context, std::uint64_t a_frame)
	{
		std::erase_if(pending, [&](const Pending& a_entry) {
			if (a_frame < a_entry.frame + kMapDelayFrames) {
				return false;
			}
			D3D11_MAPPED_SUBRESOURCE data{};
			const HRESULT hr = a_context->Map(a_entry.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &data);
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
				return false;
			}
			if (SUCCEEDED(hr)) {
				D3D11_TEXTURE2D_DESC desc{};
				a_entry.staging->GetDesc(&desc);
				Log(a_entry, data, desc.Width);
				a_context->Unmap(a_entry.staging.Get(), 0);
			}
			return true;
		});
	}

	void SurfaceProbe::Log(const Pending& a_pending, const D3D11_MAPPED_SUBRESOURCE& a_data, std::uint32_t a_size)
	{
		using DirectX::PackedVector::XMConvertHalfToFloat;
		std::uint64_t covered = 0, steep = 0, flat = 0, up = 0, down = 0;
		double slopeSum = 0, dirU = 0, dirV = 0;
		for (std::uint32_t y = 0; y < a_size; ++y) {
			const auto* row = reinterpret_cast<const std::uint16_t*>(static_cast<const std::uint8_t*>(a_data.pData) + y * a_data.RowPitch);
			for (std::uint32_t x = 0; x < a_size; ++x) {
				const auto* t = row + x * 4;
				if (XMConvertHalfToFloat(t[3]) < 0.5f) {
					continue;
				}
				const float u = XMConvertHalfToFloat(t[0]), v = XMConvertHalfToFloat(t[1]), nz = XMConvertHalfToFloat(t[2]);
				const float slope = std::sqrt(u * u + v * v);
				++covered;
				slopeSum += slope;
				steep += slope > 0.7f;
				flat += slope < 0.3f;
				up += nz > 0.7f;
				down += nz < -0.7f;
				if (slope > 1e-4f) {
					dirU += u / slope;
					dirV += v / slope;
				}
			}
		}
		const double total = static_cast<double>(a_size) * a_size;
		const auto pct = [&](std::uint64_t a_n) { return covered ? 100.0 * a_n / covered : 0.0; };
		logger::info("probe player {}: covered {:.0f}% of texture; slope mean {:.2f}, steep {:.0f}%, flat {:.0f}%; facing up {:.0f}%, facing down {:.0f}%; mean downhill uv ({:+.2f}, {:+.2f})",
			a_pending.label, 100.0 * covered / total, covered ? slopeSum / covered : 0.0, pct(steep), pct(flat), pct(up), pct(down),
			covered ? dirU / covered : 0.0, covered ? dirV / covered : 0.0);
	}
}
