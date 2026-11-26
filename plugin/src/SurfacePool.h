#pragma once

namespace skinflow
{
	// One simulated skin area: an actor's body part, which is one UV layout (head, body, hands...).
	// Everything drawn on that part lands on its surface: RaceMenu overlays, face decals and armour
	// pieces that show skin.
	struct SurfaceKey
	{
		RE::FormID actor = 0;
		std::uint16_t bodySlot = 0;       // body part slot (30 head, 32 body, 33 hands...), 0 = unknown
		const void* texture = nullptr;  // the diffuse NiSourceTexture, the key when the slot is unknown

		bool operator==(const SurfaceKey&) const = default;
	};

	struct Surface;

	// A run of seam entries on a surface that all reach one other surface (or itself), valid while
	// that surface keeps the generation it had when the seams were built.
	struct SeamBatch
	{
		Surface* other = nullptr;
		std::uint32_t generation = 0;
		std::uint32_t offset = 0;
		std::uint32_t count = 0;
	};

	// Where the skin continues across this surface's UV cuts (SeamBuilder, Flow.hlsl). Empty until
	// built for the surface's current generation.
	struct SurfaceSeams
	{
		ComPtr<ID3D11ShaderResourceView> links;  // R8_UINT, bit k = neighbour k is the skin next to the texel
		ComPtr<ID3D11ShaderResourceView> sends;  // SeamMap::Entry, grouped by sendBatches
		std::vector<SeamBatch> sendBatches;
		ComPtr<ID3D11ShaderResourceView> fills;  // SeamMap::Entry, grouped by fillBatches
		std::vector<SeamBatch> fillBatches;
	};

	// The one-off position + normal snapshot SeamBuilder asks the gravity pass for (Gravity.hlsl
	// SEAM_CAPTURE). Exists only while a build for the surface's actor is capturing or reading.
	struct SeamCapture
	{
		ComPtr<ID3D11Texture2D> texture;  // R32G32B32A32_UINT, the surface's size
		ComPtr<ID3D11RenderTargetView> rtv;
		ComPtr<ID3D11Texture2D> staging;
		std::uint64_t requestFrame = 0;   // the gravity pass writes the snapshot in this frame
		std::uint64_t capturedFrame = 0;  // frame it last did
	};

	struct Surface
	{
		SurfaceKey key{};
		std::string label;  // for logs: diffuse texture and slot of the first pass after assignment
		bool inUse = false;                  // assigned to key (until another key takes the slot)
		std::uint64_t lastRequestFrame = 0;  // frame of its last request
		std::uint64_t lastRequestTick = 0;   // SurfacePool tick of its last request (idle time for eviction)

		// Texels per side of the gravity and water textures, fitted to the part's world size once its
		// scale is measured, so a texel (one simulated drop) covers the same skin area on every part.
		std::uint32_t size = 0;

		// Double buffered: the renderer reads 'read' while this frame's gravity pass writes the other.
		std::array<ComPtr<ID3D11Texture2D>, 2> texture;
		std::array<ComPtr<ID3D11RenderTargetView>, 2> rtv;
		std::array<ComPtr<ID3D11ShaderResourceView>, 2> srv;
		std::uint32_t read = 0;

		std::uint64_t writeFrame = 0;  // frame whose capture went to the write buffer
		bool gravityReady = false;     // a capture was published since the last reset

		// World units per UV unit, the mean over covered texels: the gravity pass adds each texel's
		// value (fixed point) and a count into 'metrics', read back a few frames later without stalling.
		float uvScale = 0.0f;  // 0 until measured
		ComPtr<ID3D11Buffer> metrics;
		ComPtr<ID3D11UnorderedAccessView> metricsUav;
		ComPtr<ID3D11Buffer> metricsReadback;
		std::uint64_t metricsCopyFrame = 0;  // frame of the copy in flight, 0 = none
		std::vector<const void*> capturedGeometry;  // BSGeometry drawn into the write buffer this frame

