#pragma once

// Hearthcooking's kitchen: the fire, the pot, the spit and the oven, and how well a dish turned out.
//
// Plain C++ with no game or standard-library code (as Skyrim Yacht's dice), so the same rules run in the SKSE plugin
// and in the native tests. Units are game units, z up, the worktop at z = 0. The plugin draws what this says and feeds
// it the player's hands: chops, stirs, turns of the crank, kneading strokes.

#include "Math.h"

namespace hc
{
	enum class Flow
	{
		Pot,   // soups and stews: chop, tip the board into the pot, keep it simmering, stir, salt, ladle out
		Spit,  // roasts and grilled things: onto the spit, turn the crank, salt, take it off
		Oven   // baking: dough, knead, fill and crimp (pies), bake, pull it out
	};

	enum class Tier
	{
		Burnt = 0,
		Plain = 1,
		Hearty = 2,
		Masterwork = 3
	};

	const char* TierName(Tier a_tier);

	// ------------------------------------------------------------------ where things are
	namespace layout
	{
		// The worktop (wood, left) and the hearth slab (stone, right) meet at x = kHearthEdge
		constexpr double kTopW = 74.0, kTopD = 40.0, kHearthEdge = 5.0;

		// The cutting board: three lanes along y, an ingredient lies along x in each
		constexpr double kBoardX = -7.0, kBoardY = -1.0, kBoardW = 20.0, kBoardD = 12.5, kBoardH = 1.1;
		constexpr double kLaneY[3] = { -3.6, 0.0, 3.6 };  // board-local
		constexpr double kWholeLen = 9.0;                   // an ingredient laid on the board is this long

		// The fire (pot and spit): a ring of stones on the hearth slab
		constexpr double kFireX = 18.0, kFireY = 3.0, kFireR = 7.5;

		// The pot, on a trivet over the fire
		constexpr double kPotX = kFireX, kPotY = kFireY, kPotR = 6.2, kPotWall = 0.45;
		constexpr double kPotFloor = 3.4, kPotRim = 10.2, kLiquidZ = 7.6;

		// The spit: two posts either side of the fire, the rod along x
		constexpr double kSpitZ = 13.0, kSpitX0 = kFireX - 13.0, kSpitX1 = kFireX + 13.0;
		constexpr double kCrankOff = 16.1;  // the crank's arm, from the fire's middle along the rod (tools/make_meshes.py)
		constexpr double kCrankX = kSpitX1 + 3.0, kCrankR = 3.25;

		// The oven: a stone dome at the back right; its baking floor and mouth, the firebox under it
		constexpr double kOvenX = 18.0, kOvenY = 7.5, kOvenFloorZ = 9.0, kOvenMouthY = -1.2, kOvenMouthW = 10.0;
		constexpr double kFireboxZ = 2.6;

		// The rest of the room: the logs, the salt, the serving bowl / plate
		constexpr double kPileX = 31.5, kPileY = -7.0;
		constexpr double kSaltX = -11.0, kSaltY = -14.0;
		constexpr double kServeX = 11.0, kServeY = -14.0;
		constexpr double kRackX = -29.0;  // the ingredients waiting their turn, down the left side
	}

	// ------------------------------------------------------------------ the fire
	// Logs catch over a few seconds, burn down over about two minutes, and give less heat as they go. The heat follows
	// the logs slowly, so a change of fire takes a while to show in the pot.
	class Fire
	{
	public:
		static constexpr int    kMaxLogs = 5;
		static constexpr double kBurnTime = 120.0;

		// a_burn: how long a log burns (our version: 15 s, timing::kLogBurn)
		explicit Fire(double a_lag = 5.0, double a_burn = kBurnTime) :
			_lag(a_lag), _burn(a_burn) {}

		void   Light(int a_logs);  // fresh logs, already caught, the heat already there
		bool   AddLog();           // false if the fire is full
		bool   RemoveLog();        // the freshest log comes out; false if none
		void   Step(double a_dt);
		double Heat() const { return _heat; }    // 0 (out) .. about 1.3 (roaring)
		double Target() const;                   // what the heat is heading to
		int    Logs() const { return _count; }
		double Fuel(int a_i) const { return a_i < _count ? _log[a_i].fuel : 0.0; }
		double Glow(int a_i) const;              // how brightly a log burns, 0..1

