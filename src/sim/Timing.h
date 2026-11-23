#pragma once

// Our version's cooking rules (see DISENO.md): a fire of logs that burn 15 s each and cook while any one burns, a
// timer per piece, and the quality by the second it comes out. Plain C++ with no game or standard-library code, like
// Kitchen.h, so the same rules run in the SKSE plugin and in tests/timing_test.cpp.

#include "Kitchen.h"  // hc::Tier

namespace hc
{
	namespace timing
	{
		// ------------------------------------------------------------------ the tuning
		constexpr double kLogBurn = 15.0;      // seconds a log burns
		constexpr int    kMaxLogs = 5;         // logs burning at once
		constexpr int    kStartLogsMin = 2;    // a station's own logs, picked at random
		constexpr int    kStartLogsMax = 5;
		constexpr double kFirstCut = 0.10;     // the 2nd log cuts the time 10 %, each one after half the one before
		constexpr double kMinSize = 0.3;       // a piece's time: from 0.3x (tiny) ..
		constexpr double kMaxSize = 2.0;       // .. to 2x (huge) the recipe's

		// The windows after the piece's time T: [T, T+1) Masterwork, [T+1, T+4) good, [T+4, T+7) plain, then burnt
		constexpr double kMasterWin = 1.0;
		constexpr double kHeartyEnd = 4.0;
		constexpr double kPlainEnd = 7.0;

		// What each quality does to the game's dish
		struct Mult
		{
			double magnitude, duration, value;
		};
		Mult Multipliers(Tier a_tier);

		// How much shorter the cooking is with this many logs burning (0 .. 0.1875)
		double Reduction(int a_burning);
		// How fast the cooking goes with this many logs burning (0 when none: it pauses)
		double Rate(int a_burning);

		// A random number of logs for a station, kStartLogsMin .. kStartLogsMax, from a seed (the plugin passes its
		// own random number, the tests a fixed one)
		int StartLogs(unsigned a_seed);

		// ------------------------------------------------------------------ the fire
		class LogFire
		{
		public:
			bool   Add();                         // a log in, burning for kLogBurn; false if the fire is full
			void   Step(double a_dt);             // the logs burn down; out ones are gone
			int    Burning() const { return _n; }
			bool   Lit() const { return _n > 0; }
			double Left(int a_i) const { return a_i < _n ? _left[a_i] : 0.0; }  // seconds this log has left
			double Rate() const { return timing::Rate(_n); }

		private:
			double _left[kMaxLogs]{};
			int    _n = 0;
		};

		// ------------------------------------------------------------------ a piece cooking
		// The clock runs in cooking seconds: real seconds times the fire's rate, so a fire of three logs runs it
		// 1/0.85 as fast and a dead fire stops it. The windows are in the same seconds.
		struct Piece
		{
			double time = 10.0;    // T: its cooking time (the recipe's, by its size)
			double cooked = 0.0;   // cooking seconds so far
			bool   in = false;     // on the fire (cooking)
			bool   out = false;    // taken off: its quality is fixed
			Tier   tier = Tier::Plain;
			bool   raw = false;    // came out before its time (counts as Burnt)

			void   Step(double a_dt, double a_rate) { if (in && !out) cooked += a_dt * a_rate; }
			double Remaining() const { return time - cooked; }      // what the counter shows (negative: past it)
			Tier   TierNow(bool& a_raw) const;                       // what it would be if taken off now
			void   TakeOut();
		};

		// A piece's time, by its size against the ideal piece (total / count): T x size/ideal, kept in 0.3x .. 2x
		double PieceTime(double a_recipeTime, double a_size, double a_idealSize);

		// The tier for coming out at a_cooked cooking seconds when the time was a_time
		Tier TierAt(double a_cooked, double a_time, bool& a_raw);

		// A batch in the oven comes out whole: each piece takes its own tier
		void TakeOutAll(Piece* a_pieces, int a_count);

		// The most dishes the ingredients allow: for each ingredient, have / need, the least of them (0 if any is
		// short), and no more than the station's most
		int MaxBatch(const int* a_have, const int* a_need, int a_count, int a_stationMax);

		constexpr int kOvenMax = 4, kSpitMax = 4, kPotMax = 5;
	}
}
