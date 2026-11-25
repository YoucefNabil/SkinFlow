// SkinFlow sweat simulation: one step of the water on one surface, in its UV space.
// SkinFlow.dll runs it once per surface and frame (FlowPass::Step) and reloads this file when it
// is saved while the game runs.
//
// State per texel (RGBA32F):
//   x = water, in drops: 1 is a drop just heavy enough to run down vertical skin;
//   y = trail wetness, 1 where a drop just ran, fading out;
//   z = how far the running drop here has got towards the next texel, 0..1 (drawn in between, so
//       drops slide smoothly instead of jumping a texel at a time);
//   w = 1 + direction of that next texel (index into kDirections), 0 when the water is not running.
//
// One step:
//   - a texel whose water is heavier than its slope can hold (1 / sine of the slope; less for a
//     drop that is already running) runs towards a neighbour downhill at the run speed for that
//     slope; once it gets there it moves into it whole, keeping a little behind (the residual film);
//   - the neighbour is one of the two around the downhill direction, fixed per texel and picked by
//     how close each is, so drops follow the field on average and later drops take earlier ones' paths;
//   - every texel gathers what its neighbours moved into it (drops merge and carry on);
//   - pores add sweat: a fixed share of texels (PoreShare) are pores and get all of it, so beads
//     grow there, spaced apart;
//   - on skin facing down a heavy drop falls off; all water dries slowly.
// Each texel works out its neighbours' moves itself from their state, so the step is a gather with
// no atomics. Water that moves onto an uncovered texel (off a UV island) is lost.

#ifndef GROUP_SIZE
#	define GROUP_SIZE 8  // set by SkinFlow.dll (FlowPass::kGroupSize)
#endif

cbuffer FlowConstants : register(b0)
{
	float Dt;           // seconds
	uint Step;          // step number since the surface was assigned
	uint Size;          // texels per side
	float TexelWorld;   // world units per texel
	float Amount;       // sweat drive 0..1
	float SweatRate;    // water per square world unit and second at full sweat
	float RunSpeed;     // world units per second down vertical skin
	float Evaporation;  // water per square world unit and second
	float Residual;     // water a running drop leaves behind on each texel
	float PoreShare;    // share of texels that sweat
	float2 pad;
};

Texture2D<float4> Gravity : register(t0);  // xy downhill in UV (length = sine of the slope), z = normal.z, w = covered
Texture2D<float4> WaterIn : register(t1);
RWTexture2D<float4> WaterOut : register(u0);

static const int2 kDirections[8] = { int2(1, 0), int2(1, 1), int2(0, 1), int2(-1, 1), int2(-1, 0), int2(-1, -1), int2(0, -1), int2(1, -1) };
static const float kMinSlope = 0.05;      // flatter skin: water stays where it is
static const float kRunningHold = 0.3;    // a running drop keeps going down to this share of the hold (long runs)
static const float kMaxHold = 2.5;        // most water a bead holds on gentle slopes before it runs
static const float kTrailSeconds = 20.0;  // a trail fades over about this long
static const float kDripMass = 0.6;       // water that falls off skin facing straight down
static const float kPi = 3.14159265;

uint3 Pcg3d(uint3 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	return v;
}

float Random(int2 p, uint seed)
{
	return float(Pcg3d(uint3(p, seed)).x >> 8) * (1.0 / 16777216.0);
}

/// Pore field: a fixed share of sweat per texel, mean 1 over the surface. Only PoreShare of the
/// texels sweat, each 0.5 - 1.5 times the pore average.
float Pore(int2 p)
{
	const float h = Random(p, 0x5eed1u);
	return h < PoreShare ? (0.5 + h / PoreShare) / PoreShare : 0.0;
}

/// The neighbour texel p sends its water to: one of the two around the downhill direction, picked
/// by a fixed random number weighted by how close each is.
int Direction(int2 p, float2 downhill)
{
	const float sector = atan2(downhill.y, downhill.x) / (kPi / 4.0);
	const float lower = floor(sector);
	return (int(lower) + (Random(p, 0xd1e5u) < sector - lower ? 1 : 0)) & 7;
}

struct Run
{
	bool running;    // the water here is running this step
	bool arrives;    // it reaches the next texel this step
	int direction;   // index into kDirections, -1 when not running
	float progress;  // towards the next texel after this step (past it when arriving: the carry-over)
	float moved;     // water that moves on when arriving
};

/// What texel p's water does this step.
Run Plan(int2 p)
{
	Run r;
	r.running = false;
	r.arrives = false;
	r.direction = -1;
	r.progress = 0;
	r.moved = 0;
	if (any(p < 0) || any(p >= int(Size)))
		return r;
	const float4 g = Gravity.Load(int3(p, 0));
	const float slope = length(g.xy);
	if (g.w < 0.5 || slope < kMinSlope)
		return r;
	const float4 s = WaterIn.Load(int3(p, 0));
	const bool wasRunning = s.w >= 1.0;
	if (s.x <= min((wasRunning ? kRunningHold : 1.0) / slope, kMaxHold))
		return r;

	r.running = true;
	r.direction = wasRunning ? int(s.w) - 1 : Direction(p, g.xy);  // a running drop keeps its course to the next texel
	const float distance = (r.direction & 1) ? 1.41421356 : 1.0;
	r.progress = (wasRunning ? s.z : 0.0) + RunSpeed * slope * Dt / (TexelWorld * distance);
	if (r.progress >= 1.0) {
		r.arrives = true;
		r.progress = min(r.progress - 1.0, 0.95);
		r.moved = s.x - min(s.x, Residual);
	}
	return r;
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)] void CSMain(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= Size))
		return;
	const int2 p = int2(id.xy);
	const float4 g = Gravity.Load(int3(p, 0));
	if (g.w < 0.5) {
		WaterOut[p] = 0;
		return;
	}

	const float4 state = WaterIn.Load(int3(p, 0));
	const Run own = Plan(p);
	float water = own.arrives ? state.x - own.moved : state.x;

	float arrived = 0;
	float carried = 0;  // progress the arriving drops bring towards their next texel
	[unroll] for (int k = 0; k < 8; ++k)
	{
		const Run n = Plan(p + kDirections[k]);
		if (n.arrives && n.direction == ((k + 4) & 7)) {  // the neighbour's next texel is this one
			arrived += n.moved;
			carried = max(carried, n.progress);
		}
	}
	water += arrived;
	const float trail = arrived > 0 ? 1.0 : state.y * exp(-Dt / kTrailSeconds);

	const float texelArea = TexelWorld * TexelWorld;
	water += Dt * SweatRate * Amount * texelArea * Pore(p);

	// Running state for the next step: an arriving drop carries on from here (merged with a
	// running drop that stays); a drop still on its way keeps going; anything else rests.
	float progress = 0;
	float direction = 0;
	if (arrived > 0) {
		progress = own.running && !own.arrives ? max(carried, own.progress) : carried;
		direction = 1.0 + (own.running && !own.arrives ? own.direction : Direction(p, g.xy));
	} else if (own.running && !own.arrives) {
		progress = own.progress;
		direction = 1.0 + own.direction;
	}

	const float facingDown = -g.z;
	if (facingDown > 0.25 && water > kDripMass / facingDown) {
		water = min(water, Residual);
		progress = 0;
		direction = 0;
	}

	water = max(0.0, water - Dt * Evaporation * texelArea);
	WaterOut[p] = float4(water, trail, progress, direction);
}
