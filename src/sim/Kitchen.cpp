#include "Kitchen.h"

#include <algorithm>

namespace hc
{
	namespace
	{
		using namespace layout;

		double Lag(double a_value, double a_target, double a_tau, double a_dt) { return a_value + (a_target - a_value) * a_dt / (a_tau + a_dt); }

		// atan2 to about 1e-5, our own as sin and cos are (Math.h)
		double Atan2(double a_y, double a_x)
		{
			const double ax = m::Abs(a_x), ay = m::Abs(a_y);
			if (ax < 1e-12 && ay < 1e-12) {
				return 0.0;
			}
			const double a = ax > ay ? ay / ax : ax / ay;
			const double s = a * a;
			double       r = ((-0.0464964749 * s + 0.15931422) * s - 0.327622764) * s * a + a;
			if (ay > ax) {
				r = 1.57079637 - r;
			}
			if (a_x < 0.0) {
				r = 3.14159274 - r;
			}
			return a_y < 0.0 ? -r : r;
		}

		double Window(double a_v, double a_lo, double a_hi, double a_under, double a_over)
		{
			if (a_v < a_lo) {
				return m::Clamp(1.0 - (a_lo - a_v) * a_under, 0.0, 1.0);
			}
			if (a_v > a_hi) {
				return m::Clamp(1.0 - (a_v - a_hi) * a_over, 0.0, 1.0);
			}
			return 1.0;
		}

		const char* Pick3(double a_s, const char* a_good, const char* a_mid, const char* a_bad) { return a_s >= 0.8 ? a_good : a_s >= 0.5 ? a_mid : a_bad; }
	}

	const char* TierName(Tier a_tier)
	{
		switch (a_tier) {
		case Tier::Burnt:
			return "Burnt";
		case Tier::Hearty:
			return "Hearty";
		case Tier::Masterwork:
			return "Masterwork";
		default:
			return "Plain";
		}
	}

	// ------------------------------------------------------------------ the fire
	void Fire::Light(int a_logs)
	{
		_count = 0;
		for (int i = 0; i < a_logs && i < kMaxLogs; ++i) {
			_log[_count++] = { 10.0, 1.0 };
		}
		_heat = Target();
	}

	bool Fire::AddLog()
	{
		if (_count >= kMaxLogs) {
			return false;
		}
		_log[_count++] = { 0.0, 1.0 };
		return true;
	}

	bool Fire::RemoveLog()
	{
		if (_count == 0) {
			return false;
		}
		int best = 0;
		for (int i = 1; i < _count; ++i) {
			if (_log[i].fuel > _log[best].fuel) {
				best = i;
			}
		}
		_log[best] = _log[--_count];
		return true;
	}

	double Fire::Glow(int a_i) const
	{
		if (a_i >= _count) {
			return 0.0;
		}
		const auto& l = _log[a_i];
		return m::Min(1.0, l.age / 5.0) * (0.4 + 0.6 * l.fuel);
	}

	double Fire::Target() const
	{
		double t = 0.0;
		for (int i = 0; i < _count; ++i) {
			t += m::Min(1.0, _log[i].age / 5.0) * (0.3 + 0.7 * _log[i].fuel) * 0.3;
		}
		return t;
	}

	void Fire::Step(double a_dt)
	{
		for (int i = 0; i < _count;) {
			_log[i].age += a_dt;
			_log[i].fuel -= a_dt / _burn;
			if (_log[i].fuel <= 0.0) {
				_log[i] = _log[--_count];  // burnt out
				continue;
			}
			++i;
		}
		_heat = Lag(_heat, Target(), _lag, a_dt);
	}

	// ------------------------------------------------------------------ scoring pieces
	void HeatLog::Add(double a_heat, double a_dt)
	{
		time += a_dt;
		if (a_heat < lo) {
			cold += a_dt;
		} else if (a_heat > hi) {
			hot += a_dt;
		} else {
			inZone += a_dt;
		}
	}

	double HeatLog::Score() const { return time > 0.0 ? inZone / time : 0.0; }

	const char* HeatLog::Verdict() const
	{
		const double s = Score();
		if (s >= 0.85) {
			return "Steady all the way";
		}
		if (s >= 0.6) {
			return hot > cold ? "Ran a little hot" : "Dipped a little cool";
		}
		return hot > cold ? "Far too hot" : "Too cool for too long";
	}