		// Simulated water (FlowPass), double buffered the same way: stepped into the write buffer at
		// the surface's first draw of a frame, published by BeginFrame.
		std::array<ComPtr<ID3D11Texture2D>, 2> water;
		std::array<ComPtr<ID3D11ShaderResourceView>, 2> waterSrv;
		std::array<ComPtr<ID3D11UnorderedAccessView>, 2> waterUav;
		std::uint32_t waterRead = 0;
		std::uint64_t waterFrame = 0;  // frame whose step went to the write buffer
		std::uint32_t waterSteps = 0;  // steps since assignment (random seed)
		float simulatedSeconds = 0.0f;  // simulated time since the last reset (FlowPass warm-up)
		std::chrono::steady_clock::time_point waterTime{};  // last step, 0 = not stepped yet

		// Changes whenever the surface's texel layout or owner does (assignment, resize): seams built
		// for an older generation no longer apply.
		std::uint32_t generation = 0;
		std::uint32_t coveredTexels = 0;  // drawn texels last measured with uvScale, 0 = not yet
		// Water other surfaces (or this one) sent here across seams, R32_UINT fixed point (Flow.hlsl Inflow).
		ComPtr<ID3D11Texture2D> inflow;
		ComPtr<ID3D11UnorderedAccessView> inflowUav;
		SurfaceSeams seams;
		SeamCapture seamCapture;

		std::uint32_t WriteIndex() const { return read ^ 1u; }
		std::uint32_t WaterWriteIndex() const { return waterRead ^ 1u; }
	};

	class SurfacePool
	{
	public:
		static SurfacePool& Get();

		// Render thread. Finds or assigns the surface for a key; null when every slot is held by
		// another actor. A priority key (the player) may take another actor's least recently used slot.
		// Resizes the surface (restarting its simulation) when its measured scale calls for another size.
		Surface* Acquire(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const SurfaceKey& a_key, bool a_priority);

		// Main thread, once per frame before rendering: publishes last frame's writes.
		void BeginFrame();

		std::uint64_t Frame() const { return frame; }

		// Calls a_visit(Surface&) for every surface, under the pool's lock (a_visit must not call back
		// into the pool).
		template <class Visit>
		void ForEach(Visit&& a_visit)
		{
			std::scoped_lock guard(lock);
			for (auto& s : surfaces) {
				a_visit(s);
			}
		}
		std::uint32_t ActiveCount() const;
		std::uint32_t TakeRefusals() { return refusals.exchange(0); }

		// A surface stays with its actor for this many frames that drew skin after its last draw before
		// another actor may take it. Frames that draw no skin (menus, alt-tab, loading) do not count,
		// so a pause never costs an actor its water.
		static constexpr std::uint64_t kHoldFrames = 120;

		static constexpr std::uint32_t kMinSize = 32;
		// A fitted size within this ratio of the current one is kept: scale readings move with the
		// pose and the parts drawn, and every resize loses the water.
		static constexpr float kResizeBelow = 0.7f;
		static constexpr float kResizeAbove = 1.4f;

	private:
		static bool CreateMetrics(ID3D11Device* a_device, Surface& a_surface);
		// Replaces the gravity, water and inflow textures with new ones of a_size; keeps the old on failure.
		static bool Resize(ID3D11Device* a_device, Surface& a_surface, std::uint32_t a_size);
		static std::uint32_t FittedSize(float a_uvScale);
		void FitSize(ID3D11Device* a_device, ID3D11DeviceContext* a_context, Surface& a_surface);
		static void ResetSimulation(Surface& a_surface);
		static void Clear(ID3D11DeviceContext* a_context, Surface& a_surface);

		mutable std::mutex lock;
		std::vector<Surface> surfaces;  // fixed size after first use: pointers stay valid
		std::uint64_t frame = 1;
		std::uint64_t tick = 1;  // frames that drew skin: the clock of eviction
		bool drewSkin = false;   // a surface was requested this frame
		std::atomic<std::uint32_t> refusals{ 0 };
	};
}