	private:
		struct Log
		{
			double age = 0.0, fuel = 1.0;
		};
		Log    _log[kMaxLogs]{};
		int    _count = 0;
		double _heat = 0.0, _lag, _burn;
	};

	// ------------------------------------------------------------------ the scoring
	struct Line
	{
		const char* label = "";    // "The cuts"
		const char* verdict = "";  // "Even and neat"
		double      score = 0.0;   // 0..1
	};

	struct Result
	{
		Tier   tier = Tier::Plain;
		double quality = 0.0;  // 0..1, the weighted lines
		Line   lines[8]{};
		int    count = 0;
		const char* burntWhy = "";  // why it burnt, when it did
	};

	// How a stretch of cooking went in the heat: the share of the time the heat sat in the window.
	struct HeatLog
	{
		double lo = 0.45, hi = 0.8;
		double time = 0.0, inZone = 0.0, hot = 0.0, cold = 0.0;
		void   Add(double a_heat, double a_dt);
		double Score() const;
		const char* Verdict() const;
	};

	struct Salt
	{
		bool   wanted = false;  // the recipe has salt in it
		double amount = 0.0;    // pinches (one pinch is a good half-second hold)
		double Score() const;
		const char* Verdict() const;
	};

	// ------------------------------------------------------------------ cutting
	// An ingredient on the board, x0..x1 along its length and 0..width across it. Chops across split whichever piece
	// is under the knife; with the knife turned, chops along it split it lengthwise, so the two together dice it. The
	// pieces are scored for how even they are and how near the size the recipe wants, both ways.
	struct Cutting
	{
		static constexpr int kMaxCuts = 6;  // each way: six across, six along
		double x0 = 0.0, x1 = layout::kWholeLen;
		double width = 0.0;         // across it (0: not known, only the cuts across count)
		double target = 1.5;        // the piece size the recipe wants
		double cuts[kMaxCuts]{};    // where the knife has gone through across it, left to right
		int    cutCount = 0;
		double along[kMaxCuts]{};   // and along it, 0..width, front to back
		int    alongCount = 0;
		bool   finished = false;    // gone off the board

		// Where the knife came down: true if it cut (not too near an end or another cut)
		bool   Chop(double a_x);
		bool   ChopAlong(double a_y);
		int    Pieces() const { return cutCount + 1; }   // across
		int    Strips() const { return alongCount + 1; } // along
		void   Piece(int a_i, double& a_a, double& a_b) const;  // the i-th piece across, left to right
		void   Strip(int a_j, double& a_a, double& a_b) const;  // the j-th strip along, 0..width
		void   Finish() { finished = true; }
		double Score() const;
	};

	// ------------------------------------------------------------------ pieces in flight
	// The chopped pieces (and anything else that goes in whole) as little rounded bodies. On the board they ride with
	// it; once it tips past their grip they slide; whatever lands in the broth is in, whatever misses is lost.
	class Chunks
	{
	public:
		static constexpr int kMax = 256;

		enum class State
		{
			None,
			Board,  // lying on the board, moving with it
			Free,   // sliding, falling
			Pot,    // in the broth
			Lost    // on the worktop, the hearth or in the fire
		};

		struct Body
		{
			State  state = State::None;
			Vec3   p, v;
			Quat   q;
			Vec3   w;           // spin (world, rad/s)
			Vec3   size{ 1, 1, 1 };  // the piece's box (x along the cut)
			double r = 0.5;     // its collision radius
			int    ing = 0;     // which ingredient
			Vec3   local;       // board-local position while on the board
			double potA = 0.0, potR = 0.0, bob = 0.0;
			double cooked = 0.0;  // 0 raw .. 1 cooked (its colour)
			bool   charred = false;
			bool   aimed = false;  // thrown straight for the pot: nothing knocks it off course
		};

