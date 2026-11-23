#include "Timing.h"

namespace hc
{
	namespace timing
	{
		Mult Multipliers(Tier a_tier)
		{
			switch (a_tier) {
			case Tier::Masterwork:
				return { 3.5, 2.5, 3.5 };
			case Tier::Hearty:
				return { 2.0, 2.0, 2.0 };
			case Tier::Burnt:
				return { 0.5, 0.5, 0.5 };
			default:
				return { 1.0, 1.0, 1.0 };
			}
		}

		double Reduction(int a_burning)
		{
			double r = 0.0, step = kFirstCut;
			for (int i = 2; i <= a_burning && i <= kMaxLogs; ++i) {
				r += step;
				step *= 0.5;
			}
			return r;
		}

		double Rate(int a_burning)
		{
			if (a_burning <= 0) {
				return 0.0;
			}
			return 1.0 / (1.0 - Reduction(a_burning));  // the time is (1 - reduction) of the base: it runs this much faster
		}

		int StartLogs(unsigned a_seed)
		{
			// a small hash so neighbouring seeds don't give neighbouring counts
			unsigned x = a_seed * 2654435761u;
			x ^= x >> 15;
			x *= 2246822519u;
			x ^= x >> 13;
			return kStartLogsMin + static_cast<int>(x % static_cast<unsigned>(kStartLogsMax - kStartLogsMin + 1));
		}

		// ------------------------------------------------------------------ the fire
		bool LogFire::Add()
		{
			if (_n >= kMaxLogs) {
				return false;
			}
			_left[_n++] = kLogBurn;
			return true;
		}

		void LogFire::Step(double a_dt)
		{
			for (int i = 0; i < _n;) {
				_left[i] -= a_dt;
				if (_left[i] <= 0.0) {
					_left[i] = _left[--_n];  // burnt out
					continue;
				}
				++i;
			}
		}

		// ------------------------------------------------------------------ pieces
		Tier TierAt(double a_cooked, double a_time, bool& a_raw)
		{
			a_raw = false;
			const double over = a_cooked - a_time;
			if (over < 0.0) {
				a_raw = true;
				return Tier::Burnt;  // raw counts as burnt
			}
			if (over < kMasterWin) {
				return Tier::Masterwork;
			}
			if (over < kHeartyEnd) {
				return Tier::Hearty;
			}
			if (over < kPlainEnd) {
				return Tier::Plain;
			}
			return Tier::Burnt;
		}

		Tier Piece::TierNow(bool& a_raw) const { return TierAt(cooked, time, a_raw); }

		void Piece::TakeOut()
		{
			if (out) {
				return;
			}
			tier = TierNow(raw);
			out = true;
			in = false;
		}

		double PieceTime(double a_recipeTime, double a_size, double a_idealSize)
		{
			if (a_idealSize <= 0.0) {
				return a_recipeTime;
			}
			double f = a_size / a_idealSize;
			f = f < kMinSize ? kMinSize : f > kMaxSize ? kMaxSize : f;
			return a_recipeTime * f;
		}

		void TakeOutAll(Piece* a_pieces, int a_count)
		{
			for (int i = 0; i < a_count; ++i) {
				a_pieces[i].TakeOut();
			}
		}

		int MaxBatch(const int* a_have, const int* a_need, int a_count, int a_stationMax)
		{
			int best = a_stationMax;
			for (int i = 0; i < a_count; ++i) {
				if (a_need[i] <= 0) {
					continue;
				}
				const int n = a_have[i] / a_need[i];
				if (n < best) {
					best = n;
				}
			}
			return best < 0 ? 0 : best;
		}
	}
}