	double Salt::Score() const
	{
		if (amount < 0.7) {
			return amount / 0.7;
		}
		if (amount > 1.4) {
			return m::Max(0.0, 1.0 - (amount - 1.4) * 0.9);
		}
		return 1.0;
	}

	const char* Salt::Verdict() const
	{
		if (amount <= 0.01) {
			return "Forgot the salt";
		}
		if (amount < 0.7) {
			return "Could use more salt";
		}
		if (amount > 1.4) {
			return amount > 2.2 ? "Far too salty" : "A touch salty";
		}
		return "Seasoned just right";
	}

	// ------------------------------------------------------------------ cutting
	namespace
	{
		constexpr double kMinCut = 0.3;  // a thinner sliver than this isn't a cut

		bool Insert(double* a_cuts, int& a_count, double a_lo, double a_hi, double a_x)
		{
			if (a_count >= Cutting::kMaxCuts || a_x <= a_lo + kMinCut || a_x >= a_hi - kMinCut) {
				return false;
			}
			int at = 0;
			while (at < a_count && a_cuts[at] < a_x) {
				++at;
			}
			if ((at > 0 && a_x - a_cuts[at - 1] < kMinCut) || (at < a_count && a_cuts[at] - a_x < kMinCut)) {
				return false;
			}
			for (int k = a_count; k > at; --k) {
				a_cuts[k] = a_cuts[k - 1];
			}
			a_cuts[at] = a_x;
			++a_count;
			return true;
		}

		void Span(const double* a_cuts, int a_count, double a_lo, double a_hi, int a_i, double& a_a, double& a_b)
		{
			a_a = a_i == 0 ? a_lo : a_cuts[a_i - 1];
			a_b = a_i == a_count ? a_hi : a_cuts[a_i];
		}

		// The pieces' sizes one way: their mean and spread
		void Spread(const double* a_cuts, int a_count, double a_lo, double a_hi, double& a_mean, double& a_sd)
		{
			const int n = a_count + 1;
			a_mean = (a_hi - a_lo) / n;
			double var = 0.0;
			for (int i = 0; i < n; ++i) {
				double a, b;
				Span(a_cuts, a_count, a_lo, a_hi, i, a, b);
				var += (b - a - a_mean) * (b - a - a_mean);
			}
			a_sd = m::Sqrt(var / n);
		}
	}

	bool Cutting::Chop(double a_x) { return !finished && Insert(cuts, cutCount, x0, x1, a_x); }

	bool Cutting::ChopAlong(double a_y) { return !finished && width > 0.0 && Insert(along, alongCount, 0.0, width, a_y); }

	void Cutting::Piece(int a_i, double& a_a, double& a_b) const { Span(cuts, cutCount, x0, x1, a_i, a_a, a_b); }

	void Cutting::Strip(int a_j, double& a_a, double& a_b) const { Span(along, alongCount, 0.0, width, a_j, a_a, a_b); }

	double Cutting::Score() const
	{
		if (cutCount == 0 && alongCount == 0) {
			return 0.05;  // went in whole
		}
		double mx, sx;
		Spread(cuts, cutCount, x0, x1, mx, sx);
		auto size = [&](double a_mean) { return m::Clamp(1.0 - m::Abs(a_mean - target) / target * 0.9, 0.0, 1.0); };
		double even = m::Clamp(1.0 - sx / mx * 1.8, 0.0, 1.0);
		double sz = size(mx);
		if (width > 0.0) {
			// diced: across and along both count
			double my, sy;
			Spread(along, alongCount, 0.0, width, my, sy);
			even = 0.5 * even + 0.5 * m::Clamp(1.0 - sy / my * 1.8, 0.0, 1.0);
			sz = 0.5 * sz + 0.5 * size(my);
		}
		return 0.55 * even + 0.45 * sz;
	}

	// ------------------------------------------------------------------ pieces in flight
	Vec3 Chunks::Board::ToWorld(const Vec3& l) const
	{
		const double c = m::Cos(tilt), s = m::Sin(tilt);
		return { this->c.x + l.x * c + l.z * s, this->c.y + l.y, this->c.z - l.x * s + l.z * c };
	}

	Vec3 Chunks::Board::ToLocal(const Vec3& w) const
	{
		const double c = m::Cos(tilt), s = m::Sin(tilt);
		const Vec3   d = w - this->c;
		return { d.x * c - d.z * s, d.y, d.x * s + d.z * c };
	}

	Vec3 Chunks::Board::Up() const { return { m::Sin(tilt), 0.0, m::Cos(tilt) }; }

