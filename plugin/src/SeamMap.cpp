#include "SeamMap.h"

#include <bit>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace skinflow::SeamMap
{
	namespace
	{
		struct Vec3
		{
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;

			Vec3 operator+(const Vec3& a_other) const { return { x + a_other.x, y + a_other.y, z + a_other.z }; }
			Vec3 operator-(const Vec3& a_other) const { return { x - a_other.x, y - a_other.y, z - a_other.z }; }
			Vec3 operator*(float a_scale) const { return { x * a_scale, y * a_scale, z * a_scale }; }
		};

		float Dot(const Vec3& a_a, const Vec3& a_b) { return a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z; }
		float Length(const Vec3& a_v) { return std::sqrt(Dot(a_v, a_v)); }

		// Flow.hlsl kDirections.
		constexpr std::array<std::array<int, 2>, 8> kDirections{ { { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 } } };
		constexpr std::uint32_t kCovered = 0x80000000u;

		// A neighbour is linked when it is no further than kLinkStretch times the local texel
		// spacing (the nearer of the two texels' closest straight neighbours, so UV stretch is
		// allowed for), and always when within kLinkFloor mean texels.
		constexpr float kLinkStretch = 3.0f;
		constexpr float kLinkFloor = 2.0f;
		// The texel across a seam lies within this many steps of where the skin continues.
		constexpr float kReach = 1.25f;
		// Its normal agrees with the edge texel's (skin touching skin faces the other way).
		constexpr float kMinNormalDot = 0.5f;
		// Texels this close in UV on the same part are the edge texel's own neighbourhood.
		constexpr int kOwnNeighbourhood = 2;

		// Gravity.hlsl PackNormal.
		Vec3 UnpackNormal(std::uint32_t a_packed)
		{
			const float ex = static_cast<float>((a_packed >> 15) & 0x7FFF) / 32767.0f * 2.0f - 1.0f;
			const float ey = static_cast<float>(a_packed & 0x7FFF) / 32767.0f * 2.0f - 1.0f;
			Vec3 n{ ex, ey, 1.0f - std::abs(ex) - std::abs(ey) };
			const float fold = std::max(-n.z, 0.0f);
			n.x += n.x >= 0.0f ? -fold : fold;
			n.y += n.y >= 0.0f ? -fold : fold;
			const float length = Length(n);
			return length > 0.0f ? n * (1.0f / length) : Vec3{ 0.0f, 0.0f, 1.0f };
		}

		struct Grid
		{
			int size = 0;
			float texelWorld = 0.0f;
			std::vector<Vec3> position;
			std::vector<Vec3> normal;
			std::vector<std::uint8_t> covered;

			int Index(int a_x, int a_y) const { return a_y * size + a_x; }
			bool Inside(int a_x, int a_y) const { return a_x >= 0 && a_y >= 0 && a_x < size && a_y < size; }
			bool Covered(int a_x, int a_y) const { return Inside(a_x, a_y) && covered[Index(a_x, a_y)]; }
		};

		Grid Decode(const Part& a_part)
		{
			Grid grid;
			grid.size = static_cast<int>(a_part.size);
			grid.texelWorld = a_part.texelWorld;
			const auto count = static_cast<std::size_t>(a_part.size) * a_part.size;
			grid.position.resize(count);
			grid.normal.resize(count);
			grid.covered.assign(count, 0);
			if (a_part.texels.size() != count) {
				return grid;
			}
			for (std::size_t i = 0; i < count; ++i) {
				const auto& t = a_part.texels[i];
				if (!(t[3] & kCovered)) {
					continue;
				}
				const Vec3 p{ std::bit_cast<float>(t[0]), std::bit_cast<float>(t[1]), std::bit_cast<float>(t[2]) };
				if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
					continue;
				}
				grid.position[i] = p;
				grid.normal[i] = UnpackNormal(t[3]);
				grid.covered[i] = 1;
			}
			return grid;
		}

		std::vector<std::uint8_t> Links(const Grid& a_grid)
		{
			const int n = a_grid.size;
			constexpr float kNone = std::numeric_limits<float>::infinity();
			// Distance to the closest straight neighbour: the local texel spacing.
			std::vector<float> spacing(static_cast<std::size_t>(n) * n, kNone);
			for (int y = 0; y < n; ++y) {
				for (int x = 0; x < n; ++x) {
					const int i = a_grid.Index(x, y);
					if (!a_grid.covered[i]) {
						continue;
					}
					for (int k = 0; k < 8; k += 2) {
						const int nx = x + kDirections[k][0];
						const int ny = y + kDirections[k][1];
						if (a_grid.Covered(nx, ny)) {
							spacing[i] = std::min(spacing[i], Length(a_grid.position[a_grid.Index(nx, ny)] - a_grid.position[i]));
						}
					}
				}
			}

			std::vector<std::uint8_t> links(static_cast<std::size_t>(n) * n, 0);
			for (int y = 0; y < n; ++y) {
				for (int x = 0; x < n; ++x) {
					const int i = a_grid.Index(x, y);
					if (!a_grid.covered[i]) {
						continue;
					}
					for (int k = 0; k < 8; ++k) {
						const int nx = x + kDirections[k][0];
						const int ny = y + kDirections[k][1];
						if (!a_grid.Covered(nx, ny)) {
							continue;
						}
						const int j = a_grid.Index(nx, ny);
						float local = 0.0f;
						for (const float s : { spacing[i], spacing[j] }) {
							if (s != kNone) {
								local = std::max(local, s);
							}
						}
						const float steps = (k & 1) ? std::numbers::sqrt2_v<float> : 1.0f;
						const float limit = steps * std::max(kLinkStretch * local, kLinkFloor * a_grid.texelWorld);
						if (Length(a_grid.position[j] - a_grid.position[i]) <= limit) {
							links[i] |= static_cast<std::uint8_t>(1u << k);
						}
					}
				}
			}
			return links;
		}

		struct EdgeTexel
		{
			std::uint32_t part;
			int x;
			int y;
		};

		// Edge texels of all parts in a uniform 3D grid of cells.
		class EdgeIndex
		{
		public:
			EdgeIndex(float a_cell, const std::vector<Vec3>& a_positions) :
				cell(a_cell)
			{
				keys.reserve(a_positions.size());
				for (std::uint32_t i = 0; i < a_positions.size(); ++i) {
					const auto& p = a_positions[i];
					keys.emplace_back(Key(CellOf(p.x), CellOf(p.y), CellOf(p.z)), i);
				}
				std::ranges::sort(keys);
			}

			// Calls a_visit(edge index) for every edge texel in the cells around a_point.
			template <class Visit>
			void Near(const Vec3& a_point, Visit&& a_visit) const
			{
				const int cx = CellOf(a_point.x);
				const int cy = CellOf(a_point.y);
				const int cz = CellOf(a_point.z);
				for (int dz = -1; dz <= 1; ++dz) {
					for (int dy = -1; dy <= 1; ++dy) {
						for (int dx = -1; dx <= 1; ++dx) {
							const auto key = Key(cx + dx, cy + dy, cz + dz);
							auto it = std::ranges::lower_bound(keys, key, {}, &std::pair<std::uint64_t, std::uint32_t>::first);
							for (; it != keys.end() && it->first == key; ++it) {
								a_visit(it->second);
							}
						}
					}
				}
			}

		private:
			int CellOf(float a_value) const { return static_cast<int>(std::floor(a_value / cell)); }

			static std::uint64_t Key(int a_x, int a_y, int a_z)
			{
				constexpr std::int64_t bias = 1 << 20;
				constexpr std::uint64_t mask = (1u << 21) - 1;
				return (static_cast<std::uint64_t>(a_x + bias) & mask) | ((static_cast<std::uint64_t>(a_y + bias) & mask) << 21) |
				       ((static_cast<std::uint64_t>(a_z + bias) & mask) << 42);
			}

			float cell;
			std::vector<std::pair<std::uint64_t, std::uint32_t>> keys;
		};

		// The local map from UV steps to world displacement, fitted (least squares) to the linked
		// neighbours: world = u * du + v * dv. False when they do not span two directions.
		bool LocalFrame(const Grid& a_grid, int a_x, int a_y, std::uint8_t a_links, Vec3& a_du, Vec3& a_dv)
		{
			float a00 = 0.0f, a01 = 0.0f, a11 = 0.0f;
			Vec3 b0, b1;
			const auto& origin = a_grid.position[a_grid.Index(a_x, a_y)];
			for (int k = 0; k < 8; ++k) {
				if (!(a_links & (1u << k))) {
					continue;
				}
				const auto dx = static_cast<float>(kDirections[k][0]);
				const auto dy = static_cast<float>(kDirections[k][1]);
				const Vec3 v = a_grid.position[a_grid.Index(a_x + kDirections[k][0], a_y + kDirections[k][1])] - origin;
				a00 += dx * dx;
				a01 += dx * dy;
				a11 += dy * dy;
				b0 = b0 + v * dx;
				b1 = b1 + v * dy;
			}
			const float det = a00 * a11 - a01 * a01;
			if (det < 0.5f) {
				return false;
			}
			a_du = (b0 * a11 - b1 * a01) * (1.0f / det);
			a_dv = (b1 * a00 - b0 * a01) * (1.0f / det);
			return true;
		}

		// Sorts (part, entry) pairs into runs per part.
		void GroupByPart(std::vector<std::pair<std::uint32_t, Entry>>& a_found, std::vector<Entry>& a_entries, std::vector<SeamMap::Group>& a_groups)
		{
			std::ranges::sort(a_found, [](const auto& a_a, const auto& a_b) {
				return std::tie(a_a.first, a_a.second.from, a_a.second.to) < std::tie(a_b.first, a_b.second.from, a_b.second.to);
			});
			a_entries.reserve(a_found.size());
			for (const auto& [part, entry] : a_found) {
				if (a_groups.empty() || a_groups.back().part != part) {
					a_groups.push_back({ part, static_cast<std::uint32_t>(a_entries.size()), 0 });
				}
				a_entries.push_back(entry);
				++a_groups.back().count;
			}
		}
	}

	std::vector<PartSeams> Build(const std::vector<Part>& a_parts)
	{
		std::vector<Grid> grids;
		grids.reserve(a_parts.size());
		for (const auto& part : a_parts) {
			grids.push_back(Decode(part));
		}

		std::vector<PartSeams> out(a_parts.size());
		std::vector<EdgeTexel> edges;
		std::vector<Vec3> edgePositions;
		float cell = 0.0f;
		for (std::uint32_t p = 0; p < grids.size(); ++p) {
			const auto& grid = grids[p];
			out[p].links = Links(grid);
			cell = std::max(cell, 2.0f * grid.texelWorld);
			for (int y = 0; y < grid.size; ++y) {
				for (int x = 0; x < grid.size; ++x) {
					const int i = grid.Index(x, y);
					if (grid.covered[i] && out[p].links[i] != 0xFF) {
						edges.push_back({ p, x, y });
						edgePositions.push_back(grid.position[i]);
						++out[p].stats.edgeTexels;
					}
				}
			}
		}
		if (edges.empty() || !(cell > 0.0f)) {
			return out;
		}
		const EdgeIndex index(cell, edgePositions);

		struct Fill
		{
			std::uint32_t part;
			std::uint32_t to;
			float distance;
		};
		std::vector<std::vector<std::pair<std::uint32_t, Entry>>> sends(grids.size());
		std::vector<std::unordered_map<std::uint32_t, Fill>> fills(grids.size());

		for (const auto& edge : edges) {
			const auto& grid = grids[edge.part];
			const int i = grid.Index(edge.x, edge.y);
			const auto links = out[edge.part].links[i];
			out[edge.part].stats.openSides += static_cast<std::uint32_t>(8 - std::popcount(static_cast<unsigned>(links)));
			Vec3 du, dv;
			if (!LocalFrame(grid, edge.x, edge.y, links, du, dv)) {
				continue;
			}
			for (std::uint32_t k = 0; k < 8; ++k) {
				if (links & (1u << k)) {
					continue;
				}
				const Vec3 step = du * static_cast<float>(kDirections[k][0]) + dv * static_cast<float>(kDirections[k][1]);
				const float stepLength = Length(step);
				if (!(stepLength > 0.0f)) {
					continue;
				}
				const Vec3 target = grid.position[i] + step;
				float best = std::min(kReach * stepLength, cell);
				std::optional<std::uint32_t> across;
				index.Near(target, [&](std::uint32_t a_candidate) {
					const auto& other = edges[a_candidate];
					if (other.part == edge.part && std::max(std::abs(other.x - edge.x), std::abs(other.y - edge.y)) <= kOwnNeighbourhood) {
						return;
					}
					const auto& otherGrid = grids[other.part];
					const int j = otherGrid.Index(other.x, other.y);
					if (Dot(grid.normal[i], otherGrid.normal[j]) < kMinNormalDot) {
						return;
					}
					const float distance = Length(otherGrid.position[j] - target);
					if (distance <= best) {
						best = distance;
						across = a_candidate;
					}
				});
				if (!across) {
					continue;
				}
				const auto& other = edges[*across];
				const auto to = PackTexel(static_cast<std::uint32_t>(other.x), static_cast<std::uint32_t>(other.y));
				sends[edge.part].push_back({ other.part, { PackTexel(static_cast<std::uint32_t>(edge.x), static_cast<std::uint32_t>(edge.y), k), to } });
				if (other.part != edge.part) {
					++out[edge.part].stats.sendsToOtherParts;
				}

				const int gx = edge.x + kDirections[k][0];
				const int gy = edge.y + kDirections[k][1];
				if (grid.Inside(gx, gy) && !grid.covered[grid.Index(gx, gy)]) {
					const auto gutter = PackTexel(static_cast<std::uint32_t>(gx), static_cast<std::uint32_t>(gy));
					const auto [it, added] = fills[edge.part].try_emplace(gutter, Fill{ other.part, to, best });
					if (!added && best < it->second.distance) {
						it->second = { other.part, to, best };
					}
				}
			}
		}

		for (std::size_t p = 0; p < grids.size(); ++p) {
			GroupByPart(sends[p], out[p].sends, out[p].sendGroups);
			std::vector<std::pair<std::uint32_t, Entry>> gutter;
			gutter.reserve(fills[p].size());
			for (const auto& [texel, fill] : fills[p]) {
				gutter.push_back({ fill.part, { texel, fill.to } });
			}
			GroupByPart(gutter, out[p].fills, out[p].fillGroups);
			out[p].stats.sends = static_cast<std::uint32_t>(out[p].sends.size());
			out[p].stats.fills = static_cast<std::uint32_t>(out[p].fills.size());
		}
		return out;
	}
}