		struct Event
		{
			enum class Kind
			{
				Splash,
				Land,
				Tumble
			} kind;
			Vec3   at;
			double speed = 0.0;
		};

		// The board's pose: its middle, its tilt about y (positive tips the right side down), its own speed
		struct Board
		{
			Vec3   c{ layout::kBoardX, layout::kBoardY, layout::kBoardH * 0.5 };
			double tilt = 0.0;
			Vec3   v;
			Vec3   ToWorld(const Vec3& a_local) const;
			Vec3   ToLocal(const Vec3& a_world) const;
			Vec3   Up() const;
		};

		int  Add(int a_ing, const Vec3& a_boardLocal, const Vec3& a_size, unsigned a_seed);
		int  AddFree(int a_ing, const Vec3& a_at, const Vec3& a_vel, const Vec3& a_size, unsigned a_seed);
		void Step(double a_dt, const Board& a_board, double a_swirl);
		Body&       At(int a_i) { return _b[a_i]; }
		const Body& At(int a_i) const { return _b[a_i]; }
		int  Count() const { return _n; }
		int  CountIn(State a_s) const;
		int  CountFor(int a_ing, State a_s) const;
		void Clear() { _n = 0; }
		int  TakeEvents(Event* a_out, int a_max);  // since the last call
		void Cook(double a_amount);                // the pieces in the broth take colour
		// Where pieces go (the pot by default; a bowl at the side for a dumpling's apples): its middle at the
		// surface they rest on, its radius inside, its rim's height
		void SetBowl(const Vec3& a_c, double a_r, double a_rim, bool a_stirred);
		void Launch(int a_i, const Board& a_board);  // off the board now, on its way in

	private:
		void Push(Event::Kind a_kind, const Vec3& a_at, double a_speed);
		void Collide(Body& a_b, const Board& a_board, double a_dt);
		void Aim(Body& a_b, int a_i);  // an arc over the rim into the broth, wherever it starts

		Vec3   _bc{ layout::kPotX, layout::kPotY, layout::kLiquidZ };
		double _br = layout::kPotR, _rim = layout::kPotRim;
		bool   _stirred = true;
		Body  _b[kMax]{};
		int   _n = 0;
		Event _ev[64]{};
		int   _nev = 0;
	};

	// The board's pose as it's tipped into the pot: a_t 0 lying on the worktop .. 1 lifted to the rim and tipped right
	// over (the first half carries it across, the rest tips it); a_y is where along the pot it's held.
	Chunks::Board BoardPose(double a_t, double a_y);

	// ------------------------------------------------------------------ the pot
	class Pot
	{
	public:
		explicit Pot(bool a_salted = false);

		void   Stir(double a_dAngle);     // the ladle went this far round (radians, either way)
		void   AddPieces(int a_before, int a_new);  // fresh raw pieces dilute how done the whole pot is
		void   Step(double a_dt, double a_fireHeat, bool a_hasFood);
		double Temp() const { return _temp; }
		double Cooked() const { return _cook; }
		void   SetCooked(double a_c) { _cook = a_c; }  // our version: the timer drives how done it looks
		double Burn() const { return _burn; }
		double StirNow() const;            // 0..1, how well it's being stirred right now
		double Swirl() const { return _swirl; }  // the broth turning (rad/s), for the pieces in it
		bool   Bubbling() const { return _temp > 0.42; }
		bool   Boiling() const { return _temp > 0.82; }
		Salt&  Seasoning() { return _salt; }
		const HeatLog& Heat() const { return _heat; }

		// Pieces cut, pieces in, pieces lost; the cutting scores (one per chopped ingredient)
		Result Score(const double* a_cuts, int a_cutCount, int a_piecesIn, int a_piecesLost) const;

	private:
		double  _temp = 0.15, _cook = 0.0, _burn = 0.0;
		double  _stir = 0.0, _swirl = 0.0, _stirTime = 0.0, _cookTime = 0.0;
		HeatLog _heat{ 0.45, 0.8 };
		Salt    _salt;
	};