	int Chunks::Add(int a_ing, const Vec3& a_local, const Vec3& a_size, unsigned a_seed)
	{
		if (_n >= kMax) {
			return -1;
		}
		Rng   rng(a_seed);
		Body& b = _b[_n];
		b = {};
		b.state = State::Board;
		b.ing = a_ing;
		b.local = a_local;
		b.size = a_size;
		// about as round as its middle size, not its longest side (a thin slice is not a ball)
		double d[3] = { a_size.x, a_size.y, a_size.z };
		std::sort(d, d + 3);
		b.r = m::Max(0.3, 0.5 * d[1] * 0.9);
		b.q = Quat::FromAxisAngle({ 0, 0, 1 }, rng.Range(-0.35, 0.35));
		b.bob = rng.Range(0.0, 6.28);
		return _n++;
	}

	int Chunks::AddFree(int a_ing, const Vec3& a_at, const Vec3& a_vel, const Vec3& a_size, unsigned a_seed)
	{
		const int i = Add(a_ing, {}, a_size, a_seed);
		if (i >= 0) {
			_b[i].state = State::Free;
			_b[i].p = a_at;
			_b[i].v = a_vel;
			Rng rng(a_seed * 7u + 3u);
			_b[i].w = { rng.Range(-6, 6), rng.Range(-6, 6), rng.Range(-3, 3) };
		}
		return i;
	}

	int Chunks::CountIn(State a_s) const
	{
		int n = 0;
		for (int i = 0; i < _n; ++i) {
			n += _b[i].state == a_s;
		}
		return n;
	}

	int Chunks::CountFor(int a_ing, State a_s) const
	{
		int n = 0;
		for (int i = 0; i < _n; ++i) {
			n += _b[i].state == a_s && _b[i].ing == a_ing;
		}
		return n;
	}

	void Chunks::Push(Event::Kind a_kind, const Vec3& a_at, double a_speed)
	{
		if (_nev < 64) {
			_ev[_nev++] = { a_kind, a_at, a_speed };
		}
	}

	int Chunks::TakeEvents(Event* a_out, int a_max)
	{
		const int n = _nev < a_max ? _nev : a_max;
		for (int i = 0; i < n; ++i) {
			a_out[i] = _ev[i];
		}
		_nev = 0;
		return n;
	}

	void Chunks::Cook(double a_amount)
	{
		for (int i = 0; i < _n; ++i) {
			if (_b[i].state == State::Pot) {
				_b[i].cooked = m::Min(1.0, _b[i].cooked + a_amount);
			}
		}
	}

	namespace
	{
		double Longest(const Chunks::Body& b) { return m::Max(b.size.x, m::Max(b.size.y, b.size.z)); }
		double Shortest(const Chunks::Body& b) { return m::Min(b.size.x, m::Min(b.size.y, b.size.z)); }

		// Into the broth: it lies flat (its thinnest side up), all of it inside the pot's wall
		void Settle(Chunks::Body& b, double a_dx, double a_dy, double a_r)
		{
			b.state = Chunks::State::Pot;
			b.potA = Atan2(a_dy, a_dx);
			const double room = m::Max(0.0, a_r - Longest(b) * 0.5 - 0.35);
			b.potR = m::Min(m::Sqrt(a_dx * a_dx + a_dy * a_dy), room);
			Quat flat;
			if (b.size.x <= b.size.y && b.size.x <= b.size.z) {
				flat = Quat::FromAxisAngle({ 0, 1, 0 }, 1.5708);
			} else if (b.size.y <= b.size.z) {
				flat = Quat::FromAxisAngle({ 1, 0, 0 }, 1.5708);
			}
			b.q = Quat::FromAxisAngle({ 0, 0, 1 }, b.bob * 3.7) * flat;
			b.w = {};
		}
	}

