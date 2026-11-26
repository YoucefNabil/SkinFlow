#pragma once

#include "SeamMap.h"
#include "SurfacePool.h"

#include <future>

namespace skinflow
{
	// Keeps every simulated actor's seams (SurfaceSeams) matching its surfaces. One actor at a time:
	//   Capturing: the gravity pass also writes a position + normal snapshot of each of the actor's
	//              surfaces, all in the same frame (so the parts are in one pose);
	//   Reading:   a few frames later the snapshots are copied out without stalling;
	//   Building:  SeamMap works out the seams on a worker thread;
	// then the result is uploaded to the surfaces, if they are still the same ones. An actor is built
	// again when its set of surfaces changes (one reassigned or resized, a part appearing) or a part's
	// drawn area changes (armour showing more or less skin).
	class SeamBuilder
	{
	public:
		static SeamBuilder& Get();

		// Render thread, once per frame before any flow step or gravity capture of the frame.
		void Tick(ID3D11DeviceContext* a_context);

		// A surface drawn within this many frames belongs to its actor's current set.
		static constexpr std::uint64_t kActiveFrames = 10;
		// Gives up when the parts are not all drawn in one frame within this many frames.
		static constexpr std::uint64_t kCaptureTimeoutFrames = 30;
		static constexpr std::uint64_t kReadbackDelayFrames = 3;
		static constexpr std::uint64_t kMinFramesBetweenBuilds = 120;
		// A part's drawn area changing by more than this share calls for a rebuild.
		static constexpr float kCoverageChange = 0.03f;

	private:
		struct Member
		{
			Surface* surface = nullptr;
			std::uint32_t generation = 0;
			std::uint32_t coveredTexels = 0;
		};
		using Members = std::vector<Member>;

		enum class Stage
		{
			Idle,
			Capturing,
			Reading,
			Building
		};

		struct Job
		{
			RE::FormID actor = 0;
			Members members;
			Stage stage = Stage::Idle;
			std::uint64_t stageFrame = 0;
			std::chrono::steady_clock::time_point buildStart{};
			std::future<std::vector<SeamMap::PartSeams>> result;
		};

		// The actor whose seams are missing or stale (the player first), with its current surfaces.
		std::optional<std::pair<RE::FormID, Members>> NextActor(std::uint64_t a_frame);
		static bool Unchanged(const Members& a_built, const Members& a_now);
		bool MembersValid() const;

		void Start(ID3D11DeviceContext* a_context, RE::FormID a_actor, Members a_members, std::uint64_t a_frame);
		void Capture(ID3D11DeviceContext* a_context, std::uint64_t a_frame);
		void Read(ID3D11DeviceContext* a_context, std::uint64_t a_frame);
		void Finish(ID3D11DeviceContext* a_context, std::uint64_t a_frame);
		// Ends the job (keeping a build in flight until its result is in), freeing the snapshots.
		void Stop(std::uint64_t a_frame, std::string_view a_why);

		static bool CreateSnapshot(ID3D11Device* a_device, Surface& a_surface);
		static bool Upload(ID3D11Device* a_device, const SeamMap::PartSeams& a_part, const Members& a_members, std::uint32_t a_size, SurfaceSeams& a_out);

		Job job;
		std::unordered_map<RE::FormID, Members> built;
		std::uint64_t lastFrame = 0;
		std::uint64_t lastBuildFrame = 0;
	};
}