	// ------------------------------------------------------------------ the spit
	class Spit
	{
	public:
		static constexpr int kBands = 12;  // the sides of the meat, all the way round

		explicit Spit(bool a_salted = false);

		void   Turn(double a_dAngle);       // the crank turned (radians)
		void   Step(double a_dt, double a_fireHeat, bool a_mounted);
		double Angle() const { return _angle; }
		double Band(int a_i) const { return _band[a_i]; }
		double Mean() const;
		double Spread() const;  // how uneven the sides are (standard deviation)
		int    Charred() const;
		Salt&  Seasoning() { return _salt; }
		const HeatLog& Heat() const { return _heat; }
		Result Score() const;
		void   SetMean(double a_m);  // our version: the timer drives how done it looks; the sides keep their spread

	private:
		double  _angle = 0.0;
		double  _band[kBands]{};
		HeatLog _heat{ 0.45, 0.85 };
		Salt    _salt;
	};

	// ------------------------------------------------------------------ the oven
	class Oven
	{
	public:
		static constexpr int kSectors = 8;   // where the filling lies, round the pie
		static constexpr int kCrimps = 24;   // the edge, pinched a bit at a time

		explicit Oven(bool a_pie = false, bool a_salted = false, bool a_dough = true);

		// The dough: a stroke of the heel of the hand, this long (units), this many seconds after the last
		void   Knead(double a_length, double a_now);
		double Dough() const { return _knead; }
		void   SetKneadScale(double a_s) { _kneadScale = a_s > 0.1 ? a_s : 1.0; }  // our version: a bigger dough takes more working
		double Rhythm() const { return _strokes ? _rhythm / _strokes : 0.0; }
		int    Strokes() const { return _strokes; }

		// The pie: filling dropped at a point on the dough (disk-local, radius 1 = the edge); a pinch of the rim
		void   Fill(double a_x, double a_y);
		void   Crimp(double a_angle);
		double Coverage() const;
		double Crimped() const;
		bool   CrimpedAt(int a_k) const { return _crimp[a_k]; }
		bool   Pie() const { return _pie; }
		bool   HasDough() const { return _dough; }

		void   ResetShape()  // our version: the next piece cut from the batch's dough, to fill and close afresh
		{
			for (auto& s : _sector) {
				s = false;
			}
			for (auto& c : _crimp) {
				c = false;
			}
		}
		void   Load() { _inOven = true; }
		void   Unload() { _inOven = false; _out = true; }
		void   SetDoor(bool a_open) { if (a_open && !_door) ++_peeks; _door = a_open; }
		bool   DoorOpen() const { return _door; }
		bool   InOven() const { return _inOven; }
		bool   Out() const { return _out; }
		void   Step(double a_dt, double a_fireHeat);
		double Temp() const { return _temp; }
		double Rise() const { return _rise; }
		double Brown() const { return _brown; }
		void   SetBrown(double a_b) { _brown = a_b; _rise = m::Min(1.15, m::Max(_rise, a_b * 1.1)); }  // our version
		int    Peeks() const { return _peeks; }
		Salt&  Seasoning() { return _salt; }
		const HeatLog& Heat() const { return _heat; }
		Result Score() const;

	private:
		bool    _pie, _dough;
		double  _knead = 0.0, _rhythm = 0.0, _last = -100.0, _kneadScale = 1.0;
		int     _strokes = 0;
		bool    _sector[kSectors * 2]{};  // two rings
		bool    _crimp[kCrimps]{};
		bool    _inOven = false, _out = false, _door = true;
		int     _peeks = 0;
		double  _temp = 0.2, _rise = 0.0, _brown = 0.0;
		HeatLog _heat{ 0.5, 0.85 };
		Salt    _salt;
	};

	// Puts the weighted lines together into a tier. Lines with a negative weight are left out (and the rest share it).
	// a_key is the line that says whether it's done at all: under 0.4 the dish can be no better than Plain, under 0.97
	// no better than Hearty, however well the rest went.
	Result Grade(const Line* a_lines, const double* a_weights, int a_count, int a_key, bool a_burnt, const char* a_burntWhy);
}