	void Chunks::Collide(Body& b, const Board& a_board, double a_dt)
	{
		constexpr double kG = 140.0;
		// the board (its top face)
		{
			const Vec3 lp = a_board.ToLocal(b.p);
			const double hw = kBoardW * 0.5, hd = kBoardD * 0.5, hh = kBoardH * 0.5;
			if (m::Abs(lp.x) <= hw && m::Abs(lp.y) <= hd && lp.z < hh + b.r && lp.z > -hh) {
				const Vec3 n = a_board.Up();
				b.p += n * (hh + b.r - lp.z);
				Vec3         vr = b.v - a_board.v;
				const double vn = vr.Dot(n);
				if (vn < 0.0) {
					vr -= n * (vn * 1.15);
				}
				// Coulomb friction: slides once the board tips past about 19 degrees
				Vec3         vt = vr - n * vr.Dot(n);
				const double sp = vt.Length();
				const double drop = 0.35 * kG * n.z * a_dt;
				vt = sp > drop ? vt * ((sp - drop) / sp) : Vec3{};
				b.v = a_board.v + vt + n * m::Max(0.0, vr.Dot(n));
				b.w = n.Cross(vt) * (1.0 / b.r);
			}
		}
		// the pot: its wall (inside and out) and its rim
		{
			const double dx = b.p.x - kPotX, dy = b.p.y - kPotY;
			const double d = m::Sqrt(dx * dx + dy * dy);
			const double ri = kPotR, ro = kPotR + kPotWall;
			const Vec3   radial = d > 1e-6 ? Vec3{ dx / d, dy / d, 0.0 } : Vec3{ 1, 0, 0 };
			if (b.p.z < kPotRim + b.r && b.p.z > kPotFloor - b.r) {
				if (d >= ri - b.r && d <= ro + b.r) {
					if (d >= ri && d <= ro && b.p.z > kPotRim - b.r * 0.6) {
						b.p.z = kPotRim + b.r;  // on the rim: it'll roll off one side or the other
						if (b.v.z < 0.0) {
							b.v.z = -b.v.z * 0.25;
						}
						b.v += radial * ((d > (ri + ro) * 0.5 ? 1.0 : -1.0) * 8.0 * a_dt);
						Push(Event::Kind::Tumble, b.p, m::Abs(b.v.z));
					} else if (d < (ri + ro) * 0.5) {
						b.p += radial * ((ri - b.r) - d);  // the inside of the wall
						const double vr = b.v.Dot(radial);
						if (vr > 0.0) {
							b.v -= radial * (vr * 1.3);
						}
					} else {
						b.p += radial * ((ro + b.r) - d);  // the outside
						const double vr = b.v.Dot(radial);
						if (vr < 0.0) {
							b.v -= radial * (vr * 1.3);
						}
					}
				}
			}
			// into the broth
			if (d < ri - b.r * 0.6 && b.p.z < kLiquidZ + b.r * 0.2 && b.p.z > kPotFloor) {
				Settle(b, dx, dy, kPotR);
				Push(Event::Kind::Splash, b.p, -b.v.z);
				return;
			}
		}
		// the worktop and the hearth
		if (b.p.z < b.r) {
			b.p.z = b.r;
			const double sp = m::Abs(b.v.z);
			b.v = { b.v.x * 0.3, b.v.y * 0.3, sp * 0.15 };
			b.state = State::Lost;
			const double fx = b.p.x - kFireX, fy = b.p.y - kFireY;
			b.charred = b.p.x > kHearthEdge && fx * fx + fy * fy < kFireR * kFireR;
			Push(Event::Kind::Land, b.p, sp);
		}
	}

	void Chunks::SetBowl(const Vec3& a_c, double a_r, double a_rim, bool a_stirred)
	{
		_bc = a_c;
		_br = a_r;
		_rim = a_rim;
		_stirred = a_stirred;
	}

	void Chunks::Launch(int a_i, const Board& a_board)
	{
		if (a_i >= 0 && a_i < _n && _b[a_i].state == State::Board) {
			_b[a_i].p = a_board.ToWorld(_b[a_i].local);
			_b[a_i].state = State::Free;
			Aim(_b[a_i], a_i);
		}
	}

	void Chunks::Aim(Body& b, int a_i)
	{
		constexpr double kG = 140.0;
		Rng          rng(static_cast<unsigned>(a_i) * 2654435761u + 17u);
		const double room = m::Max(0.3, (_br - b.r - 0.6) * 0.75);
		const double ang = rng.Range(0.0, 6.2832), rad = room * m::Sqrt(rng.Range(0.0, 1.0));
		const Vec3   to{ _bc.x + m::Cos(ang) * rad, _bc.y + m::Sin(ang) * rad, _bc.z };
		const double top = m::Max(b.p.z, _rim + b.r) + 1.5;  // over the rim
		const double up = m::Sqrt(2.0 * kG * (top - b.p.z));
		const double t = up / kG + m::Sqrt(2.0 * (top - to.z) / kG);
		b.v = { (to.x - b.p.x) / t, (to.y - b.p.y) / t, up };
		b.w = { rng.Range(-5, 5), rng.Range(-5, 5), rng.Range(-3, 3) };
		b.aimed = true;
	}

