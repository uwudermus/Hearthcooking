// Our version's rules (DISENO.md): the log fire, the per-piece timers and the quality windows.
#include "../src/sim/Timing.h"

#include <cmath>
#include <cstdio>

using namespace hc;
using namespace hc::timing;

static int g_fail = 0;
#define CHECK(c, ...)                                        \
	do {                                                     \
		if (!(c)) {                                          \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			std::printf(__VA_ARGS__);                        \
			std::printf("\n");                               \
			++g_fail;                                        \
		}                                                    \
	} while (0)

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

static void TestReduction()
{
	CHECK(Near(Reduction(1), 0.0), "1 log: no cut");
	CHECK(Near(Reduction(2), 0.10), "2 logs: 10%%");
	CHECK(Near(Reduction(3), 0.15), "3 logs: 15%%");
	CHECK(Near(Reduction(4), 0.175), "4 logs: 17.5%%");
	CHECK(Near(Reduction(5), 0.1875), "5 logs: 18.75%%");
	CHECK(Near(Reduction(9), 0.1875), "no more than 5 count");
	CHECK(Rate(0) == 0.0, "no fire: it pauses");
	// a 10 s dish over 3 logs is ready in 8.5 real seconds
	Piece p;
	p.time = 10.0;
	p.in = true;
	double t = 0.0;
	while (p.Remaining() > 0.0) {
		p.Step(0.01, Rate(3));
		t += 0.01;
	}
	CHECK(std::fabs(t - 8.5) < 0.02, "3 logs: 8.5 s, got %.2f", t);
}

static void TestFire()
{
	LogFire f;
	for (int i = 0; i < 7; ++i) {
		f.Add();
	}
	CHECK(f.Burning() == kMaxLogs, "at most 5");
	for (int i = 0; i < 1990; ++i) {
		f.Step(0.01);
	}
	CHECK(f.Lit(), "still burning at 19.9 s");
	for (int i = 0; i < 20; ++i) {
		f.Step(0.01);
	}
	CHECK(!f.Lit(), "out after 20 s");
	// cooking pauses with the fire out
	Piece p;
	p.time = 10.0;
	p.in = true;
	p.Step(5.0, f.Rate());
	CHECK(p.cooked == 0.0, "no fire, no cooking");
}

static void TestWindows()
{
	// the example: a 10 s dish
	struct
	{
		double at;
		Tier   tier;
		bool   raw;
	} cases[] = {
		{ 1.0, Tier::Burnt, true }, { 9.9, Tier::Burnt, true },
		{ 10.0, Tier::Masterwork, false }, { 10.9, Tier::Masterwork, false },
		{ 11.0, Tier::Hearty, false }, { 13.9, Tier::Hearty, false },
		{ 14.0, Tier::Plain, false }, { 16.9, Tier::Plain, false },
		{ 17.0, Tier::Burnt, false }, { 40.0, Tier::Burnt, false },
	};
	for (const auto& c : cases) {
		bool       raw = false;
		const Tier t = TierAt(c.at, 10.0, raw);
		CHECK(t == c.tier && raw == c.raw, "at %.1f: %s%s", c.at, TierName(t), raw ? " (raw)" : "");
	}
}

static void TestBatch()
{
	// the dough cut in 3: two even pieces and one half-size; out together at 10 s
	const double total = 3.0, ideal = total / 3;
	Piece p[3];
	p[0].time = PieceTime(10.0, 1.25, ideal);
	p[1].time = PieceTime(10.0, 1.25, ideal);
	p[2].time = PieceTime(10.0, 0.5, ideal);
	CHECK(Near(p[2].time, 5.0), "half size, half time: %.2f", p[2].time);
	CHECK(Near(PieceTime(10.0, 0.05, ideal), 3.0), "tiny: no less than 3 s");
	for (auto& x : p) {
		x.in = true;
	}
	for (int i = 0; i < 1260; ++i) {  // 12.6 s
		for (auto& x : p) {
			x.Step(0.01, Rate(1));
		}
	}
	TakeOutAll(p, 3);
	CHECK(p[0].tier == Tier::Masterwork && p[1].tier == Tier::Masterwork, "the big ones (12.5 s) at 12.6 s: %s %s", TierName(p[0].tier), TierName(p[1].tier));
	CHECK(p[2].tier == Tier::Burnt && !p[2].raw, "the small one burnt: %s", TierName(p[2].tier));

	const int have[2] = { 3, 7 }, need[2] = { 1, 2 };
	CHECK(MaxBatch(have, need, 2, kOvenMax) == 3, "3 flour, 7 butter by 2: 3");
	const int have2[1] = { 9 }, need2[1] = { 1 };
	CHECK(MaxBatch(have2, need2, 1, kOvenMax) == 4, "the oven's most: 4");
}

static void TestMult()
{
	const auto mw = Multipliers(Tier::Masterwork);
	CHECK(Near(mw.magnitude, 3.5) && Near(mw.duration, 2.5) && Near(mw.value, 3.5), "masterwork");
	const auto b = Multipliers(Tier::Burnt);
	CHECK(Near(b.magnitude, 0.5) && Near(b.value, 0.5), "burnt / raw halved");
	int seen[6] = {};
	for (unsigned s = 0; s < 1000; ++s) {
		const int n = StartLogs(s);
		CHECK(n >= 2 && n <= 5, "start logs %d", n);
		if (n >= 0 && n < 6) {
			++seen[n];
		}
	}
	CHECK(seen[2] && seen[3] && seen[4] && seen[5], "every count from 2 to 5 comes up");
}

int main()
{
	TestReduction();
	TestFire();
	TestWindows();
	TestBatch();
	TestMult();
	std::printf(g_fail ? "%d FAILED\n" : "timing: all passed\n", g_fail);
	return g_fail ? 1 : 0;
}
