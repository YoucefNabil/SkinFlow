#include "SurfacePool.h"

#include "Settings.h"

namespace skinflow
{
	SurfacePool& SurfacePool::Get()
	{
		static SurfacePool pool;
		return pool;
	}

	Surface* SurfacePool::Acquire(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const SurfaceKey& a_key, bool a_priority)
	{
		std::scoped_lock guard(lock);
		if (surfaces.empty()) {
			surfaces.resize(Settings::Get().maxSurfaces);
		}

		for (auto& s : surfaces) {
			if (s.inUse && s.key == a_key) {
				s.lastRequestFrame = frame;
				FitSize(a_device, a_context, s);
				return &s;
			}
		}

		// A free slot, else the least recently requested one that has been idle for kHoldFrames
		// (any held slot for a priority key).
		Surface* oldest = nullptr;
		for (auto& s : surfaces) {
			if (!s.inUse) {
				oldest = &s;
				break;
			}
			const bool evictable = frame - s.lastRequestFrame > kHoldFrames || (a_priority && s.key.actor != a_key.actor);
			if (evictable && (!oldest || s.lastRequestFrame < oldest->lastRequestFrame)) {
				oldest = &s;
			}
		}
		if (!oldest) {
			refusals.fetch_add(1, std::memory_order_relaxed);
			return nullptr;
		}

		if ((!oldest->metrics && !CreateMetrics(a_device, *oldest)) ||
			(!oldest->texture[0] && !Resize(a_device, *oldest, Settings::Get().maxResolution))) {
			return nullptr;
		}
		oldest->key = a_key;
		oldest->inUse = true;
		oldest->lastRequestFrame = frame;
		oldest->label.clear();
		oldest->uvScale = 0.0f;  // keeps its size until the new part is measured
		oldest->metricsCopyFrame = 0;
		ResetSimulation(*oldest);
		Clear(a_context, *oldest);
		return oldest;
	}

	std::uint32_t SurfacePool::FittedSize(float a_uvScale)
	{
		const float cells = a_uvScale / Settings::Get().GetLive().simulation.cellSize;
		const auto size = static_cast<std::uint32_t>(std::ceil(cells / 8.0f)) * 8;  // whole compute groups
		return std::clamp(size, kMinSize, Settings::Get().maxResolution);
	}

	void SurfacePool::FitSize(ID3D11Device* a_device, ID3D11DeviceContext* a_context, Surface& a_surface)
	{
		if (a_surface.uvScale <= 0.0f) {
			return;
		}
		const auto fitted = FittedSize(a_surface.uvScale);
		const float ratio = static_cast<float>(fitted) / static_cast<float>(a_surface.size);
		if (ratio >= kResizeBelow && ratio <= kResizeAbove) {
			return;
		}
		const auto previous = a_surface.size;
		if (!Resize(a_device, a_surface, fitted)) {
			return;
		}
		logger::info("{}: {} -> {} texels per side ({:.1f} world units per UV unit)", a_surface.label, previous, fitted, a_surface.uvScale);
		ResetSimulation(a_surface);
		Clear(a_context, a_surface);
	}

	void SurfacePool::ResetSimulation(Surface& a_surface)
	{
		a_surface.read = 0;
		a_surface.writeFrame = 0;
		a_surface.capturedGeometry.clear();
		a_surface.waterRead = 0;
		a_surface.waterFrame = 0;
		a_surface.waterSteps = 0;
		a_surface.waterTime = {};
	}

	void SurfacePool::BeginFrame()
	{
		std::scoped_lock guard(lock);
		for (auto& s : surfaces) {
			if (s.inUse && s.writeFrame == frame) {
				s.read = s.WriteIndex();
			}
			if (s.inUse && s.waterFrame == frame) {
				s.waterRead = s.WaterWriteIndex();
			}
			// Free surfaces nobody asked for in ~10 s so new actors can take them.
			if (s.inUse && frame - s.lastRequestFrame > 600) {
				s.inUse = false;
			}
		}
		++frame;
	}

	std::uint32_t SurfacePool::ActiveCount() const
	{
		std::scoped_lock guard(lock);
		return static_cast<std::uint32_t>(std::ranges::count_if(surfaces, [this](const Surface& s) { return s.inUse && frame - s.lastRequestFrame < 2; }));
	}