	void Chunks::Step(double a_dt, const Board& a_board, double a_swirl)
	{
		constexpr double kG = 140.0, kSub = 1.0 / 240.0;
		const Quat       boardQ = Quat::FromAxisAngle({ 0, 1, 0 }, a_board.tilt);
		double           left = a_dt;
		while (left > 1e-9) {
			const double dt = left < kSub ? left : kSub;
			left -= dt;
			for (int i = 0; i < _n; ++i) {
				Body& b = _b[i];
				switch (b.state) {
				case State::Board:
					b.p = a_board.ToWorld(b.local);
					if (a_board.tilt > 0.33) {  // past its grip: off it goes, and into the pot (nothing is ever spilt)
						b.state = State::Free;
						b.q = boardQ * b.q;
						Aim(b, i);
					}
					break;
				case State::Free:
					b.v.z -= kG * dt;
					b.p += b.v * dt;
					b.q.Integrate(b.w, dt);
					if (b.aimed) {
						// straight in: only the broth stops it
						const double dx = b.p.x - _bc.x, dy = b.p.y - _bc.y;
						if (b.v.z < 0.0 && b.p.z < _bc.z + b.r * 0.2 && dx * dx + dy * dy < _br * _br) {
							Settle(b, dx, dy, _br);
							Push(Event::Kind::Splash, b.p, -b.v.z);
						} else if (b.p.z < _bc.z - 6.0) {
							Aim(b, i + 101);  // can't happen, but never lost
						}
					} else {
						Collide(b, a_board, dt);
					}
					break;
				case State::Pot:
					{
						const double ri = _br;
						if (_stirred) {
							b.potA += a_swirl * dt * (0.55 + 0.45 * (1.0 - b.potR / ri));
							b.bob += dt;
							b.q.Integrate({ 0.0, 0.0, a_swirl * 0.6 }, dt);
						}
						b.p = { _bc.x + m::Cos(b.potA) * b.potR, _bc.y + m::Sin(b.potA) * b.potR,
							_bc.z - Shortest(b) * 0.2 + (_stirred ? 0.08 * m::Sin(b.bob * 2.1) : Shortest(b) * 0.5 + (i % 3) * 0.35) };
					}
					break;
				default:
					break;
				}
			}
			// the free pieces knock against each other
			for (int i = 0; i < _n; ++i) {
				if (_b[i].state != State::Free || _b[i].aimed) {
					continue;
				}
				for (int j = i + 1; j < _n; ++j) {
					if (_b[j].state != State::Free && _b[j].state != State::Board) {
						continue;
					}
					Body&        a = _b[i];
					Body&        c = _b[j];
					const Vec3   d = c.p - a.p;
					const double dist = d.Length(), minD = a.r + c.r;
					if (dist >= minD || dist < 1e-6) {
						continue;
					}
					const Vec3   n = d * (1.0 / dist);
					// eased apart, never flung: pieces that start out touching would otherwise burst off the board
					const double push = m::Min(0.02, (minD - dist) * (c.state == State::Board ? 1.0 : 0.5));
					a.p -= n * push;
					if (c.state == State::Free) {
						c.p += n * push;
					}
					const double rel = (c.v - a.v).Dot(n);
					if (rel < 0.0) {
						const Vec3 imp = n * (rel * 0.6);
						a.v += imp;
						if (c.state == State::Free) {
							c.v -= imp;
						}
					}
				}
			}
		}
	}

	Chunks::Board BoardPose(double a_t, double a_y)
	{
		auto smooth = [](double a, double b, double x) {
			const double t = m::Clamp((x - a) / (b - a), 0.0, 1.0);
			return t * t * (3.0 - 2.0 * t);
		};
		const double hw = kBoardW * 0.5, hh = kBoardH * 0.5;
		const Vec3   rest{ kBoardX + hw, a_y, kBoardH };
		const Vec3   goal{ kPotX - kPotR + 1.6, a_y, kPotRim + 1.4 };
		const double carry = smooth(0.0, 0.5, a_t);
		const Vec3   edge = rest + (goal - rest) * carry;
		Chunks::Board b;
		b.tilt = 0.82 * smooth(0.3, 1.0, a_t);
		b.c = {};
		// the top right edge sits at 'edge': put the middle where that holds for this tilt
		const Vec3 off = b.ToWorld({ hw, 0.0, hh });  // with c at zero, this is the edge's offset from the middle
		b.c = edge - off;
		return b;
	}

