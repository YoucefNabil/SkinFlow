#pragma once

namespace skinflow::SeamMap
{
	// Where an actor's skin continues across the cuts of its UV layouts, worked out from one capture
	// of every texel's position and normal (Gravity.hlsl SEAM_CAPTURE) on all of the actor's surfaces
	// at once. Pure CPU work on copied data: SeamBuilder runs it on a worker thread.
	//
	// For each texel, a neighbour is linked when it is the skin next to it: covered and close in 3D
	// (a texel next to it in UV but far away in 3D belongs to another island). An edge texel has open
	// sides. For each open side the skin would continue one step further along the local UV-to-world
	// map; the edge texel of any of the actor's surfaces nearest that point, facing the same way and
	// not this texel's own neighbourhood, is the texel across the seam. Water running out through
	// that side goes there (sends), and the gutter texel on that side shows it (fills).

	// One surface's capture.
	struct Part
	{
		std::uint32_t size = 0;   // texels per side
		float texelWorld = 0.0f;  // mean world units per texel
		// size * size texels, row by row: xyz = position (float bits), w = packed normal with bit 31
		// set where the texel is covered (0 elsewhere).
		std::vector<std::array<std::uint32_t, 4>> texels;
	};

	// As Flow.hlsl reads them (StructuredBuffer<uint2>).
	struct Entry
	{
		std::uint32_t from;  // CSSend: texel | direction << 24; CSFill: gutter texel
		std::uint32_t to;    // the texel across the seam, on the group's part
	};

	// A run of entries that all reach the same part.
	struct Group
	{
		std::uint32_t part;
		std::uint32_t offset;
		std::uint32_t count;
	};

	struct Stats
	{
		std::uint32_t edgeTexels = 0;
		std::uint32_t openSides = 0;
		std::uint32_t sends = 0;
		std::uint32_t sendsToOtherParts = 0;
		std::uint32_t fills = 0;
	};

	struct PartSeams
	{
		std::vector<std::uint8_t> links;  // size * size, bit k = neighbour k (Flow.hlsl kDirections) is linked
		std::vector<Entry> sends;
		std::vector<Group> sendGroups;
		std::vector<Entry> fills;
		std::vector<Group> fillGroups;
		Stats stats;
	};

	// Largest surface side the entries can address (12 bits per coordinate).
	inline constexpr std::uint32_t kMaxSize = 4096;

	constexpr std::uint32_t PackTexel(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_direction = 0)
	{
		return a_x | (a_y << 12) | (a_direction << 24);
	}

	// One result per part, in the same order.
	std::vector<PartSeams> Build(const std::vector<Part>& a_parts);
}