	bool SurfacePool::CreateMetrics(ID3D11Device* a_device, Surface& a_surface)
	{
		D3D11_BUFFER_DESC metricsDesc{};
		metricsDesc.ByteWidth = 2 * sizeof(std::uint32_t);
		metricsDesc.Usage = D3D11_USAGE_DEFAULT;
		metricsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		metricsDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		D3D11_UNORDERED_ACCESS_VIEW_DESC metricsView{};
		metricsView.Format = DXGI_FORMAT_R32_TYPELESS;
		metricsView.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		metricsView.Buffer.NumElements = 2;
		metricsView.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		D3D11_BUFFER_DESC readbackDesc{};
		readbackDesc.ByteWidth = metricsDesc.ByteWidth;
		readbackDesc.Usage = D3D11_USAGE_STAGING;
		readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(a_device->CreateBuffer(&metricsDesc, nullptr, a_surface.metrics.ReleaseAndGetAddressOf())) ||
			FAILED(a_device->CreateUnorderedAccessView(a_surface.metrics.Get(), &metricsView, a_surface.metricsUav.ReleaseAndGetAddressOf())) ||
			FAILED(a_device->CreateBuffer(&readbackDesc, nullptr, a_surface.metricsReadback.ReleaseAndGetAddressOf()))) {
			logger::error("surface metrics buffer creation failed");
			return false;
		}
		constexpr std::string_view metricsName = "SkinFlow::SurfaceMetrics";
		a_surface.metrics->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(metricsName.size()), metricsName.data());
		return true;
	}

	bool SurfacePool::Resize(ID3D11Device* a_device, Surface& a_surface, std::uint32_t a_size)
	{
		Surface next;  // built aside, so a failure leaves the surface as it was
		const auto res = a_size;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = res;
		desc.Height = res;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		for (std::size_t i = 0; i < 2; ++i) {
			if (FAILED(a_device->CreateTexture2D(&desc, nullptr, next.texture[i].ReleaseAndGetAddressOf())) ||
				FAILED(a_device->CreateRenderTargetView(next.texture[i].Get(), nullptr, next.rtv[i].ReleaseAndGetAddressOf())) ||
				FAILED(a_device->CreateShaderResourceView(next.texture[i].Get(), nullptr, next.srv[i].ReleaseAndGetAddressOf()))) {
				logger::error("surface texture creation failed ({}x{})", res, res);
				return false;
			}
			const auto name = std::format("SkinFlow::Surface{}", i);
			next.texture[i]->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
		}

		D3D11_TEXTURE2D_DESC waterDesc = desc;
		waterDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;  // Flow.hlsl state; small per-frame sweat gains vanish in half floats
		waterDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		for (std::size_t i = 0; i < 2; ++i) {
			if (FAILED(a_device->CreateTexture2D(&waterDesc, nullptr, next.water[i].ReleaseAndGetAddressOf())) ||
				FAILED(a_device->CreateShaderResourceView(next.water[i].Get(), nullptr, next.waterSrv[i].ReleaseAndGetAddressOf())) ||
				FAILED(a_device->CreateUnorderedAccessView(next.water[i].Get(), nullptr, next.waterUav[i].ReleaseAndGetAddressOf()))) {
				logger::error("water texture creation failed ({}x{})", res, res);
				return false;
			}
			const auto name = std::format("SkinFlow::Water{}", i);
			next.water[i]->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
		}

		a_surface.texture = std::move(next.texture);
		a_surface.rtv = std::move(next.rtv);
		a_surface.srv = std::move(next.srv);
		a_surface.water = std::move(next.water);
		a_surface.waterSrv = std::move(next.waterSrv);
		a_surface.waterUav = std::move(next.waterUav);
		a_surface.size = a_size;
		return true;
	}

	void SurfacePool::Clear(ID3D11DeviceContext* a_context, Surface& a_surface)
	{
		constexpr float zero[4]{};
		for (auto& view : a_surface.rtv) {
			a_context->ClearRenderTargetView(view.Get(), zero);
		}
		constexpr UINT zeros[4]{};
		a_context->ClearUnorderedAccessViewUint(a_surface.metricsUav.Get(), zeros);
		for (auto& view : a_surface.waterUav) {
			a_context->ClearUnorderedAccessViewFloat(view.Get(), zero);
		}
	}
}