	// ------------------------------------------------------------------ grading
	Result Grade(const Line* a_lines, const double* a_weights, int a_count, int a_key, bool a_burnt, const char* a_burntWhy)
	{
		Result r;
		double sum = 0.0, wsum = 0.0;
		for (int i = 0; i < a_count && r.count < 8; ++i) {
			if (a_weights[i] < 0.0) {
				continue;
			}
			r.lines[r.count++] = a_lines[i];
			sum += a_lines[i].score * a_weights[i];
			wsum += a_weights[i];
		}
		r.quality = wsum > 0.0 ? sum / wsum : 0.0;
		if (a_burnt) {
			r.tier = Tier::Burnt;
			r.burntWhy = a_burntWhy;
		} else if (r.quality >= 0.82) {
			r.tier = Tier::Masterwork;
		} else if (r.quality >= 0.6) {
			r.tier = Tier::Hearty;
		} else {
			r.tier = Tier::Plain;
		}
		if (!a_burnt && a_key >= 0 && a_key < a_count) {
			const double k = a_lines[a_key].score;
			if (k < 0.4 && r.tier > Tier::Plain) {
				r.tier = Tier::Plain;
			} else if (k < 0.97 && r.tier > Tier::Hearty) {  // (a masterwork only when it came out done just right, in the gold)
				r.tier = Tier::Hearty;
			}
		}
		return r;
	}

	// ------------------------------------------------------------------ the pot
	Pot::Pot(bool a_salted) { _salt.wanted = a_salted; }

	double Pot::StirNow() const { return m::Min(1.0, _stir / 1.5); }

	void Pot::Stir(double a_dA)
	{
		_stir += m::Abs(a_dA) * 0.25;
		_swirl = m::Clamp(_swirl + a_dA * 0.6, -6.0, 6.0);
	}

	void Pot::AddPieces(int a_before, int a_new)
	{
		// the pot's doneness is the pieces' average: fresh raw ones bring it down
		if (a_new > 0 && a_before + a_new > 0) {
			_cook *= static_cast<double>(a_before) / (a_before + a_new);
		}
	}

	void Pot::Step(double a_dt, double a_fireHeat, bool a_hasFood)
	{
		_temp = Lag(_temp, a_fireHeat, 6.0, a_dt);
		_stir /= 1.0 + a_dt * 0.5;
		_swirl /= 1.0 + a_dt * 0.8;
		if (!a_hasFood) {
			return;
		}
		_cookTime += a_dt;
		_heat.Add(_temp, a_dt);
		if (_temp > 0.35) {
			_cook += (_temp - 0.3) * 0.055 * a_dt;
		}
		const double s = StirNow();
		if (s > 0.25) {
			_stirTime += a_dt;
		}
		_burn += a_dt * (m::Max(0.0, _temp - 0.72) * 0.25 * (1.0 - 0.85 * s) + m::Max(0.0, _temp - 0.95) * 0.4);
		if (_temp > 0.5 && s < 0.1) {
			_burn += 0.003 * a_dt;  // left alone, it catches on the bottom even at a simmer
		}
	}

	Result Pot::Score(const double* a_cuts, int a_cutCount, int a_in, int a_lost) const
	{
		Line   l[6];
		double w[6];
		// the cuts
		double cut = 0.0;
		for (int i = 0; i < a_cutCount; ++i) {
			cut += a_cuts[i];
		}
		cut = a_cutCount ? cut / a_cutCount : 0.0;
		l[0] = { "The cuts", Pick3(cut, "Even and neat", "A little uneven", "Ragged"), cut };
		w[0] = a_cutCount ? 0.2 : -1.0;
		// into the pot
		const int    total = a_in + a_lost;
		const double spill = total ? m::Clamp(1.0 - static_cast<double>(a_lost) / total * 1.5, 0.0, 1.0) : 1.0;
		l[1] = { "Into the pot", a_lost == 0 ? "Every piece went in" : spill >= 0.6 ? "A few went astray" : "Half of it on the hearth", spill };
		w[1] = total ? 0.1 : -1.0;
		// the heat
		l[2] = { "The heat", _heat.Verdict(), _heat.Score() };
		w[2] = -1.0;  // (the heat isn't judged: it shows in what it does to the dish)
		// the stirring
		const double stirShare = _cookTime > 0.0 ? m::Min(1.0, _stirTime / _cookTime * 2.0) : 0.0;
		const double stir = 0.5 * (1.0 - m::Min(1.0, _burn * 1.5)) + 0.5 * stirShare;
		l[3] = { "The stirring", _burn > 0.4 ? "Caught on the bottom" : Pick3(stir, "Never left it", "Now and then", "Hardly touched it"), stir };
		w[3] = 0.15;
		// the salt
		l[4] = { "The salt", _salt.Verdict(), _salt.Score() };
		w[4] = _salt.wanted ? 0.1 : -1.0;
		// done
		const double done = Window(_cook, 0.92, 1.3, 2.5, 1.2);
		const char*  dv = _cook < 0.6 ? "Still raw" : _cook < 0.92 ? "Not quite done" : _cook <= 1.3 ? "Cooked through, just right" : _cook < 1.9 ? "Cooked a bit long" : "Boiled to mush";
		l[5] = { "Done", dv, done };
		w[5] = 0.25;
		return Grade(l, w, 6, 5, _burn >= 1.0, "It caught on the bottom and burnt");
	}

	// ------------------------------------------------------------------ the spit
	Spit::Spit(bool a_salted) { _salt.wanted = a_salted; }

	void Spit::Turn(double a_dA) { _angle += a_dA; }

	void Spit::Step(double a_dt, double a_fireHeat, bool a_mounted)
	{
		if (!a_mounted) {
			return;
		}
		_heat.Add(a_fireHeat, a_dt);
		const double h = m::Max(0.0, a_fireHeat - 0.25);
		for (int i = 0; i < kBands; ++i) {
			const double c = m::Cos(_angle + i * (2.0 * m::kPi / kBands));  // 1: this side faces the fire
			const double f = c > 0.0 ? c * c : 0.0;
			_band[i] += a_dt * (h * 0.08 * f + h * 0.024);  // the side to the fire browns faster, the rest steadily
		}
		// the heat spreads round through the meat a little, side to side (a turn now and then is enough)
		double prev[kBands];
		for (int i = 0; i < kBands; ++i) {
			prev[i] = _band[i];
		}
		for (int i = 0; i < kBands; ++i) {
			const double nb = 0.5 * (prev[(i + kBands - 1) % kBands] + prev[(i + 1) % kBands]);
			_band[i] += (nb - prev[i]) * m::Min(1.0, a_dt * 0.02);
		}
	}

	double Spit::Mean() const
	{
		double s = 0.0;
		for (double b : _band) {
			s += b;
		}
		return s / kBands;
	}

	double Spit::Spread() const
	{
		const double mean = Mean();
		double       v = 0.0;
		for (double b : _band) {
			v += (b - mean) * (b - mean);
		}
		return m::Sqrt(v / kBands);
	}

	void Spit::SetMean(double a_m)
	{
		const double d = a_m - Mean();
		for (double& b : _band) {
			b = m::Max(0.0, b + d);
		}
	}

	int Spit::Charred() const
	{
		int n = 0;
		for (double b : _band) {
			n += b > 1.85;
		}
		return n;
	}

	Result Spit::Score() const
	{
		Line         l[4];
		double       w[4];
		const double even = m::Clamp(1.0 - Spread() * 2.0, 0.0, 1.0);
		l[0] = { "All the way round", Pick3(even, "Evenly browned", "Patchy", "One side black, one side raw"), even };
		w[0] = 0.35;
		const double mean = Mean();
		const double done = Window(mean, 0.9, 1.3, 2.5, 2.0);
		l[1] = { "Done", mean < 0.6 ? "Still bloody" : mean < 0.9 ? "Underdone" : mean <= 1.3 ? "Done to a turn" : "Overdone", done };
		w[1] = 0.3;
		l[2] = { "The fire", _heat.Verdict(), _heat.Score() };
		w[2] = -1.0;  // (the heat isn't judged: it shows in what it does to the dish)
		l[3] = { "The salt", _salt.Verdict(), _salt.Score() };
		w[3] = _salt.wanted ? 0.15 : -1.0;
		const bool burnt = Charred() >= 4 || mean > 1.7;
		Result r = Grade(l, w, 4, 1, burnt, Charred() >= 4 ? "One side went black" : "Left over the fire too long");
		if (!burnt && even < 0.6 && r.tier > Tier::Plain) {
			r.tier = Tier::Plain;  // (one side done and the other not is no better than plain, however done it is on the whole)
		}
		return r;
	}

	// ------------------------------------------------------------------ the oven
	Oven::Oven(bool a_pie, bool a_salted, bool a_dough) :
		_pie(a_pie), _dough(a_dough)
	{
		_salt.wanted = a_salted;
	}

	void Oven::Knead(double a_length, double a_now)
	{
		const double gap = a_now - _last;
		double       q = 0.3;
		if (_strokes == 0 || (gap >= 0.4 && gap <= 1.3)) {
			q = 1.0;
		} else if (gap >= 0.28 && gap <= 2.2) {
			q = 0.6;
		}
		_rhythm += q;
		++_strokes;
		_knead += 0.17 * m::Clamp(a_length / 5.0, 0.3, 1.2);  // about a dozen good pushes
		_last = a_now;
	}

	void Oven::Fill(double a_x, double a_y)
	{
		const double r = m::Sqrt(a_x * a_x + a_y * a_y);
		if (r > 1.05) {
			return;
		}
		const double a = Atan2(a_y, a_x) + m::kPi;
		int          k = static_cast<int>(a / (2.0 * m::kPi) * kSectors);
		k = k < 0 ? 0 : k >= kSectors ? kSectors - 1 : k;
		_sector[(r < 0.5 ? 0 : kSectors) + k] = true;
	}

	void Oven::Crimp(double a_angle)
	{
		double a = a_angle;
		while (a < 0.0) {
			a += 2.0 * m::kPi;
		}
		while (a >= 2.0 * m::kPi) {
			a -= 2.0 * m::kPi;
		}
		int k = static_cast<int>(a / (2.0 * m::kPi) * kCrimps);
		_crimp[k < 0 ? 0 : k >= kCrimps ? kCrimps - 1 : k] = true;
	}

	double Oven::Coverage() const
	{
		int in = 0, out = 0;
		for (int k = 0; k < kSectors; ++k) {
			in += _sector[k];
			out += _sector[kSectors + k];
		}
		return 0.35 * in / kSectors + 0.65 * out / kSectors;
	}

	double Oven::Crimped() const
	{
		int n = 0;
		for (bool c : _crimp) {
			n += c;
		}
		return static_cast<double>(n) / kCrimps;
	}

	void Oven::Step(double a_dt, double a_fireHeat)
	{
		if (_door) {
			_temp = Lag(_temp, a_fireHeat * 0.45, 3.0, a_dt);  // the heat pours out of the open mouth
		} else {
			_temp = Lag(_temp, a_fireHeat, 9.0, a_dt);
		}
		if (!_inOven) {
			return;
		}
		_heat.Add(_temp, a_dt);
		_rise = m::Min(1.15, _rise + m::Max(0.0, _temp - 0.3) * 0.07 * a_dt);
		_brown += m::Max(0.0, _temp - 0.4) * 0.075 * a_dt * (0.4 + 0.6 * m::Min(1.0, _rise));
	}

	Result Oven::Score() const
	{
		Line   l[5];
		double w[5];
		const double dev = Window(_knead, 0.9, 1.3, 2.0, 2.0);
		const double knead = 0.6 * dev + 0.4 * Rhythm();
		l[0] = { "The dough", _knead < 0.6 ? "Barely worked" : _knead < 0.9 ? "Underworked" : _knead <= 1.3 ? (Rhythm() > 0.75 ? "Smooth and springy" : "Smooth, worked in fits") : "Overworked and tough",
			knead };
		w[0] = _dough ? 0.3 : -1.0;
		const double fill = 0.5 * Coverage() + 0.5 * Crimped();
		l[1] = { "The filling", Crimped() < 0.5 ? "The edge left open" : Pick3(fill, "Full to the edge, well sealed", "A bit thin in places", "Sparse"), fill };
		w[1] = _pie ? 0.15 : -1.0;
		l[2] = { "The oven", _heat.Verdict(), _heat.Score() };
		w[2] = -1.0;  // (the heat isn't judged: it shows in what it does to the dish)
		const double bake = Window(_brown, 0.85, 1.2, 4.0, 2.0) * (_rise < 0.75 ? 0.6 + _rise * 0.5 : 1.0);
		l[3] = { "The bake", _brown < 0.5 ? "Pale and doughy" : _brown < 0.85 ? "A shade underbaked" : _brown <= 1.2 ? "Golden brown" : "Dark on top", bake };
		w[3] = 0.4;
		l[4] = { "The salt", _salt.Verdict(), _salt.Score() };
		w[4] = _salt.wanted ? 0.1 : -1.0;
		return Grade(l, w, 5, 3, _brown > 1.6, "Left in the oven too long");
	}
}
