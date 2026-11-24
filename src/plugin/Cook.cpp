// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Cook.h"

#include "Audio.h"
#include "Config.h"
#include <unordered_set>
#include "Kit.h"
#include "Overlay.h"
#include "Recipes.h"
#include "Scene.h"
#include "Ui.h"

#include "../sim/Kitchen.h"
#include "../sim/Timing.h"

namespace Cook
{
	namespace
	{
		using hc::Quat;
		using hc::Vec3;
		namespace L = hc::layout;
		using namespace Ui;
		using Recipes::Role;

		constexpr double kPi = 3.14159265358979;
		constexpr double kCarryZ = 9.0;    // how high things are carried
		constexpr double kBoardTop = L::kBoardH;
		constexpr int    kMaxUnits = 28;  // (our version: the ingredients for up to 4 or 5 portions)

		// ---------------------------------------------------------------- small helpers
		double Clamp(double v, double a, double b) { return v < a ? a : v > b ? b : v; }
		double Smooth(double t)
		{
			t = Clamp(t, 0.0, 1.0);
			return t * t * (3.0 - 2.0 * t);
		}
		float Rand01() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); }
		double Wrap(double a)
		{
			while (a > kPi) {
				a -= 2 * kPi;
			}
			while (a < -kPi) {
				a += 2 * kPi;
			}
			return a;
		}
		float Hash(int a_k, int a_salt = 0)
		{
			unsigned h = static_cast<unsigned>(a_k) * 0x9E3779B1u + static_cast<unsigned>(a_salt) * 0x85EBCA77u;
			h ^= h >> 15;
			h *= 0x2C1B3C6Du;
			h ^= h >> 12;
			return (h & 0xFFFFFF) / 16777216.0f;
		}
		Quat RotX(double a) { return Quat::FromAxisAngle({ 1, 0, 0 }, a); }
		Quat RotY(double a) { return Quat::FromAxisAngle({ 0, 1, 0 }, a); }
		Quat RotZ(double a) { return Quat::FromAxisAngle({ 0, 0, 1 }, a); }

		// A rotation from its matrix (rows)
		Quat FromRows(const double m[3][3])
		{
			Quat         q;
			const double tr = m[0][0] + m[1][1] + m[2][2];
			if (tr > 0) {
				const double s = std::sqrt(tr + 1.0) * 2;
				q = { 0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s };
			} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
				const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
				q = { (m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s };
			} else if (m[1][1] > m[2][2]) {
				const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
				q = { (m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s };
			} else {
				const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
				q = { (m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s };
			}
			q.Normalize();
			return q;
		}

		// ---------------------------------------------------------------- state
		enum class Phase
		{
			Idle,
			Book,
			Cooking,
			Result
		};

		enum class Hold
		{
			None,
			Ingredient,  // carrying something from the cloth
			Log,         // a log from the pile
			FireLog,     // a burning log pulled out
			Board,       // tipping the board into the pot
			Ladle,       // stirring
			LadleOut,    // the ladle lifted out, to the bowl
			Salt,        // a pinch of salt
			Crank,       // turning the spit
			Meat,        // the roast lifted off the spit
			Dough,       // kneading, or carrying the dough to the oven
			Spread,      // spreading the filling
			Crimp,       // pinching the edge
			Door,        // the oven door
			Bread,       // taking it out of the oven
			Peel,        // the peel: to the dough to slide it on, then into the oven
			Mix,         // stirring the flour into the water
			Ball,        // the ball of dough, out of the bowl onto the board
			Apples,      // the bowl of cut filling, over the dough
			Roll,        // the rolling pin pressed and rolled over the dough
			Sauce        // a sauce's bowl of liquids, lifted to pour into the pot
		};

		struct Unit
		{
			int                       ing = 0;
			Role                      role = Role::Toss;
			std::string               name, model;
			std::string               key;  // name and model words, lower case: what it's known by (see Recipes::MatchKey)
			Scene::Id                 prop = -1;
			std::vector<Scene::Id>    segs, cores;  // on the board: its own mesh cut to each piece, and its solid inside
			enum class At
			{
				Cloth,
				Held,
				Board,
				Pot,
				Spit,
				Dough,
				Gone
			} at = At::Cloth;
			bool                      fitted = false;
			Quat                      lie;          // lays it along x, flat
			Vec3                      center;       // its middle, in its own space
			double                    dims[3]{ 6, 2, 2 };  // long, middle, short (its own size)
			Vec3                      clothAt;
			int                       lane = -1;
			hc::Cutting               cut;
			double                    boardLen = 8.0;
			double                    spitX = 0.0;
			bool                      wentWhole = false;  // into the pot without being cut
			bool                      thin = false;       // a leek, a carrot: only ever cut across
			bool                      dice = false;       // the oven: cut on the board first, then into the bowl
			bool                      byBowl = false;     // a sauce's liquid poured into its bowl: the bottle stood beside it
			bool                      bulb = false;       // a head of garlic: broken into its cloves (a knock of the knife) before it's diced
			hc::timing::Piece         timer;              // our version: a meat on the spit cooks to its own time
			int                       dish = 0;           // which portion it's for
			int                       servedSlot = -1;    // the plate it went onto
		};

		// A piece: the ingredient's own mesh cut to a box (x along its length, y across, z up; about its middle, at the
		// board's scale; whole where cut is false), its body's middle at mid in the same frame
		struct Piece
		{
			Scene::Id prop = -1;
			Scene::Id core = -1;  // its solid inside: a smooth egg of flesh the thing's size, cut the same
			int       unit = 0;
			double    box[6]{};  // x0 x1 y0 y1 z0 z1
			Vec3      mid;
			bool      cut = false;
			bool      onDough = false;  // the oven: set on the dough, here (from its middle)
			Vec3      doughOff;
			double    turn = 0.0;
			Scene::Id alt = -1;  // a leek's piece of our own solid leek, waiting: it takes over once it lands in the pot
		};

		struct Grain
		{
			Vec3   p, v;
			double life = 1.0, age = 0.0;
			float  col[4]{ 1, 1, 1, 1 };
			float  size = 0.12f;
			Render3D::Sprite::Kind kind = Render3D::Sprite::Kind::Grain;
			bool   gravity = true;
		};

		struct Props
		{
			Scene::Id kitchen = -1, room = -1, cloth = -1, saltPile = -1, board = -1, knife = -1, salt = -1, firepit = -1, pot = -1, broth = -1, ladle = -1, ladleSoup = -1, ladleStew = -1, sauceBowl = -1, sauceIn = -1, pin = -1, eggA = -1, eggB = -1, yolk = -1, eggWhite = -1, flourHeap = -1, spoon = -1, bowl = -1, posts = -1, rod = -1,
					  plate = -1, ovenBase = -1, ovenDome = -1, door = -1, peel = -1, dough = -1, disk = -1, result = -1, heldLog = -1,
					  mixBowl = -1, mixWater = -1, mixBall = -1, fillBowl = -1, slab = -1, dishRef = -1;
			Scene::Id fireLogs[hc::Fire::kMaxLogs]{ -1, -1, -1, -1, -1 };
			Scene::Id pile[3]{ -1, -1, -1 };
			std::vector<Scene::Id> butter;  // butter cut into the flour
			std::vector<Scene::Id> extraServe;  // our version: a bowl or plate per portion after the first
			std::vector<Scene::Id> slotDish;    // and the stew ladled into each
			std::vector<Scene::Id> doughPieces; // the big dough cut up
			std::vector<Scene::Id> crimps, pieRim, galette, leafBits, dumpFolds;  // (a dumpling's folds; a pie's fluted rim, a scallop a pinch; a crostata's eight pleats)
			std::vector<std::pair<Scene::Id, Vec3>> fill;  // the filling on the pie (disk-local, units)
			std::array<bool, static_cast<int>(Kit::Role::Count)> game{};  // this one is the game's own mesh (fitted), not ours
			std::vector<std::pair<Scene::Id, int>> decor;                   // the clutter, and its spot
		};

		// Something of the game's, fitted into the space the kitchen gives it
		struct Fitting
		{
			Scene::Id  id = -1;
			Kit::Role  role = Kit::Role::Count;
			Vec3       at;        // where its base's middle goes
			Vec3       size;      // the space (0: any)
			double     rotZ = 0;
			bool       stretch = false;
		};
		std::vector<Fitting> g_fits;

		std::recursive_mutex g_mtx;
		Phase                g_phase = Phase::Idle;
		bool                 g_ovenStation = false;
		bool                 g_quick = false;  // Quick Cook was chosen: let the game's menu be
		std::vector<const Recipes::Recipe*> g_book;
		int                  g_sel = 0;
		const Recipes::Recipe* g_recipe = nullptr;
		hc::Flow             g_flow = hc::Flow::Pot;
		double               g_clock = 0.0;
		std::string          g_toast;
		double               g_toastAt = -10.0;
		bool                 g_help = false, g_confirm = false, g_soundBox = false;  // (g_soundBox: M's panel of sliders)

		// the dish on the go
		std::vector<Unit>        g_units;
		std::vector<Piece>       g_pieces;
		hc::Chunks               g_chunks;
		hc::Fire                 g_fire{ 5.0 };
		std::unique_ptr<hc::Pot>  g_pot;
		std::unique_ptr<hc::Spit> g_spit;
		std::unique_ptr<hc::Oven> g_oven;
		Props                    P;

		// The floor of the oven's baking chamber: the game's oven has it higher, at its arched mouth (its lower
		// opening is the firebox)
		double OvenFloor() { return L::kOvenFloorZ + (P.game[static_cast<int>(Kit::Role::Oven)] ? 3.6 : 0.0); }
		std::vector<Grain>       g_grains;
		float                    g_broth[3]{ 0.6f, 0.42f, 0.22f };
		int                      g_tossedIn = 0;

		// hands
		Hold   g_hold = Hold::None;
		int    g_holdUnit = -1;
		Vec3   g_carry;              // where the held thing is
		double g_grabX = 0, g_grabY = 0;
		double g_tip = 0, g_tipWant = 0, g_boardY = L::kBoardY;
		Vec3   g_boardCarry;  // the oven: where the lifted board's middle is
		double g_boardLift = 0;  // the oven: 0 down on the table .. 1 lifted
		bool   g_tipped = false;     // the board's last lot has gone over its edge
		hc::Chunks::Board g_boardPose, g_boardPrev;
		double g_knifeX = L::kBoardX, g_knifeY = L::kBoardY, g_knifeAnim = -1;
		bool   g_knifeOn = false, g_knifeCut = false;
		bool   g_knifeAlong = false;  // turned to cut along the food (the second way, for dice)
		int    g_knifeLane = -1;
		double g_ladleA = -kPi * 0.6, g_ladleR = 3.6, g_lastStir = 0, g_stirred = 0;
		Vec3   g_ladleOutAt;
		double g_crankPrev = 0, g_crankRun = 0;
		double g_pinch = 0;
		double g_door = 1.0, g_doorWant = 1.0;  // the oven door, 0 shut .. 1 open
		bool   g_doorShut = false, g_doorReopened = false;  // (for the card) shut on the bake, opened again after
		Vec3   g_doughAt{ L::kBoardX, L::kBoardY, kBoardTop };
		// the oven's bowls, behind the board: water and flour stirred to a dough; the cut filling waiting
		const Vec3 kMixAt{ -11.0, 11.5, 0.0 }, kFillAt{ -1.0, 11.5, 0.0 };
		bool   g_pinHeld = false;  // the rolling pin taken up (it follows the pointer; held down, it rolls)
		Vec3   g_pinAt{};
		double g_pinRoll = 0.0;   // how far it's rolled (its turn)
		double g_rollDX = 1.0, g_rollDY = 0.0;  // which way it's being rolled, smoothed
		double g_pinAxis = 0.0;   // which way it rolls: 0 left-right, 1 up-down (snapped, with a margin), and as drawn
		double g_pinTurn = 0.0;
		double g_pushAcc = 0.0;          // kneading or rolling in one hold: how far it's moved since the last push counted
		Vec3   g_doughLast{};
		bool   g_doughLastOk = false;
		// A sauce (a basting sauce): its liquids poured into a bowl first, one after another, then the bowl into the pot
		const Vec3 kSauceAt{ -4.0, 11.5, 0.0 };
		int    g_sauceN = 0;     // in the bowl so far
		double g_sauceTip = 0.0;
		double g_eggT = -1.0;  // an egg cracked over the mixing bowl: time since (-1: none yet)
		bool SauceBowl();
		bool NearScreen(const Vec3& a_p, float a_mx, float a_my, double a_r, float a_minPx);
		// A hiss at the fire, no more than one every three seconds.
		void Hiss(float a_vol, float a_pan = 0.0f)
		{
			static double s_last = -10.0;
			if (Audio::Now() - s_last >= 3.0) {
				s_last = Audio::Now();
				Audio::Take("hiss_", a_vol, 0.1f, a_pan);
			}
		}

		constexpr double kPieRim = 0.9;  // a crimped pie's rim, scaled to the size it comes out of the oven at
		void ArrangeCloth();
		// how far round the spoon goes before the next stir is heard: half a turn, a turn, a turn and a half or two, at random
		double g_stirGap = kPi;
		void   NextStirGap() { g_stirGap = kPi * (1 + static_cast<int>(Rand01() * 4.0) % 4); }
		int g_campfire = -1;  // which campfire recording plays at the pot and spit (picked once per cook)

		// The fire's loop: the oven's own roar in its box, a campfire anywhere else (one of two recordings).
		void FireLoop(double a_vol)
		{
			if (g_campfire < 0) {
				g_campfire = Rand01() < 0.5 ? 0 : 1;
			}
			const char* names[3] = { "loop_fire", "loop_fire2", "loop_oven" };
			const int on = g_flow == hc::Flow::Oven ? 2 : g_campfire;
			for (int k = 0; k < 3; ++k) {
				Audio::SetLoop(names[k], k == on ? static_cast<float>(a_vol) : 0.0f);
			}
		}
		double g_spin = 0.0;  // how far the pot's stew has turned with the stirring (keeps going one way, never jumps back)
		double g_mix = 0.0, g_mixA = 0.0;  // how far the flour is stirred in (1: a ball), the spoon's angle
		bool   g_bowlWater = true;         // a bowl of water to start (else the milk is the wet, and goes in last)
		bool   g_milkIn = false;
		// a topped bread (garlic bread): no dough - the bread itself is cut, buttered and topped
		bool   g_topping = false, g_buttered = false;
		int    BreadUnit();
		bool   BreadCut();
		bool   BreadOnBoard();
		// where the oven's cut things go: a pie's into a bowl of their own, a bread's into the dough's bowl
		// A bread's cut things (potato bread's potato): into the right bowl, then poured from it into the mixing bowl
		bool g_cutInMix = false;
		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}
		// what a unit is known by: its name and its model's words, so a translated name still matches
		const std::string& KeyOf(const Unit& a_u) { return a_u.key.empty() ? a_u.name : a_u.key; }
		// an ingredient's key from its (shown) name
		std::string IngKey(const std::string& a_name)
		{
			if (g_recipe) {
				for (const auto& in : g_recipe->ings) {
					if (in.name == a_name) {
						return in.key;
					}
				}
			}
			return a_name;
		}

		bool Crostata()
		{
			std::string n = g_recipe ? g_recipe->key : std::string();
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return n.find("crostata") != std::string::npos;
		}
		bool BreadCuts() { return g_recipe && g_recipe->oven && !g_recipe->pie && !g_topping; }
		Vec3 CutBowl() { return g_recipe && (g_recipe->pie || g_topping || !g_cutInMix) ? kFillAt : kMixAt; }
		Vec3 PourAt() { return BreadCuts() ? kMixAt : g_doughAt; }  // where the right bowl tips toward
		bool CutsWaiting();  // a bread's cut things still in the right bowl
		void DoughPush(double a_len);  // a push of the dough: kneaded, or rolled out
		bool   g_ball = false;             // a ball of dough in the bowl
		bool   g_doughOnBoard = false;     // the dough out on the board (kneaded, filled, folded there)
		// a dumpling isn't crimped: its four sides are folded up over the filling, one at a time
		double g_fold[4]{}, g_foldWant[4]{};
		double g_flareAt = -10.0, g_flareX = 0.0, g_dripNext = 0.0;
		// the spit's fat dripping into the fire, and its flare-up (above); pieces of leek in the pot drawn from our own solid
		// leek (along x, 16 long, its middle at 0)
		std::unordered_set<Scene::Id> g_ownLeek;
		constexpr double kOwnLeekThin = 0.6;  // (ours across, to the game's: its stalk a sixteenth of its length across, as the game's is)
		double g_flat = 0.0;                 // a dumpling's dough rolled out square (0 a ball .. 1 flat)
		double g_bowlTip = 0.0;              // the fruit bowl lifted and tipped over the dough
		bool   g_bowlPoured = false;
		int    g_foldSide = -1;
		bool   Dumpling()
		{
			if (!g_recipe || !g_recipe->pie) {
				return false;
			}
			std::string n = g_recipe->key;
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return n.find("dumpling") != std::string::npos;
		}
		int    g_doughParts = 0, g_doughNeeded = 0;
		bool   g_rolled = false;
		double g_squash = 0, g_squashA = 0, g_strokeLen = 0;
		int    g_stock = 0;
		int    g_lastFill = 0;          // the filling being spread
		Vec3   g_lastSpread;
		double g_peelT = -1;        // the peel's trip into or out of the oven
		// how many times faster than real each cooks
		constexpr double kPotSpeed = 2.0, kSpitSpeed = 1.5, kOvenSpeed = 2.0;
		Vec3   g_peelAt;            // the peel in the hand
		Vec3   g_peelFrom;          // where its trip into the oven starts
		bool   g_peelLoaded = false;  // the dough on it
		bool   g_peelBread = false;   // the bake on it, out of the oven
		double g_peelDwell = -1.0;    // when the empty peel went into the open oven's mouth (-1: it isn't there)
		bool   g_peelIn = true;
		bool   g_served = false;
		double g_serveAt = 0;
		hc::Result g_result;

		// ---- our version (DISENO.md): the dish's timer, the station's firewood
		hc::timing::Piece g_timer;
		int    g_stationLogs = 0;   // the station's own logs left (2..5 at the start)
		int    g_invLogsUsed = 0;   // firewood from the player's pack put on the fire (taken when the dish is served)
		int    g_portions = 1;      // how many dishes at once (the recipe book's slider)
		struct Slot
		{
			int      count = 0;  // what's on it (meats; a ladle of stew)
			hc::Tier tier = hc::Tier::Masterwork;
			bool     raw = false;
		};
		std::vector<Slot> g_slots;  // a bowl or plate per portion, along the front of the table
		// the oven's batch: the dough cut into pieces, each baking to its own time by its size
		std::vector<double>            g_cuts;      // where the knife went, 0..1 along the dough
		bool                           g_batchCut = false;
		std::vector<hc::timing::Piece> g_batch;
		std::vector<std::pair<hc::Tier, bool>> g_portionTiers;  // each portion as it came off the fire (tier, raw)

		RE::TESBoundObject* Firewood() { return RE::TESForm::LookupByID<RE::TESBoundObject>(0x0006F993); }  // Skyrim.esm Firewood01

		int PackLogs()
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			auto* wood = Firewood();
			if (!pc || !wood) {
				return 0;
			}
			const auto counts = pc->GetInventoryCounts([wood](RE::TESBoundObject& a_o) { return &a_o == wood; });
			const auto it = counts.find(wood);
			return it == counts.end() ? 0 : (std::max)(0, it->second);
		}

		int LogsLeft() { return g_stationLogs + (std::max)(0, PackLogs() - g_invLogsUsed); }

		void UseLog()
		{
			if (g_stationLogs > 0) {
				--g_stationLogs;
			} else {
				++g_invLogsUsed;
			}
		}

		void TakePackLogs()
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			auto* wood = Firewood();
			const int n = (std::min)(g_invLogsUsed, PackLogs());
			if (pc && wood && n > 0) {
				pc->RemoveItem(wood, n, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
			}
			g_invLogsUsed = 0;
		}

		bool HasWord(const std::string& a_key, std::initializer_list<const char*> a_words)
		{
			for (const char* w : a_words) {
				if (a_key.find(w) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// The dish's time with one log burning (DISENO.md section 3)
		double BaseTime(const Recipes::Recipe& a_r)
		{
			switch (a_r.flow) {
			case hc::Flow::Spit:
				return 15.0;
			case hc::Flow::Oven:
				if (a_r.pie) {
					return 22.0;
				}
				return HasWord(a_r.key, { "sweet", "roll", "cake", "tart", "cookie", "muffin", "bun" }) ? 12.0 : 17.0;
			default:
				return 20.0;
			}
		}

		// How done the dish looks for the timer: the station's own scale, raw .. its gold window (lo..hi) at T..T+4
		// .. as burnt as the station shows it at T+7, and on past that
		int ServeCount() { return g_recipe ? (std::max)(1, g_portions) : 1; }
		bool FlourRecipe();
		bool NeedCut() { return g_recipe && g_flow == hc::Flow::Oven && ServeCount() > 1 && !g_batchCut && FlourRecipe(); }

		void Toast(std::string a_text);

		// The cuts made: the pieces' sizes and their times (an even piece takes the recipe's time; a small one less,
		// a big one more: DISENO.md 5)
		void FinishCuts()
		{
			std::vector<double> c = g_cuts;
			std::sort(c.begin(), c.end());
			c.insert(c.begin(), 0.0);
			c.push_back(1.0);
			const int    n = ServeCount();
			const double ideal = 1.0 / n;
			g_batch.assign(n, hc::timing::Piece{});
			std::string sizes;
			for (int i = 0; i < n; ++i) {
				const double size = c[i + 1] - c[i];
				g_batch[i].time = hc::timing::PieceTime(g_timer.time, size, ideal);
				sizes += fmt::format("{}{:.0f}%", i ? "  " : "", size * 100.0);
			}
			g_batchCut = true;
			Toast(fmt::format("Pieces: {}", sizes));
			spdlog::info("Dough cut in {}: {}", n, sizes);
		}

		// Where portion k's bowl or plate stands: in a row along the front, centred on the game's own place for it
		Vec3 SlotAt(int a_k)
		{
			const int    n = ServeCount();
			const double gap = g_flow == hc::Flow::Pot ? 8.5 : 10.5;
			return { L::kServeX + (a_k - (n - 1) * 0.5) * gap, L::kServeY, 0.0 };
		}

		// How many go on one plate: the spit's meats per dish; a ladle per bowl
		int PerSlot()
		{
			if (g_flow != hc::Flow::Spit) {
				return 1;
			}
			int meats = 0;
			if (g_recipe) {
				for (const auto& in : g_recipe->ings) {
					meats += in.role == Recipes::Role::Meat ? in.count : 0;
				}
			}
			return (std::max)(1, meats);
		}

		void FillSlot(int a_k, hc::Tier a_t, bool a_raw)
		{
			auto& sl = g_slots[a_k];
			if (sl.count == 0 || a_t < sl.tier || (a_t == sl.tier && a_raw)) {
				sl.tier = a_t;
				sl.raw = a_raw;
			}
			++sl.count;
		}

		bool AllServed()
		{
			for (const auto& sl : g_slots) {
				if (sl.count < PerSlot()) {
					return false;
				}
			}
			return !g_slots.empty();
		}

		const char* TierWord(hc::Tier a_t, bool a_raw) { return a_raw ? "Raw" : hc::TierName(a_t); }
		ImU32 TierColour(hc::Tier a_t, bool a_raw)
		{
			return a_raw ? IM_COL32(200, 160, 150, 255) : a_t == hc::Tier::Masterwork ? IM_COL32(255, 214, 120, 255) : a_t == hc::Tier::Hearty ? IM_COL32(222, 196, 140, 255) :
			       a_t == hc::Tier::Plain ? IM_COL32(241, 238, 232, 255) : IM_COL32(230, 110, 90, 255);
		}

		// How many at once: no more than the ingredients carried; the spit takes 4 meats, the pot 5 bowls, the oven 4
		// pieces of the dough
		int MaxPortions(const Recipes::Recipe& a_r)
		{
			int most = 1;
			if (a_r.flow == hc::Flow::Spit) {
				int meats = 0;
				for (const auto& in : a_r.ings) {
					meats += in.role == Recipes::Role::Meat ? in.count : 0;
				}
				most = (std::max)(1, hc::timing::kSpitMax / (std::max)(1, meats));
			} else if (a_r.flow == hc::Flow::Pot) {
				most = hc::timing::kPotMax;
			} else {
				most = hc::timing::kOvenMax;
			}
			return std::clamp((std::min)(most, Recipes::CanMake(a_r)), 1, most);
		}

		double TimerLookOf(const hc::timing::Piece& a_p, double a_lo, double a_hi, double a_burnt);
		double TimerLook(double a_lo, double a_hi, double a_burnt) { return TimerLookOf(g_timer, a_lo, a_hi, a_burnt); }

		double TimerLookOf(const hc::timing::Piece& a_p, double a_lo, double a_hi, double a_burnt)
		{
			const double c = a_p.cooked, T = a_p.time;
			if (T <= 0.0) {
				return 0.0;
			}
			if (c <= T) {
				return a_lo * c / T;
			}
			const double over = c - T;
			if (over <= hc::timing::kHeartyEnd) {
				return a_lo + (a_hi - a_lo) * over / hc::timing::kHeartyEnd;
			}
			const double span = hc::timing::kPlainEnd - hc::timing::kHeartyEnd;
			return a_hi + (a_burnt - a_hi) * (over - hc::timing::kHeartyEnd) / span;
		}

		RE::TESBoundObject* g_given = nullptr;
		std::string g_givenName;
		bool   g_burntLook = false;

		// UI -> kitchen
		enum class Act
		{
			Select,
			CookByHand,
			QuickCook,
			Leave,
			Abandon,
			AbandonYes,
			AbandonNo,
			Again,
			Done,
			Help,
			Mute,
			Knife,
			Portions,
			Cut,
			CutUndo
		};
		bool board_kneading();
		void NextKnife();
		Scene::Id AddResult();
		// some bakes a touch bigger than the rest (a braided bread is long and thin, and read small; a plain loaf too)
		double BakeSize()
		{
			std::string n = g_recipe ? g_recipe->key : std::string();
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (n.find("dumpling") != std::string::npos) {
				return 5.1;  // a little parcel
			}
			return n.find("braided") != std::string::npos || n == "bread" || n.find("potato bread") != std::string::npos ? 6.6 : 6.0;
		}
		bool MenuBtnInk(const char* a_id, const std::string& a_label, float a_cx, float a_y, bool a_enabled);
		std::vector<std::pair<Act, int>> g_acts;
		std::mutex                       g_actMtx;
		void Post(Act a_act, int a_arg = 0)
		{
			std::lock_guard lk(g_actMtx);
			g_acts.emplace_back(a_act, a_arg);
		}

		void Toast(std::string a_text)
		{
			g_toast = std::move(a_text);
			g_toastAt = g_clock;
		}

		RE::GPtr<RE::CraftingMenu> Crafting()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui ? ui->GetMenu<RE::CraftingMenu>() : RE::GPtr<RE::CraftingMenu>();
		}

		void ShowCrafting(bool a_show)
		{
			if (auto m = Crafting(); m && m->uiMovie) {
				m->uiMovie->SetVisible(a_show);
			}
		}

		// ---------------------------------------------------------------- the kitchen's furniture
		Scene::Id Add(const char* a_name)
		{
			const auto id = Scene::Add(std::string("Hearthcooking\\") + a_name + ".nif");
			if (id < 0) {
				spdlog::warn("Kitchen: {} didn't load", a_name);
			}
			return id;
		}

		void Put(Scene::Id a_id, const Vec3& a_at, double a_rotZ = 0.0, float a_scale = 1.0f)
		{
			Scene::Place(a_id, a_at, RotZ(a_rotZ), a_scale);
		}

		// the rolled-out dough's look: the dough's own (not a plain hull's), pale and soft
		void DoughLook(Scene::Look& a_lk)
		{
			if (auto* sk = P.dough >= 0 ? Scene::Skin(P.dough) : nullptr) {
				a_lk.skin = sk;
				a_lk.uv[0] = a_lk.uv[1] = 0.35f;
			}
			a_lk.tint[0] = 1.0f, a_lk.tint[1] = 0.96f, a_lk.tint[2] = 0.88f;
			a_lk.lift[0] = 0.95f, a_lk.lift[1] = 0.88f, a_lk.lift[2] = 0.72f, a_lk.lift[3] = 0.25f;
		}

		// how far the finished stew shows in the pot over its pieces (0 .. 1), with its doneness
		double StewFade()
		{
			if (!g_pot || g_flow != hc::Flow::Pot || g_chunks.CountIn(hc::Chunks::State::Pot) + g_tossedIn == 0) {
				return 0.0;
			}
			const double k = Clamp((g_pot->Cooked() - 0.5) / (0.95 - 0.5), 0.0, 1.0)  /* (from a quarter along the meter: 0.5 of 2; whole by ready) */;
			return k * k * (3.0 - 2.0 * k);
		}

		Vec3 FireCenter()
		{
			// the oven's fire burns inside it, at the back of the dome (the bread bakes at the front)
			return g_flow == hc::Flow::Oven ? Vec3{ L::kOvenX, L::kOvenY + 2.5, OvenFloor() + 0.5 } : Vec3{ L::kFireX, L::kFireY, 0.6 };
		}

		// ---- the game's meshes: how one lies (longest side along x, shortest up) and fitting one into a space
		struct Lie
		{
			Quat   q;
			Vec3   c;          // its middle, in its own space
			double d[3]{ 1, 1, 1 };  // long, middle, short
		};

		bool LieOf(Scene::Id a_id, Lie& a_out)
		{
			Vec3 mn, mx;
			if (a_id < 0 || !Scene::Bounds(a_id, mn, mx)) {
				return false;
			}
			const double e[3] = { mx.x - mn.x, mx.y - mn.y, mx.z - mn.z };
			int          ord[3] = { 0, 1, 2 };
			std::sort(ord, ord + 3, [&](int a, int b) { return e[a] > e[b]; });
			double m[3][3]{};
			for (int r = 0; r < 3; ++r) {
				m[r][ord[r]] = 1.0;
			}
			const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			                   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
			if (det < 0) {
				for (auto& v : m[1]) {
					v = -v;
				}
			}
			a_out.q = FromRows(m);
			a_out.c = { (mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, (mn.z + mx.z) * 0.5 };
			for (int k = 0; k < 3; ++k) {
				a_out.d[k] = (std::max)(0.05, e[ord[k]]);
			}
			return true;
		}

		struct Pose
		{
			Vec3  pos;
			Quat  q;
			float s = 1.0f;
			float w[3]{ 1, 1, 1 };
		};

		// Fits a mesh's bounds into a box: its base's middle at a_at, a_size across (x, y) and high (z), turned a_rotZ
		// (and the INI's turn and tilt for it). Uniform: as big as fits. Stretched: exactly the box.
		bool FitBox(Scene::Id a_id, const Kit::Mesh& a_m, const Vec3& a_at, const Vec3& a_size, double a_rotZ, bool a_stretch, Pose& a_out)
		{
			Vec3 mn, mx;
			if (a_id < 0 || !Scene::Bounds(a_id, mn, mx)) {
				return false;
			}
			const Quat q = RotZ(a_rotZ + a_m.turn * kPi / 180.0) * RotX(a_m.tilt * kPi / 180.0);
			Vec3       lo{ 1e9, 1e9, 1e9 }, hi{ -1e9, -1e9, -1e9 };
			for (int c = 0; c < 8; ++c) {
				const Vec3 v = q.Rotate({ (c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y, (c & 4) ? mx.z : mn.z });
				lo = { (std::min)(lo.x, v.x), (std::min)(lo.y, v.y), (std::min)(lo.z, v.z) };
				hi = { (std::max)(hi.x, v.x), (std::max)(hi.y, v.y), (std::max)(hi.z, v.z) };
			}
			const double e[3] = { (std::max)(0.01, hi.x - lo.x), (std::max)(0.01, hi.y - lo.y), (std::max)(0.01, hi.z - lo.z) };
			const double want[3] = { a_size.x, a_size.y, a_size.z };
			double       r[3]{}, fit = 1e9, sum = 0;
			int          n = 0;
			for (int k = 0; k < 3; ++k) {
				if (want[k] > 0) {
					r[k] = want[k] / e[k];
					fit = (std::min)(fit, r[k]);
					sum += r[k];
					++n;
				}
			}
			if (n == 0) {
				fit = 1.0;
			}
			Pose p;
			p.q = q;
			if (a_stretch && n > 0) {
				p.s = 1.0f;
				for (int k = 0; k < 3; ++k) {
					p.w[k] = static_cast<float>(want[k] > 0 ? r[k] : sum / n);
				}
			} else {
				p.s = static_cast<float>(fit);
			}
			const Vec3 bc{ (lo.x + hi.x) * 0.5 * p.s, (lo.y + hi.y) * 0.5 * p.s, lo.z * p.s };
			p.pos = a_at - Vec3{ bc.x * p.w[0], bc.y * p.w[1], bc.z * p.w[2] };
			a_out = p;
			return true;
		}

		void Apply(Scene::Id a_id, const Pose& a_p)
		{
			Scene::Place(a_id, a_p.pos, a_p.q, a_p.s);
			Scene::StretchWorld(a_id, a_p.w[0], a_p.w[1], a_p.w[2]);
			Scene::Hide(a_id, false);
		}

		bool Game(Kit::Role a_r) { return P.game[static_cast<int>(a_r)]; }

		// One of the kitchen's things: the game's mesh if there is one (and it loads), else ours
		Scene::Id KitAdd(Kit::Role a_r)
		{
			const auto& m = Kit::Get(a_r);
			P.game[static_cast<int>(a_r)] = false;
			if (m.vanilla && !m.path.empty()) {
				const auto id = Scene::Add(m.path);
				if (id >= 0) {
					P.game[static_cast<int>(a_r)] = true;
					Scene::Hide(id, true);  // until it's measured and fitted
					return id;
				}
				spdlog::warn("Kitchen: the game's {} ({}) didn't load; using ours", Kit::Name(a_r), m.path);
			}
			const auto ours = Kit::Ours(a_r);
			return ours.empty() ? -1 : Scene::Add(ours);
		}

		// Places a fixed thing: ours exactly where it was made to go, the game's fitted into the same space
		void Settle(Scene::Id a_id, Kit::Role a_r, const Vec3& a_at, const Vec3& a_size, double a_rotZ, bool a_stretch, const Vec3& a_oursAt, double a_oursRot = 0.0)
		{
			if (a_id < 0) {
				return;
			}
			if (Game(a_r)) {
				g_fits.push_back({ a_id, a_r, a_at, a_size, a_rotZ, a_stretch });
			} else {
				Put(a_id, a_oursAt, a_oursRot);
			}
		}

		// The game's pot, when a mod has replaced it with one of another shape (tall, on its side, with a long handle):
		// stretched into a vanilla pot's round, low box it would be crushed, the broth hanging over it. Then ours is used
		// instead, and kept in the INI.
		void CheckPot()
		{
			if (P.pot < 0 || !Game(Kit::Role::Pot)) {
				return;
			}
			Vec3 mn, mx;
			if (!Scene::Bounds(P.pot, mn, mx)) {
				return;  // not measured yet
			}
			const double w = mx.x - mn.x, d = mx.y - mn.y, h = mx.z - mn.z;
			const double big = (std::max)(w, d), narrow = (std::min)(w, d);
			const double flat = big > 0.01 ? h / big : 0.0;
			if (big > 0.01 && narrow >= 0.72 * big && flat >= 0.25 && flat <= 0.8) {
				return;  // round and low: a pot as the game's (36 x 37 across, 17 high)
			}
			spdlog::warn("Kitchen: the game's pot {} is not a pot's shape ({:.0f} x {:.0f} x {:.0f}): a replacer? Using ours (Hearthcooking.ini [Meshes] Pot=ours)",
				Kit::Get(Kit::Role::Pot).path, w, d, h);
			Kit::SetMesh(Kit::Role::Pot, "ours");
			std::erase_if(g_fits, [](const auto& f) { return f.id == P.pot; });
			Scene::Hide(P.pot, true);
			P.pot = KitAdd(Kit::Role::Pot);
			Scene::LookOf(P.pot).hideFlat = true;
			Put(P.pot, { L::kPotX, L::kPotY, 0.0 }, 0.0);
		}

		void Refit()
		{
			CheckPot();
			for (const auto& f : g_fits) {
				Pose p;
				if (FitBox(f.id, f.role == Kit::Role::Count ? Kit::Mesh{} : Kit::Get(f.role), f.at, f.size, f.rotZ, f.stretch, p)) {
					Apply(f.id, p);
				}
			}
		}

		// A log (ours, or the game's firewood) lying along a_dir's angle at a_at, a_len long
		void PlaceLog(Scene::Id a_id, const Vec3& a_at, double a_rotZ, double a_len, double a_tiltY = 0.0)
		{
			if (a_id < 0) {
				return;
			}
			if (!Game(Kit::Role::Log)) {
				Scene::Place(a_id, a_at, RotZ(a_rotZ) * RotY(a_tiltY), static_cast<float>(a_len / 8.4));
				return;
			}
			Lie lie;
			if (!LieOf(a_id, lie)) {
				Scene::Hide(a_id, true);
				return;
			}
			const double s = a_len / lie.d[0];
			const Quat   q = RotZ(a_rotZ) * RotY(a_tiltY) * lie.q;
			Scene::Place(a_id, a_at - q.Rotate(lie.c) * s, q, static_cast<float>(s));
		}

		constexpr double kClothShift = 4.0;  // the cloth, in from the table's left edge

		void BuildKitchen(bool a_oven, hc::Flow a_flow)
		{
			Scene::RemoveAll();
			P = Props{};
			g_fits.clear();
			// the table (the game's, stretched under the worktop's space) and the room (the hearth slab, the wall)
			P.kitchen = KitAdd(Kit::Role::Table);
			Settle(P.kitchen, Kit::Role::Table, { -13.5, 0.0, -14.0 }, { 37.0, 37.0, 14.0 }, 0.0, true, {});
			P.room = Add("Room");
			Put(P.room, {});
			P.cloth = Add("Cloth");
			Put(P.cloth, { kClothShift, 0.0, 0.0 });
			for (int k = 0; k < 3; ++k) {
				P.pile[k] = KitAdd(Kit::Role::Log);
			}
			for (auto& f : P.fireLogs) {
				f = KitAdd(Kit::Role::Log);
				Scene::Hide(f, true);
			}
			P.heldLog = KitAdd(Kit::Role::Log);
			Scene::Hide(P.heldLog, true);
			if (!a_oven) {
				P.firepit = KitAdd(Kit::Role::FirePit);
				Settle(P.firepit, Kit::Role::FirePit, { L::kFireX, L::kFireY, 0.0 }, { 2 * (L::kFireR + 1.8), 2 * (L::kFireR + 1.8), 0.0 }, 0.0, false, { L::kFireX, L::kFireY, 0.0 });
			}
			if (a_flow != hc::Flow::Spit) {
				P.board = KitAdd(Kit::Role::Board);
			}
			if (a_flow == hc::Flow::Oven) {
				P.knife = KitAdd(Kit::Role::Knife);  // a pie's fruit is cut first
			}
			if (a_flow == hc::Flow::Pot) {
				P.knife = KitAdd(Kit::Role::Knife);
				P.pot = KitAdd(Kit::Role::Pot);
				Scene::LookOf(P.pot).hideFlat = true;  // (a game pot can come with a stew already in it: only ours shows)
				Settle(P.pot, Kit::Role::Pot, { L::kPotX, L::kPotY, 0.0 }, { 2.163 * (L::kPotR + 0.9), 2.163 * (L::kPotR + 0.9), 1.0815 * L::kPotRim }, 0.0, true, { L::kPotX, L::kPotY, 0.0 });
				P.broth = Add("Broth");
				P.dishRef = AddResult();  // the finished stew's own food (not its bowl), fading in over the broth as it comes together
				Scene::Hide(P.dishRef, true);
				Scene::LookOf(P.dishRef).onlyFlat = true;
				Scene::LookOf(P.dishRef).unshadowed = true;  // (as the broth: no shadow of the pot's wall across it)
				P.ladleSoup = Add("Broth");  // a ladleful, carried to the bowl
				Scene::Hide(P.ladleSoup, true);
				P.ladleStew = AddResult();  // ... the finished stew's own look in it, once it shows in the pot
				Scene::Hide(P.ladleStew, true);
				Scene::LookOf(P.ladleStew).onlyFlat = true;
				P.ladle = KitAdd(Kit::Role::Ladle);
				P.sauceBowl = -1;
				P.sauceIn = -1;
				if (g_recipe && SauceBowl()) {
					// a sauce's bowl, on the worktop behind the board, and what's poured into it
					P.sauceBowl = KitAdd(Kit::Role::Bowl);
					Settle(P.sauceBowl, Kit::Role::Bowl, kSauceAt, { 7.5, 7.5, 0.0 }, 0.0, false, kSauceAt);
					P.sauceIn = Add("Broth");
					Scene::Hide(P.sauceIn, true);
				}
				P.bowl = KitAdd(Kit::Role::Bowl);
				Settle(P.bowl, Kit::Role::Bowl, SlotAt(0), { 7.0, 7.0, 0.0 }, 0.0, false, SlotAt(0));
				for (int k = 1; k < ServeCount(); ++k) {
					const auto id = KitAdd(Kit::Role::Bowl);
					Settle(id, Kit::Role::Bowl, SlotAt(k), { 7.0, 7.0, 0.0 }, 0.0, false, SlotAt(k));
					P.extraServe.push_back(id);
				}
			}
			if (a_flow == hc::Flow::Spit) {
				P.posts = KitAdd(Kit::Role::Spit);
				if (Game(Kit::Role::Spit)) {
					// the game's spit comes dressed: pots, chains, meat on its own turner. Only its frame stays
					Scene::HideShapes(P.posts, { "pot", "chain", "lid", "beef", "venison", "goat", "meat", "turner", "chicken", "rabbit", "salmon", "food" });
					// and nothing of it above our rod's middle: its own skewer (part of the frame's mesh) would run
					// through ours and flicker; the posts' tips end under the rod that rests across them
					auto& pl = Scene::LookOf(P.posts);
					pl.clips[0][0] = 0.0f, pl.clips[0][1] = 0.0f, pl.clips[0][2] = 1.0f;
					pl.clips[0][3] = static_cast<float>(L::kSpitZ - 0.65);  // (under the whole of its skewer, which lies where our rod does)
				}
				// (the game's frame stands a little under the rod, which rests across its top as in the game, rather than
				// running through its top bar and flickering in and out of it)
				Settle(P.posts, Kit::Role::Spit, { L::kFireX, L::kFireY, 0.0 }, { 2 * L::kSpitX1 - 2 * L::kFireX + 2.0, 0.0, Game(Kit::Role::Spit) ? L::kSpitZ - 0.45 : L::kSpitZ + 1.4 }, 0.0, true, { L::kFireX, L::kFireY, 0.0 });
				P.rod = Add(Game(Kit::Role::Spit) ? "SpitCrank" : "SpitRod");  // the game's frame has its own skewer
			}
			if (a_flow != hc::Flow::Pot) {
				P.plate = KitAdd(Kit::Role::Plate);
				Settle(P.plate, Kit::Role::Plate, SlotAt(0), { 9.4, 9.4, 0.0 }, 0.0, false, SlotAt(0));
				for (int k = 1; k < ServeCount(); ++k) {
					const auto id = KitAdd(Kit::Role::Plate);
					Settle(id, Kit::Role::Plate, SlotAt(k), { 9.4, 9.4, 0.0 }, 0.0, false, SlotAt(k));
					P.extraServe.push_back(id);
				}
			}
			if (a_oven) {
				P.ovenDome = KitAdd(Kit::Role::Oven);
				P.pin = Add("RollingPin");  // for rolling a pie's or dumpling's dough out
				Scene::Hide(P.pin, true);
				P.yolk = Add("Dough");  // a cracked egg's yolk and white, in the bowl
				P.eggWhite = Add("DoughDisk");
				Scene::Hide(P.yolk, true);
				Scene::Hide(P.eggWhite, true);
				P.flourHeap = Add("Dough");  // the flour heaped in the mixing bowl
				P.spoon = Add("Spoon");      // and the spoon it's stirred with
				Scene::Hide(P.flourHeap, true);
				Scene::Hide(P.spoon, true);
				P.butter.clear();
				if (Game(Kit::Role::Oven)) {
					// the game's oven, in the space ours takes (its mouth toward you; OvenTurn in the INI if it isn't)
					Settle(P.ovenDome, Kit::Role::Oven, { L::kOvenX, L::kOvenY - 0.5, 0.0 }, { 25.0, 23.0, L::kOvenFloorZ + 9.5 }, 0.0, true, {});
				} else {
					P.ovenBase = Add("OvenBase");
					Put(P.ovenBase, { L::kOvenX, L::kOvenY, 0.0 });
					Put(P.ovenDome, { L::kOvenX, L::kOvenY, 0.0 });
				}
				P.door = Add(Game(Kit::Role::Oven) ? "OvenDoorArch" : "OvenDoor");  // (the game's mouth is a round arch)
				P.peel = KitAdd(Kit::Role::Peel);
				// the mixing bowl (water, then dough) and the bowl the cut filling waits in
				P.mixBowl = KitAdd(Kit::Role::Bowl);
				Settle(P.mixBowl, Kit::Role::Bowl, kMixAt, { 8.0, 8.0, 0.0 }, 0.0, false, kMixAt);
				P.mixWater = Add("Broth");
				P.mixBall = Add("Dough");
				Scene::Hide(P.mixBall, true);
				P.fillBowl = KitAdd(Kit::Role::Bowl);
				Settle(P.fillBowl, Kit::Role::Bowl, kFillAt, { 8.5, 8.5, 0.0 }, 0.0, false, kFillAt);
				P.dough = Add("Dough");
				Scene::Hide(P.dough, true);
				P.slab = Add("Hull");  // a dumpling's dough rolled out square
				Scene::Hide(P.slab, true);
				P.disk = Add("DoughDisk");
				Scene::Hide(P.disk, true);
			}
			// no clutter: only what's used
		}

		// ---------------------------------------------------------------- the ingredients
		const Recipes::Ingredient& Ing(const Unit& a_u) { return g_recipe->ings[a_u.ing]; }

		double Thick(const Unit& a_u, double a_len, int a_k) { return a_u.dims[a_k] * a_len / a_u.dims[0]; }

		// How it lies flat: its longest side along x, its shortest up
		bool Fit(Unit& a_u)
		{
			if (a_u.fitted) {
				return true;
			}
			Lie lie;
			if (!LieOf(a_u.prop, lie)) {
				return false;
			}
			a_u.lie = lie.q;
			a_u.center = lie.c;
			for (int k = 0; k < 3; ++k) {
				a_u.dims[k] = lie.d[k];
			}
			a_u.boardLen = Clamp(a_u.dims[0] * 0.5, 2.5, 9.0);  // half the game's own size, within reason
			a_u.cut.x0 = 0.0;
			a_u.cut.x1 = a_u.boardLen;
			a_u.cut.target = (std::max)(0.6, a_u.boardLen / 6.0);
			a_u.cut.width = Thick(a_u, a_u.boardLen, 1) * 0.96;
			{
				std::string n = KeyOf(a_u);
				std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				// (by name only: everything else diced is cut both ways, whatever its shape)
				a_u.thin = n.find("leek") != std::string::npos || n.find("carrot") != std::string::npos ||
				           n.find("garlic") != std::string::npos || n.find("root") != std::string::npos || n.find("stalk") != std::string::npos;
			}
			a_u.fitted = true;
			return true;
		}

		// Puts a unit lying along x with its middle at a_at, a_len long (and turned a_rot about z)
		void Lay(Unit& a_u, const Vec3& a_at, double a_len, const Quat& a_extra = {})
		{
			if (!Fit(a_u)) {
				Scene::Hide(a_u.prop, true);
				return;
			}
			const double s = a_len / a_u.dims[0];
			const Quat   q = a_extra * a_u.lie;
			const Vec3   c = q.Rotate(a_u.center) * s;
			Scene::Place(a_u.prop, a_at - c, q, static_cast<float>(s));
			Scene::Hide(a_u.prop, false);
		}


		// A sauce made in a bowl first: its liquids into it, then it into the pot
		bool SauceBowl()
		{
			std::string n = g_recipe ? g_recipe->key : std::string();
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return g_flow == hc::Flow::Pot && n.find("sauce") != std::string::npos;
		}
		bool Liquid(const Unit& a_u)
		{
			std::string n = KeyOf(a_u);
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			for (const char* w : { "ale", "mead", "wine", "milk", "honey", "vinegar", "brandy", "rum", "cider", "oil" }) {
				if (n.find(w) != std::string::npos && n.find("kale") == std::string::npos) {
					return true;
				}
			}
			return false;
		}
		bool OverSauceBowl(float a_mx, float a_my) { return NearScreen(kSauceAt + Vec3{ 0, 0, 1.5 }, a_mx, a_my, 4.5, 50.0f); }

		// Bottles and jugs stand up (as the game models them), not lie on their side
		bool Upright(const Unit& a_u)
		{
			std::string n = KeyOf(a_u);
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			for (const char* w : { "ale", "mead", "wine", "milk", "jug", "bottle", "vinegar", "brandy", "rum", "flin", "sujamma", "cider" }) {
				if (n.find(w) != std::string::npos && n.find("kale") == std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// Stood upright as its mesh is made, its foot at a_at, a_len tall at its longest
		void Stand(Unit& a_u, const Vec3& a_at, double a_len, double a_turn)
		{
			Vec3 mn, mx;
			if (!Fit(a_u) || !Scene::Bounds(a_u.prop, mn, mx)) {
				Scene::Hide(a_u.prop, true);
				return;
			}
			const double s = a_len / a_u.dims[0];
			const Quat   q = RotZ(a_turn);
			const Vec3   foot{ (mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, mn.z };
			Scene::Place(a_u.prop, a_at - q.Rotate(foot) * s, q, static_cast<float>(s));
			Scene::Hide(a_u.prop, false);
		}

		// A form's own textures on its mesh (the game shares one mesh between several things - its stews, its butter
		// - and tells them apart by alternate textures)
		void Swaps(Scene::Id a_id, RE::TESForm* a_form)
		{
			auto* sw = a_id >= 0 && a_form ? a_form->As<RE::TESModelTextureSwap>() : nullptr;
			if (!sw || !sw->alternateTextures) {
				return;
			}
			for (std::uint32_t i = 0; i < sw->numAlternateTextures; ++i) {
				const auto& alt = sw->alternateTextures[i];
				if (!alt.textureSet) {
					continue;
				}
				const char* name = alt.name3D.c_str();
				spdlog::info("Textures: {} wears {} on '{}' ({})", a_form->GetName(), alt.textureSet->textures[0].textureName.c_str(), name ? name : "", alt.index3D);
				Scene::SwapTextures(a_id, name ? name : "", static_cast<int>(alt.index3D), alt.textureSet->textures[0].textureName.c_str(),
					alt.textureSet->textures[1].textureName.c_str());
			}
		}

		// An egg cracked over the mixing bowl: tapped on its edge, pulled apart in two halves of shell (the egg's own
		// mesh, each cut at its middle), the yolk and white dropping in; the shells set down beside the bowl
		void DrawEgg()
		{
			const double T = g_eggT;
			Lie          lie;
			const bool   shells = T >= 0.0 && P.eggA >= 0 && P.eggB >= 0 && LieOf(P.eggA, lie);
			for (int k = 0; k < 2; ++k) {
				const auto id = k ? P.eggB : P.eggA;
				if (id < 0) {
					continue;
				}
				Scene::Hide(id, !shells);
				if (!shells) {
					continue;
				}
				const double s = k ? 1.0 : -1.0;  // which end: -x (k 0) or +x (k 1)
				const double ks = 2.3 / (std::max)(0.01, lie.d[0]);
				Quat         q;
				Vec3         at;
				if (T < 1.4) {
					const double tap = T < 0.3 ? -std::sin(T / 0.3 * kPi) * 0.7 : 0.0;  // knocked on the rim
					const double open = Clamp((T - 0.3) / 0.45, 0.0, 1.0);
					q = RotY(-s * open * 1.35) * lie.q;  // the cracked ends tip down and apart
					at = kMixAt + Vec3{ 1.2 + s * open * 0.85, 0.0, 6.0 + tap };
				} else {
					// set down by the bowl, hollow up
					q = RotZ(0.6 + k * 1.9) * RotY(s * kPi * 0.5) * lie.q;
					at = kMixAt + Vec3{ 5.4 + k * 1.4, -3.6 + k * 1.3, ks * lie.d[0] * 0.5 * 0.95 };  // (standing on its round end)
				}
				Scene::Place(id, at - q.Rotate(lie.c) * ks, q, static_cast<float>(ks));
				// cut at its middle: only its own end shows
				auto&      lk = Scene::LookOf(id);
				const Vec3 n = q.Rotate(lie.q.Conjugate().Rotate({ 1, 0, 0 })) * (k ? -1.0 : 1.0);
				std::memset(lk.clips, 0, sizeof(lk.clips));
				lk.clips[0][0] = static_cast<float>(n.x), lk.clips[0][1] = static_cast<float>(n.y), lk.clips[0][2] = static_cast<float>(n.z);
				lk.clips[0][3] = static_cast<float>(n.Dot(at));
				lk.flesh[3] = 0.0f;
				lk.shadow = T >= 1.4;
			}
			// the yolk and white: dropping from the shell, then lying on what's in the bowl until it's stirred in
			const bool   inBowl = T >= 0.45 && !g_ball && !g_doughOnBoard && g_mix < 0.3;
			const double fall = Clamp((T - 0.45) / 0.35, 0.0, 1.0);
			const double fade = 1.0 - Clamp(g_mix / 0.3, 0.0, 1.0);
			const double top = kMixAt.z + 1.5 + 0.4 * g_mix + 0.12;
			const Vec3   rest = kMixAt + RotZ(g_mixA * 0.3).Rotate({ 0.5, 0.3, 0.0 });
			const Vec3   from = kMixAt + Vec3{ 1.2, 0.0, 5.6 };
			const Vec3   at = from + (Vec3{ rest.x, rest.y, top } - from) * (fall * fall);
			if (P.eggWhite >= 0) {
				Scene::Hide(P.eggWhite, !inBowl);
				if (inBowl) {
					const double spread = 0.5 + 1.1 * Clamp((T - 0.75) / 0.4, 0.0, 1.0);  // runs out once it lands
					Scene::Place(P.eggWhite, at + Vec3{ 0, 0, -0.08 }, RotZ(0.7), static_cast<float>(spread));
					Scene::Stretch(P.eggWhite, 1.0f, 0.85f, 0.06f);
					auto& wl = Scene::LookOf(P.eggWhite);
					wl.lift[0] = 0.97f, wl.lift[1] = 0.95f, wl.lift[2] = 0.86f, wl.lift[3] = 0.85f;
					wl.alpha = static_cast<float>(0.55 * fade);
					wl.shadow = false;
				}
			}
			if (P.yolk >= 0) {
				Scene::Hide(P.yolk, !inBowl);
				if (inBowl) {
					Scene::Place(P.yolk, at + Vec3{ 0, 0, 0.12 }, {}, 0.48f);
					Scene::Stretch(P.yolk, 1.0f, 1.0f, 0.72f);
					auto& yl = Scene::LookOf(P.yolk);
					yl.lift[0] = 1.0f, yl.lift[1] = 0.62f, yl.lift[2] = 0.08f, yl.lift[3] = 0.92f;
					yl.alpha = static_cast<float>((std::max)(0.02, fade));
					yl.shadow = false;
				}
			}
		}

		// The flour heaped in the mixing bowl, a dusting of it, the butter cut in over it, and the wooden spoon: stirred, the
		// heap sinks and smooths into the dough, the butter going into it
		void DrawFlour(std::vector<Render3D::Sprite>& a_sp)
		{
			bool flour = false, butter = false;
			for (const auto& u : g_units) {
				std::string n = KeyOf(u);
				std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				flour |= u.at == Unit::At::Dough && n.find("flour") != std::string::npos;
				butter |= u.at == Unit::At::Dough && n.find("butter") != std::string::npos;
			}
			const bool   mixing = g_flow == hc::Flow::Oven && !g_topping && g_doughParts > 0 && !g_ball && !g_doughOnBoard;
			const double m = Clamp(g_mix, 0.0, 1.0);
			const double top = kMixAt.z + 1.5 + 0.4 * g_mix;
			const bool   heap = mixing && flour && m < 0.85;
			if (P.flourHeap >= 0) {
				Scene::Hide(P.flourHeap, !heap);
				if (heap) {
					const double r = 2.7 * (1.0 - 0.35 * m);
					Scene::Place(P.flourHeap, Vec3{ kMixAt.x, kMixAt.y, top + 0.1 }, RotZ(g_mixA * 0.3), static_cast<float>(r));
					Scene::Stretch(P.flourHeap, 1.0f, 1.0f, static_cast<float>(0.34 * (1.0 - 0.7 * m)));
					auto& fl = Scene::LookOf(P.flourHeap);
					const double wet = g_milkIn ? 0.35 : 0.0;
					fl.lift[0] = static_cast<float>(0.95 - 0.03 * wet), fl.lift[1] = static_cast<float>(0.92 - 0.04 * wet), fl.lift[2] = static_cast<float>(0.85 - 0.06 * wet);
					fl.lift[3] = 0.9f;
					fl.shadow = false;
				}
			}
			// powdery: specks over the heap, and a little on the rim
			if (heap) {
				const double r = 2.5 * (1.0 - 0.35 * m);
				for (int k = 0; k < 70; ++k) {
					const double a = Hash(k, 51) * 6.283 + g_mixA * 0.3, rr = std::sqrt(Hash(k, 52)) * r;
					Render3D::Sprite g;
					g.pos[0] = static_cast<float>(kMixAt.x + std::cos(a) * rr);
					g.pos[1] = static_cast<float>(kMixAt.y + std::sin(a) * rr);
					g.pos[2] = static_cast<float>(top + 0.1 + 0.34 * (1.0 - 0.7 * m) * r * std::sqrt((std::max)(0.0, 1.0 - (rr / r) * (rr / r))) + 0.04);
					g.size = static_cast<float>(0.05 + 0.06 * Hash(k, 53));
					g.color[0] = 0.97f, g.color[1] = 0.95f, g.color[2] = 0.9f;
					g.color[3] = static_cast<float>(0.8 * (1.0 - m));
					g.kind = Render3D::Sprite::Kind::Grain;
					a_sp.push_back(g);
				}
			}
			// the butter: cubes cut in over the flour, sinking in as it's stirred
			const int  nb = butter && heap ? 7 : 0;
			while (static_cast<int>(P.butter.size()) < nb) {
				P.butter.push_back(Add("Hull"));
			}
			for (int k = 0; k < static_cast<int>(P.butter.size()); ++k) {
				const auto id = P.butter[k];
				const double sink = Clamp(m / 0.45, 0.0, 1.0);
				const bool   show = k < nb && sink < 1.0;
				Scene::Hide(id, !show);
				if (!show) {
					continue;
				}
				const double a = Hash(k, 54) * 6.283 + g_mixA * 0.3, rr = 0.4 + std::sqrt(Hash(k, 55)) * 1.4;
				const double sz = 0.62 + 0.18 * Hash(k, 56);
				const double hh = 0.34 * 2.7 * std::sqrt((std::max)(0.0, 1.0 - (rr / 2.7) * (rr / 2.7)));
				Scene::Place(id, Vec3{ kMixAt.x + std::cos(a) * rr, kMixAt.y + std::sin(a) * rr, top + 0.1 + hh * (1.0 - sink) }, RotZ(Hash(k, 57) * 3.0) * RotY((Hash(k, 58) - 0.5) * 0.5), 1.0f);
				Scene::Stretch(id, static_cast<float>(sz * (1.0 - 0.6 * sink)), static_cast<float>(sz * 0.8 * (1.0 - 0.6 * sink)), static_cast<float>(sz * 0.55 * (1.0 - 0.6 * sink)));
				auto& bl = Scene::LookOf(id);
				bl.lift[0] = 0.99f, bl.lift[1] = 0.9f, bl.lift[2] = 0.55f, bl.lift[3] = 0.92f;
				bl.shadow = false;
			}
			// the spoon: in the bowl while there's something to stir, going round with the stirring
			if (P.spoon >= 0) {
				Scene::Hide(P.spoon, !mixing);
				if (mixing) {
					const double a = g_mixA;
					const double rr = g_hold == Hold::Mix ? 1.3 : 1.6;
					const Vec3   bowl{ kMixAt.x + std::cos(a) * rr, kMixAt.y + std::sin(a) * rr, top + 0.15 };
					// its handle out over the rim, leaning up away from the middle
					const Quat q = RotZ(a) * RotY(-0.55);
					Scene::Place(P.spoon, bowl, q, 1.0f);
				}
			}
		}

		// An ingredient's mesh, as it looks in the game
		Scene::Id AddUnitMesh(const Unit& a_u)
		{
			if (a_u.model.empty()) {
				return -1;
			}
			const auto id = Scene::Add(a_u.model);
			if (g_recipe && a_u.ing >= 0 && a_u.ing < static_cast<int>(g_recipe->ings.size()) && a_u.model == g_recipe->ings[a_u.ing].model) {
				Swaps(id, g_recipe->ings[a_u.ing].form);  // (its own mesh only: another's would be wrongly dressed)
			}
			return id;
		}

		// our version: a dough of flour (the oven's batches are cut from one dough; a dish on bread already made isn't)
		bool FlourRecipe()
		{
			if (!g_recipe) {
				return false;
			}
			for (const auto& in : g_recipe->ings) {
				if (Lower(in.name).find("flour") != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		int UnitsWanted()
		{
			int n = 0;
			for (const auto& in : g_recipe->ings) {
				if (in.role != Role::Salt) {
					n += in.count * (g_flow != hc::Flow::Oven || FlourRecipe() ? g_portions : 1);
				}
			}
			return (std::min)(n, kMaxUnits);
		}

		void BuildUnits()
		{
			g_units.clear();
			for (int i = 0; i < static_cast<int>(g_recipe->ings.size()); ++i) {
				const auto& in = g_recipe->ings[i];
				if (in.role == Role::Salt) {
					continue;
				}
				const int reps = g_flow != hc::Flow::Oven || FlourRecipe() ? g_portions : 1;
				for (int c = 0; c < in.count * reps && static_cast<int>(g_units.size()) < kMaxUnits; ++c) {
					Unit u;
					u.dish = c / (std::max)(1, in.count);
					u.ing = i;
					u.role = in.role;
					u.name = in.name;
					u.model = in.model;
					u.key = in.key;
					// kept like the kitchen's things: the mesh it has is written into the INI once, and used from then on
					if (const auto set = Kit::IngredientMesh(u.name); !set.empty()) {
						u.model = set;
					} else if (!u.model.empty()) {
						Kit::SetIngredientMesh(u.name, u.model);
					}
					{
						std::string ln = u.name;
						std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
						u.bulb = ln.find("garlic") != std::string::npos && ln.find("bread") == std::string::npos;
					}
					u.prop = AddUnitMesh(u);
					if (u.prop < 0) {
						u.prop = Add("Chunk");  // a stand-in if its mesh won't load
						if (u.prop >= 0) {
							u.fitted = true;
							u.dims[0] = 1.0;
							u.dims[1] = 0.8;
							u.dims[2] = 0.7;
							u.boardLen = 5.0;
							u.cut.x1 = 5.0;
						}
					}
					Scene::Hide(u.prop, true);
					const int k = static_cast<int>(g_units.size());
					// (with many, closer together: five down a column and narrower columns)
					const int  total = UnitsWanted();
					const bool many = total > 8;
					const int  rows = many ? 5 : 4;
					u.clothAt = { -31.5 + (many ? 3.9 : 5.0) * (k / rows) + kClothShift, 5.5 - (k % rows) * (many ? 5.0 : 6.5), 0.0 };  // clear of the recipe card
					g_units.push_back(std::move(u));
				}
				ArrangeCloth();
			}
		}

		int FreeLane()
		{
			for (int l = 0; l < 3; ++l) {
				bool used = false;
				for (const auto& u : g_units) {
					used |= u.at == Unit::At::Board && u.lane == l;
				}
				if (!used) {
					return l;
				}
			}
			return -1;
		}

		// ---------------------------------------------------------------- the pieces
		// How thick it is at a point along its length (round things narrow toward their ends)
		std::pair<double, double> Cross(const Unit& u, double a_x)
		{
			const double L = u.boardLen, dy = Thick(u, L, 1), dz = Thick(u, L, 2);
			const double t = Clamp(std::abs(a_x) * 2.0 / L, 0.0, 1.0);
			double       f = 1.0;
			if (u.dims[0] < 1.6 * u.dims[1]) {
				f = std::sqrt((std::max)(0.04, 1.0 - t * t));  // an apple, a cabbage, a potato
			} else if (t > 0.85) {
				f = std::sqrt((std::max)(0.08, 1.0 - std::pow((t - 0.85) / 0.15, 2.0)));  // a leek, a carrot: only the tips
			}
			return { dy * f * 0.96, dz * f * 0.96 };
		}

		// A recipe card's line for cutting something up (garlic: broken into cloves first)
		std::string DiceLine(const std::string& a_name, int a_count)
		{
			std::string l = a_name;
			std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const std::string n = a_count > 1 ? fmt::format(" ({})", a_count) : "";
			if (l.find("garlic") != std::string::npos && l.find("bread") == std::string::npos) {
				return fmt::format("{}{}: break apart the cloves, then dice", a_name, n);
			}
			return fmt::format("Dice the {}{}", a_name, n);
		}

		// What it looks like inside, where there's a texture for it: an atlas of its section across and along
		ID3D11ShaderResourceView* FleshTexture(const Unit& u)
		{
			std::string n = KeyOf(u);
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			static const std::pair<const char*, const char*> kBy[] = {
				{ "venison", "venison" }, { "pheasant", "pheasant" }, { "chicken", "chicken" }, { "beef", "beef" }, { "horker", "beef" },
				{ "mammoth", "beef" }, { "goat", "beef" }, { "horse", "beef" }, { "carrot", "carrot" }, { "potato", "potato" },
				{ "apple", "apple" }, { "tomato", "tomato" }, { "cabbage", "cabbage" }, { "leek", "leek" }
			};
			for (const auto& [word, tex] : kBy) {
				if (n.find(word) != std::string::npos) {
					return Scene::Texture(fmt::format("textures\\Hearthcooking\\flesh_{}.dds", tex).c_str());
				}
			}
			return nullptr;
		}

		// The colour inside it
		void Flesh(const Unit& u, float (&a_out)[3])
		{
			std::string n = KeyOf(u);
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			auto has = [&](std::initializer_list<const char*> ws) {
				for (const char* w : ws) {
					if (n.find(w) != std::string::npos) {
						return true;
					}
				}
				return false;
			};
			float c[3] = { 0.92f, 0.88f, 0.76f };
			if (has({ "crab" })) {
				c[0] = 0.96f, c[1] = 0.94f, c[2] = 0.9f;  // crab meat: white inside
			} else if (has({ "beef", "venison", "meat", "horker", "mammoth", "goat", "horse", "dog", "rabbit", "haunch", "leg", "steak" })) {
				c[0] = 0.72f, c[1] = 0.3f, c[2] = 0.27f;
			} else if (has({ "salmon" })) {
				c[0] = 0.95f, c[1] = 0.55f, c[2] = 0.4f;
			} else if (has({ "crab" })) {
				c[0] = 0.96f, c[1] = 0.94f, c[2] = 0.9f;  // white inside
			} else if (has({ "clam" })) {
				c[0] = 0.98f, c[1] = 0.92f, c[2] = 0.84f;  // clam meat: near white, the palest orange
			} else if (has({ "chicken", "pheasant", "fish" })) {
				c[0] = 0.95f, c[1] = 0.84f, c[2] = 0.8f;  // pale pink
			} else if (has({ "carrot" })) {
				c[0] = 0.98f, c[1] = 0.58f, c[2] = 0.2f;
			} else if (has({ "yam", "pumpkin", "gourd" })) {
				c[0] = 0.97f, c[1] = 0.6f, c[2] = 0.3f;  // an ash yam: orange like a sweet potato
			} else if (has({ "tomato" })) {
				c[0] = 0.9f, c[1] = 0.22f, c[2] = 0.14f;
			} else if (has({ "garlic" })) {
				c[0] = 0.96f, c[1] = 0.93f, c[2] = 0.8f;
			} else if (has({ "cabbage" })) {
				c[0] = 0.93f, c[1] = 0.92f, c[2] = 0.78f;  // pale cream, greener toward the leaves' edge
			} else if (has({ "leek" })) {
				c[0] = 0.9f, c[1] = 0.93f, c[2] = 0.76f;
			} else if (has({ "cheese" })) {
				c[0] = 0.96f, c[1] = 0.88f, c[2] = 0.58f;
			} else if (has({ "apple", "pear" })) {
				c[0] = 0.96f, c[1] = 0.92f, c[2] = 0.74f;
			} else if (has({ "potato" })) {
				c[0] = 0.94f, c[1] = 0.88f, c[2] = 0.66f;
			}
			std::memcpy(a_out, c, sizeof(c));
		}

		int NewPiece(int a_unit, int a_body, const double (&a_box)[6], const Vec3& a_mid, bool a_cut)
		{
			auto& u = g_units[a_unit];
			Piece p;
			p.unit = a_unit;
			std::copy(std::begin(a_box), std::end(a_box), p.box);
			p.mid = a_mid;
			p.cut = a_cut;
			// a leek, into the pot: from our own solid leek (the game's is loose layers and open leaves, which floated as
			// shards and fanned strips once cut up); on the cloth and the board it's the game's own
			p.prop = AddUnitMesh(u);
			if (u.name.find("eek") != std::string::npos) {
				p.alt = Add("Leek");  // (the game's own on the board and falling; ours once it's in the pot)
				if (p.alt >= 0) {
					Scene::Hide(p.alt, true);
					g_ownLeek.insert(p.alt);
				}
			}
			if (p.prop < 0) {
				p.prop = Add("Chunk");  // its mesh won't load: a plain piece
			}
			if (a_cut) {
				p.core = Add("Hull");
			}
			if (static_cast<int>(g_pieces.size()) <= a_body) {
				g_pieces.resize(a_body + 1);
			}
			g_pieces[a_body] = p;
			return a_body;
		}

		// How far apart the pieces of one thing sit on the board, so the cuts show
		double Spread(int a_k, int a_n) { return (a_k - (a_n - 1) * 0.5) * 0.06; }  // a hair: the cuts show, the skin reads whole

		void HideBoardBits(Unit& u)
		{
			for (auto id : u.segs) {
				Scene::Hide(id, true);
			}
			for (auto id : u.cores) {
				Scene::Hide(id, true);
			}
		}

		// Its pieces stacked in layers when it's diced both ways (a horizontal cut or two, as a cook would), so a
		// tall thing comes apart in dice rather than in posts
		int Layers(const Unit&) { return 1; }  // pieces go over as they were cut on the board
		// What's on the board goes over: a body per piece, diced pieces in layers, empty corners (past a round thing's
		// edge) left out
		void FinishBoard()
		{
			for (int i = 0; i < static_cast<int>(g_units.size()); ++i) {
				auto& u = g_units[i];
				if (u.at != Unit::At::Board || u.cut.finished) {
					continue;
				}
				const int    nx = u.cut.Pieces(), ny = u.cut.Strips(), nz = Layers(u);
				const bool   cut = nx * ny > 1;
				const double half = u.boardLen * 0.5, w = u.cut.width, h = Thick(u, u.boardLen, 2);
				for (int ix = 0; ix < nx; ++ix) {
					double xa, xb;
					u.cut.Piece(ix, xa, xb);
					xa -= half, xb -= half;
					const auto [cy, cz] = Cross(u, Clamp(0.0, xa, xb));  // its fullest, within this piece
					const double ry = (std::max)(0.2, cy * 0.5), rz = (std::max)(0.2, cz * 0.5);
					for (int iy = 0; iy < ny; ++iy) {
						double ya, yb;
						u.cut.Strip(iy, ya, yb);
						ya -= w * 0.5, yb -= w * 0.5;
						if (!cut || ny == 1) {
							ya = -ry, yb = ry;
						}
						for (int iz = 0; iz < nz; ++iz) {
							double za = -h * 0.5 + h * iz / nz, zb = -h * 0.5 + h * (iz + 1) / nz;
							if (nz == 1) {
								za = -rz, zb = rz;
							}
							// is there anything in this cell?
							const double py = Clamp(0.0, ya, yb) / ry, pz = Clamp(0.0, za, zb) / rz;
							if (py * py + pz * pz > 0.92) {
								continue;
							}
							// the body: what of the cell the thing fills
							const double ey0 = (std::max)(ya, -ry), ey1 = (std::min)(yb, ry), ez0 = (std::max)(za, -rz), ez1 = (std::min)(zb, rz);
							const Vec3   mid{ (xa + xb) * 0.5, (ey0 + ey1) * 0.5, (ez0 + ez1) * 0.5 };
							const Vec3   size{ xb - xa, (std::max)(0.2, ey1 - ey0), (std::max)(0.2, ez1 - ez0) };
							const Vec3   local{ mid.x + Spread(ix, nx), L::kLaneY[u.lane] + mid.y + Spread(iy, ny), L::kBoardH * 0.5 + h * 0.5 + mid.z };
							const int    b = g_chunks.Add(i, local, size, static_cast<unsigned>(g_pieces.size() * 31 + 7));
							if (b < 0) {
								continue;
							}
							// (an outer piece is open on its outer side: the cuts' range can fall short of the thing's own edge, which
							// was cut off flat, its inside showing - a pale band down an apple's side)
							const double box[6] = { cut && ix > 0 ? xa : -1e7, cut && ix < nx - 1 ? xb : 1e7, ny > 1 && iy > 0 ? ya : -1e7, ny > 1 && iy < ny - 1 ? yb : 1e7,
								                    nz > 1 && iz > 0 ? za : -1e7, nz > 1 && iz < nz - 1 ? zb : 1e7 };
							NewPiece(i, b, box, mid, cut);
							g_chunks.At(b).q = {};  // a fresh piece lies as it was cut, not turned
						}
					}
				}
				u.wentWhole = !cut;
				u.cut.Finish();
				Scene::Hide(u.prop, true);
				HideBoardBits(u);
				u.at = Unit::At::Pot;  // on its way
			}
		}


		// ---------------------------------------------------------------- starting a dish
		void BrothColour()
		{
			std::string s = g_recipe->key;
			for (const auto& in : g_recipe->ings) {
				s += " " + in.key;
			}
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			auto has = [&](const char* w) { return s.find(w) != std::string::npos; };
			const float* c = nullptr;
			static const float tomato[3] = { 0.72f, 0.24f, 0.12f }, green[3] = { 0.55f, 0.52f, 0.26f }, cream[3] = { 0.86f, 0.78f, 0.6f },
							   brown[3] = { 0.46f, 0.27f, 0.13f }, plain[3] = { 0.62f, 0.44f, 0.24f };
			if (has("tomato")) {
				c = tomato;
			} else if (has("milk") || has("cheese") || has("chowder") || has("fondue") || has("cream") || has("potato soup")) {
				c = cream;
			} else if (has("beef") || has("venison") || has("horker") || has("meat") || has("stew") || has("goat") || has("mammoth")) {
				c = brown;
			} else if (has("cabbage") || has("leek") || has("vegetable")) {
				c = green;
			} else {
				c = plain;
			}
			std::memcpy(g_broth, c, sizeof(g_broth));
		}

		void ResetHands()
		{
			g_hold = Hold::None;
			g_holdUnit = -1;
			g_tip = g_tipWant = 0;
			g_tipped = false;
			g_boardLift = 0;
			g_boardPose = hc::BoardPose(0.0, L::kBoardY);
			g_boardPrev = g_boardPose;
			g_boardY = L::kBoardY;
			g_knifeAnim = -1;
			g_knifeOn = false;
			g_knifeAlong = false;
			g_ladleA = -kPi * 0.6;
			g_ladleR = 3.6;
			g_pinch = 0;
			g_door = g_doorWant = 1.0;
			g_doorShut = g_doorReopened = false;
			g_ownLeek.clear();
			g_doughParts = 0;
			g_rolled = false;
			g_mix = 0.0;
			g_spin = 0.0;
			g_eggT = -1.0;
			g_campfire = -1;
			g_sauceN = 0;
			g_sauceTip = 0.0;
			g_cutInMix = false;
			g_pinHeld = false;
			g_ball = false;
			g_milkIn = false;
			for (int k = 0; k < 4; ++k) {
				g_fold[k] = g_foldWant[k] = 0.0;
			}
			g_foldSide = -1;
			g_flat = 0.0;
			g_bowlTip = 0.0;
			g_bowlPoured = false;
			g_doughOnBoard = false;
			g_squash = 0;
			g_stock = 0;
			g_peelT = -1;
			g_peelLoaded = false;
			g_peelBread = false;
			g_served = false;
			g_grains.clear();
			g_tossedIn = 0;
			g_crankRun = 0;
			g_stirred = 0;
		}

		void ShowStation()
		{
			// the book over the kitchen as it stands: the fire going, nothing out yet
			g_flow = g_ovenStation ? hc::Flow::Oven : hc::Flow::Pot;
			g_recipe = nullptr;
			g_units.clear();
			g_pieces.clear();
			g_chunks.Clear();
			BuildKitchen(g_ovenStation, g_flow);
			g_fire = hc::Fire(g_ovenStation ? 9.0 : 5.0);
			g_fire.Light(2);
			g_pot.reset();
			g_spit.reset();
			g_oven.reset();
			ResetHands();
			Scene::SetCamera({ 0.0, -1.0, 0.0 }, 96.0f, true);
		}

		void StartCooking(const Recipes::Recipe* a_r)
		{
			g_recipe = a_r;
			g_flow = a_r->flow;
			g_portions = std::clamp(g_portions, 1, MaxPortions(*a_r));
			g_slots.assign(g_portions, Slot{});
			g_cuts.clear();
			g_batchCut = false;
			g_batch.clear();
			g_chunks.Clear();
			g_pieces.clear();
			BuildKitchen(a_r->oven, g_flow);
			g_fire = hc::Fire(a_r->oven ? 9.0 : 5.0, hc::timing::kLogBurn);
			g_fire.Light(0);  // every fire starts cold: firewood goes in (the pot's once everything's in it)
			g_timer = {};
			g_timer.time = BaseTime(*a_r);
			g_portions = std::clamp(g_portions, 1, MaxPortions(*a_r));
			g_portionTiers.clear();
			g_stationLogs = hc::timing::StartLogs(static_cast<unsigned>(std::rand()) ^ static_cast<unsigned>(g_clock * 1000.0));
			g_invLogsUsed = 0;
			spdlog::info("{}: {:.0f} s with one log; the station has {} logs, the pack {}", a_r->name, g_timer.time, g_stationLogs, PackLogs());
			g_pot.reset();
			g_spit.reset();
			g_oven.reset();
			if (g_flow == hc::Flow::Pot) {
				g_pot = std::make_unique<hc::Pot>(a_r->salted);
				BrothColour();
			} else if (g_flow == hc::Flow::Spit) {
				g_spit = std::make_unique<hc::Spit>(a_r->salted);
			} else {
				g_oven = std::make_unique<hc::Oven>(a_r->pie, a_r->salted, a_r->dough);
			}
			if (a_r->salted) {
				P.salt = KitAdd(Kit::Role::SaltBowl);
				Settle(P.salt, Kit::Role::SaltBowl, { L::kSaltX, L::kSaltY, 0.0 }, { 4.4, 4.4, 0.0 }, 0.0, false, { L::kSaltX, L::kSaltY, 0.0 });
				if (Game(Kit::Role::SaltBowl) && Kit::Get(Kit::Role::Salt).vanilla) {
					P.saltPile = KitAdd(Kit::Role::Salt);
					Settle(P.saltPile, Kit::Role::Salt, { L::kSaltX, L::kSaltY, 0.7 }, { 3.0, 3.0, 0.0 }, 0.0, false, {});
				}
			}
			ResetHands();
			BuildUnits();
			g_doughNeeded = 0;
			for (const auto& u : g_units) {
				g_doughNeeded += u.role == Role::Dough;
			}
			if (g_flow == hc::Flow::Oven && FlourRecipe() && g_portions > 1) {
				g_oven->SetKneadScale(1.5);  // one big dough for every portion: half again the work
			}
			// no dough to make (a dish built on bread already made): the first filling is the base
			if (g_flow == hc::Flow::Oven && g_doughNeeded == 0) {
				g_doughParts = 1;
				g_oven->Knead(5.0, 0.0);
				g_ball = g_doughOnBoard = true;
			}
			// the oven: a pie's fruit and vegetables are cut first, and wait in a bowl
			if (g_flow == hc::Flow::Oven) {
				for (auto& u : g_units) {
					std::string n = KeyOf(u);
					std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					const bool soft = n.find("honey") != std::string::npos || n.find("sugar") != std::string::npos || n.find("butter") != std::string::npos ||
					                  n.find("milk") != std::string::npos || n.find("egg") != std::string::npos || n.find("jug") != std::string::npos ||
					                  n.find("sack") != std::string::npos || n.find("bread") != std::string::npos || n.find("jazbay") != std::string::npos ||
					                  n.find("berr") != std::string::npos || n.find("ear") != std::string::npos || n.find("mirriam") != std::string::npos;
					u.dice = u.role == Role::Filling && !soft && !u.model.empty();
				}
				g_topping = false;
				g_buttered = false;
				bool flour = false;
				for (auto& u : g_units) {
					std::string n = KeyOf(u);
					std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					flour |= n.find("flour") != std::string::npos;
					g_topping |= Lower(u.name) == "bread";
				}
				g_topping = g_topping && !flour && !g_recipe->pie;
				if (g_topping) {
					// no dough to make: the bread is the base
					g_doughNeeded = 0;
					g_doughParts = 1;
					g_ball = true;
					g_doughOnBoard = false;
					g_oven->Knead(5.0, 0.0);
					for (auto& u : g_units) {
						std::string n = KeyOf(u);
						std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
						if (Lower(u.name) == "bread") {
							u.thin = true;  // cut one way, into slices
						}
					}
				}
				g_chunks.SetBowl({ CutBowl().x, CutBowl().y, 1.0 }, 3.5, 2.6, false);
				g_bowlWater = true;
				for (const auto& u : g_units) {
					std::string n = KeyOf(u);
					std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (u.role == Role::Dough && n.find("milk") != std::string::npos) {
						g_bowlWater = false;  // the milk is the wet: an empty bowl, the dry things first
					}
				}
			} else {
				g_chunks.SetBowl({ L::kPotX, L::kPotY, L::kLiquidZ }, L::kPotR, L::kPotRim, true);
			}
			g_phase = Phase::Cooking;
			g_served = false;
			g_help = false;
			g_soundBox = false;
			g_confirm = false;
			Scene::SetCamera({ 0.0, -1.0, 0.0 }, 88.0f);
			Audio::Play({ "ui_equip", 0.8f });
			spdlog::info("Cooking {} by hand ({})", a_r->name, g_flow == hc::Flow::Pot ? "pot" : g_flow == hc::Flow::Spit ? "spit" : "oven");
		}

		void CloseKitchen(bool a_closeCrafting)
		{
			Audio::StopAll();
			Scene::Close();
			g_phase = Phase::Idle;
			g_recipe = nullptr;
			g_units.clear();
			g_pieces.clear();
			g_chunks.Clear();
			ShowCrafting(true);
			if (a_closeCrafting) {
				SKSE::GetTaskInterface()->AddTask([] {
					if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::CraftingMenu::MENU_NAME)) {
						RE::CraftingMenu::QuitMenu();
					}
				});
			}
		}

		// ---------------------------------------------------------------- serving
		// The finished dish: its mesh, wearing the textures its form gives it (as the inventory shows it)
		Scene::Id AddResult()
		{
			auto* form = g_recipe ? g_recipe->result : nullptr;
			auto* m = form ? form->As<RE::TESModel>() : nullptr;
			if (!m || !m->GetModel() || !*m->GetModel()) {
				return -1;
			}
			const auto id = Scene::Add(m->GetModel());
			Swaps(id, form);
			return id;
		}

		void Serve()
		{
			if (g_served || !g_recipe) {
				return;
			}
			g_served = true;
			if (g_flow == hc::Flow::Pot) {
				std::vector<double> cuts;
				int                 lost = g_chunks.CountIn(hc::Chunks::State::Lost);
				for (const auto& u : g_units) {
					if (u.role == Role::Chop) {
						cuts.push_back(u.wentWhole ? 0.05 : u.cut.Score());
					}
					if (u.at == Unit::At::Cloth || u.at == Unit::At::Board) {
						++lost;  // never went in
					}
				}
				const int in = g_chunks.CountIn(hc::Chunks::State::Pot) + g_tossedIn;
				g_result = g_pot->Score(cuts.data(), static_cast<int>(cuts.size()), in, lost);
			} else if (g_flow == hc::Flow::Spit) {
				g_result = g_spit->Score();
			} else {
				g_oven->Unload();
				g_result = g_oven->Score();
			}
			// our version: each portion's tier is the second it came off the fire; the old lines stay as advice
			g_portionTiers.clear();
			if (g_flow != hc::Flow::Oven) {
				for (const auto& sl : g_slots) {
					if (sl.count > 0) {
						g_portionTiers.emplace_back(sl.tier, sl.raw);
					}
				}
			}
			if (g_flow == hc::Flow::Oven && !g_batch.empty()) {
				for (int i = 0; i < static_cast<int>(g_batch.size()); ++i) {
					g_batch[i].TakeOut();
					g_portionTiers.emplace_back(g_batch[i].tier, g_batch[i].raw);
					if (i < static_cast<int>(g_slots.size())) {
						FillSlot(i, g_batch[i].tier, g_batch[i].raw);
					}
				}
			}
			if (g_portionTiers.empty()) {
				g_timer.TakeOut();
				g_portionTiers.emplace_back(g_timer.tier, g_timer.raw);
			}
			const int n = static_cast<int>(g_portionTiers.size());
			{
				auto verdict = [](hc::Tier t, bool raw) {
					return raw ? "Raw: taken off too soon" : t == hc::Tier::Masterwork ? "Masterwork: on the very second" :
					             t == hc::Tier::Hearty ? "Hearty: a moment late" : t == hc::Tier::Plain ? "Plain: left a while" : "Burnt: left far too long";
				};
				auto score = [](hc::Tier t, bool raw) {
					return raw ? 0.0 : t == hc::Tier::Masterwork ? 1.0 : t == hc::Tier::Hearty ? 0.75 : t == hc::Tier::Plain ? 0.5 : 0.1;
				};
				static const char* kLabels[] = { "Portion 1", "Portion 2", "Portion 3", "Portion 4", "Portion 5" };
				hc::Result r;
				hc::Tier   best = hc::Tier::Burnt;
				bool       bestRaw = true;
				double     q = 0.0;
				for (int i = 0; i < n; ++i) {
					const auto [t, raw] = g_portionTiers[i];
					if (t > best || (t == best && !raw)) {
						best = t;
						bestRaw = raw;
					}
					q += score(t, raw);
					if (r.count < 5) {
						r.lines[r.count++] = { n == 1 ? "The timing" : kLabels[i], verdict(t, raw), score(t, raw) };
					}
				}
				for (int i = 0; i < g_result.count && r.count < 8; ++i) {
					r.lines[r.count++] = g_result.lines[i];
				}
				r.quality = q / (std::max)(1, n);
				r.tier = best;
				r.burntWhy = bestRaw ? "It came off the fire raw" : "It stayed on past its time";
				g_result = r;
			}
			TakePackLogs();
			g_given = nullptr;
			for (const auto& [t, raw] : g_portionTiers) {
				Recipes::Consume(*g_recipe);
				auto* gv = Recipes::Give(*g_recipe, t);
				if (!g_given) {
					g_given = gv;
				}
			}
			g_givenName = g_given && g_given->GetName() ? g_given->GetName() : g_recipe->name;
			if (n > 1) {
				g_givenName = fmt::format("{} portions of {}", n, g_recipe->name);
			}
			g_burntLook = g_result.tier == hc::Tier::Burnt;
			g_phase = Phase::Result;
			g_serveAt = g_clock;
			Audio::Play({ g_result.tier == hc::Tier::Burnt ? "burnt" : "done", 0.9f });
			spdlog::info("Served {}: {} (quality {:.2f})", g_recipe->name, hc::TierName(g_result.tier), g_result.quality);
			// the dish itself, on the plate or in the bowl
			for (auto& u : g_units) {
				Scene::Hide(u.prop, true);
				HideBoardBits(u);
			}
			for (auto& p : g_pieces) {
				Scene::Hide(p.prop, true);
				Scene::Hide(p.core, true);
			}
			if (P.dough >= 0) {
				Scene::Hide(P.dough, true);
			}
			if (P.disk >= 0) {
				Scene::Hide(P.disk, true);
			}
			for (auto& [id, at] : P.fill) {
				Scene::Hide(id, true);
			}
			for (auto* v : { &P.crimps, &P.pieRim, &P.galette, &P.leafBits, &P.dumpFolds }) {
				for (auto id : *v) {
					Scene::Hide(id, true);
				}
			}
			if (n > 1) {
				return;  // our version: the portions stay on their plates, the kitchen in view
			}
			if (P.bowl >= 0) {
				Scene::Hide(P.bowl, true);
			}
			if (P.result >= 0) {
				Scene::Remove(P.result);
			}
			P.result = AddResult();
			Scene::SetCamera({ L::kServeX, L::kServeY + 2.0, 1.0 }, 42.0f);
		}

		// ---------------------------------------------------------------- picking
		bool NearScreen(const Vec3& a_p, float a_mx, float a_my, double a_r, float a_minPx = 16.0f)
		{
			float sx = 0, sy = 0;
			if (!Scene::Project(a_p, sx, sy)) {
				return false;
			}
			const float r = (std::max)(S(a_minPx), static_cast<float>(a_r) * Scene::PixelsPerUnit(a_p));
			return std::hypot(sx - a_mx, sy - a_my) <= r;
		}

		bool OnPlane(float a_mx, float a_my, double a_z, double& a_x, double& a_y) { return Scene::Unproject(a_mx, a_my, a_z, a_x, a_y); }

		bool OverBoard(float a_mx, float a_my, double& a_lx, double& a_ly)
		{
			double x = 0, y = 0;
			if (!OnPlane(a_mx, a_my, kBoardTop, x, y)) {
				return false;
			}
			a_lx = x - L::kBoardX;
			a_ly = y - g_boardY;
			return std::abs(a_lx) <= L::kBoardW * 0.5 && std::abs(a_ly) <= L::kBoardD * 0.5;
		}

		bool OverPot(float a_mx, float a_my, double a_extra = 0.0)
		{
			double x = 0, y = 0;
			return OnPlane(a_mx, a_my, L::kPotRim, x, y) && std::hypot(x - L::kPotX, y - L::kPotY) <= L::kPotR + a_extra;
		}

		bool OverFire(float a_mx, float a_my)
		{
			if (g_flow == hc::Flow::Oven) {
				double x = 0, y = 0;
				return OnPlane(a_mx, a_my, OvenFloor() + 1.0, x, y) && std::abs(x - L::kOvenX) < 7.0 && y > L::kOvenMouthY - 4.0 && y < L::kOvenY + 7.0;
			}
			double x = 0, y = 0;
			return OnPlane(a_mx, a_my, 1.0, x, y) && std::hypot(x - L::kFireX, y - L::kFireY) <= L::kFireR + 1.5;
		}

		bool OverServe(float a_mx, float a_my) { return NearScreen({ L::kServeX, L::kServeY, 1.0 }, a_mx, a_my, 5.0, 40.0f); }

		// The bowl or plate with room under the hand (-1: none)
		int SlotUnder(float a_mx, float a_my)
		{
			for (int k = 0; k < static_cast<int>(g_slots.size()); ++k) {
				if (g_slots[k].count < PerSlot() && NearScreen(SlotAt(k) + Vec3{ 0, 0, 1.0 }, a_mx, a_my, ServeCount() > 1 ? 4.5 : 5.0, ServeCount() > 1 ? 32.0f : 40.0f)) {
					return k;
				}
			}
			return -1;
		}

		Vec3 SpitAxis(double a_x) { return { L::kFireX + a_x, L::kFireY, L::kSpitZ }; }
		Vec3 CrankGrip()
		{
			const double a = g_spit ? g_spit->Angle() : 0.0;
			return { L::kFireX + L::kCrankOff + 0.95, L::kFireY - std::sin(a) * L::kCrankR, L::kSpitZ + std::cos(a) * L::kCrankR };
		}

		// all done but the baking: it takes the dish's own shape
		bool FoldedOver();
		const char* Btn(const char* a_xbox);  // a controller button's name, Xbox's or PlayStation's
		// down in the pot: darker deeper in and toward its wall (the renderer's hollow)
		void PotHollow(Scene::Look& a_lk, bool a_in)
		{
			if (!a_in) {
				a_lk.hollowDepth = 0.0f;
				return;
			}
			const double rim = L::kPotRim * 1.0815;
			a_lk.hollow[0] = static_cast<float>(L::kPotX);
			a_lk.hollow[1] = static_cast<float>(L::kPotY);
			a_lk.hollow[2] = static_cast<float>(rim);
			a_lk.hollow[3] = static_cast<float>(L::kPotR * 1.06);
			a_lk.hollowDepth = static_cast<float>(rim - L::kPotFloor * 1.0815);
		}
		// clam meat: near white, the palest orange (its own texture is a deep orange) - on the cloth, the board, in the pot
		void ClamLook(const Unit& u, Scene::Look& lk)
		{
			// berries and grapes: no shadow on them or from them (small, dark and glossy, they went black in it)
			{
				std::string n = KeyOf(u);
				std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (n.find("berr") != std::string::npos || n.find("grape") != std::string::npos) {
					lk.shadow = false;
					lk.unshadowed = true;
				}
			}
			if (u.name.find("Clam") == std::string::npos && u.name.find("clam") == std::string::npos) {
				return;
			}
			static constexpr float kClam[3] = { 0.98f, 0.9f, 0.8f };
			for (int k = 0; k < 3; ++k) {
				lk.lift[k] = kClam[k] * 0.85f + lk.lift[k] * 0.15f;
			}
			lk.lift[3] = (std::max)(lk.lift[3], 0.6f);
		}
		// what's chopped for the pot, in the order it goes in: a tomato first (it cooks down), the rest after
		int ChopRank(const std::string& a_name)
		{
			std::string n = a_name;
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			// (a tomato cooks down first; a chowder's clam goes in before its potato)
			return n.find("tomato") != std::string::npos ? 0 : n.find("clam") != std::string::npos ? 1 : 2;
		}
		bool DoughReady()
		{
			if (g_topping) {
				bool topped = true;
				for (const auto& u : g_units) {
					topped &= !u.dice || u.at == Unit::At::Dough;
				}
				return g_oven && g_buttered && BreadCut() && topped && !g_oven->Out();
			}
			return g_oven && g_doughOnBoard && g_doughParts > 0 && (g_doughNeeded == 0 || (g_doughParts >= g_doughNeeded && g_oven->Dough() >= 0.9)) &&
			       (!g_oven->Pie() || (g_rolled && g_oven->Crimped() >= 0.9)) && !g_oven->Out() && FoldedOver();
		}
		// a dumpling's last corner well over (three quarters of its fold) before it shows as the dumpling
		bool FoldedOver()
		{
			if (!Dumpling()) {
				return true;
			}
			for (int k = 0; k < 4; ++k) {
				if (g_fold[k] < 0.75) {
					return false;
				}
			}
			return true;
		}
		int BreadUnit()
		{
			for (int i = 0; i < static_cast<int>(g_units.size()); ++i) {
				std::string n = KeyOf(g_units[i]);
				std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (Lower(g_units[i].name) == "bread") {
					return i;
				}
			}
			return -1;
		}
		bool BreadOnBoard()
		{
			const int b = BreadUnit();
			return g_topping && b >= 0 && g_units[b].at == Unit::At::Board;
		}
		bool BreadCut()
		{
			const int b = BreadUnit();
			// a garlic bread's loaf in five slices (four cuts), one for each it makes
			const int need = g_recipe && g_recipe->makes > 1 ? (std::min)(g_recipe->makes - 1, hc::Cutting::kMaxCuts) : 1;
			return b >= 0 && g_units[b].cut.cutCount >= need;
		}
		bool OverMix(float mx, float my) { return NearScreen(kMixAt + Vec3{ 0, 0, 1.6 }, mx, my, 4.2, 40.0f); }
		bool OverFill(float mx, float my) { return NearScreen(kFillAt + Vec3{ 0, 0, 1.4 }, mx, my, 6.0, 70.0f); }
		// The big dough kneaded on the board, waiting for the knife
		bool CutStage()
		{
			return NeedCut() && g_oven && g_doughOnBoard && !g_topping && g_doughParts >= g_doughNeeded && g_oven->Dough() >= 0.9 && !g_oven->InOven() &&
			       !g_peelLoaded && !g_rolled && g_flat <= 0.0 && g_hold == Hold::None;
		}

		// How far the dough ball reaches each way along x (for the knife and the cut pieces)
		double DoughRadius()
		{
			const double grow = Clamp(static_cast<double>(g_doughParts) / (std::max)(1, g_doughNeeded), 0.3, 1.0);
			return 3.0 * std::cbrt(grow) * (FlourRecipe() && ServeCount() > 1 ? std::cbrt(static_cast<double>(ServeCount())) * 0.85 : 1.0);
		}

		bool AnyOnBoard()
		{
			for (const auto& u : g_units) {
				if (u.at == Unit::At::Board) {
					return true;
				}
			}
			return false;
		}
		// something already being cut (or its pieces still on the board): one thing on the board at a time
		bool OneOnBoard() { return AnyOnBoard() || g_chunks.CountIn(hc::Chunks::State::Board) > 0; }
		bool CutsWaiting()
		{
			if (!BreadCuts() || g_cutInMix) {
				return false;
			}
			for (const auto& u : g_units) {
				if (u.dice) {
					return true;
				}
			}
			return false;
		}

		int InFillBowl()
		{
			if (BreadCuts() && g_cutInMix) {
				return 0;  // (poured into the mixing bowl)
			}
			int n = 0;
			for (int b = 0; b < g_chunks.Count() && b < static_cast<int>(g_pieces.size()); ++b) {
				n += g_chunks.At(b).state == hc::Chunks::State::Pot && !g_pieces[b].onDough;
			}
			return n;
		}
		Vec3 PeelRest() { return { -1.0, -17.0, 0.3 }; }
		Vec3 MouthAt() { return { L::kOvenX, L::kOvenMouthY - 0.6, OvenFloor() + 2.5 }; }
		Vec3 BreadAt() { return { L::kOvenX, L::kOvenMouthY + 4.2, OvenFloor() }; }
		// over the oven's mouth or where the bake sits in it (generous: it's a peel, not a pin)
		bool InMouth(float a_mx, float a_my) { return NearScreen(MouthAt(), a_mx, a_my, 8.0, 70.0f) || NearScreen(BreadAt() + Vec3{ 0, 0, 1.0 }, a_mx, a_my, 7.0, 70.0f); }
		Vec3 DoorCentre()
		{
			const bool   g = P.game[static_cast<int>(Kit::Role::Oven)];
			const double sc = g ? 0.7 : 1.0, wide = g ? 0.95 : 1.0;
			return { L::kOvenX - (g ? 3.22 : 4.9) + std::cos(-g_door * 1.75) * 5.0 * sc * wide, L::kOvenMouthY - (g ? 2.2 : 0.35) + std::sin(-g_door * 1.75) * 5.0 * sc * wide, L::kOvenFloorZ + (g ? 1.4 : 0.0) + 3.2 * sc * (g ? 0.82 : 1.0) };
		}

		int UnitAt(float a_mx, float a_my, Unit::At a_where)
		{
			for (int i = 0; i < static_cast<int>(g_units.size()); ++i) {
				const auto& u = g_units[i];
				if (u.at != a_where) {
					continue;
				}
				Vec3 at = u.clothAt + Vec3{ 0, 0, 1.0 };
				if (a_where == Unit::At::Spit) {
					at = SpitAxis(u.spitX);
				}
				if (NearScreen(at, a_mx, a_my, a_where == Unit::At::Spit ? 3.5 : 3.0, 26.0f)) {
					return i;
				}
			}
			return -1;
		}

		bool AllInPot()
		{
			if (g_sauceN > 0) {
				return false;  // (a sauce's bowl not poured in yet)
			}
			for (const auto& u : g_units) {
				if (u.at == Unit::At::Cloth || u.at == Unit::At::Board || u.at == Unit::At::Held) {
					return false;
				}
			}
			return true;
		}

		int Mounted()
		{
			int n = 0;
			for (const auto& u : g_units) {
				n += u.at == Unit::At::Spit;
			}
			return n;
		}

		int MeatCount()
		{
			int n = 0;
			for (const auto& u : g_units) {
				n += u.role == Role::Meat;
			}
			return n;
		}

		// how long a meat lies along the spit: shorter when it shares it
		double MeatLen(const Unit& u)
		{
			const int n = MeatCount();
			return Clamp(u.dims[0] * 0.5, 4.0, n > 2 ? 5.0 : n > 1 ? 7.0 : 9.0);
		}

		// a free place along the rod, spread for however many meats there are
		double FreeSpitSpot(const Unit* a_self)
		{
			static constexpr double k1[1] = { 0.0 }, k2[2] = { -4.0, 4.0 }, k3[3] = { -6.0, 0.0, 6.0 }, k4[4] = { -7.5, -2.5, 2.5, 7.5 };
			const int     n = (std::min)(4, (std::max)(1, MeatCount()));
			const double* spots = n == 1 ? k1 : n == 2 ? k2 : n == 3 ? k3 : k4;
			for (int k = 0; k < n; ++k) {
				bool used = false;
				for (const auto& o : g_units) {
					used |= &o != a_self && o.at == Unit::At::Spit && std::abs(o.spitX - spots[k]) < 0.5;
				}
				if (!used) {
					return spots[k];
				}
			}
			return 0.0;
		}

		// ---------------------------------------------------------------- particles
		void Spray(const Vec3& a_at, int a_n, const float (&a_col)[4], double a_speed, double a_up, float a_size, bool a_gravity = true,
			Render3D::Sprite::Kind a_kind = Render3D::Sprite::Kind::Grain, double a_life = 0.9)
		{
			for (int k = 0; k < a_n && g_grains.size() < 600; ++k) {
				Grain g;
				g.p = a_at + Vec3{ (Rand01() - 0.5) * 0.6, (Rand01() - 0.5) * 0.6, 0.0 };
				const double a = Rand01() * 2 * kPi;
				g.v = { std::cos(a) * a_speed * Rand01(), std::sin(a) * a_speed * Rand01(), a_up * (0.5 + Rand01()) };
				g.life = a_life * (0.6 + 0.8 * Rand01());
				std::memcpy(g.col, a_col, sizeof(g.col));
				g.size = a_size * (0.7f + 0.6f * Rand01());
				g.kind = a_kind;
				g.gravity = a_gravity;
				g_grains.push_back(g);
			}
		}

		void SaltOn(const Vec3& a_at, double a_amount)
		{
			static const float white[4] = { 0.95f, 0.95f, 0.93f, 1.0f };
			Spray(a_at + Vec3{ 0, 0, 5.0 }, static_cast<int>(10 + a_amount * 40), white, 1.2, -2.0, 0.09f);
			Audio::Take("salt_", 0.6f);
		}

		void Flour(const Vec3& a_at)
		{
			static const float dust[4] = { 0.92f, 0.9f, 0.86f, 0.35f };
			Spray(a_at + Vec3{ 0, 0, 0.8 }, 10, dust, 3.0, 2.0, 1.6f, false, Render3D::Sprite::Kind::Smoke, 1.4);
			Audio::Take("flour_", 0.7f);
		}

		// The order a dough's things go into its bowl, by recipe: groups of words, each group after the ones before
		std::vector<std::vector<std::string>> DoughOrder()
		{
			std::string n = g_recipe ? g_recipe->key : std::string();
			std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (n.find("potato bread") != std::string::npos) {
				return { { "milk" }, { "egg" }, { "flour", "butter" } };  // (then the potato poured in, the salt, and stirred)
			}
			if (n.find("sweet roll") != std::string::npos) {
				return { { "milk" }, { "egg" }, { "flour" }, { "butter" } };
			}
			if (n.find("apple pie") != std::string::npos) {
				return { { "egg" }, { "flour" }, { "butter" } };  // (after the apples are cut): the egg, the flour, the butter
			}
			if (n == "bread") {
				return { { "milk" }, { "flour", "butter" }, { "egg" } };  // the milk, then the flour, then the egg
			}
			if (n.find("crostata") != std::string::npos) {
				return { { "flour" }, { "butter" } };  // the flour, then the butter cut into it
			}
			return {};
		}

		// The dough's things laid out on the cloth in the order they go in (the first at the top)
		void ArrangeCloth()
		{
			{
				// what's chopped for the pot: a tomato at the top, the rest under it, in their own spots
				std::vector<int> idx;
				std::vector<Vec3> spots;
				for (int i = 0; i < static_cast<int>(g_units.size()); ++i) {
					if (g_units[i].role == Role::Chop) {
						idx.push_back(i);
						spots.push_back(g_units[i].clothAt);
					}
				}
				// (their spots as laid out: down a column, then the next)
				std::sort(spots.begin(), spots.end(), [](const Vec3& a, const Vec3& b) { return std::abs(a.x - b.x) > 1.0 ? a.x < b.x : a.y > b.y; });
				std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return ChopRank(KeyOf(g_units[a])) < ChopRank(KeyOf(g_units[b])); });
				for (std::size_t k = 0; k < idx.size(); ++k) {
					g_units[idx[k]].clothAt = spots[k];
				}
			}
			const auto order = DoughOrder();
			if (order.empty()) {
				return;
			}
			auto rank = [&](const Unit& a_u) {
				std::string n = KeyOf(a_u);
				std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				for (int g = 0; g < static_cast<int>(order.size()); ++g) {
					for (std::size_t w = 0; w < order[g].size(); ++w) {
						if (n.find(order[g][w]) != std::string::npos) {
							return g * 8 + static_cast<int>(w);
						}
					}
				}
				return 1000;
			};
			std::vector<int> idx;
			for (int i = 0; i < static_cast<int>(g_units.size()); ++i) {
				if (g_units[i].role == Role::Dough) {
					idx.push_back(i);
				}
			}
			std::vector<Vec3> spots;
			for (int i : idx) {
				spots.push_back(g_units[i].clothAt);  // (their spots, top to bottom as they were given)
			}
			// (the spots read top to bottom, left to right on a row, whatever order the things were given in: the first
			// goes in is never below the next)
			std::sort(spots.begin(), spots.end(), [](const Vec3& a, const Vec3& b) { return std::abs(a.y - b.y) > 1.0 ? a.y > b.y : a.x < b.x; });
			std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return rank(g_units[a]) < rank(g_units[b]); });
			for (std::size_t k = 0; k < idx.size(); ++k) {
				g_units[idx[k]].clothAt = spots[k];
			}
			// side by side on one row they crowd (a sack of flour against a bowl of butter): the first up above the next
			if (idx.size() >= 2) {
				auto&       first = g_units[idx[0]].clothAt;
				const auto& next = g_units[idx[1]].clothAt;
				if (std::abs(first.y - next.y) < 1.0) {
					first.y = next.y + 5.5;
				}
			}
		}

		// What has to go in before this (lower-case name): "" if nothing's waiting
		std::string MustGoFirst(const std::string& a_name)
		{
			const auto order = DoughOrder();
			auto group = [&](const std::string& a_n) {
				for (int g = 0; g < static_cast<int>(order.size()); ++g) {
					for (const auto& w : order[g]) {
						if (a_n.find(w) != std::string::npos) {
							return g;
						}
					}
				}
				return -1;
			};
			const int mine = group(a_name);
			if (mine <= 0) {
				return {};
			}
			for (const auto& u : g_units) {
				std::string n = KeyOf(u);
				std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				const int g = group(n);
				if (g >= 0 && g < mine && u.at != Unit::At::Dough && u.at != Unit::At::Pot) {
					return u.name;
				}
			}
			return {};
		}

		// The bowl tipped over the dough: the fruit pours out onto it, a neat mound in the middle
		void PourOnDough()
		{
			if (g_topping && !g_buttered) {
				Toast("Butter the bread first");
				return;
			}
			if (g_topping) {
				g_bowlPoured = true;
				int k = 0;
				for (int b = 0; b < g_chunks.Count() && b < static_cast<int>(g_pieces.size()); ++b) {
					auto& body = g_chunks.At(b);
					auto& pc = g_pieces[b];
					if (body.state != hc::Chunks::State::Pot || pc.onDough) {
						continue;
					}
					// scattered along the slices
					const double t = (k % 12) / 11.0 - 0.5, w = ((k * 7) % 5) / 4.0 - 0.5;
					pc.onDough = true;
					pc.doughOff = { t * 7.0, w * 2.0, 0.6 + 0.2 * (k / 12) };
					pc.turn = Hash(b, 41) * 6.28;
					body.state = hc::Chunks::State::Lost;
					g_units[pc.unit].at = Unit::At::Dough;
					++k;
				}
				Audio::Take("knead_", 0.45f);
				return;
			}
			if (g_doughNeeded > 0 && g_oven->Dough() < 0.9) {
				Toast("Knead the dough first");
				return;
			}
			if (g_oven->Pie() && g_flat < 1.0) {
				Toast(Dumpling() ? "Roll the dough out square first" : "Roll the dough out first");
				return;
			}
			g_bowlPoured = true;
					// onto the dough, heaped in the middle (the edge is left for folding)
					int k = 0;
					for (int b = 0; b < g_chunks.Count() && b < static_cast<int>(g_pieces.size()); ++b) {
						auto& body = g_chunks.At(b);
						auto& pc = g_pieces[b];
						if (body.state != hc::Chunks::State::Pot || pc.onDough) {
							continue;
						}
						// a neat mound in the middle, well inside the edge that folds up over it
						const double r = 0.42 * std::sqrt(static_cast<double>(k % 24) + 0.5), a = k * 2.39996;
						pc.onDough = true;
						pc.doughOff = { std::cos(a) * (std::min)(r, 2.2), std::sin(a) * (std::min)(r, 2.2), 0.2 + 0.35 * (k / 24) + 0.25 * (1.0 - (std::min)(r, 2.2) / 2.2) };
						pc.turn = Hash(b, 41) * 6.28;
						body.state = hc::Chunks::State::Lost;
						g_units[pc.unit].at = Unit::At::Dough;
						++k;
					}
					if (k > 0) {
						if (!g_rolled) {
							g_rolled = true;  // flattened out to take it
						}
						for (int r = 0; r < 2; ++r) {
							for (int a = 0; a < 16; ++a) {
								const double ang = a / 16.0 * 2.0 * kPi, rad = r ? 0.75 : 0.35;
								g_oven->Fill(std::cos(ang) * rad, std::sin(ang) * rad);
							}
						}
						Audio::Take("knead_", 0.45f);
					}
		}

		// ---------------------------------------------------------------- hands: picking up
		void Grab(float mx, float my)
		{
			if (g_phase != Phase::Cooking || g_confirm) {
				return;
			}
			const bool pot = g_flow == hc::Flow::Pot, spit = g_flow == hc::Flow::Spit, oven = g_flow == hc::Flow::Oven;
			// the rolling pin in hand: pressed down, to roll
			if (oven && g_pinHeld) {
				g_hold = Hold::Roll;
				g_pushAcc = 0.0;
				g_doughLastOk = false;
				return;
			}
			// a sauce's bowl, with something in it: lifted, to pour into the pot
			if (pot && SauceBowl() && g_sauceN > 0 && OverSauceBowl(mx, my)) {
				g_hold = Hold::Sauce;
				return;
			}
			// the knife comes down where it is
			if ((pot || oven) && g_knifeOn && g_knifeAnim < 0 && g_tip <= 0.001) {
				g_knifeAnim = 0.0;
				g_knifeCut = false;
				return;
			}
			// the board, by its left end
			if ((pot || (oven && AnyOnBoard() && !g_doughOnBoard && !BreadOnBoard())) && P.board >= 0) {
				double lx = 0, ly = 0;
				if (OverBoard(mx, my, lx, ly) && lx < -L::kBoardW * 0.5 + 3.6) {
					bool any = g_chunks.CountIn(hc::Chunks::State::Board) > 0;
					for (const auto& u : g_units) {
						any |= u.at == Unit::At::Board;
					}
					// diced, not sliced: whatever isn't long and thin is cut both ways before the board lifts
					for (const auto& u : g_units) {
						if (any && u.at == Unit::At::Board && (u.bulb || (u.thin && u.cut.cutCount == 0))) {
							Toast(u.bulb ? "Knock it apart into its cloves with the knife, then dice them" : "Cut it first");
							return;
						}
						if (any && u.at == Unit::At::Board && !u.thin && (u.cut.cutCount == 0 || u.cut.alongCount == 0)) {
							Toast(u.cut.cutCount == 0 ? "Cut it, then right-click to turn the knife and cut it the other way" : "Now right-click to turn the knife and cut it the other way");
							return;
						}
					}
					if (any) {
						g_hold = Hold::Board;
						g_boardCarry = g_boardPose.c;
						double x = 0, y = 0;
						OnPlane(mx, my, kBoardTop, x, y);
						g_grabX = x;
						g_grabY = y - g_boardY;
						return;
					}
				}
			}
			// the ladle (anywhere in the pot)
			if (pot && (OverPot(mx, my, -0.6) || NearScreen({ L::kPotX + std::cos(g_ladleA) * g_ladleR, L::kPotY + std::sin(g_ladleA) * g_ladleR, L::kLiquidZ + 3.0 }, mx, my, 3.0))) {
				g_hold = Hold::Ladle;
				return;
			}
			// the spit's crank, and the roast on it
			if (spit && NearScreen(CrankGrip(), mx, my, 2.6, 30.0f)) {
				g_hold = Hold::Crank;
				float hx = 0, hy = 0;
				Scene::Project({ L::kFireX + L::kCrankOff, L::kFireY, L::kSpitZ }, hx, hy);
				g_crankPrev = std::atan2(my - hy, mx - hx);
				return;
			}
			if (spit) {
				if (const int k = UnitAt(mx, my, Unit::At::Spit); k >= 0) {
					g_hold = Hold::Meat;
					g_holdUnit = k;
					return;
				}
			}
			// the oven: its door, what's in it, the dough
			if (oven) {
				if (g_oven->InOven() && g_peelT < 0 && NearScreen(BreadAt() + Vec3{ 0, 0, 1.5 }, mx, my, 4.0, 30.0f)) {
					Toast(g_door > 0.6 ? "Too hot for hands: take it out with the peel" : "Open the door, then take it out with the peel");
					return;
				}
				if (g_peelBread && g_peelT < 0 && P.peel >= 0) {
					// the bake resting on the peel: picking it (or the peel) up carries both
					if (NearScreen(PeelRest() + Vec3{ 0, 0, 1.0 }, mx, my, 6.0, 60.0f)) {
						g_hold = Hold::Peel;
						g_peelAt = PeelRest();
						return;
					}
				}
				// the bowls: a ball of dough to lift out, flour to stir, the cut filling to carry over
				if (P.mixBowl >= 0 && OverMix(mx, my)) {
					if (g_ball && !g_doughOnBoard) {
						g_hold = Hold::Ball;
						return;
					}
					if (!g_ball && g_doughParts > 0) {
						g_hold = Hold::Mix;
						double x = 0, y = 0;
						if (OnPlane(mx, my, 1.8, x, y)) {
							g_mixA = std::atan2(y - kMixAt.y, x - kMixAt.x);
						}
						return;
					}
				}
				if (P.fillBowl >= 0 && InFillBowl() > 0 && OverFill(mx, my)) {
					g_hold = Hold::Apples;
					return;
				}
				if (P.peel >= 0 && g_peelT < 0 && NearScreen(PeelRest(), mx, my, 4.5, 40.0f)) {
					g_hold = Hold::Peel;
					g_peelLoaded = false;
					g_peelAt = PeelRest();
					return;
				}
				if (NearScreen(DoorCentre(), mx, my, 3.5, 30.0f)) {
					g_hold = Hold::Door;
					g_grabX = mx;
					g_grabY = g_door;
					return;
				}
				if (g_doughParts > 0 && g_doughOnBoard && !g_oven->InOven() && !g_oven->Out()) {
					double x = 0, y = 0;
					if (OnPlane(mx, my, kBoardTop + 1.0, x, y)) {
						const double dx = x - g_doughAt.x, dy = y - g_doughAt.y, r = std::hypot(dx, dy);
						const double R = g_rolled ? 5.0 : 3.2;
						// the edge isn't folded or crimped till all the filling is on
						bool fillingOff = false;
						for (const auto& u : g_units) {
							fillingOff |= u.role == Role::Filling && u.at != Unit::At::Dough;
						}
						if (g_rolled && fillingOff && r > 2.6 && r < R + 1.2) {
							Toast(Dumpling() ? "All the filling on first, then fold" : Crostata() ? "All the fruit on first, then fold the edge" : "All the filling on first, then crimp");
							return;
						}
						if (g_rolled && Dumpling() && r > 2.6 && r < R + 1.2) {
							// a side to fold in: the quarter nearest where it was taken
							const double a = std::atan2(dy, dx);
							g_foldSide = static_cast<int>(std::lround((a - kPi * 0.25) / (kPi * 0.5) + 4.0)) % 4;  // the nearest corner
							g_hold = Hold::Crimp;
							return;
						}
						if (g_rolled && std::abs(r - R) < 1.2) {
							g_hold = Hold::Crimp;
							return;
						}
						if (g_rolled && r < R - 1.0 && g_stock > 0) {
							g_hold = Hold::Spread;
							g_lastSpread = { x, y, 0 };
							return;
						}
						if (r < R + 0.8) {
							g_hold = Hold::Dough;
							g_grabX = x;
							g_grabY = y;
							g_strokeLen = 0;
							g_pushAcc = 0.0;
							g_doughLastOk = false;
							return;
						}
					}
				}
			}
			// the rolling pin, taken up: rolls from the dough's middle
			// the rolling pin: a click takes it up (it goes back on the table once the dough's rolled out); in hand, held
			// down, it presses and rolls over the dough
			if (oven && P.pin >= 0 && g_oven->Pie()) {
				const bool ready = g_doughOnBoard && !g_rolled && g_oven->Dough() >= 0.9 && g_flat < 1.0 && !NeedCut();
				if (NearScreen({ L::kBoardX, L::kBoardY - L::kBoardD * 0.5 - 1.3, 0.72 }, mx, my, 5.0, 40.0f)) {
					if (ready) {
						g_pinHeld = true;
					} else {
						Toast(g_flat >= 1.0 || g_rolled ? "It's rolled out already" : NeedCut() && g_oven->Dough() >= 0.9 ? fmt::format("Cut the dough into {} with the knife first", ServeCount()) : "Knead the dough first, then roll it out");
					}
					return;
				}
			}
			// salt
			if (P.salt >= 0 && NearScreen({ L::kSaltX, L::kSaltY, 1.5 }, mx, my, 2.6, 26.0f)) {
				g_hold = Hold::Salt;
				g_pinch = 0;
				return;
			}
			// logs: from the pile
			for (int k = 0; k < 3; ++k) {
				if (NearScreen({ L::kPileX, L::kPileY + (k - 1) * 1.6, 1.5 }, mx, my, 3.0, 22.0f)) {
					if (g_recipe && LogsLeft() <= 0) {
						Toast("No firewood left");
						return;
					}
					g_hold = Hold::Log;
					return;
				}
			}
			// (a log once in the fire stays there: it burns down, it isn't taken back out)
			// an ingredient off the cloth
			if (const int k = UnitAt(mx, my, Unit::At::Cloth); k >= 0) {
				g_hold = Hold::Ingredient;
				g_holdUnit = k;
				g_units[k].at = Unit::At::Held;
				return;
			}
		}

		// ---------------------------------------------------------------- hands: carrying
		void Carry(float mx, float my, double dt)
		{
			double x = 0, y = 0;
			switch (g_hold) {
			case Hold::Ingredient:
			case Hold::Log:
			case Hold::FireLog:
			case Hold::Meat:
			case Hold::Bread:
				if (OnPlane(mx, my, kCarryZ, x, y)) {
					g_carry = { x, y, kCarryZ };
				}
				break;
			case Hold::Board:
				if (g_flow == hc::Flow::Oven) {
					// lifted by its left end and carried; over the bowl it tips
					if (OnPlane(mx, my, 6.0, x, y)) {
						g_boardCarry = { x + L::kBoardW * 0.5 - 1.8, y, 6.0 };
					}
					{
						// it tips once its right end (the far one) is over the bowl
						const double ex = g_boardCarry.x + L::kBoardW * 0.5 - 2.0, ey = g_boardCarry.y;
						g_tipWant = std::hypot(ex - CutBowl().x, ey - CutBowl().y) < 4.5 ? 1.0 : 0.0;
					}
					break;
				}
				if (OnPlane(mx, my, kBoardTop, x, y)) {
					g_tipWant = Clamp((x - g_grabX) / 10.0, 0.0, 1.0);
					g_boardY = Clamp(y - g_grabY, L::kPotY - 8.0, L::kPotY + 8.0);
				}
				break;
			case Hold::Ladle:
				if (OnPlane(mx, my, L::kLiquidZ, x, y)) {
					const double dx = x - L::kPotX, dy = y - L::kPotY, r = std::hypot(dx, dy);
					if (r > L::kPotR + 3.0) {
						g_hold = Hold::LadleOut;  // lifted out
						Audio::Take("splash_", 0.35f);
						break;
					}
					const double a = std::atan2(dy, dx);
					const double dA = Wrap(a - g_ladleA);
					g_ladleA = a;
					g_ladleR = Clamp(r, 0.8, L::kPotR - 2.6);  // the cup well in, so the handle clears the rim
					if (g_pot && std::abs(dA) < 1.5) {
						g_pot->Stir(dA);
						g_stirred += std::abs(dA);
						if (g_stirred > g_stirGap && Audio::Now() - g_lastStir > 0.75) {
							g_stirred = 0;
							g_lastStir = Audio::Now();
							NextStirGap();
							Audio::Take("stir_", 0.5f);
						}
					}
				}
				break;
			case Hold::LadleOut:
				if (OnPlane(mx, my, kCarryZ, x, y)) {
					g_ladleOutAt = { x, y, kCarryZ };
					if (std::hypot(x - L::kPotX, y - L::kPotY) < L::kPotR - 1.0) {
						g_hold = Hold::Ladle;  // back in
					}
				}
				break;
			case Hold::Salt:
				g_pinch = (std::min)(1.4, g_pinch + dt / 0.5);
				if (OnPlane(mx, my, kCarryZ, x, y)) {
					g_carry = { x, y, kCarryZ };
				}
				break;
			case Hold::Crank:
				{
					float hx = 0, hy = 0;
					if (g_spit && Scene::Project({ L::kFireX + L::kCrankOff, L::kFireY, L::kSpitZ }, hx, hy)) {
						const double a = std::atan2(my - hy, mx - hx);
						const double dA = Wrap(a - g_crankPrev);
						g_crankPrev = a;
						if (std::abs(dA) < 1.2) {
							g_spit->Turn(dA);
							g_crankRun += std::abs(dA);
							if (g_crankRun > kPi) {
								g_crankRun = 0;
								Audio::Take("crank_", 0.45f);
							}
						}
					}
				}
				break;
			case Hold::Mix:
				if (OnPlane(mx, my, 1.8, x, y)) {
					const double a = std::atan2(y - kMixAt.y, x - kMixAt.x), dA = Wrap(a - g_mixA);
					g_mixA = a;
					if (CutsWaiting() && std::abs(dA) < 1.5 && g_doughParts >= (std::max)(1, g_doughNeeded)) {
						if (Audio::Now() - g_lastStir > 1.5) {
							g_lastStir = Audio::Now();
							Toast("Pour in what's in the right bowl first");
						}
					} else if (g_recipe->salted && g_oven->Seasoning().amount < 0.3 && std::abs(dA) < 1.5 && g_doughParts >= (std::max)(1, g_doughNeeded)) {
						if (Audio::Now() - g_lastStir > 1.5) {
							g_lastStir = Audio::Now();
							Toast("A pinch of salt first");
						}
					} else if (std::abs(dA) < 1.5 && g_doughParts >= (std::max)(1, g_doughNeeded)) {
						g_mix = (std::min)(1.0, g_mix + std::abs(dA) / (2.0 * kPi * 4.0));  // four turns round
						g_stirred += std::abs(dA);
						if (g_stirred > g_stirGap && Audio::Now() - g_lastStir > 0.3) {
							g_stirred = 0;
							g_lastStir = Audio::Now();
							NextStirGap();
							Audio::Take("knead_", 0.35f);
						}
						if (g_mix >= 1.0) {
							g_ball = true;
							g_hold = Hold::None;
							Toast("A ball of dough: lift it onto the board");
						}
					} else if (g_doughParts < (std::max)(1, g_doughNeeded) && Audio::Now() - g_lastStir > 1.5) {
						g_lastStir = Audio::Now();
						Toast("Everything for the dough goes in the bowl first");
					}
				}
				break;
			case Hold::Ball:
				if (OnPlane(mx, my, kCarryZ, x, y)) {
					g_carry = { x, y, kCarryZ };
				}
				break;
			case Hold::Apples:
				// carried well up, so it pours from over the dough instead of down in it
				if (OnPlane(mx, my, 8.0, x, y)) {
					g_carry = { x, y, 8.0 };
				}
				break;
			case Hold::Peel:
				if (OnPlane(mx, my, kBoardTop + 1.0, x, y)) {
					g_peelAt = { x, y, kBoardTop + 1.2 };
					// at the oven it's raised to the mouth (where the door is), not pushed in under it
					{
						double hx = 0, hy = 0;
						const double mz = OvenFloor() - 1.3;  // (low enough that what's on it clears the arch)
						if (OnPlane(mx, my, mz, hx, hy) && std::abs(hx - L::kOvenX) < L::kOvenMouthW && hy > L::kOvenMouthY - 9.0) {
							const double up = Clamp((hy - (L::kOvenMouthY - 9.0)) / 4.0, 0.0, 1.0);  // lifting as it comes up to it
							g_peelAt = g_peelAt + (Vec3{ hx, hy, mz } - g_peelAt) * up;
						}
					}
					// into the open oven for what's baking
					// held in the open oven's mouth a moment, it slides under the bake and takes it (passing by doesn't)
					const bool inMouth = g_door > 0.6 && InMouth(mx, my);
					if (!(inMouth && !g_peelBread && g_oven->InOven())) {
						g_peelDwell = -1.0;
					} else if (g_peelDwell < 0.0) {
						g_peelDwell = Audio::Now();  // (when it went in)
					}
					if (g_peelDwell >= 0.0 && Audio::Now() - g_peelDwell > 0.3) {
						g_oven->Unload();
						g_peelBread = true;
						g_peelDwell = -1.0;
					}
					// slid under the dough: it's on
					if (!g_peelLoaded && !g_peelBread && !g_oven->InOven() && !g_oven->Out() && g_doughOnBoard && std::hypot(x - g_doughAt.x, y - g_doughAt.y) < 3.5) {
						if (g_doughNeeded > 0 && g_doughParts < g_doughNeeded) {
							if (Audio::Now() - g_lastStir > 1.5) {
								g_lastStir = Audio::Now();
								Toast("The dough isn't all in yet");
							}
						} else if (NeedCut()) {
							if (Audio::Now() - g_lastStir > 1.5) {
								g_lastStir = Audio::Now();
								Toast(fmt::format("Cut the dough into {} with the knife first", ServeCount()));
							}
						} else {
							g_peelLoaded = true;
						}
					}
				}
				break;
			case Hold::Dough:
				if (OnPlane(mx, my, kBoardTop + 1.0, x, y)) {
					// kneading stays on the board: the dough never leaves it (the peel carries it to the oven)
					x = Clamp(x, L::kBoardX - L::kBoardW * 0.5 + 3.0, L::kBoardX + L::kBoardW * 0.5 - 3.0);
					y = Clamp(y, L::kBoardY - L::kBoardD * 0.5 + 3.0, L::kBoardY + L::kBoardD * 0.5 - 3.0);
					// a push: from where it was taken to where it's let go
					const double dx = x - g_grabX, dy = y - g_grabY;
					g_strokeLen = std::hypot(dx, dy);
					g_squash = Clamp(g_strokeLen / 6.0, 0.0, 1.0);
					g_squashA = std::atan2(dy, dx);
					g_carry = { x, y, kCarryZ };
				}
				break;
			case Hold::Sauce:
				if (OnPlane(mx, my, 9.0, x, y)) {
					g_carry = { x, y, 9.0 };
				}
				break;
			case Hold::Roll:
				// pressed down and rolled about over the dough: every bit of the way over it rolls it out
				if (OnPlane(mx, my, kBoardTop + 1.0, x, y)) {
					if (g_doughLastOk) {
						const double ddx = x - g_doughLast.x, ddy = y - g_doughLast.y, d = std::hypot(ddx, ddy);
						g_pinRoll += d;
						if (d > 0.02) {
							// the way it's going, smoothed; rolls left-right while that's mostly it, up-down once it's mostly that
							g_rollDX += (std::abs(ddx) / d - g_rollDX) * 0.25;
							g_rollDY += (std::abs(ddy) / d - g_rollDY) * 0.25;
							if (g_rollDX > g_rollDY * 1.4) {
								g_pinAxis = 0.0;
							} else if (g_rollDY > g_rollDX * 1.4) {
								g_pinAxis = 1.0;
							}
						}
						if (std::hypot(x - g_doughAt.x, y - g_doughAt.y) < 6.0) {
							g_pushAcc += d;
							if (g_pushAcc >= 2.0) {
								DoughPush(g_pushAcc);
								g_pushAcc = 0.0;
							}
						}
					}
					g_doughLast = { x, y, 0.0 };
					g_doughLastOk = true;
				}
				break;
			case Hold::Spread:
				if (OnPlane(mx, my, kBoardTop + 1.0, x, y) && g_stock > 0) {
					const double dx = x - g_doughAt.x, dy = y - g_doughAt.y;
					if (std::hypot(x - g_lastSpread.x, y - g_lastSpread.y) > 0.7 && std::hypot(dx, dy) < 4.6) {
						g_lastSpread = { x, y, 0 };
						g_oven->Fill(dx / 5.0, dy / 5.0);
						--g_stock;
						const auto id = Add("Chunk");
						P.fill.emplace_back(id, Vec3{ dx, dy, static_cast<double>(g_lastFill) });
						Audio::Take("knead_", 0.15f, 0.2f);
					}
				}
				break;
			case Hold::Crimp:
				if (g_foldSide >= 0 && OnPlane(mx, my, kBoardTop + 1.0, x, y)) {
					// a dumpling's side, drawn in over the filling: once it's past halfway in, it folds
					const double r = std::hypot(x - g_doughAt.x, y - g_doughAt.y);
					if (r < 2.0 && g_foldWant[g_foldSide] < 1.0) {
						g_foldWant[g_foldSide] = 1.0;
						const double a0 = kPi * 0.25 + g_foldSide * kPi * 0.5;
						for (int k = -3; k <= 3; ++k) {
							g_oven->Crimp(a0 + k * (kPi * 0.5 / 6.0));
						}
						Audio::Take("knead_", 0.4f);
					}
					break;
				}
				if (OnPlane(mx, my, kBoardTop + 1.0, x, y)) {
					const double dx = x - g_doughAt.x, dy = y - g_doughAt.y;
					if (std::abs(std::hypot(dx, dy) - 5.0) < 1.6) {
						const double a = std::atan2(dy, dx);
						const int    before = static_cast<int>(std::lround(g_oven->Crimped() * hc::Oven::kCrimps));
						g_oven->Crimp(a);
						const int after = static_cast<int>(std::lround(g_oven->Crimped() * hc::Oven::kCrimps));
						if (after > before) {
							Audio::Take("knead_", 0.2f, 0.3f);
						}
					}
				}
				break;
			case Hold::Door:
				{
					const float px = Scene::PixelsPerUnit({ L::kOvenX, L::kOvenMouthY, L::kOvenFloorZ });
					const double want = Clamp(g_grabY + (g_grabX - mx) / (px * 9.0), 0.0, 1.0);
					g_doorWant = want;
				}
				break;
			default:
				break;
			}
		}

		// ---------------------------------------------------------------- hands: letting go
		void PutBack(int a_unit)
		{
			if (a_unit >= 0 && a_unit < static_cast<int>(g_units.size())) {
				g_units[a_unit].at = Unit::At::Cloth;
			}
		}

		void Drop(float mx, float my)
		{
			const Hold h = g_hold;
			g_hold = Hold::None;
			const int ui = g_holdUnit;
			g_holdUnit = -1;
			switch (h) {
			case Hold::Ingredient:
				{
					if (ui < 0) {
						break;
					}
					auto& u = g_units[ui];
					double lx = 0, ly = 0;
					if (g_flow == hc::Flow::Pot) {
						if (u.role == Role::Chop && P.board >= 0 && g_tip < 0.05 && OverBoard(mx, my, lx, ly) && OneOnBoard()) {
							Toast("One at a time: into the pot with what's on the board first");
							PutBack(ui);
							break;
						}
						if (u.role == Role::Chop && P.board >= 0 && g_tip < 0.05 && OverBoard(mx, my, lx, ly)) {
							const int lane = FreeLane();
							if (lane >= 0 && Fit(u)) {
								u.at = Unit::At::Board;
								u.lane = lane;
								break;
							}
							Toast("The board is full");
						} else if (SauceBowl() && Liquid(u) && OverSauceBowl(mx, my)) {
							// poured into the sauce's bowl (the ale before the honey), the bottle stood beside it
							bool aleWaiting = false;
							std::string ln = u.name;
							std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							for (const auto& o : g_units) {
								std::string on = o.name;
								std::transform(on.begin(), on.end(), on.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
								aleWaiting |= on.find("ale") != std::string::npos && on.find("kale") == std::string::npos && !o.byBowl && o.at != Unit::At::Pot;
							}
							if (ln.find("honey") != std::string::npos && aleWaiting) {
								Toast("The ale goes in first");
								PutBack(ui);
								break;
							}
							u.at = Unit::At::Gone;
							u.byBowl = true;
							++g_sauceN;
							Audio::Play({ ln.find("honey") != std::string::npos ? "splash_0" : "pour", 0.6f });
							break;
						} else if (SauceBowl() && Liquid(u) && OverPot(mx, my, 1.0)) {
							Toast("Into the bowl first, then the bowl into the pot");
							PutBack(ui);
							break;
						} else if (OverPot(mx, my, 1.0)) {
							u.at = Unit::At::Pot;
							if (u.role == Role::Chop) {
								// in whole: a piece the size of the thing
								Fit(u);
								u.wentWhole = true;
								const double len = u.boardLen;
								const int    b = g_chunks.AddFree(ui, g_carry, {}, { len, Thick(u, len, 1), Thick(u, len, 2) }, static_cast<unsigned>(ui * 13 + 5));
								if (b >= 0) {
									const double box[6] = { -1e7, 1e7, -1e7, 1e7, -1e7, 1e7 };
									NewPiece(ui, b, box, {}, false);
								}
							} else {
								++g_tossedIn;
								const bool milk = u.role == Role::Toss && u.name.find("Milk") != std::string::npos;  // (milk: no sound)
								if (!milk)
								Audio::Play({ u.role == Role::Toss && (u.name.find("Ale") != std::string::npos ||
																	u.name.find("Wine") != std::string::npos || u.name.find("Mead") != std::string::npos)
												  ? "pour"
												  : "splash_0",
									0.525f });  // (a quarter quieter)
								static const float drop[4] = { 0.5f, 0.35f, 0.2f, 0.9f };
								Spray({ L::kPotX, L::kPotY, L::kLiquidZ }, 10, drop, 4.0, 18.0, 0.18f);
							}
							break;
						}
						PutBack(ui);
					} else if (g_flow == hc::Flow::Spit) {
						if (u.role == Role::Meat && NearScreen(SpitAxis(0.0), mx, my, 11.0, 60.0f)) {
							// salted first, and a fire laid under the spit, then it goes on
							if (g_recipe->salted && Mounted() == 0 && g_spit->Seasoning().amount < 0.7) {
								Toast("Salt the meat first");
								PutBack(ui);
								break;
							}
							if (g_fire.Logs() == 0) {
								Toast("Put firewood on the fire first");
								PutBack(ui);
								break;
							}
							if (Mounted() >= hc::timing::kSpitMax) {
								Toast("The spit is full");
								PutBack(ui);
								break;
							}
							u.at = Unit::At::Spit;
							Fit(u);
							u.spitX = FreeSpitSpot(&u);
							if (u.timer.cooked <= 0.0) {
								u.timer.time = g_timer.time;  // its own clock from now
							}
							u.timer.in = true;
							Audio::Take("log_", 0.3f);
							break;
						}
						if (u.role != Role::Meat && Mounted() > 0 && NearScreen(SpitAxis(0.0), mx, my, 9.0, 50.0f)) {
							u.at = Unit::At::Gone;  // rubbed on
							Audio::Take("knead_", 0.4f);
							break;
						}
						PutBack(ui);
					} else {
						double x = 0, y = 0;
						const bool onDough = OnPlane(mx, my, kBoardTop + 1.0, x, y) && std::hypot(x - g_doughAt.x, y - g_doughAt.y) < 7.0;
						if (g_topping && BreadUnit() == ui) {
							bool waiting = false;
							for (const auto& d : g_units) {
								waiting |= d.dice && d.at != Unit::At::Pot && d.at != Unit::At::Dough;
							}
							if (waiting) {
								Toast("Cut the garlic first, and into its bowl");
								PutBack(ui);
								break;
							}
							if (P.board >= 0 && OverBoard(mx, my, lx, ly) && !AnyOnBoard() && Fit(u)) {
								u.at = Unit::At::Board;
								u.lane = 1;
								g_doughAt = { L::kBoardX, L::kBoardY + L::kLaneY[1], kBoardTop };
								break;
							}
							PutBack(ui);
							break;
						}
						if (u.dice) {
							// cut on the board first (while the board's free of dough)
							if (P.board >= 0 && OverBoard(mx, my, lx, ly) && !g_doughOnBoard && OneOnBoard()) {
								Toast("One at a time: into its bowl with what's on the board first");
								PutBack(ui);
								break;
							}
							if (P.board >= 0 && OverBoard(mx, my, lx, ly)) {
								const int lane = g_doughOnBoard ? -1 : FreeLane();
								if (lane >= 0 && Fit(u)) {
									u.at = Unit::At::Board;
									u.lane = lane;
									break;
								}
								Toast(g_doughOnBoard ? "The dough has the board: cut it first next time" : "The board is full");
							} else {
								Toast(fmt::format("Cut the {} on the board first", u.name));
							}
							PutBack(ui);
							break;
						}
						if (g_topping && u.role == Role::Dough) {
							// the butter, spread over the cut bread
							if (BreadOnBoard() && BreadCut() && P.board >= 0 && OverBoard(mx, my, lx, ly)) {
								u.at = Unit::At::Dough;
								g_buttered = true;
								Audio::Take("knead_", 0.45f);
							} else {
								Toast(BreadOnBoard() ? "Cut the bread first, then butter it" : "Butter goes on the cut bread");
								PutBack(ui);
							}
							break;
						}
						if (u.role == Role::Dough && P.mixBowl >= 0 && OverMix(mx, my) && !g_ball) {
							bool waiting = false;
							std::string what;
							for (const auto& d : g_units) {
								if (d.dice && d.at != Unit::At::Pot && d.at != Unit::At::Dough) {
									waiting = true;
									what = d.name;
								}
							}
							if (waiting) {
								Toast(fmt::format("Cut the {} first, and into its bowl", what));
								PutBack(ui);
								break;
							}
							// some doughs go together in an order (a sweet roll's milk last; a potato bread's milk before its flour)
							std::string ln = u.name;
							std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							if (const auto before = MustGoFirst(ln); !before.empty()) {
								Toast(fmt::format("The {} goes in first", before));
								PutBack(ui);
								break;
							}
							g_milkIn |= ln.find("milk") != std::string::npos;
							u.at = Unit::At::Dough;
							++g_doughParts;
							if (ln.find("egg") != std::string::npos && !u.model.empty()) {
								// cracked over the bowl: the shell in two, the yolk and white into it
								for (auto* id : { &P.eggA, &P.eggB }) {
									if (*id >= 0) {
										Scene::Remove(*id);
									}
									*id = AddUnitMesh(u);
								}
								g_eggT = 0.0;
								Audio::Take("egg_", 0.7f, 0.05f);
							} else if (ln.find("milk") != std::string::npos) {
								// poured in, quietly (no flour dust off it)
							} else {
								Flour(kMixAt + Vec3{ 0, 0, 1.0 });
							}
							break;
						}
						if (u.role == Role::Dough) {
							Toast(g_bowlWater ? "Into the bowl of water, to make the dough" : "Into the mixing bowl, to make the dough");
							PutBack(ui);
							break;
						}
						if (u.role == Role::Filling && g_doughOnBoard && onDough && !g_oven->InOven()) {
							u.at = Unit::At::Dough;
							if (g_oven->Pie()) {
								if (!g_rolled) {
									g_rolled = true;
									Flour(g_doughAt);
								}
								// its berries (or fruit) scattered over the whole inside: all of them in, it's covered
								int total = 0, done = 0;
								for (const auto& o : g_units) {
									if (o.role == Role::Filling && !o.dice) {
										++total;
										done += o.at == Unit::At::Dough;
									}
								}
								const int per = 60 / (std::max)(1, total);
								for (int k = 0; k < per; ++k) {
									const int    n = (done - 1) * per + k;  // the next in one spiral over the lot
									const double r = 4.1 * std::sqrt((n + 0.5) / (per * (std::max)(1, total))), a = n * 2.39996;
									const double ox = std::cos(a) * r, oy = std::sin(a) * r;
									g_oven->Fill(ox / 5.0, oy / 5.0);
									P.fill.emplace_back(Add("Dough"), Vec3{ ox, oy, static_cast<double>(ui) });
								}
								if (done >= total) {
									for (int rr = 0; rr < 3; ++rr) {
										for (int aa = 0; aa < 24; ++aa) {
											const double ang = aa / 24.0 * 2.0 * kPi, rad = 0.25 + rr * 0.3;
											g_oven->Fill(std::cos(ang) * rad, std::sin(ang) * rad);
										}
									}
								}
								Audio::Take("knead_", 0.4f);
							} else {
								Audio::Take("knead_", 0.5f);  // worked into the dough
							}
							break;
						}
						PutBack(ui);
					}
				}
				break;
			case Hold::Log:
				if (g_flow == hc::Flow::Pot && OverFire(mx, my) && g_fire.Logs() == 0 &&
					(!AllInPot() || (g_recipe && g_recipe->salted && g_pot && g_pot->Seasoning().amount < 0.7))) {
					Toast(!AllInPot() ? "Everything into the pot first, then the firewood under it" : "Salt it first, then the firewood under it");
				} else if (OverFire(mx, my) && g_fire.AddLog()) {
					if (g_recipe) {
						UseLog();
					}
					Audio::Take("log_", 0.8f);
					static const float spark[4] = { 1.0f, 0.55f, 0.15f, 1.0f };
					Spray(FireCenter() + Vec3{ 0, 0, 1.0 }, 18, spark, 4.0, 12.0, 0.12f, false, Render3D::Sprite::Kind::Ember, 1.2);
				} else if (OverFire(mx, my)) {
					Toast("The fire can't take another log");
				}
				break;
			case Hold::FireLog:
				if (OverFire(mx, my)) {
					g_fire.AddLog();  // put back
				} else {
					static const float smoke[4] = { 0.3f, 0.28f, 0.26f, 0.5f };
					Spray(g_carry, 6, smoke, 1.5, 3.0, 1.4f, false, Render3D::Sprite::Kind::Smoke, 1.6);
					Hiss(0.4f);
				}
				break;
			case Hold::Board:
				g_tipWant = 0.0;  // (the oven's board goes back down where it was)
				break;
			case Hold::Mix:
				break;
			case Hold::Ball:
				{
					double lx = 0, ly = 0;
					if (P.board >= 0 && OverBoard(mx, my, lx, ly)) {
						if (AnyOnBoard()) {
							Toast("Clear the board first");
						} else {
							g_doughOnBoard = true;
							Flour(g_doughAt);
						}
					}
				}
				break;
			case Hold::Apples:
				break;  // (poured as it tipped; else the bowl goes back)
			case Hold::LadleOut:
				if (const int k = SlotUnder(mx, my); k >= 0) {
					if (g_chunks.CountIn(hc::Chunks::State::Pot) + g_tossedIn > 0) {
						Audio::Take("splash_", 0.6f);
						bool           raw = false;
						const hc::Tier t = g_timer.TierNow(raw);
						FillSlot(k, t, raw);
						if (AllServed()) {
							Serve();
						} else {
							Audio::Play({ t == hc::Tier::Burnt ? "burnt" : "done", 0.6f });
						}
					} else {
						Toast("There's nothing in the pot yet");
					}
				}
				break;
			case Hold::Salt:
				{
					Vec3 target;
					bool hit = false;
					if (g_flow == hc::Flow::Pot && OverPot(mx, my, 1.0)) {
						target = { L::kPotX, L::kPotY, L::kLiquidZ };
						g_pot->Seasoning().amount += g_pinch;
						hit = true;
					} else if (const int mu = g_flow == hc::Flow::Spit ? UnitAt(mx, my, Unit::At::Cloth) : -1; mu >= 0 && g_units[mu].role == Role::Meat) {
						target = g_units[mu].clothAt + Vec3{ 0, 0, 1.0 };  // rubbed into the meat before it goes on
						g_spit->Seasoning().amount += g_pinch;
						hit = true;
					} else if (g_flow == hc::Flow::Spit && Mounted() > 0 && NearScreen(SpitAxis(0.0), mx, my, 9.0, 50.0f)) {
						target = SpitAxis(0.0);
						g_spit->Seasoning().amount += g_pinch;
						hit = true;
					} else if (g_flow == hc::Flow::Oven && P.mixBowl >= 0 && !g_doughOnBoard && OverMix(mx, my) && CutsWaiting()) {
						Toast("Pour in what's in the right bowl first, then the salt");
						target = { g_carry.x, g_carry.y, 0.0 };
						hit = false;
						g_pinch = 0;
						break;
					} else if (g_flow == hc::Flow::Oven && P.mixBowl >= 0 && !g_doughOnBoard && OverMix(mx, my)) {
						target = kMixAt + Vec3{ 0, 0, 1.6 };  // into the bowl, with the flour
						g_oven->Seasoning().amount += g_pinch;
						hit = true;
					} else if (g_flow == hc::Flow::Oven && g_doughParts > 0 && g_doughOnBoard && !g_oven->InOven() && NearScreen(g_doughAt, mx, my, 5.0, 40.0f)) {
						target = g_doughAt;
						g_oven->Seasoning().amount += g_pinch;
						hit = true;
					}
					if (hit) {
						SaltOn(target, g_pinch);
					} else {
						SaltOn({ g_carry.x, g_carry.y, 0.0 }, g_pinch * 0.5);  // spilt
					}
					g_pinch = 0;
				}
				break;
			case Hold::Meat:
				if (ui >= 0) {
					const int k = SlotUnder(mx, my);
					if (k < 0) {
						break;  // back on the spit
					}
					auto& u = g_units[ui];
					u.timer.TakeOut();
					u.at = Unit::At::Gone;
					u.servedSlot = k;
					FillSlot(k, u.timer.tier, u.timer.raw);
					Audio::Play({ u.timer.tier == hc::Tier::Burnt ? "burnt" : "done", 0.6f });
					if (AllServed()) {
						Serve();
					}
				}
				break;
			case Hold::Peel:
				if (!g_peelBread && !g_peelLoaded && g_oven->InOven() && g_peelT < 0 && g_door > 0.6 && InMouth(mx, my)) {
					g_oven->Unload();
					g_peelBread = true;
					g_peelDwell = -1.0;
					break;  // it rests on the peel
				}
				if (g_peelBread) {
					if (OverServe(mx, my) || SlotUnder(mx, my) >= 0) {
						Serve();  // off the peel onto the plate
						g_peelBread = false;
					} else if (g_door > 0.6 && InMouth(mx, my)) {
						// back into the oven, to bake on
						g_peelFrom = g_peelAt;
						g_peelT = 0.0;
						g_peelIn = true;
						g_peelBread = false;
					}
					break;  // else it rests on the peel
				}
				if (g_peelLoaded && NearScreen(MouthAt(), mx, my, 6.0, 50.0f)) {
					if (g_fire.Logs() == 0) {
						Toast("Put firewood in the oven first");
						g_peelLoaded = false;
					} else if (g_door < 0.6) {
						Toast("Open the oven door first");
						g_peelLoaded = false;  // the dough slides back onto the board
					} else {
						g_peelFrom = g_peelAt;
						g_peelT = 0.0;  // in it goes
						g_peelIn = true;
					}
				} else if (g_peelLoaded) {
					g_peelLoaded = false;  // put down: the dough back on the board
					Toast("Carry it into the open oven");
				}
				g_peelLoaded = g_peelLoaded && g_peelT >= 0;
				break;
			case Hold::Dough:
				{
					double x = 0, y = 0;
					const bool board = OnPlane(mx, my, kBoardTop + 1.0, x, y) && std::abs(x - L::kBoardX) < L::kBoardW * 0.5 + 1.0 && std::abs(y - L::kBoardY) < L::kBoardD * 0.5 + 1.0;
					if (NearScreen(MouthAt(), mx, my, 6.0, 50.0f)) {
						Toast("Use the peel: drag it under the dough, then into the oven");
					} else if (board && g_strokeLen > 1.2 && !g_rolled && g_doughParts > 0 && g_oven->Pie() && g_oven->Dough() >= 0.9) {
						Toast("Pick up the rolling pin to roll it out");
					} else if (board && g_strokeLen > 1.2) {
						DoughPush(g_strokeLen);  // a push: kneaded
					}
					g_squash = 0;
				}
				break;
			case Hold::Door:
				g_doorWant = g_doorWant > 0.5 ? 1.0 : 0.0;
				break;
			case Hold::Sauce:
				break;  // (poured as it tipped over the pot; else it goes back)
			case Hold::Roll:
				g_pushAcc = 0.0;
				g_doughLastOk = false;  // (lifted: still in hand)
				break;
			case Hold::Bread:
				if (OverServe(mx, my) || SlotUnder(mx, my) >= 0) {
					Serve();
				}
				break;
			default:
				break;
			}
		}

		// A push of the dough a_len long: kneading it, or once kneaded (a pie's, a dumpling's) rolling it out
		void DoughPush(double a_len)
		{
			if (!g_oven || g_rolled || g_doughParts <= 0) {
				return;
			}
			if (g_oven->Pie() && g_oven->Dough() >= 0.9) {
				if (g_flat >= 1.0 || NeedCut()) {
					return;
				}
				g_flat = (std::min)(1.0, g_flat + a_len / 30.0);
				if (g_flat >= 1.0) {
					Toast("Rolled out: now the filling");
					g_pinHeld = false;  // the pin back on the table
					if (g_hold == Hold::Roll) {
						g_hold = Hold::None;
					}
				}
			} else {
				g_oven->Knead(a_len, g_clock);
				Audio::Take("knead_", 0.7f);
				static const float dust[4] = { 0.92f, 0.9f, 0.86f, 0.18f };
				Spray(g_doughAt + Vec3{ 0, 0, 1.0 }, 3, dust, 2.0, 1.0, 1.2f, false, Render3D::Sprite::Kind::Smoke, 1.0);
			}
		}

		void Cancel()
		{
			if (g_hold == Hold::Ingredient) {
				PutBack(g_holdUnit);
			} else if (g_hold == Hold::FireLog) {
				g_fire.AddLog();
			} else if (g_hold == Hold::Board) {
				g_tipWant = 0;
			}
			if (g_hold == Hold::None || g_hold == Hold::Roll) {
				g_pinHeld = false;  // the pin put back
			}
			g_hold = Hold::None;
			g_holdUnit = -1;
			g_pinch = 0;
			g_squash = 0;
		}

		void HandleInput(double dt)
		{
			float mx = 0, my = 0;
			Overlay::MousePos(mx, my);
			for (const auto& p : Overlay::TakePresses()) {
				if (p.button == 1 && p.down) {
					if (g_hold == Hold::None && (g_flow == hc::Flow::Pot || g_flow == hc::Flow::Oven) && g_phase == Phase::Cooking) {
						g_knifeAlong = !g_knifeAlong;  // the knife turns: across, along
					}
					Cancel();
					continue;
				}
				if (p.button != 0) {
					continue;
				}
				if (p.down) {
					if (!p.onUi && g_hold == Hold::None) {
						Grab(p.x, p.y);
					}
				} else if (g_hold != Hold::None) {
					Drop(p.x, p.y);
				}
			}
			if (g_hold != Hold::None && !Overlay::ButtonHeld(0)) {
				Drop(mx, my);  // a release that never arrived
			}
			if (g_hold != Hold::None) {
				Carry(mx, my, dt);
			}
			if (g_pinHeld) {
				double x = 0, y = 0;
				if (OnPlane(mx, my, kBoardTop + 1.0, x, y)) {
					g_pinAt = { x, y, 0.0 };
				}
			}
			// the knife follows the mouse over the board
			g_knifeOn = false;
			if ((g_flow == hc::Flow::Pot || (g_flow == hc::Flow::Oven && (AnyOnBoard() || CutStage()))) && g_hold == Hold::None && P.knife >= 0 && g_tip < 0.01 && g_phase == Phase::Cooking) {
				double lx = 0, ly = 0;
				if (OverBoard(mx, my, lx, ly) && lx > -L::kBoardW * 0.5 + 3.6) {
					int    best = -1;
					double bd = 1e9;
					for (int l = 0; l < 3; ++l) {
						for (const auto& u : g_units) {
							if (u.at == Unit::At::Board && u.lane == l && std::abs(ly - L::kLaneY[l]) < bd) {
								bd = std::abs(ly - L::kLaneY[l]);
								best = l;
							}
						}
					}
					g_knifeOn = true;
					g_knifeLane = best;
					if (g_knifeAnim < 0) {
						if (g_knifeAlong && best >= 0) {
							// along the food: over its middle, wherever across it the mouse is
							double w = 1.0;
							for (const auto& u : g_units) {
								if (u.at == Unit::At::Board && u.lane == best) {
									w = (std::max)(w, u.cut.width);
								}
							}
							g_knifeX = L::kBoardX;
							g_knifeY = g_boardY + L::kLaneY[best] + Clamp(ly - L::kLaneY[best], -w * 0.5 + 0.2, w * 0.5 - 0.2);
						} else {
							g_knifeX = L::kBoardX + Clamp(lx, -L::kBoardW * 0.5 + 1.0, L::kBoardW * 0.5 - 1.0);
							g_knifeY = g_boardY + (best >= 0 ? L::kLaneY[best] : ly);
						}
						if (CutStage() && !AnyOnBoard()) {
							// the big dough: the knife across it, wherever along it the mouse is
							const double R = DoughRadius();
							g_knifeLane = -1;
							g_knifeX = Clamp(L::kBoardX + lx, g_doughAt.x - R * 0.9, g_doughAt.x + R * 0.9);
							g_knifeY = g_doughAt.y;
						}
					}
				}
			}
		}

		// ---------------------------------------------------------------- every frame: the kitchen moves
		void Step(double dt)
		{
			g_fire.Step(dt);
			// the knife's chop lands a moment after the click
			if (g_knifeAnim >= 0) {
				g_knifeAnim += dt;
				if (!g_knifeCut && g_knifeAnim >= 0.06) {
					g_knifeCut = true;
					if (CutStage() && g_knifeLane < 0) {
						const double R = DoughRadius();
						const double at = Clamp((g_knifeX - (g_doughAt.x - R)) / (2.0 * R), 0.02, 0.98);
						bool         tooClose = false;
						for (double c : g_cuts) {
							tooClose |= std::abs(c - at) < 0.04;
						}
						if (!tooClose && static_cast<int>(g_cuts.size()) < ServeCount() - 1) {
							g_cuts.push_back(at);
							Audio::Take("chop_", 0.8f);
							if (static_cast<int>(g_cuts.size()) == ServeCount() - 1) {
								FinishCuts();
							}
						}
					}
					bool cut = false;
					for (int i = 0; i < static_cast<int>(g_units.size()); ++i) {
						auto& u = g_units[i];
						if (u.at != Unit::At::Board || u.lane != g_knifeLane || u.cut.finished) {
							continue;
						}
						if (u.bulb) {
							// the first knock breaks the head into its cloves, laid out in a row to dice
							u.bulb = false;
							if (u.prop >= 0) {
								Scene::Remove(u.prop);
							}
							u.model = "Hearthcooking\\GarlicCloves.nif";
							u.prop = Scene::Add(u.model);
							u.segs.clear();
							u.fitted = false;
							Fit(u);
							Toast("Broken into its cloves: now dice them");
							Audio::Take("knead_", 0.6f);
							continue;
						}
						if (g_knifeAlong && u.thin) {
							Toast(fmt::format("A {} is only cut across", u.name));
							continue;
						}
						if ((g_knifeAlong ? u.cut.alongCount : u.cut.cutCount) >= hc::Cutting::kMaxCuts) {
							Toast(g_knifeAlong ? "Six cuts along is enough" : "Six cuts across is enough");
							continue;
						}
						if (g_knifeAlong) {
							const double y = (g_knifeY - g_boardY - L::kLaneY[u.lane]) + u.cut.width * 0.5;
							cut |= u.cut.ChopAlong(y);
						} else {
							const double x = (g_knifeX - L::kBoardX) + u.boardLen * 0.5;
							cut |= u.cut.Chop(x);
						}
					}
					if (cut) {
						Audio::Take("chop_", 0.8f, 0.08f, static_cast<float>(Clamp((g_knifeX - 0.0) / 40.0, -0.8, 0.8)));
					}
				}
				if (g_knifeAnim > 0.2) {
					g_knifeAnim = -1;
				}
			}
			if (g_flow == hc::Flow::Pot && g_pot && SauceBowl()) {
				// the sauce's bowl: lifted over the pot it tips, and what's in it goes in
				const bool over = g_hold == Hold::Sauce && std::hypot(g_carry.x - L::kPotX, g_carry.y - L::kPotY) < L::kPotR + 2.5;
				g_sauceTip += Clamp((over ? 1.0 : 0.0) - g_sauceTip, -dt * 3.0, dt * 2.0);
				if (g_sauceTip > 0.6 && g_sauceN > 0) {
					for (auto& o : g_units) {
						if (o.byBowl && o.at == Unit::At::Gone) {
							o.at = Unit::At::Pot;
							++g_tossedIn;
						}
					}
					g_sauceN = 0;
					Audio::Play({ "pour", 0.6f });  // (a quarter quieter)
					static const float drop[4] = { 0.6f, 0.38f, 0.14f, 0.9f };
					Spray({ L::kPotX, L::kPotY, L::kLiquidZ }, 14, drop, 4.0, 18.0, 0.18f);
				}
				if (g_hold == Hold::Sauce && g_sauceN == 0 && g_sauceTip < 0.05) {
					g_hold = Hold::None;  // emptied: it goes back
				}
			}
			if (g_flow == hc::Flow::Pot && g_pot) {
				// the board tips toward what's asked, and eases back
				g_tip += Clamp(g_tipWant - g_tip, -dt * 2.2, dt * 1.6);
				if (g_tip > 0.25 && !g_tipped) {
					g_tipped = true;
					FinishBoard();
				}
				if (g_tip < 0.02 && g_tipped && g_chunks.CountIn(hc::Chunks::State::Free) == 0) {
					g_tipped = false;  // ready for another lot
				}
				g_boardPrev = g_boardPose;
				g_boardPose = hc::BoardPose(g_tip, g_boardY);
				g_boardPose.v = dt > 0 ? (g_boardPose.c - g_boardPrev.c) * (1.0 / dt) : Vec3{};
				const int before = g_chunks.CountIn(hc::Chunks::State::Pot);
				g_chunks.Step(dt, g_boardPose, g_pot->Swirl());
				g_spin = std::fmod(g_spin + g_pot->Swirl() * dt * 0.55, 6.283185307);  // (as its pieces go round)
				hc::Chunks::Event ev[32];
				const int         n = g_chunks.TakeEvents(ev, 32);
				for (int k = 0; k < n; ++k) {
					const auto& e = ev[k];
					const float pan = static_cast<float>(Clamp(e.at.x / 40.0, -0.8, 0.8));
					if (e.kind == hc::Chunks::Event::Kind::Splash) {
						Audio::Take("splash_", static_cast<float>(Clamp(0.3 + e.speed / 60.0, 0.3, 0.9)), 0.1f, pan);
						static const float drop[4] = { 0.55f, 0.4f, 0.25f, 0.9f };
						float c[4] = { g_broth[0] * 0.9f, g_broth[1] * 0.9f, g_broth[2] * 0.9f, 0.9f };
						(void)drop;
						Spray(e.at, 6, c, 3.0, 14.0, 0.16f);
					} else if (e.kind == hc::Chunks::Event::Kind::Land) {
						const double fx = e.at.x - L::kFireX, fy = e.at.y - L::kFireY;
						if (fx * fx + fy * fy < L::kFireR * L::kFireR) {
							Hiss(0.5f, pan);
						}
					}
				}
				const int after = g_chunks.CountIn(hc::Chunks::State::Pot);
				if (after > before) {
					g_pot->AddPieces(before, after - before);
				}
				g_pot->Step(dt * kPotSpeed, g_fire.Heat(), after + g_tossedIn > 0);
				g_timer.in = after + g_tossedIn > 0;
				g_timer.Step(dt, hc::timing::Rate(g_fire.Logs()));
				g_pot->SetCooked(TimerLook(0.92, 1.3, 1.9));
				g_chunks.Cook(dt * kPotSpeed * 0.02 * Clamp(g_pot->Temp() - 0.3, 0.0, 1.0));
				const double t = g_pot->Temp();
				// the real recording only: it murmurs as the pot warms and comes up to a full boil
				Audio::SetLoop("loop_boil", static_cast<float>(Clamp((t - 0.38) * 0.6, 0.0, 0.25) + Clamp((t - 0.8) * 2.75, 0.0, 0.55)));
			}
			if (g_flow == hc::Flow::Spit && g_spit) {
				const int m = Mounted();
				g_spit->Step(dt * kSpitSpeed, g_fire.Heat(), m > 0);
				const double rate = hc::timing::Rate(g_fire.Logs());
				for (int i = 0; i < static_cast<int>(g_units.size()); ++i) {
					auto& u = g_units[i];
					if (u.role != Role::Meat || u.timer.out) {
						continue;
					}
					u.timer.in = u.at == Unit::At::Spit && !(g_hold == Hold::Meat && g_holdUnit == i);  // (not while it's in the hand)
					u.timer.Step(dt, rate);
				}
			}
			if (g_flow == hc::Flow::Oven && g_oven) {
				g_door += Clamp(g_doorWant - g_door, -dt * 3.0, dt * 3.0);
				g_oven->SetDoor(g_door > 0.5);
				if (g_oven->InOven() && g_door < 0.3) {
					g_doorShut = true;
				}
				if (g_doorShut && g_door > 0.6 && (g_oven->InOven() || g_oven->Out())) {
					g_doorReopened = g_doorReopened || g_oven->Brown() >= 0.5 || g_oven->Out();
				}
				g_oven->Step(dt * kOvenSpeed, g_fire.Heat());
				g_timer.in = g_oven->InOven();
				g_timer.Step(dt, hc::timing::Rate(g_fire.Logs()));
				if (g_batch.empty() && ServeCount() > 1 && !FlourRecipe()) {
					g_batch.assign(ServeCount(), hc::timing::Piece{});
					for (auto& pc : g_batch) {
						pc.time = g_timer.time;
					}
				}
				for (auto& pc : g_batch) {
					pc.in = g_oven->InOven();
					pc.Step(dt, hc::timing::Rate(g_fire.Logs()));
				}
				if (g_timer.in || g_oven->Out()) {
					g_oven->SetBrown(TimerLook(0.85, 1.2, 1.6));
				}
				for (int k = 0; k < 4; ++k) {
					g_fold[k] += Clamp(g_foldWant[k] - g_fold[k], -dt * 3.0, dt * 2.2);
				}
				if (g_topping && !g_doughOnBoard && DoughReady()) {
					g_doughOnBoard = true;  // the topped bread is what the peel takes
					if (const int b = BreadUnit(); b >= 0) {
						g_units[b].at = Unit::At::Dough;  // (the dish's own shape stands for the slices now)
						HideBoardBits(g_units[b]);
					}
				}
				// the fruit bowl: lifted, its fruit riding in it; over the dough it tips and pours
				{
					const bool held = g_hold == Hold::Apples;
					const bool over = held && (BreadCuts() ? std::hypot(g_carry.x - kMixAt.x, g_carry.y - kMixAt.y) < 4.5 && !g_cutInMix
					                                       : std::hypot(g_carry.x - g_doughAt.x, g_carry.y - g_doughAt.y) < 4.5 && (g_doughOnBoard || BreadOnBoard()));
					g_bowlTip += Clamp((over && !g_bowlPoured ? 1.0 : 0.0) - g_bowlTip, -dt * 3.0, dt * 2.0);
					if (g_bowlTip > 0.6 && !g_bowlPoured && BreadCuts() && g_doughParts < (std::max)(1, g_doughNeeded)) {
						Toast("The milk, egg and flour go in first, then this");
						g_hold = Hold::None;  // the bowl goes back
					} else if (g_bowlTip > 0.6 && !g_bowlPoured && BreadCuts()) {
						g_cutInMix = true;  // into the mixing bowl, after the dough's things, before the salt
						g_bowlPoured = true;
					} else if (g_bowlTip > 0.6 && !g_bowlPoured) {
						PourOnDough();
						if (!g_bowlPoured) {
							g_hold = Hold::None;  // not ready for it: the bowl goes back
						}
					}
					if (!held) {
						g_bowlPoured = false;
					}
					const Vec3 c = held && !(BreadCuts() && g_cutInMix) ? g_carry + Vec3{ 0, 0, 0.6 } : Vec3{ CutBowl().x, CutBowl().y, 1.0 };
					g_chunks.SetBowl(c, 3.5, c.z + 1.6, false);
				}
				// the board: lifted and carried to the bowl, tipped over it, and back down
				g_boardY = L::kBoardY;
				const bool lifting = g_hold == Hold::Board;
				g_boardLift += Clamp((lifting ? 1.0 : 0.0) - g_boardLift, -dt * 3.0, dt * 3.0);
				g_tip += Clamp((lifting ? g_tipWant : 0.0) - g_tip, -dt * 2.5, dt * 1.8);
				{
					const auto rest = hc::BoardPose(0.0, L::kBoardY);
					g_boardPrev = g_boardPose;
					g_boardPose = rest;
					g_boardPose.c = rest.c + (g_boardCarry - rest.c) * g_boardLift;  // (back down from where it was let go)
					g_boardPose.tilt = g_tip * 1.1;
					g_boardPose.v = dt > 0 ? (g_boardPose.c - g_boardPrev.c) * (1.0 / dt) : Vec3{};
				}
				if (g_tip > 0.3 && !g_tipped) {
					// over it goes: off the board into the bowl
					g_tipped = true;
					FinishBoard();
					for (int b = 0; b < g_chunks.Count(); ++b) {
						g_chunks.Launch(b, g_boardPose);
					}
				}
				if (g_tip < 0.02 && !lifting) {
					g_tipped = false;
				}
				// the cut filling, flying into its bowl
				g_chunks.Step(dt, g_boardPose, 0.0);
				hc::Chunks::Event ev[32];
				g_chunks.TakeEvents(ev, 32);  // (into its bowl without a sound)
				if (g_eggT >= 0.0 && g_eggT < 100.0) {
					g_eggT += dt;
				}
				if (g_peelT >= 0) {
					g_peelT += dt / 0.7;
					if (g_peelT >= 1.0) {
						g_peelT = -1;
						if (g_peelIn) {
							g_oven->Load();
							if (g_flow == hc::Flow::Oven) {
								Toast("Shut the door, and let it bake");
							}
						}
					}
				}
			}
			FireLoop(Clamp(g_fire.Heat() * 0.75, 0.0, 0.85));
			// the grains, drops, sparks and puffs
			for (auto& g : g_grains) {
				g.age += dt;
				if (g.gravity) {
					g.v.z -= 60.0 * dt;
				} else {
					g.v = g.v * (1.0 / (1.0 + dt * 1.5));
					g.v.z += (g.kind == Render3D::Sprite::Kind::Smoke ? 3.0 : 6.0) * dt;
				}
				g.p += g.v * dt;
				if (g.p.z < 0.05) {
					g.p.z = 0.05;
					g.v = {};
				}
			}
			std::erase_if(g_grains, [](const Grain& g) { return g.age > g.life; });
		}

		// ---------------------------------------------------------------- every frame: the kitchen as it looks
		// The ingredient's own mesh, laid in the frame (x along it, y across, z up) with its middle at a_c, cut to the
		// box (a bound past 1e6 is no cut); the inside shows through the cuts as solid flesh
		// Cuts a thing's look to the box (frame axes about a_c; a bound past 1e6 is no cut)
		void ClipTo(Scene::Look& lk, const Vec3& a_c, const Quat& a_frame, const double (&a_box)[6], bool a_cut)
		{
			for (int k = 0; k < 6; ++k) {
				const int    axis = k / 2;
				const bool   hi = (k & 1) != 0;
				const double v = a_box[k];
				const Vec3   n = a_frame.Rotate({ axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0 }) * (hi ? 1.0 : -1.0);
				const bool   on = a_cut && std::abs(v) < 1e6;
				lk.clips[k][0] = on ? static_cast<float>(n.x) : 0.0f;
				lk.clips[k][1] = on ? static_cast<float>(n.y) : 0.0f;
				lk.clips[k][2] = on ? static_cast<float>(n.z) : 0.0f;
				lk.clips[k][3] = static_cast<float>(n.Dot(a_c) + (hi ? v : -v));
			}
			std::memset(lk.shape, 0, sizeof(lk.shape));
		}

		// A piece of a thing: its own mesh (the skin) cut to the box, and inside it the core - a closed egg of its
		// flesh the thing's size, cut the same, whose cut faces are drawn solid. Laid in the frame (x along it, y
		// across, z up) with the whole's middle at a_c. Nothing can be seen through.
		void PlacePiece(Scene::Id a_id, Scene::Id a_core, Unit& u, const Vec3& a_c, const Quat& a_frame, const double (&a_boxIn)[6], bool a_cut, double a_f = 1.0)
		{
			if (!Fit(u)) {
				Scene::Hide(a_id, true);
				Scene::Hide(a_core, true);
				return;
			}
			// a_f: the whole shrunk about its middle (a piece breaking down in the pot), its cuts with it
			double a_box[6];
			for (int k = 0; k < 6; ++k) {
				a_box[k] = std::abs(a_boxIn[k]) < 1e6 ? a_boxIn[k] * a_f : a_boxIn[k];
			}
			const bool   own = g_ownLeek.count(a_id) > 0;  // (our leek: already along x, its middle at 0, 16 long)
			const double s = (own ? u.boardLen / 16.0 : u.boardLen / u.dims[0]) * a_f;
			const Quat   q = own ? a_frame : a_frame * u.lie;
			Scene::Place(a_id, a_c - (own ? Vec3{} : q.Rotate(u.center)) * s, q, static_cast<float>(s));
			if (own) {
				Scene::Stretch(a_id, 1.0f, static_cast<float>(kOwnLeekThin), static_cast<float>(kOwnLeekThin));  // (as thick as the game's leek)
			}
			Scene::Hide(a_id, false);
			auto& lk = Scene::LookOf(a_id);
			ClipTo(lk, a_c, a_frame, a_box, a_cut);
			ClamLook(u, lk);
			float fl[3];
			Flesh(u, fl);
			std::copy(fl, fl + 3, lk.flesh);
			lk.flesh[3] = a_cut ? 3.0f : 0.0f;  // seen from inside, the skin shows its flesh
			auto* fleshTex = FleshTexture(u);
			lk.fleshTex = fleshTex;
			if (a_core < 0) {
				return;
			}
			// every cut piece is solid: its cut faces are drawn wherever they're inside the thing
			if (!a_cut) {
				Scene::Hide(a_core, true);
				return;
			}
			// the solid: a box round the piece (the cell, within the thing's measured bounds); the renderer draws
			// its cut faces exactly where they're inside the thing's own mesh
			double R[3] = { u.boardLen * 0.5 * a_f, (std::max)(0.05, Thick(u, u.boardLen, 1) * 0.5) * a_f, (std::max)(0.05, Thick(u, u.boardLen, 2) * 0.5) * a_f };
			if (own) {
				R[1] = R[2] = 0.95 * kOwnLeekThin * u.boardLen / 16.0 * a_f;  // (our leek's stalk: its rings fill the cut face)
			}
			double       lo[3], hi[3];
			for (int ax = 0; ax < 3; ++ax) {
				lo[ax] = (std::max)(a_box[ax * 2], -R[ax]);
				hi[ax] = (std::min)(a_box[ax * 2 + 1], R[ax]);
				if (hi[ax] <= lo[ax]) {
					Scene::Hide(a_core, true);
					return;
				}
			}
			const Vec3 mid{ (lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5 };
			Scene::Place(a_core, a_c + a_frame.Rotate(mid), a_frame, 1.0f);
			Scene::Stretch(a_core, static_cast<float>((hi[0] - lo[0]) * 1.04), static_cast<float>((hi[1] - lo[1]) * 1.04), static_cast<float>((hi[2] - lo[2]) * 1.04));
			Scene::Hide(a_core, false);
			auto& cl = Scene::LookOf(a_core);
			ClipTo(cl, a_c, a_frame, a_box, true);
			std::copy(fl, fl + 3, cl.flesh);
			cl.flesh[3] = 4.0f;
			cl.fleshTex = fleshTex;
			// the thing's bounds as a box (axes over half-sizes), on the face and on the skin that counts for it
			const Vec3 axv[3] = { a_frame.Rotate({ 1, 0, 0 }) * (1.0 / (R[0] * 1.01)), a_frame.Rotate({ 0, 1, 0 }) * (1.0 / (R[1] * 1.01)), a_frame.Rotate({ 0, 0, 1 }) * (1.0 / (R[2] * 1.01)) };
			for (auto* look : { &cl, &lk }) {
				look->shape[0][0] = static_cast<float>(a_c.x);
				look->shape[0][1] = static_cast<float>(a_c.y);
				look->shape[0][2] = static_cast<float>(a_c.z);
				// (a leek, a carrot: a tight round section; a cabbage: a tight ball - their loose outer leaves stay leaves)
				look->shape[0][3] = u.thin ? 1.0f : u.name.find("abbage") != std::string::npos ? 0.5f : 2.0f;
				for (int j = 0; j < 3; ++j) {
					look->shape[j + 1][0] = static_cast<float>(axv[j].x);
					look->shape[j + 1][1] = static_cast<float>(axv[j].y);
					look->shape[j + 1][2] = static_cast<float>(axv[j].z);
				}
			}
			cl.cookMode = lk.cookMode;
			cl.cook[0] = lk.cook[0];
			std::copy(std::begin(lk.lift), std::end(lk.lift), cl.lift);
			std::copy(std::begin(lk.tint), std::end(lk.tint), cl.tint);
			cl.shadow = false;  // the skin casts the shadow
		}

		void PlaceBoardUnit(Unit& u)
		{
			const double h = Thick(u, u.boardLen, 2);
			const Vec3   at = g_boardPose.ToWorld({ 0.0, L::kLaneY[u.lane], L::kBoardH * 0.5 + h * 0.5 });
			const Quat   frame = RotY(g_boardPose.tilt);
			const int    nx = u.cut.Pieces(), ny = u.cut.Strips(), n = nx * ny;
			// its own mesh cut to each piece (the first is the thing itself), nudged apart so the cuts show
			if (u.segs.empty()) {
				u.segs.push_back(u.prop);
			}
			while (static_cast<int>(u.segs.size()) < n) {
				const auto id = u.model.empty() ? Add("Chunk") : AddUnitMesh(u);
				u.segs.push_back(id >= 0 ? id : Add("Chunk"));
			}
			while (static_cast<int>(u.cores.size()) < n) {
				u.cores.push_back(Add("Hull"));
			}
			const double half = u.boardLen * 0.5, w = u.cut.width;
			for (int ix = 0; ix < nx; ++ix) {
				double a, b;
				u.cut.Piece(ix, a, b);
				for (int iy = 0; iy < ny; ++iy) {
					double ya, yb;
					u.cut.Strip(iy, ya, yb);
					// (the outer pieces open on their outer side: the cuts' range can fall short of the thing's own edge)
					const double box[6] = { nx > 1 && ix > 0 ? a - half : -1e7, nx > 1 && ix < nx - 1 ? b - half : 1e7, ny > 1 && iy > 0 ? ya - w * 0.5 : -1e7,
						                    ny > 1 && iy < ny - 1 ? yb - w * 0.5 : 1e7, -1e7, 1e7 };
					const Vec3   c = at + frame.Rotate({ Spread(ix, nx), Spread(iy, ny), 0.0 });
					PlacePiece(u.segs[ix * ny + iy], u.cores[ix * ny + iy], u, c, frame, box, n > 1);
				}
			}
			for (int k = n; k < static_cast<int>(u.segs.size()); ++k) {
				Scene::Hide(u.segs[k], true);
			}
			for (int k = n; k < static_cast<int>(u.cores.size()); ++k) {
				Scene::Hide(u.cores[k], true);
			}
		}

		void Look3D()
		{
			const double t = g_clock;
			std::vector<Render3D::Sprite> sp;
			const double heat = g_fire.Heat();
			Refit();
			for (int k = 0; k < 3; ++k) {
				PlaceLog(P.pile[k], { L::kPileX + (k == 2 ? 0.0 : (k ? 1.0 : -1.0)), L::kPileY + (k == 2 ? 0.0 : -0.4), 0.95 + (k == 2 ? 1.65 : 0.0) }, kPi * 0.5 + 0.12 * k, 8.4);
				Scene::Hide(P.pile[k], false);
			}
			// ---- the fruit bowl, lifted and tipping toward the dough
			if (P.sauceBowl >= 0) {
				Pose p;
				p.pos = kSauceAt;
				const bool fitted = !Game(Kit::Role::Bowl) || FitBox(P.sauceBowl, Kit::Get(Kit::Role::Bowl), kSauceAt, { 7.5, 7.5, 0.0 }, 0.0, false, p);
				const bool held = g_hold == Hold::Sauce;
				const Vec3 base = held ? g_carry : kSauceAt;
				const double ang = std::atan2(L::kPotY - base.y, L::kPotX - base.x);
				const Quat   T = RotZ(ang) * RotY(g_sauceTip * 1.7) * RotZ(-ang);  // its far side dips toward the pot
				if (fitted) {
					p.pos = base + T.Rotate(p.pos - kSauceAt);
					p.q = T * p.q;
					Apply(P.sauceBowl, p);
				}
				if (P.sauceIn >= 0) {
					Scene::Hide(P.sauceIn, g_sauceN == 0);
					if (g_sauceN > 0) {
						Scene::Place(P.sauceIn, base + T.Rotate({ 0, 0, 1.3 + 0.3 * g_sauceN }), T * RotZ(g_clock * 0.1), 0.42f);
						auto& sl = Scene::LookOf(P.sauceIn);
						bool honey = false;
						for (const auto& o : g_units) {
							honey |= o.byBowl && o.at == Unit::At::Gone && o.name.find("oney") != std::string::npos;
						}
						// ale, amber; the honey stirring a deeper gold into it
						sl.lift[0] = honey ? 0.78f : 0.62f, sl.lift[1] = honey ? 0.48f : 0.38f, sl.lift[2] = honey ? 0.12f : 0.12f, sl.lift[3] = 0.92f;
						sl.shadow = false;
					}
				}
			}
			if (P.fillBowl >= 0 && g_flow == hc::Flow::Oven && (g_hold == Hold::Apples || g_bowlTip > 0.01)) {
				Pose p;
				p.pos = kFillAt;
				const bool fitted = !Game(Kit::Role::Bowl) || FitBox(P.fillBowl, Kit::Get(Kit::Role::Bowl), kFillAt, { 8.5, 8.5, 0.0 }, 0.0, false, p);
				if (fitted) {
					const Vec3   base = g_hold == Hold::Apples ? g_carry : kFillAt;
					const double ang = std::atan2(PourAt().y - base.y, PourAt().x - base.x);
					const Quat   T = RotZ(ang) * RotY(g_bowlTip * 1.7) * RotZ(-ang);  // its far side dips toward the dough (or the mixing bowl)
					p.pos = base + T.Rotate(p.pos - kFillAt);
					p.q = T * p.q;
					Apply(P.fillBowl, p);
				}
			}
			const Vec3   fc = FireCenter();
			// ---- the fire: its logs, flames, embers, smoke
			const size_t fireFrom = sp.size();
			for (int k = 0; k < hc::Fire::kMaxLogs; ++k) {
				const auto id = P.fireLogs[k];
				if (id < 0) {
					continue;
				}
				if (k >= g_fire.Logs()) {
					Scene::Hide(id, true);
					continue;
				}
				const double glow = g_fire.Glow(k), fuel = g_fire.Fuel(k);
				Vec3         at;
				double       rz = 0;
				if (g_flow == hc::Flow::Oven) {
					at = { fc.x - 2.4 + (k % 3) * 2.4, fc.y + (k % 2) * 0.7, fc.z + 0.5 + (k / 3) * 1.0 };
					rz = (k % 2 ? 0.25 : -0.25);  // across the back of the dome
				} else {
					static constexpr double kA[5] = { 0.3, 2.4, 4.4, 1.3, 3.4 };
					at = { fc.x + std::cos(kA[k]) * 1.2, fc.y + std::sin(kA[k]) * 1.2, 0.95 + (k >= 3 ? 1.3 : 0.0) };
					rz = kA[k] + kPi * 0.5;
				}
				PlaceLog(id, at, rz, (g_flow == hc::Flow::Oven ? 5.5 : 8.4) * (0.8 + 0.2 * fuel), k >= 3 ? 0.15 : 0.0);
				auto& lk = Scene::LookOf(id);
				const float dark = static_cast<float>(0.25 + 0.75 * fuel);
				lk.tint[0] = lk.tint[1] = lk.tint[2] = dark;
				const float e = static_cast<float>(glow * Clamp(heat * 1.2, 0.0, 1.2) * (1.0 - fuel * 0.6));
				lk.emissive[0] = 0.9f * e;
				lk.emissive[1] = 0.28f * e;
				lk.emissive[2] = 0.05f * e;
				Scene::Hide(id, false);
				// flames along the log
				for (int f = 0; f < 3; ++f) {
					Render3D::Sprite s;
					const double off = (f - 1) * 2.4;
					s.pos[0] = static_cast<float>(at.x + std::cos(rz - kPi * 0.5) * off * 0.0 + std::cos(rz) * off);
					s.pos[1] = static_cast<float>(at.y + std::sin(rz) * off);
					s.pos[2] = static_cast<float>(at.z + 0.6);
					s.size = static_cast<float>((0.9 + 1.4 * glow) * Clamp(heat * 1.3, 0.2, 1.4));
					s.stretch = 1.9f;
					s.seed = static_cast<float>(k * 3 + f) * 1.37f;
					s.color[3] = static_cast<float>(glow * Clamp(heat * 1.5, 0.0, 1.0));
					s.kind = Render3D::Sprite::Kind::Flame;
					sp.push_back(s);
				}
			}
			if (heat > 0.02) {
				const int n = static_cast<int>(2 + heat * 6);
				for (int k = 0; k < n; ++k) {
					Render3D::Sprite s;
					const double a = k * 2.399 + 0.3, r = (g_flow == hc::Flow::Oven ? 0.8 : 1.4) * Hash(k, 3);
					s.pos[0] = static_cast<float>(fc.x + std::cos(a) * r * (g_flow == hc::Flow::Oven ? 2.5 : 1.0));
					s.pos[1] = static_cast<float>(fc.y + std::sin(a) * r);
					s.pos[2] = static_cast<float>(fc.z + 0.6);
					s.size = static_cast<float>(1.4 + 2.2 * heat * (0.6 + 0.4 * Hash(k, 4)));
					s.stretch = 2.0f;
					s.seed = 20.0f + k * 2.11f;
					s.color[3] = static_cast<float>(Clamp(heat * 1.3, 0.0, 1.0));
					s.kind = Render3D::Sprite::Kind::Flame;
					sp.push_back(s);
				}
				Render3D::Sprite g;
				g.pos[0] = static_cast<float>(fc.x);
				g.pos[1] = static_cast<float>(fc.y);
				g.pos[2] = static_cast<float>(fc.z + 0.8);
				g.size = static_cast<float>(6.0 + 4.0 * heat);
				g.color[0] = 1.0f;
				g.color[1] = 0.42f;
				g.color[2] = 0.12f;
				g.color[3] = static_cast<float>(0.22 * Clamp(heat, 0.0, 1.2));
				g.kind = Render3D::Sprite::Kind::Glow;
				sp.push_back(g);
				// a flare-up where the spit's fat lands: a tall lick of flame, a burst of light and a spark or two
				if (g_flow == hc::Flow::Spit && g_clock >= g_flareAt && g_clock < g_flareAt + 0.45) {
					const double f = 1.0 - (g_clock - g_flareAt) / 0.45;
					Render3D::Sprite fl;
					fl.pos[0] = static_cast<float>(g_flareX);
					fl.pos[1] = static_cast<float>(fc.y);
					fl.pos[2] = static_cast<float>(fc.z + 1.2);
					fl.size = static_cast<float>(1.6 + 1.6 * f);
					fl.stretch = 2.6f;
					fl.seed = static_cast<float>(g_flareAt * 7.0);
					fl.color[3] = static_cast<float>(f);
					fl.kind = Render3D::Sprite::Kind::Flame;
					sp.push_back(fl);
					Render3D::Sprite gl = g;
					gl.pos[0] = static_cast<float>(g_flareX);
					gl.size = 5.0f;
					gl.color[3] = static_cast<float>(0.3 * f);
					sp.push_back(gl);
					static double s_sparked = -1.0;
					if (s_sparked != g_flareAt) {
						s_sparked = g_flareAt;
						static const float spark[4] = { 1.0f, 0.6f, 0.2f, 1.0f };
						Spray(Vec3{ g_flareX, fc.y, fc.z + 1.0 }, 3, spark, 2.0, 5.0, 0.1f, true, Render3D::Sprite::Kind::Ember, 0.6);
						Hiss(0.45f, 0.0f);
					}
				}
				// embers rising, and a little smoke
				for (int k = 0; k < 14; ++k) {
					const double p = std::fmod(t * (0.3 + 0.25 * Hash(k, 5)) + Hash(k, 6), 1.0);
					Render3D::Sprite e;
					e.pos[0] = static_cast<float>(fc.x + (Hash(k, 7) - 0.5) * 4.0 + std::sin(t * 2.0 + k) * p * 1.5);
					e.pos[1] = static_cast<float>(fc.y + (Hash(k, 8) - 0.5) * 3.0);
					e.pos[2] = static_cast<float>(fc.z + 1.0 + p * (6.0 + 14.0 * heat));
					e.size = 0.13f;
					e.color[0] = 1.0f;
					e.color[1] = 0.55f;
					e.color[2] = 0.18f;
					e.color[3] = static_cast<float>((1.0 - p) * Clamp(heat * 1.4, 0.0, 1.0));
					e.kind = Render3D::Sprite::Kind::Ember;
					sp.push_back(e);
				}
			}
			if (g_flow == hc::Flow::Oven) {
				sp.resize(fireFrom);  // the oven's fire is shut in its dome: its glow lights the kitchen, its flames aren't seen
			}
			for (int k = 0; k < 6 && g_flow != hc::Flow::Pot; ++k) {
				const double p = std::fmod(t * 0.12 + k / 6.0, 1.0);
				Render3D::Sprite s;
				s.pos[0] = static_cast<float>(fc.x + std::sin(t * 0.4 + k) * 2.0 * p);
				s.pos[1] = static_cast<float>(fc.y + (g_flow == hc::Flow::Oven ? -2.0 * p : 0.0));
				s.pos[2] = static_cast<float>(fc.z + 6.0 + p * 22.0);
				s.size = static_cast<float>(2.0 + p * 6.0);
				s.color[0] = s.color[1] = s.color[2] = 0.32f;
				s.color[3] = static_cast<float>(0.16 * (1.0 - p) * Clamp(heat, 0.2, 1.0));
				s.seed = k * 3.3f;
				s.kind = Render3D::Sprite::Kind::Smoke;
				sp.push_back(s);
			}
			{
				// (a flare-up brightens the fire's light for a moment)
				const double flare = g_flow == hc::Flow::Spit && g_clock >= g_flareAt && g_clock < g_flareAt + 0.45 ? 0.35 * (1.0 - (g_clock - g_flareAt) / 0.45) : 0.0;
				Scene::SetFire(fc + Vec3{ 0, 0, 3.0 }, static_cast<float>(heat + flare));
			}
			// heat shimmer: over the open fire, or at the oven's mouth (more with its door open)
			if (g_flow == hc::Flow::Oven) {
				Scene::SetHaze(MouthAt() + Vec3{ 0, 0, 3.0 }, static_cast<float>(Clamp(heat, 0.0, 1.2) * (g_door > 0.5 ? 1.0 : 0.35)));
			} else if (g_flow == hc::Flow::Spit) {
				Scene::SetHaze(fc + Vec3{ 0, 0, 7.0 }, static_cast<float>(Clamp(heat, 0.0, 1.2) * 0.8));
			} else {
				Scene::SetHaze(fc, 0.0f);
			}
			// black smoke when it's burning: off the meat on the spit, out of the oven's mouth
			{
				double burn = 0.0;
				Vec3   from = fc;
				if (g_flow == hc::Flow::Spit && g_spit) {
					for (int b = 0; b < hc::Spit::kBands; ++b) {
						burn = (std::max)(burn, g_spit->Band(b) - 1.45);
					}
					from = SpitAxis(0.0) + Vec3{ 0, 0, 1.5 };
				} else if (g_flow == hc::Flow::Oven && g_oven && g_oven->InOven()) {
					burn = g_oven->Brown() - 1.3;
					from = MouthAt() + Vec3{ 0, 0, 2.0 };
				}
				if (burn > 0.0) {
					const double k = Clamp(burn * 2.5, 0.0, 1.0);
					for (int j = 0; j < 6; ++j) {
						const double p = std::fmod(t * 0.28 + j / 6.0, 1.0);
						Render3D::Sprite s;
						s.pos[0] = static_cast<float>(from.x + (Hash(j, 71) - 0.5) * 3.0 + std::sin(t * 0.7 + j) * p * 2.0);
						s.pos[1] = static_cast<float>(from.y + (Hash(j, 72) - 0.5) * 2.0);
						s.pos[2] = static_cast<float>(from.z + p * 16.0);
						s.size = static_cast<float>(1.8 + p * 6.0);
						s.color[0] = s.color[1] = s.color[2] = 0.12f;
						s.color[3] = static_cast<float>(0.45 * (1.0 - p) * k);
						s.seed = 90.0f + j * 2.3f;
						s.kind = Render3D::Sprite::Kind::Smoke;
						sp.push_back(s);
					}
				}
			}

			// ---- the log carried
			if (P.heldLog >= 0) {
				const bool held = g_hold == Hold::Log || g_hold == Hold::FireLog;
				Scene::Hide(P.heldLog, !held);
				if (held) {
					PlaceLog(P.heldLog, g_carry, 0.4, 8.4);
					auto& lk = Scene::LookOf(P.heldLog);
					const float e = g_hold == Hold::FireLog ? 0.5f : 0.0f;
					lk.emissive[0] = 0.9f * e;
					lk.emissive[1] = 0.3f * e;
					lk.emissive[2] = 0.05f * e;
				}
			}

			// ---- the ingredients
			for (int i = 0; i < static_cast<int>(g_units.size()); ++i) {
				auto& u = g_units[i];
				if (u.prop < 0) {
					continue;
				}
				if (u.servedSlot >= 0) {
					int nth = 0;
					for (int j = 0; j < i; ++j) {
						nth += g_units[j].servedSlot == u.servedSlot;
					}
					Lay(u, SlotAt(u.servedSlot) + Vec3{ 0.0, (nth - (PerSlot() - 1) * 0.5) * 2.4, 1.3 }, MeatLen(u), RotZ(0.3));
					continue;
				}
				switch (u.at) {
				case Unit::At::Cloth:
					// the same size everywhere: on the cloth, in the hand and on the board
					Fit(u);
					if (Upright(u)) {
						Stand(u, u.clothAt + Vec3{ 0, 0, 0.2 }, u.boardLen, Hash(i, 9) * 6.28);  // a bottle or jug stands
					} else {
						Lay(u, u.clothAt + Vec3{ 0, 0, Thick(u, u.boardLen, 2) * 0.5 + 0.2 }, u.boardLen, RotZ(0.25 * (Hash(i, 9) - 0.5)));
					}
					break;
				case Unit::At::Held:
					Fit(u);
					Lay(u, g_carry, u.boardLen, RotZ(0.3));
					break;
				case Unit::At::Board:
					PlaceBoardUnit(u);
						if (g_topping && g_buttered && &u - g_units.data() == BreadUnit()) {
							for (auto id : u.segs) {
								auto& bl = Scene::LookOf(id);  // buttered: a golden sheen
								bl.lift[0] = 1.0f, bl.lift[1] = 0.86f, bl.lift[2] = 0.42f, bl.lift[3] = 0.3f;
							}
						}
					break;
				case Unit::At::Spit:
					{
						const double len = MeatLen(u);
						const double a = g_spit ? g_spit->Angle() : 0.0;
						const double own = TimerLookOf(u.timer, 0.9, 1.3, 1.7);
						Lay(u, SpitAxis(u.spitX), len, RotX(a));
						auto& lk = Scene::LookOf(u.prop);
						lk.cookMode = 2;
						{
							// red meat darkens on the spit; chicken browns golden
							std::string ln = u.name;
							std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							lk.sear = ln.find("chicken") == std::string::npos;
						}
						for (int b = 0; b < hc::Spit::kBands; ++b) {
							lk.cook[b] = static_cast<float>((std::max)(0.0, g_spit->Band(b) - g_spit->Mean() + own));
						}
						// its fat running as it roasts: glistening, browning unevenly
						{
							const double m = own;
							lk.fx[3] = static_cast<float>(Clamp((m - 0.1) * 1.6, 0.0, 1.0) * Clamp((1.7 - m) * 2.0, 0.25, 1.0));
							lk.fx[2] = 0.7f;
							// now and then a drop of fat off its underside, into the fire: a flare-up where it lands
							if (g_fire.Heat() > 0.3 && m > 0.2 && m < 1.5 && g_clock >= g_dripNext) {
								g_dripNext = g_clock + 0.7 + 1.1 * Rand01();
								const auto   ax = SpitAxis(u.spitX);
								const double x = (Rand01() - 0.5) * len * 0.8;
								static const float fat[4] = { 0.95f, 0.78f, 0.4f, 0.9f };
								Spray(Vec3{ ax.x + x, ax.y, ax.z - 1.4 }, 1, fat, 0.1, -1.0, 0.13f, true, Render3D::Sprite::Kind::Grain, 0.5);
								g_flareAt = g_clock + 0.32;
								g_flareX = ax.x + x;
							}
						}
						lk.cookAxis[0] = 1.0f;
						lk.cookAxis[1] = lk.cookAxis[2] = 0.0f;
						lk.cookRef[0] = 0.0f;
						lk.cookRef[1] = static_cast<float>(std::sin(a));
						lk.cookRef[2] = static_cast<float>(-std::cos(a));
						// it sizzles and smokes as it browns
						if (g_fire.Heat() > 0.4 && g_spit->Mean() > 0.15) {
							for (int k = 0; k < 3; ++k) {
								const double p = std::fmod(t * 0.5 + k / 3.0 + i * 0.13, 1.0);
								Render3D::Sprite s;
								const auto   at = SpitAxis(u.spitX);
								s.pos[0] = static_cast<float>(at.x + (Hash(k + i * 5, 11) - 0.5) * len);
								s.pos[1] = static_cast<float>(at.y);
								s.pos[2] = static_cast<float>(at.z + 1.0 + p * 8.0);
								s.size = static_cast<float>(1.0 + p * 2.5);
								s.color[0] = s.color[1] = s.color[2] = 0.75f;
								s.color[3] = static_cast<float>(0.12 * (1.0 - p) * Clamp(g_spit->Mean(), 0.0, 1.2));
								s.seed = k * 5.1f + i;
								s.kind = Render3D::Sprite::Kind::Smoke;
								sp.push_back(s);
							}
						}
					}
					break;
				default:
					if (u.byBowl && (u.at == Unit::At::Gone || u.at == Unit::At::Pot)) {
						// poured: its bottle (or pot of honey) stood by the sauce's bowl
						int k = 0;
						for (int j = 0; j < i; ++j) {
							k += g_units[j].byBowl;
						}
						// the honey pot on the bowl's left, the bottles on its right
						const bool honeyPot = u.name.find("oney") != std::string::npos;
						Stand(u, kSauceAt + (honeyPot ? Vec3{ -8.8, -0.8, 0.0 } : Vec3{ 7.4 + k * 2.6, -1.2 + k * 0.8, 0.0 }), u.boardLen, 0.4 + k);  // (clear of the bowl)
						HideBoardBits(u);
						break;
					}
					Scene::Hide(u.prop, true);
					HideBoardBits(u);
					break;
				}
				if (u.at != Unit::At::Board) {
					auto& lk = Scene::LookOf(u.prop);
					ClamLook(u, lk);
					std::memset(lk.clips, 0, sizeof(lk.clips));
					lk.flesh[3] = 0.0f;
					if (u.at != Unit::At::Pot && u.at != Unit::At::Gone) {
						for (auto id : u.segs) {
							if (id != u.prop) {
								Scene::Hide(id, true);
							}
						}
						for (auto id : u.cores) {
							Scene::Hide(id, true);
						}
					}
				}
			}
			if (g_hold == Hold::Meat && g_holdUnit >= 0) {
				auto&        u = g_units[g_holdUnit];
				const double len = MeatLen(u);
				Lay(u, g_carry, len, RotX(g_spit ? g_spit->Angle() : 0.0));
			}

			// ---- the board and knife (the pot)
			if (P.board >= 0) {
				const auto pose = g_flow != hc::Flow::Spit ? g_boardPose : hc::BoardPose(0.0, L::kBoardY);
				if (Game(Kit::Role::Board)) {
					Pose p0;
					if (FitBox(P.board, Kit::Get(Kit::Role::Board), { 0.0, 0.0, -L::kBoardH * 0.5 }, { L::kBoardW, L::kBoardD, L::kBoardH * 2.0 }, 0.0, false, p0)) {
						const Quat t = RotY(pose.tilt);
						Scene::Place(P.board, pose.c + t.Rotate(p0.pos), t * p0.q, p0.s);
						Scene::Hide(P.board, false);
					}
				} else {
					Scene::Place(P.board, pose.c, RotY(pose.tilt), 1.0f);
					// ours wears the game's own wood (the table's), lighter, a patch of it to a face
					if (Game(Kit::Role::Table)) {
						if (auto* wood = Scene::Skin(P.kitchen)) {
							auto& lk = Scene::LookOf(P.board);
							lk.skin = wood;
							lk.tint[0] = 1.45f, lk.tint[1] = 1.3f, lk.tint[2] = 1.12f;
							lk.uv[0] = lk.uv[1] = 0.45f;
						}
					}
				}
			}
			if (P.knife >= 0) {
				if (g_knifeOn || g_knifeAnim >= 0) {
					const double a = g_knifeAnim < 0 ? 0.0 : g_knifeAnim < 0.06 ? g_knifeAnim / 0.06 : 1.0 - (g_knifeAnim - 0.06) / 0.14;
					double       top = 0.0;  // how tall what's on its lane stands
					for (const auto& u : g_units) {
						if (u.at == Unit::At::Board && u.lane == g_knifeLane) {
							top = (std::max)(top, Thick(u, u.boardLen, 2));
						}
					}
					const double z = kBoardTop + (top + 1.6) * (1.0 - Clamp(a, 0.0, 1.0));
					if (Game(Kit::Role::Knife)) {
						// the game's knife on its edge across the lane: long along y, its blade's depth up, its thin side along x
						Lie lie;
						if (LieOf(P.knife, lie)) {
							static const double kCyc[3][3] = { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } };
							// turned over about its length so the edge is down, toward the food
							// (the silver knife is modelled the other way up: it isn't turned)
							std::string kp = Kit::Get(Kit::Role::Knife).path;
							std::transform(kp.begin(), kp.end(), kp.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							const bool  flipped = kp.find("silver") != std::string::npos;
							const Quat  q = (g_knifeAlong ? RotZ(kPi * 0.5) : Quat{}) * (flipped ? Quat{} : RotY(kPi)) * FromRows(kCyc) * lie.q;
							const double ks = 8.5 / lie.d[0];
							const Vec3   c{ g_knifeX, g_knifeY, z + lie.d[1] * ks * 0.5 };
							Scene::Place(P.knife, c - q.Rotate(lie.c) * ks, q, static_cast<float>(ks));
							Scene::Hide(P.knife, false);
						}
					} else {
						if (g_knifeAlong) {
							Scene::Place(P.knife, { g_knifeX - 3.8, g_knifeY, z }, RotZ(-kPi * 0.5), 1.0f);
						} else {
							Scene::Place(P.knife, { g_knifeX, g_knifeY - 3.8, z }, {}, 1.0f);
						}
					}
				} else if (Game(Kit::Role::Knife)) {
					Lie lie;
					if (LieOf(P.knife, lie)) {
						const Quat   q = RotZ(0.2) * lie.q;
						const double ks = 8.5 / lie.d[0];
						const Vec3   c{ L::kBoardX + 3.0, L::kBoardY - 9.6, lie.d[2] * ks * 0.5 };
						Scene::Place(P.knife, c - q.Rotate(lie.c) * ks, q, static_cast<float>(ks));
						Scene::Hide(P.knife, false);
					}
				} else {
					Scene::Place(P.knife, { L::kBoardX + 8.2, L::kBoardY - 9.2, 0.12 }, RotZ(kPi * 0.5) * RotY(kPi * 0.5), 1.0f);  // its tip to the left
				}
			}
			// ---- the pieces: the ingredient's own mesh, cut to each
			for (int b = 0; b < g_chunks.Count() && b < static_cast<int>(g_pieces.size()); ++b) {
				const auto& body = g_chunks.At(b);
				auto&       pc = g_pieces[b];
				// a leek's piece landing in the pot: from here on, our own solid leek's (as the cabbage turns to its bits there)
				if (pc.alt >= 0 && body.state == hc::Chunks::State::Pot) {
					Scene::Hide(pc.prop, true);
					std::swap(pc.prop, pc.alt);
					pc.alt = -1;
				}
				if (pc.prop < 0 || g_units.empty()) {
					continue;
				}
				if (body.state == hc::Chunks::State::None ||
					(g_flow == hc::Flow::Oven && g_recipe && !g_recipe->pie && !g_topping && body.state == hc::Chunks::State::Pot && g_ball)) {
					Scene::Hide(pc.prop, true);  // (a bread's potato worked into the ball of dough)
					Scene::Hide(pc.core, true);
					continue;
				}
				auto&      u = g_units[(std::min)(pc.unit, static_cast<int>(g_units.size()) - 1)];
				Quat       frame = body.state == hc::Chunks::State::Board ? RotY(g_boardPose.tilt) * body.q : body.q;
				Vec3       at = body.p;
				if (pc.onDough) {
					// on the dough until it's folded up (then the dish's own shape stands for it)
					if (!g_oven || DoughReady() || (!g_doughOnBoard && !g_topping) || g_oven->InOven() || g_oven->Out() || g_peelT >= 0 || g_peelLoaded) {
						Scene::Hide(pc.prop, true);
						Scene::Hide(pc.core, true);
						continue;
					}
					// a dumpling's filling goes under each corner as it folds over it (the corner over its quarter)
					if (Dumpling() && g_rolled) {
						const double pa = std::atan2(pc.doughOff.y, pc.doughOff.x);
						const int    k = static_cast<int>(std::floor(Wrap(pa) / (kPi * 0.5) + 4.0)) % 4;  // (corner k points at pi/4 + k pi/2)
						if (g_fold[k] > 0.45 && std::hypot(pc.doughOff.x, pc.doughOff.y) > 1.1) {  // (the middle of the heap stays, showing between the corners)
							Scene::Hide(pc.prop, true);
							Scene::Hide(pc.core, true);
							continue;
						}
					}
					frame = RotZ(pc.turn) * body.q;
					at = g_doughAt + pc.doughOff + Vec3{ 0, 0, 0.9 };
					if (Dumpling() && g_rolled) {
						// pressed down into the parcel as its corners come over it
						const double ff = (g_fold[0] + g_fold[1] + g_fold[2] + g_fold[3]) * 0.25;
						at = g_doughAt + Vec3{ pc.doughOff.x * (1.0 - 0.3 * ff), pc.doughOff.y * (1.0 - 0.3 * ff), pc.doughOff.z * (1.0 - 0.6 * ff) + 0.9 - 0.45 * ff };
					}
				}
				// in the fruit bowl as it tips to pour: they ride with it, not through its side
				if (!pc.onDough && g_flow == hc::Flow::Oven && body.state == hc::Chunks::State::Pot && g_bowlTip > 0.001 && (g_hold == Hold::Apples || g_bowlTip > 0.01) &&
					!(BreadCuts() && g_cutInMix)) {
					const Vec3   base = g_hold == Hold::Apples ? g_carry : kFillAt;  // (the bowl turns about this, as it's drawn)
					const double ang = std::atan2(PourAt().y - base.y, PourAt().x - base.x);
					const Quat   T = RotZ(ang) * RotY(g_bowlTip * 1.7) * RotZ(-ang);
					at = base + T.Rotate(at - base);
					frame = T * frame;
				}
				// in the pot it breaks down as it cooks: smaller and smaller (to about half by the end)
				// (it goes with the pot's own doneness, the meter's: half done, half broken down)
				const double done = g_pot && body.state == hc::Chunks::State::Pot && g_flow == hc::Flow::Pot ? Clamp(g_pot->Cooked() / 0.95, 0.0, 1.3) : 0.0;
				const double broken = Clamp(done, 0.0, 1.0);
				// (a pie's fruit in its bowl or on the dough is heaped smaller, so it all fits)
				// (a bread's potato, stirred into its dough, breaks up smaller and smaller as it's mixed)
				const double mixedIn = BreadCuts() && g_cutInMix ? 1.0 - 0.8 * Clamp(g_mix, 0.0, 1.0) : 1.0;
				const double shrink = g_flow == hc::Flow::Oven && pc.onDough ? 0.42 : g_flow == hc::Flow::Oven && body.state == hc::Chunks::State::Pot ? 0.7 * mixedIn : 1.0 - 0.55 * broken;
				if (g_flow == hc::Flow::Pot && body.state == hc::Chunks::State::Pot) {
					// sunk into it as it thickens, some deeper than others; under the finished stew by the end, and gone
					const double fade = StewFade();
					// gone one by one as it cooks down - every one of them before the finished stew starts to show
					const double cookedNow = g_pot ? g_pot->Cooked() : 0.0;
					if (fade > 0.0 || cookedNow > 0.3 + 0.18 * Hash(b, 42)) {
						Scene::Hide(pc.prop, true);
						Scene::Hide(pc.core, true);
						for (int m = 0; m < 2; ++m) {
							if (b * 2 + m < static_cast<int>(P.leafBits.size()) && P.leafBits[b * 2 + m] >= 0) {
								Scene::Hide(P.leafBits[b * 2 + m], true);
							}
						}
						continue;
					}
					at.z -= 0.45 * broken + 0.35 * broken * Hash(b, 41) + 1.6 * fade;
					// bobbing on the simmer, each its own way, turning a little (a leek as it was in 0.4.8: still)
					if (g_pot && u.name.find("eek") == std::string::npos) {
						const double boil = Clamp((g_pot->Temp() - 0.35) / 0.5, 0.0, 1.0);
						const double ph = g_clock * (1.3 + 0.9 * Hash(b, 43)) + Hash(b, 44) * 6.283;
						at.z += (0.1 + 0.16 * boil) * std::sin(ph);
						frame = RotX(0.08 * boil * std::sin(ph * 0.7 + 1.1)) * RotY(0.08 * boil * std::cos(ph * 0.6)) * frame;
					}
				}
				const Vec3   c = at - frame.Rotate(pc.mid * shrink);  // where its whole would have its middle
				auto& lk = Scene::LookOf(pc.prop);
				lk.shadow = body.state != hc::Chunks::State::Pot;
				PotHollow(lk, g_flow == hc::Flow::Pot && body.state == hc::Chunks::State::Pot);
				// boiled, not browned: softening, a little darker and taking the broth's colour as it cooks (well
				// on the way by halfway), skin and cut faces alike
				// cooked, not washed out: darker and warmer, keeping their own colour, only a little of the broth's
				const double boiled = Clamp(done * 1.1, 0.0, 1.0);
				lk.cookMode = body.charred ? 1 : 0;
				lk.cook[0] = body.charred ? 1.7f : 0.0f;
				static constexpr double kWarm[3] = { 0.62, 0.5, 0.36 };  // the deepest a stewed piece goes, per channel
				for (int k = 0; k < 3; ++k) {
					lk.lift[k] = g_broth[k] * 0.7f;
					lk.tint[k] = static_cast<float>(1.0 - (1.0 - kWarm[k]) * boiled);
				}
				lk.lift[3] = static_cast<float>(0.18 * boiled);
				PlacePiece(pc.prop, pc.core, u, c, frame, pc.box, pc.cut, shrink);
				// a cabbage in the pot: its leaves are open shells, and cut to bits (and sinking, tumbling) they showed as
				// loose shards - in the pot each bit is drawn as what chopped cabbage is: a few thin, curved layers of leaf, cut
				// square, in strips or wedges, stacked a little apart (about its size)
				// a cabbage or a leek in the pot: drawn as what they are chopped - thin curved bits of leaf - not their own meshes cut
				// up (open leaves and loose layers, which floated as shards and fanned strips however they were cut)
				// (a leek's stalk is our own solid one: its slices drawn as they are. Its leafy part - from a third of the way along
				// from the root - is chopped greens: curved bits of leaf, not the thin flat leaf blades cut short)
				bool leekBits = false;
				if (u.name.find("eek") != std::string::npos && std::abs(pc.box[0]) < 1e6 && std::abs(pc.box[1]) < 1e6) {
					leekBits = (pc.box[0] + pc.box[1]) * 0.5 / u.boardLen + 0.5 > 0.36;
				}
				if (g_flow == hc::Flow::Pot && body.state == hc::Chunks::State::Pot && (u.name.find("abbage") != std::string::npos || leekBits)) {
					const double R[3] = { u.boardLen * 0.5 * shrink, (std::max)(0.05, Thick(u, u.boardLen, 1) * 0.5) * shrink, (std::max)(0.05, Thick(u, u.boardLen, 2) * 0.5) * shrink };
					double       lo[3], hi[3];
					for (int ax = 0; ax < 3; ++ax) {
						lo[ax] = (std::max)(std::abs(pc.box[ax * 2]) < 1e6 ? pc.box[ax * 2] * shrink : -R[ax], -R[ax]);
						hi[ax] = (std::min)(std::abs(pc.box[ax * 2 + 1]) < 1e6 ? pc.box[ax * 2 + 1] * shrink : R[ax], R[ax]);
					}
					Scene::Hide(pc.prop, true);
					if (pc.core >= 0) {
						Scene::Hide(pc.core, true);
					}
					// each piece as two bits of leaf (a cabbage's worth fills the pot as it should)
					if (static_cast<int>(P.leafBits.size()) < (b + 1) * 2) {
						P.leafBits.resize((b + 1) * 2, -1);
					}
					for (int m = 0; m < 2; ++m) {
						const int j = b * 2 + m;
						auto&     leaf = P.leafBits[j];
						if (leaf < 0) {
							leaf = Add(fmt::format("LeafBit{}", j % 9).c_str());
						}
						if (leaf < 0) {
							continue;
						}
						if (!(hi[0] > lo[0] && hi[1] > lo[1] && hi[2] > lo[2])) {
							Scene::Hide(leaf, true);
							continue;
						}
						// the two side by side, across the piece
						const Vec3 mid{ (lo[0] + hi[0]) * 0.5 + (m - 0.5) * 0.5 * (hi[0] - lo[0]) + (Hash(j, 87) - 0.5) * 0.6,
							(lo[1] + hi[1]) * 0.5 + (m - 0.5) * 0.5 * (hi[1] - lo[1]) + (Hash(j, 88) - 0.5) * 0.6, (lo[2] + hi[2]) * 0.5 };
						// (a thin slice across a wide head: sized between the two)
						// (a leek's piece: the slice's length along it, about the stalk across - its fanned leaves aren't counted)
						const double size = leekBits ? Clamp((hi[0] - lo[0]) * 1.5, 0.55, u.boardLen * 0.11) * (0.8 + 0.35 * Hash(j, 83))
						                             : std::sqrt((hi[0] - lo[0]) * (hi[1] - lo[1])) * 1.0 * (0.8 + 0.35 * Hash(j, 83));
						// floating near flat in the broth, each turned and tipped a little its own way
						const Quat q = RotZ(Hash(j, 84) * 6.283 + g_spin) * RotX((Hash(j, 85) - 0.5) * 0.45) * RotY((Hash(j, 86) - 0.5) * 0.35);
						Scene::Place(leaf, c + frame.Rotate(mid), q, static_cast<float>(size));
						Scene::Hide(leaf, false);
						auto& cl = Scene::LookOf(leaf);
						if (auto* tex = Scene::Skin(pc.prop)) {
							cl.skin = tex;  // (the cabbage's own leaf: green and veined)
							cl.uv[0] = 0.35f, cl.uv[1] = 0.35f;
							cl.uv[2] = static_cast<float>(Hash(j, 81)), cl.uv[3] = static_cast<float>(Hash(j, 82));
						}
						// its texture's dark patches lifted to a pale leaf green (they read as shadows on it), wilting
						// toward the broth's colour as it cooks
						static constexpr float kLeaf[3] = { 0.8f, 0.88f, 0.62f };
						// (a leek: pale near the root, green toward the leaves - by where the piece was along it)
						static constexpr float kLeekWhite[3] = { 0.8f, 0.88f, 0.55f }, kLeekGreen[3] = { 0.5f, 0.66f, 0.3f };
						const float g = leekBits ? static_cast<float>(0.55 + 0.45 * Clamp(((lo[0] + hi[0]) * 0.5 / (std::max)(shrink, 1e-3) / u.boardLen + 0.5 - 0.36) / 0.3, 0.0, 1.0)) : 0.0f;
						for (int k = 0; k < 3; ++k) {
							cl.tint[k] = lk.tint[k];
							const float leaf = leekBits ? kLeekWhite[k] * (1.0f - g) + kLeekGreen[k] * g : kLeaf[k];
							cl.lift[k] = leaf * 0.7f + lk.lift[k] * 0.3f;
						}
						cl.lift[3] = (leekBits ? 0.6f : 0.35f) + lk.lift[3];
						cl.shadow = false;
						PotHollow(cl, true);
					}
				} else {
					for (int m = 0; m < 2; ++m) {
						if (b * 2 + m < static_cast<int>(P.leafBits.size()) && P.leafBits[b * 2 + m] >= 0) {
							Scene::Hide(P.leafBits[b * 2 + m], true);
						}
					}
				}
			}
			// ---- the pot, the broth, the ladle, bubbles and steam
			if (P.broth >= 0 && g_pot) {
				const bool  any = g_chunks.CountIn(hc::Chunks::State::Pot) + g_tossedIn > 0;
				const float c = static_cast<float>(Clamp(g_pot->Cooked(), 0.0, 1.5));
				const float thick = any ? (std::min)(c / 0.95f, 1.0f) : 0.0f;  // how far it's come together
				// it cooks down: a little lower, thick and opaque, deeper and richer in colour
				Scene::Place(P.broth, { L::kPotX, L::kPotY, L::kLiquidZ - 0.45 * thick }, RotZ(std::fmod(t * 0.05, 6.283) + g_spin), 1.0f);
				auto& lk = Scene::LookOf(P.broth);
				lk.unshadowed = true;  // (the pot wall's shadow lay across it)
				PotHollow(lk, true);
				if (P.pot >= 0) {
					auto& pl = Scene::LookOf(P.pot);
					PotHollow(pl, true);  // (its inside darker down toward the bottom)
					pl.occludes = false;  // (too big to shade by as a ball)
				}
				if (P.dishRef >= 0) {
					PotHollow(Scene::LookOf(P.dishRef), true);
				}
				static constexpr float kDeep[3] = { 0.78f, 0.6f, 0.42f };  // what the broth's colour comes to, by channel
				for (int j = 0; j < 3; ++j) {
					lk.lift[j] = any ? g_broth[j] * (1.0f - (1.0f - kDeep[j]) * thick) * (1.0f - 0.12f * (std::max)(0.0f, c - 1.0f)) : (j == 2 ? 0.52f : 0.55f);
				}
				lk.lift[3] = any ? 0.82f + 0.17f * thick : 0.6f;  // plain water until something's in it; thick by the end
				{
					// its surface rippling, harder as it boils (calmer as it thickens); fat glinting on it once there's food in
					const double tpNow = g_pot->Temp();
					lk.fx2[0] = static_cast<float>(0.2 + 0.8 * Clamp((tpNow - 0.3) / 0.55, 0.0, 1.0)) * (1.0f - 0.4f * thick);
					lk.fx2[1] = any ? 0.55f * (1.0f - 0.5f * thick) : 0.0f;
					if (P.dishRef >= 0) {
						auto& dr = Scene::LookOf(P.dishRef);
						dr.fx2[0] = lk.fx2[0] * 0.5f;
						dr.fx2[1] = 0.3f;
					}
				}
				// the finished stew showing through more and more (from a third of the way), the whole of its look by the end
				if (P.dishRef >= 0) {
					const double fade = StewFade();
					Vec3         fmn, fmx;
					const bool   show = fade > 0.01 && !SauceBowl() && Scene::FlatBounds(P.dishRef, fmn, fmx);  // (a sauce is just its colour)
					Scene::Hide(P.dishRef, !show);
					if (show) {
						const double wide = (std::max)(fmx.x - fmn.x, fmx.y - fmn.y);
						// a little past the pot's inside, and trimmed to it (below): no gap at the edge. A sauce's dish is
						// mostly jar - its sauce a small part of what's measured - so it's taken much wider
						const double fill = SauceBowl() ? 6.4 : 1.12;
						const double sc = fill * 2.0 * (L::kPotR - 0.2) / (std::max)(0.1, wide);
						const Vec3   mid{ (fmn.x + fmx.x) * 0.5, (fmn.y + fmx.y) * 0.5, fmx.z * 0.3 };  // (its top, as flattened below)
						const Vec3   top{ L::kPotX, L::kPotY, L::kLiquidZ - 0.45 * thick + 0.08 };
						const Quat   q = RotZ(std::fmod(t * 0.05, 6.283) + g_spin);
						Scene::Place(P.dishRef, top - q.Rotate(mid) * sc, q, static_cast<float>(sc));
						Scene::Stretch(P.dishRef, 1.0f, 1.0f, 0.3f);  // (flattened: only its top shows over the broth)
						auto& dl = Scene::LookOf(P.dishRef);
						dl.alpha = static_cast<float>(fade);
						dl.shadow = false;
						// trimmed to the pot: six planes round its inside (nothing out through its wall)
						for (int k = 0; k < 6; ++k) {
							const double ca = std::cos(k * kPi / 3.0), sa = std::sin(k * kPi / 3.0);
							dl.clips[k][0] = static_cast<float>(ca), dl.clips[k][1] = static_cast<float>(sa), dl.clips[k][2] = 0.0f;
							dl.clips[k][3] = static_cast<float>(ca * L::kPotX + sa * L::kPotY + 0.93 * L::kPotR);
						}
					}
				}
				const double tp = g_pot->Temp();
				if (tp > 0.42) {
					const int n = static_cast<int>((tp - 0.42) * 40.0);
					for (int k = 0; k < n; ++k) {
						const double rate = 0.9 + Hash(k, 12);
						const double ph = t * rate + Hash(k, 13);
						const int    cyc = static_cast<int>(std::floor(ph));
						const double p = ph - cyc;
						const double a = Hash(k * 17 + cyc, 14) * 6.283, r = std::sqrt(Hash(k * 17 + cyc, 15)) * (L::kPotR - 0.8);
						Render3D::Sprite s;
						s.pos[0] = static_cast<float>(L::kPotX + std::cos(a) * r);
						s.pos[1] = static_cast<float>(L::kPotY + std::sin(a) * r);
						s.pos[2] = static_cast<float>(L::kLiquidZ - 0.45 * thick + 0.05);
						s.size = static_cast<float>(0.12 + 0.35 * p);
						s.color[0] = s.color[1] = s.color[2] = 0.95f;
						s.color[3] = static_cast<float>(0.8 * (1.0 - p * p));
						s.kind = Render3D::Sprite::Kind::Bubble;
						sp.push_back(s);
					}
				}
				if (tp > 0.35) {
					for (int k = 0; k < 7; ++k) {
						const double p = std::fmod(t * 0.22 + k / 7.0, 1.0);
						Render3D::Sprite s;
						s.pos[0] = static_cast<float>(L::kPotX + (Hash(k, 16) - 0.5) * 6.0 + std::sin(t * 0.6 + k) * p * 2.0);
						s.pos[1] = static_cast<float>(L::kPotY + (Hash(k, 17) - 0.5) * 6.0);
						s.pos[2] = static_cast<float>(L::kLiquidZ + 1.0 + p * 16.0);
						s.size = static_cast<float>(1.6 + p * 5.0);
						s.color[0] = s.color[1] = s.color[2] = 0.92f;
						s.color[3] = static_cast<float>(0.14 * (1.0 - p) * Clamp((tp - 0.35) * 3.0, 0.0, 1.0));
						s.seed = 40.0f + k * 1.9f;
						s.kind = Render3D::Sprite::Kind::Smoke;
						sp.push_back(s);
					}
				}
				// smoke when it catches
				if (g_pot->Burn() > 0.35) {
					for (int k = 0; k < 4; ++k) {
						const double p = std::fmod(t * 0.3 + k / 4.0, 1.0);
						Render3D::Sprite s;
						s.pos[0] = static_cast<float>(L::kPotX + (Hash(k, 18) - 0.5) * 4.0);
						s.pos[1] = static_cast<float>(L::kPotY + (Hash(k, 19) - 0.5) * 4.0);
						s.pos[2] = static_cast<float>(L::kLiquidZ + 2.0 + p * 14.0);
						s.size = static_cast<float>(2.0 + p * 5.0);
						s.color[0] = s.color[1] = s.color[2] = 0.18f;
						s.color[3] = static_cast<float>(0.3 * (1.0 - p) * Clamp(g_pot->Burn(), 0.0, 1.0));
						s.seed = 60.0f + k;
						s.kind = Render3D::Sprite::Kind::Smoke;
						sp.push_back(s);
					}
				}
			}
			if (P.ladle >= 0) {
				Vec3 mn, mx;
				if (Game(Kit::Role::Ladle) && Scene::Bounds(P.ladle, mn, mx) && mx.x - mn.x > mx.y - mn.y) {
					// the game's ladle as it was made: upright, its cup at the +x end opening up, the handle rising
					// along -x. Turned so the handle points out of the pot (and away from it once lifted out); the cup
					// sits in the broth, or level under the pointer with a ladleful in it.
					const double len = mx.x - mn.x, ls = 12.0 / len;
					const double cupR = (mx.y - mn.y) * 0.5;
					const Vec3   cup{ mx.x - cupR, (mn.y + mx.y) * 0.5, mn.z + cupR * 0.9 };  // the middle of its cup, at the rim
					const bool   out = g_hold == Hold::LadleOut;
					// leaned back so the handle climbs out over the rim (the cup tips a little with it); held out, nearly level
					const Quat   q = RotZ(g_ladleA - kPi) * RotY(out ? 0.12 : 0.85);
					const Vec3   at = out ? g_ladleOutAt : Vec3{ L::kPotX + std::cos(g_ladleA) * g_ladleR, L::kPotY + std::sin(g_ladleA) * g_ladleR, L::kLiquidZ - 0.3 };
					Scene::Place(P.ladle, at - q.Rotate(cup) * ls, q, static_cast<float>(ls));
					Scene::Hide(P.ladle, false);
					const bool full = out && g_chunks.CountIn(hc::Chunks::State::Pot) + g_tossedIn > 0;
					// a ladleful drips from the bottom of the cup as it's carried
					if (full) {
						static double s_drip = 0.0;
						s_drip -= 1.0 / 60.0;
						if (s_drip <= 0.0) {
							s_drip = 0.25 + 0.35 * Rand01();
							const float drop[4] = { g_broth[0] * 0.9f, g_broth[1] * 0.9f, g_broth[2] * 0.9f, 0.9f };
							Spray(at + Vec3{ 0, 0, -cupR * ls * 1.1 }, 1, drop, 0.2, -0.5, 0.16f, true, Render3D::Sprite::Kind::Grain, 0.8);
						}
					}
					if (P.ladleSoup >= 0) {
						const double r = cupR * ls * 0.82;
						Scene::Place(P.ladleSoup, at + Vec3{ 0, 0, -cupR * ls * 0.3 }, RotZ(g_clock * 0.3), static_cast<float>(r / 6.1));
						Scene::Hide(P.ladleSoup, !full);
						// the stew itself in the cup, as much as it shows in the pot
						Vec3         fmn, fmx;
						const double fade = StewFade();
						const bool   stew = full && fade > 0.01 && !SauceBowl() && P.ladleStew >= 0 && Scene::FlatBounds(P.ladleStew, fmn, fmx);
						if (P.ladleStew >= 0) {
							Scene::Hide(P.ladleStew, !stew);
						}
						if (stew) {
							const double wide = (std::max)(fmx.x - fmn.x, fmx.y - fmn.y);
							const double sc = 2.0 * r / (std::max)(0.1, wide);
							const Vec3   mid{ (fmn.x + fmx.x) * 0.5, (fmn.y + fmx.y) * 0.5, fmx.z * 0.3 };
							const Quat   rq = RotZ(g_clock * 0.3);
							Scene::Place(P.ladleStew, at + Vec3{ 0, 0, -cupR * ls * 0.3 + 0.04 } - rq.Rotate(mid) * sc, rq, static_cast<float>(sc));
							Scene::Stretch(P.ladleStew, 1.0f, 1.0f, 0.3f);
							auto& dl = Scene::LookOf(P.ladleStew);
							dl.alpha = static_cast<float>(fade);
							dl.shadow = false;
						}
						if (P.broth >= 0) {
							auto&       sl = Scene::LookOf(P.ladleSoup);
							const auto& bl = Scene::LookOf(P.broth);
							std::copy(std::begin(bl.lift), std::end(bl.lift), sl.lift);
							std::copy(std::begin(bl.tint), std::end(bl.tint), sl.tint);
							sl.shadow = false;
						}
					}
				} else if (g_hold == Hold::LadleOut) {
					const double handle = std::atan2(-0.45, -0.25);
					Scene::Place(P.ladle, g_ladleOutAt, RotZ(g_ladleA - handle), 1.0f);
				} else {
					const Vec3   at{ L::kPotX + std::cos(g_ladleA) * g_ladleR, L::kPotY + std::sin(g_ladleA) * g_ladleR, L::kLiquidZ + 0.4 };
					const double handle = std::atan2(-0.45, -0.25);  // which way the mesh's handle points
					Scene::Place(P.ladle, at, RotZ(g_ladleA - handle), 1.0f);
				}
			}
			// ---- the spit's rod and crank
			if (P.rod >= 0) {
				Scene::Place(P.rod, { L::kFireX, L::kFireY, L::kSpitZ }, RotX(g_spit ? g_spit->Angle() : 0.0), 1.0f);
			}
			// ---- the oven: door, peel, dough, the bake
			if (P.door >= 0) {
				// smaller, and out in front of the mouth: the game's oven stands further forward than ours did
				const bool gameOven = Game(Kit::Role::Oven);
				Scene::Place(P.door, { L::kOvenX - (gameOven ? 3.22 : 4.9), L::kOvenMouthY - (gameOven ? 2.2 : 0.35), L::kOvenFloorZ + (gameOven ? 1.4 : 0.0) }, RotZ(-g_door * 1.75), gameOven ? 0.7f : 1.0f);
				Scene::Stretch(P.door, gameOven ? 0.95f : 1.0f, 1.0f, gameOven ? 0.82f : 1.0f);  // (its bottom where it was; shorter, so its top clears the arch's stones)
			}
			if (P.peel >= 0) {
				Vec3   at = PeelRest();
				double rz = 0.35;
				if (g_hold == Hold::Peel) {
					at = g_peelAt;
					rz = 0.0;
				}
				if (g_peelT >= 0) {
					const double k = std::sin(Clamp(g_peelT, 0.0, 1.0) * kPi);
					at = g_peelFrom + (BreadAt() + Vec3{ 0, 0, 0.3 } - g_peelFrom) * Clamp(g_peelT * 1.4, 0.0, 1.0);
					at.z += k * 1.5;
					rz = 0.0;
				}
				Lie lie;
				if (Game(Kit::Role::Peel) && LieOf(P.peel, lie)) {
					const double ps = 21.0 / lie.d[0];
					const Quat   q = RotZ(rz + kPi * 0.5) * lie.q;
					const Vec3   c = at + Vec3{ 0.0, -7.0, lie.d[2] * ps * 0.5 };
					Scene::Place(P.peel, c - q.Rotate(lie.c) * ps, q, static_cast<float>(ps));
					Scene::Hide(P.peel, false);
				} else {
					Scene::Place(P.peel, at, RotZ(rz), 1.0f);
					// in the cutting board's wood
					if (auto* wood = Game(Kit::Role::Table) && P.kitchen >= 0 ? Scene::Skin(P.kitchen) : nullptr) {  // (as the board wears it)
						auto& pl = Scene::LookOf(P.peel);
						pl.skin = wood;
						// light oak: the grain kept, the whole drawn to a pale golden brown
						pl.tint[0] = 1.6f, pl.tint[1] = 1.4f, pl.tint[2] = 1.15f;
						pl.lift[0] = 0.8f, pl.lift[1] = 0.62f, pl.lift[2] = 0.4f, pl.lift[3] = 0.45f;
						pl.uv[0] = pl.uv[1] = 0.45f;
					}
				}
			}
			if (g_oven) {
				const bool baking = g_oven->InOven() || (g_peelT >= 0 && g_peelIn);
				Vec3       at = g_doughAt;
				if (g_hold == Hold::Dough && g_strokeLen > 0 && !board_kneading()) {
					at = { g_carry.x, g_carry.y, kBoardTop + 4.0 };
				}
				if (g_hold == Hold::Peel && g_peelLoaded) {
					at = g_peelAt + Vec3{ 0, 0, 0.3 };  // on the peel
				}
				if (g_peelT >= 0) {
					at = g_peelFrom + (BreadAt() - g_peelFrom) * Clamp(g_peelT * 1.4, 0.0, 1.0);
					at.z += std::sin(Clamp(g_peelT, 0.0, 1.0) * kPi) * 1.5 + 0.3;
				}
				if (g_oven->InOven()) {
					at = BreadAt();
				}
				if (g_hold == Hold::Bread) {
					at = g_carry;
				}
				if (g_peelBread) {
					at = (g_hold == Hold::Peel ? g_peelAt : PeelRest()) + Vec3{ 0, 0, 0.3 };
				}
				// ready for the oven: it takes the shape of what it'll be (raw); in the oven and after, the dish itself
				const bool ready = DoughReady();
				const bool showDish = baking || g_hold == Hold::Bread || g_peelBread || ready;
				if (showDish && P.result < 0 && g_recipe) {
					P.result = AddResult();
				}
				Vec3 mn, mx;
				if (showDish && P.result >= 0 && Scene::Bounds(P.result, mn, mx)) {
					const double big = (std::max)(mx.x - mn.x, mx.y - mn.y);
					const double s = BakeSize() / (std::max)(0.1, big);  // one size: raw on the board, in the oven, on the plate
					const Vec3   c{ (mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, mn.z };
					Scene::Place(P.result, at - c * s, {}, static_cast<float>(s));
					auto& lk = Scene::LookOf(P.result);
					lk.cookMode = 1;
					const double br = g_oven->Brown();
					lk.cook[0] = static_cast<float>(br > 1.2 ? (br - 1.2) * 1.6 : 0.0);  // its own colour is "golden": browner only once it's past done
					lk.lift[0] = 0.94f;
					lk.lift[1] = 0.86f;
					lk.lift[2] = 0.7f;
					lk.lift[3] = -static_cast<float>(Clamp(0.85 - br * 0.9, 0.0, 0.85));  // pale and doughy at first (its fruit stays fruit)
					Scene::Hide(P.result, false);
					Scene::Hide(P.dough, true);
					Scene::Hide(P.disk, true);
					Scene::Hide(P.slab, true);  // (the rolled-out sheet is the dumpling now)
				} else {
					if (P.result >= 0 && !showDish) {
						Scene::Hide(P.result, true);
					}
					for (auto id : P.doughPieces) {
						Scene::Hide(id, true);
					}
					const bool dough = g_doughParts > 0 && g_doughOnBoard && !g_oven->Out();
					const bool square = Dumpling() && g_flat > 0.0 && !g_rolled;
					Scene::Hide(P.dough, !dough || g_rolled || square);
					Scene::Hide(P.disk, !dough || !g_rolled || square || Dumpling());  // (a dumpling's sheet is drawn in its parts)
					Scene::Hide(P.slab, !dough || !square);
					DrawEgg();
					DrawFlour(sp);
					// the rolling pin: out beside the dough while it's to be rolled; held, it rolls across under the pointer
					if (P.pin >= 0) {
						const bool rolling = dough && !g_rolled && g_oven->Pie() && g_oven->Dough() >= 0.9 && g_flat < 1.0;
						const bool shown = g_oven->Pie();  // (on the table from the start)
						Scene::Hide(P.pin, !shown);
						if (shown) {
							const double f = Clamp(g_flat, 0.0, 1.0);
							if (g_pinHeld) {
								// in hand: hovering over the board, down on the dough while pressed, turned across the way it's going
								const double h = (Dumpling() ? 2.6 - 2.34 * f : 1.0 + 1.0 * (1.0 - f)) + 0.05;
								const bool   onDough = std::hypot(g_pinAt.x - g_doughAt.x, g_pinAt.y - g_doughAt.y) < 6.0;
								const double z = kBoardTop + (onDough ? h : 0.0) + 0.72 + (g_hold == Hold::Roll ? 0.0 : 1.6);
								g_pinTurn += (g_pinAxis - g_pinTurn) * 0.25;  // (turning to it, not snapping)
								Scene::Place(P.pin, { g_pinAt.x, g_pinAt.y, z }, RotZ(kPi * 0.5 + g_pinTurn * kPi * 0.5) * RotX(g_pinRoll / 0.72), 1.0f);
							} else {
								Scene::Place(P.pin, { L::kBoardX, L::kBoardY - L::kBoardD * 0.5 - 1.3, 0.72 }, RotZ(0.04), 1.0f);  // on the table, in front of the board
							}
							if (auto* wood = Game(Kit::Role::Table) && P.kitchen >= 0 ? Scene::Skin(P.kitchen) : nullptr) {  // (the board's wood)
								auto& pl = Scene::LookOf(P.pin);
								pl.skin = wood;
								// birch: pale and creamy, a little of the grain showing through
								pl.tint[0] = 1.5f, pl.tint[1] = 1.45f, pl.tint[2] = 1.3f;
								pl.lift[0] = 0.88f, pl.lift[1] = 0.8f, pl.lift[2] = 0.64f, pl.lift[3] = 0.6f;
								pl.uv[0] = pl.uv[1] = 0.3f;
							}
						}
					}
					if (dough && square) {
						// rolled out from a ball to a square sheet
						// as its corners fold in, what's left flat is the square between them: the diamond through the
						// edges' middles (the folded corners lie over it)
						double folded = 0.0;
						for (int k = 0; k < 4; ++k) {
							folded += g_fold[k] * 0.25;
						}
						// (rolled to the sheet the folding starts from: 6.8 a side, thin)
						const double f = Clamp(g_flat, 0.0, 1.0), side = (4.5 + 2.3 * f) * (1.0 - 0.293 * folded), h = 2.6 - 2.34 * f;
						Scene::Place(P.slab, at + Vec3{ 0, 0, h * 0.5 + 0.05 }, RotZ(kPi * 0.25 * folded), 1.0f);
						Scene::Stretch(P.slab, static_cast<float>(side), static_cast<float>(side), static_cast<float>(h));
						auto& sl = Scene::LookOf(P.slab);
						DoughLook(sl);
					} else if (dough && !g_rolled && g_oven->Pie() && g_flat > 0.0) {
						// a pie's dough rolled out from a ball to a round sheet
						Scene::Hide(P.dough, true);
						Scene::Hide(P.disk, false);
						const double f = Clamp(g_flat, 0.0, 1.0), r = 3.0 + 2.0 * f;
						Scene::Place(P.disk, at + Vec3{ 0, 0, 0.3 }, {}, static_cast<float>(r));
						Scene::Stretch(P.disk, 1.0f, 1.0f, static_cast<float>(2.0 * 5.0 / r * (1.0 + 2.0 * (1.0 - f))));
					} else if (dough && !g_rolled) {
						const double r = DoughRadius();
						const double sq = g_squash * 0.45;
						const Quat   q = RotZ(g_squashA);
						Scene::Place(P.dough, at + Vec3{ 0, 0, r * 0.62 * (1.0 - sq) + 0.1 }, q, static_cast<float>(r));
						Scene::Stretch(P.dough, static_cast<float>(1.0 + sq), static_cast<float>(1.0 - sq * 0.3), static_cast<float>(0.62 * (1.0 - sq)));
						auto& lk = Scene::LookOf(P.dough);
						const float smooth = static_cast<float>(Clamp(g_oven->Dough(), 0.0, 1.0));
						lk.tint[0] = lk.tint[1] = lk.tint[2] = 0.88f + 0.12f * smooth;
						if (!g_cuts.empty()) {
							// our version: the cuts made so far, the pieces eased apart
							const Scene::Look   base = lk;  // (a copy: adding props may move the looks)
							std::vector<double> c = g_cuts;
							std::sort(c.begin(), c.end());
							c.insert(c.begin(), 0.0);
							c.push_back(1.0);
							const int    n = static_cast<int>(c.size()) - 1;
							const double R = DoughRadius(), x0 = g_doughAt.x - R * 1.25, span = R * 2.5;
							while (static_cast<int>(P.doughPieces.size()) < n) {
								const auto id = Add("Dough");
								if (id < 0) {
									break;
								}
								P.doughPieces.push_back(id);
							}
							Scene::Hide(P.dough, true);
							for (int i = 0; i < n && i < static_cast<int>(P.doughPieces.size()); ++i) {
								const auto   id = P.doughPieces[i];
								const double off = (i - (n - 1) * 0.5) * 0.9;  // apart a little
								Scene::Place(id, at + Vec3{ off, 0, r * 0.62 * (1.0 - sq) + 0.1 }, q, static_cast<float>(r));
								Scene::Stretch(id, static_cast<float>(1.0 + sq), static_cast<float>(1.0 - sq * 0.3), static_cast<float>(0.62 * (1.0 - sq)));
								auto& pl = Scene::LookOf(id);
								pl = base;
								for (auto& cl : pl.clips) {
									cl[0] = cl[1] = cl[2] = cl[3] = 0.0f;
								}
								// not drawn left of its first cut or right of its second (world planes: n.p > d)
								const double a = x0 + span * c[i] + off, b = x0 + span * c[i + 1] + off;
								if (i > 0) {
									pl.clips[0][0] = -1.0f, pl.clips[0][3] = static_cast<float>(-a);
								}
								if (i < n - 1) {
									pl.clips[1][0] = 1.0f, pl.clips[1][3] = static_cast<float>(b);
								}
								pl.flesh[0] = 0.93f, pl.flesh[1] = 0.86f, pl.flesh[2] = 0.72f, pl.flesh[3] = 1.0f;
								Scene::Hide(id, false);
							}
						}
					} else if (dough) {
						// a pie's sheet drawn in under its fluted rim as the rim's made (the scallops are its edge then)
						const double c = g_oven->Crimped() >= 0.9 ? 1.0 : Clamp(g_oven->Crimped(), 0.0, 1.0);
						// a crostata's sheet drawn in to its folded size too (its edge is under the pleats)
						const double r = !g_oven->Pie() || Dumpling() ? 5.0 : Crostata() ? 5.0 - 5.0 * (1.0 - kPieRim) * c : 5.0 - (5.0 - 4.3 * kPieRim) * c;
						Scene::Place(P.disk, at + Vec3{ 0, 0, 0.3 }, {}, static_cast<float>(r));
						Scene::Stretch(P.disk, 1.0f, 1.0f, static_cast<float>(2.0 * 5.0 / r));
					}
				}
				// the mixing bowl: water clouding with flour as it's stirred, then a ball of dough
				if (P.mixWater >= 0) {
					const double m = Clamp(static_cast<double>(g_doughParts) / (std::max)(1, g_doughNeeded), 0.0, 1.0);
					const bool   water = !g_ball && g_doughNeeded > 0;
					Scene::Place(P.mixWater, kMixAt + Vec3{ 0, 0, 1.5 + 0.4 * g_mix }, RotZ(g_mixA * 0.3), 0.52f);
					Scene::Hide(P.mixWater, !water);
					auto& wl = Scene::LookOf(P.mixWater);
					// clear water (a cool blue over the bowl's wood), clouding with the flour as it goes in and is stirred
					const double cloud = Clamp(m * 0.45 + g_mix * 0.6, 0.0, 1.0);
					if (g_bowlWater) {
						wl.lift[0] = static_cast<float>(0.32 + 0.6 * cloud);
						wl.lift[1] = static_cast<float>(0.46 + 0.4 * cloud);
						wl.lift[2] = static_cast<float>(0.6 + 0.12 * cloud);
						wl.lift[3] = static_cast<float>(0.45 + 0.5 * cloud);
					} else {
						// an empty bowl, then the flour and the rest heaped in it, then the milk wetting it
						Scene::Hide(P.mixWater, !water || g_doughParts == 0);
						wl.lift[0] = g_milkIn ? 0.95f : 0.9f;
						wl.lift[1] = g_milkIn ? 0.92f : 0.86f;
						wl.lift[2] = g_milkIn ? 0.84f : 0.76f;
						wl.lift[3] = 0.95f;
					}
					wl.shadow = false;
					if (P.mixBall >= 0) {
						const bool inBowl = (g_mix > 0.25 || g_ball) && !g_doughOnBoard && g_hold != Hold::Ball && !g_topping;
						const bool held = g_hold == Hold::Ball;
						const double r = 1.0 + 1.6 * Clamp(g_mix, 0.0, 1.0);
						Scene::Hide(P.mixBall, !(inBowl || held));
						if (inBowl || held) {
							const Vec3 c = held ? g_carry : kMixAt + Vec3{ 0, 0, 1.6 + r * 0.45 };
							Scene::Place(P.mixBall, c, RotZ(g_mixA), static_cast<float>(r));
							Scene::Stretch(P.mixBall, 1.0f, 1.0f, 0.75f);
							auto& bl = Scene::LookOf(P.mixBall);
							bl.tint[0] = bl.tint[1] = bl.tint[2] = static_cast<float>(0.82 + 0.1 * g_mix);
						}
					}
				}
				// the filling on the pie, and its crimped edge
				for (auto& [id, off] : P.fill) {
					if (id < 0) {
						continue;
					}
					const bool show = g_rolled && !showDish && g_peelT < 0 && g_hold != Hold::Dough && !g_oven->Out() && !g_oven->InOven();
					Scene::Hide(id, !show);
					if (show) {
						const int ui = static_cast<int>(off.z);
						// a round berry (or bit of fruit), wearing the colour of what it came from
						const float br = static_cast<float>(0.32 + 0.1 * Hash(id, 24));
						// (drawn in with the pie's edge as it's crimped or folded: the whole pie a tenth smaller by the end)
						const double shrink = Dumpling() ? 1.0 : 1.0 - (1.0 - kPieRim) * (g_oven->Crimped() >= 0.9 ? 1.0 : Clamp(g_oven->Crimped(), 0.0, 1.0));
						Scene::Place(id, at + Vec3{ off.x * shrink, off.y * shrink, 0.75 + br }, RotZ(Hash(id, 21) * 6.28), br);
						auto& lk = Scene::LookOf(id);
						std::string bn;
						for (const auto& u : g_units) {
							if (&u - g_units.data() == ui) {
								lk.skin = Scene::Skin(u.prop);
								bn = u.name;
							}
						}
						lk.shadow = false;  // (berries on the dough: no shadow on them or from them)
						lk.unshadowed = true;
						lk.uv[0] = lk.uv[1] = 0.12f;
						lk.uv[2] = 0.45f + Hash(id, 22) * 0.1f;
						lk.uv[3] = 0.45f + Hash(id, 23) * 0.1f;
						// the berries' own colour (their mesh's texture is as much leaf and stalk as berry)
						std::transform(bn.begin(), bn.end(), bn.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
						const float* col = nullptr;
						static const float kSnow[3] = { 0.36f, 0.035f, 0.05f }, kJuniper[3] = { 0.16f, 0.19f, 0.34f }, kJazbay[3] = { 0.2f, 0.06f, 0.24f };
						col = bn.find("snowberr") != std::string::npos ? kSnow : bn.find("juniper") != std::string::npos ? kJuniper : bn.find("jazbay") != std::string::npos ? kJazbay : nullptr;
						if (col) {
							lk.lift[0] = col[0], lk.lift[1] = col[1], lk.lift[2] = col[2], lk.lift[3] = 0.9f;
						}
					}
				}
				if (g_rolled && Dumpling()) {
					// the square sheet as its four corner flaps (together, the whole sheet), each rolled up the side of the
					// filling and its corner laid back over the top, by how far it's folded: a parcel, the apple showing
					// between the corners (each flap drawn at the nearest of nine stages of its fold)
					constexpr int kStages = 9;
					while (static_cast<int>(P.dumpFolds.size()) < 4 * kStages) {
						P.dumpFolds.push_back(Add(fmt::format("DumplingFold{}", P.dumpFolds.size() % kStages).c_str()));
					}
					for (auto id : P.crimps) {
						Scene::Hide(id, true);
					}
					const bool sheet = !showDish && g_peelT < 0 && g_hold != Hold::Dough && !g_oven->Out() && !g_oven->InOven();
					if (P.slab >= 0) {
						Scene::Hide(P.slab, true);
					}
					for (int k = 0; k < 4; ++k) {
						const int    st = static_cast<int>(std::lround(Clamp(g_fold[k], 0.0, 1.0) * (kStages - 1)));
						const double a = kPi * 0.25 + k * kPi * 0.5;
						for (int j = 0; j < kStages; ++j) {
							const auto id = P.dumpFolds[k * kStages + j];
							const bool show = sheet && j == st;
							Scene::Hide(id, !show);
							if (show) {
								Scene::Place(id, at + Vec3{ 0, 0, 0.02 }, RotZ(a), 1.0f);
								DoughLook(Scene::LookOf(id));
							}
						}
					}
				} else if (g_rolled) {
					const bool live = !showDish && g_peelT < 0 && g_hold != Hold::Dough;
					if (Crostata()) {
						// a galette: eight broad pleats of the rim, each laid in over the fruit as its three pinches are made
						while (static_cast<int>(P.galette.size()) < 8) {
							P.galette.push_back(Add(fmt::format("GaletteFold{}", P.galette.size()).c_str()));
						}
						for (int j = 0; j < 8; ++j) {
							const auto id = P.galette[j];
							const bool show = live && (g_oven->Crimped() >= 0.9 || g_oven->CrimpedAt(3 * j) || g_oven->CrimpedAt(3 * j + 1) || g_oven->CrimpedAt(3 * j + 2));
							Scene::Hide(id, !show);
							if (show) {
								const double a = (3 * j + 1.5) / hc::Oven::kCrimps * 2 * kPi - kPi;
								Scene::Place(id, at, RotZ(a), static_cast<float>(kPieRim));
								DoughLook(Scene::LookOf(id));
							}
						}
					} else {
						// a fluted rim: a scallop for each pinch, standing up off the sheet's edge (the sheet drawn in under it)
						while (static_cast<int>(P.pieRim.size()) < hc::Oven::kCrimps) {
							P.pieRim.push_back(Add("PieCrimp"));
						}
						for (int k = 0; k < hc::Oven::kCrimps; ++k) {
							const auto id = P.pieRim[k];
							// (the step's done at nine tenths: the last few pinches are taken as made, so no gaps are left)
							const bool show = live && (g_oven->CrimpedAt(k) || g_oven->Crimped() >= 0.9);
							Scene::Hide(id, !show);
							if (show) {
								const double a = (k + 0.5) / hc::Oven::kCrimps * 2 * kPi - kPi;
								Scene::Place(id, at, RotZ(a), static_cast<float>(kPieRim));
								DoughLook(Scene::LookOf(id));
							}
						}
					}
				}
			}
			// ---- our version: each portion ladled into its own bowl (the game's dish, in place of the empty bowl)
			if ((g_flow == hc::Flow::Pot || g_flow == hc::Flow::Oven) && ServeCount() > 1) {
				P.slotDish.resize(g_slots.size(), -1);
				for (int k = 0; k < static_cast<int>(g_slots.size()); ++k) {
					if (g_slots[k].count == 0) {
						continue;
					}
					auto& id = P.slotDish[k];
					if (id < 0) {
						id = AddResult();
						if (id < 0) {
							continue;
						}
						const Scene::Id bowl = k == 0 ? P.bowl : (k - 1 < static_cast<int>(P.extraServe.size()) ? P.extraServe[k - 1] : -1);
						if (bowl >= 0 && g_flow == hc::Flow::Pot) {
							Scene::Hide(bowl, true);  // (the stew comes in its own bowl; the bake lies on the plate)
						}
					}
					Vec3 mn, mx;
					if (!Scene::Bounds(id, mn, mx)) {
						continue;
					}
					const double big = (std::max)(mx.x - mn.x, mx.y - mn.y);
					const bool   oven = g_flow == hc::Flow::Oven;
					// a bake by its piece's size: a small piece smaller on its plate
					double       f = 1.0;
					if (oven && k < static_cast<int>(g_batch.size()) && g_timer.time > 0.0) {
						f = Clamp(std::sqrt(g_batch[k].time / g_timer.time), 0.6, 1.3);
					}
					const double sc = (oven ? BakeSize() * 0.8 * f : 7.5) / (std::max)(0.1, big);
					const Vec3   c{ (mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, mn.z };
					const Quat   q = RotZ(0.6 * k);
					Scene::Place(id, SlotAt(k) + Vec3{ 0, 0, oven ? 0.45 : 0.0 } - q.Rotate(c) * sc, q, static_cast<float>(sc));
					auto& lk = Scene::LookOf(id);
					lk.lift[3] = 0.0f;
					lk.fx[2] = 0.0f;
					lk.cookMode = g_slots[k].tier == hc::Tier::Burnt ? 1 : 0;
					lk.cook[0] = 1.65f;
					Scene::Hide(id, false);
				}
			}
			// ---- the dish served
			if (g_phase == Phase::Result && P.result >= 0) {
				Vec3 mn, mx;
				if (Scene::Bounds(P.result, mn, mx)) {
					const double big = (std::max)(mx.x - mn.x, mx.y - mn.y);
					const double s = (g_flow == hc::Flow::Pot ? 7.5 : BakeSize()) / (std::max)(0.1, big);
					const Vec3   c{ (mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, mn.z };
					const Quat   q = RotZ(std::fmod((g_clock - g_serveAt) * 0.4, 6.283));
					const Vec3   on{ L::kServeX, L::kServeY, g_flow == hc::Flow::Pot ? 0.0 : 0.45 };
					Scene::Place(P.result, on - q.Rotate(c) * s, q, static_cast<float>(s));
					auto& lk = Scene::LookOf(P.result);
					lk.lift[3] = 0.0f;
					lk.fx[2] = 0.0f;
					lk.cookMode = g_burntLook ? 1 : 0;
					lk.cook[0] = 1.65f;
					if (g_flow == hc::Flow::Oven && g_oven && !g_burntLook) {
						// served as it came out: underdone stays pale (as it looked in the oven)
						const double br = g_oven->Brown();
						lk.cookMode = 1;
						lk.cook[0] = static_cast<float>(br > 1.2 ? (br - 1.2) * 1.6 : 0.0);  // (golden as it's made, browner only past done)
						lk.lift[0] = 0.94f, lk.lift[1] = 0.86f, lk.lift[2] = 0.7f;
						lk.lift[3] = -static_cast<float>(Clamp(0.85 - br * 0.9, 0.0, 0.85));
					}
					Scene::Hide(P.result, false);
				}
			}
			// ---- the salt in the hand, the grains in the air
			if (g_hold == Hold::Salt) {
				for (int k = 0; k < static_cast<int>(4 + g_pinch * 10); ++k) {
					Render3D::Sprite s;
					s.pos[0] = static_cast<float>(g_carry.x + (Hash(k, 24) - 0.5) * 0.8);
					s.pos[1] = static_cast<float>(g_carry.y + (Hash(k, 25) - 0.5) * 0.8);
					s.pos[2] = static_cast<float>(g_carry.z + Hash(k, 26) * 0.5);
					s.size = 0.1f;
					s.color[0] = s.color[1] = s.color[2] = 0.96f;
					s.color[3] = 1.0f;
					s.kind = Render3D::Sprite::Kind::Grain;
					sp.push_back(s);
				}
			}
			// the pinch of salt lying on what's in the mixing bowl, until it's stirred in
			if (g_flow == hc::Flow::Oven && g_oven && P.mixBowl >= 0 && !g_ball && !g_doughOnBoard && g_oven->Seasoning().amount > 0.01 && g_mix < 0.3) {
				const double amt = Clamp(g_oven->Seasoning().amount, 0.0, 1.0);
				const int    n = static_cast<int>(14 + amt * 40);
				const float  fade = static_cast<float>(1.0 - Clamp(g_mix / 0.3, 0.0, 1.0));
				for (int k = 0; k < n; ++k) {
					const double a = Hash(k, 31) * 6.283, r = std::sqrt(Hash(k, 32)) * 1.1;
					Render3D::Sprite s;
					s.pos[0] = static_cast<float>(kMixAt.x + 0.4 + std::cos(a) * r);
					s.pos[1] = static_cast<float>(kMixAt.y - 0.3 + std::sin(a) * r * 0.8);
					s.pos[2] = static_cast<float>(kMixAt.z + 1.5 + 0.4 * g_mix + 0.12 + (1.1 - r) * 0.12);
					s.size = 0.09f;
					s.color[0] = s.color[1] = 0.97f;
					s.color[2] = 0.95f;
					s.color[3] = fade;
					s.kind = Render3D::Sprite::Kind::Grain;
					sp.push_back(s);
				}
			}
			for (const auto& g : g_grains) {
				Render3D::Sprite s;
				s.pos[0] = static_cast<float>(g.p.x);
				s.pos[1] = static_cast<float>(g.p.y);
				s.pos[2] = static_cast<float>(g.p.z);
				const double k = g.age / (std::max)(0.01, g.life);
				s.size = g.kind == Render3D::Sprite::Kind::Smoke ? g.size * static_cast<float>(1.0 + k * 1.5) : g.size;
				std::memcpy(s.color, g.col, sizeof(s.color));
				s.color[3] *= static_cast<float>(1.0 - k);
				s.kind = g.kind;
				s.seed = static_cast<float>(&g - g_grains.data()) * 0.7f;
				sp.push_back(s);
			}
			Scene::SetSprites(std::move(sp));
		}

		bool board_kneading()
		{
			// a stroke that stays on the board is kneading; one that leaves it is carrying the dough away
			return std::abs(g_carry.x - L::kBoardX) < L::kBoardW * 0.5 + 1.0 && std::abs(g_carry.y - L::kBoardY) < L::kBoardD * 0.5 + 1.0;
		}

		// ---------------------------------------------------------------- the menu's buttons, and the keys
		void Leave()
		{
			spdlog::info("Left the kitchen");
			CloseKitchen(true);
		}

		void ProcessActs()
		{
			std::vector<std::pair<Act, int>> acts;
			{
				std::lock_guard lk(g_actMtx);
				acts.swap(g_acts);
			}
			for (const auto& [act, arg] : acts) {
				switch (act) {
				case Act::Select:
					g_sel = arg;
					g_portions = 1;
					break;
				case Act::Portions:
					g_portions = (std::max)(1, arg);
					break;
				case Act::Cut:
					if (NeedCut() && static_cast<int>(g_cuts.size()) < ServeCount() - 1) {
						const double at = Clamp(arg / 1000.0, 0.02, 0.98);
						bool         tooClose = false;
						for (double c : g_cuts) {
							tooClose |= std::abs(c - at) < 0.03;
						}
						if (!tooClose) {
							g_cuts.push_back(at);
							Audio::Take("chop_", 0.6f);
							if (static_cast<int>(g_cuts.size()) == ServeCount() - 1) {
								FinishCuts();
							}
						}
					}
					break;
				case Act::CutUndo:
					if (!g_batchCut && !g_cuts.empty()) {
						g_cuts.pop_back();
					}
					break;
				case Act::CookByHand:
					if (g_phase == Phase::Book && g_sel >= 0 && g_sel < static_cast<int>(g_book.size())) {
						const auto* r = g_book[g_sel];
						if (Recipes::CanMake(*r) > 0) {
							StartCooking(r);
						} else {
							Toast("You don't have what it takes");
						}
					}
					break;
				case Act::QuickCook:
					// the game's own menu, as it was: its dishes are Plain
					g_quick = true;
					spdlog::info("Quick Cook: the game's own menu");
					Audio::StopAll();
					Scene::Close();
					g_phase = Phase::Idle;
					ShowCrafting(true);
					break;
				case Act::Leave:
					Leave();
					return;
				case Act::Abandon:
					if (g_phase == Phase::Cooking) {
						g_confirm = true;
					}
					break;
				case Act::AbandonYes:
					g_confirm = false;
					spdlog::info("Put the dish aside (nothing used up)");
					Audio::StopAll();
					ShowStation();
					g_phase = Phase::Book;
					g_book = Recipes::ForStation(g_ovenStation);
					break;
				case Act::AbandonNo:
					g_confirm = false;
					break;
				case Act::Again:
					Audio::StopAll();
					ShowStation();
					g_phase = Phase::Book;
					g_book = Recipes::ForStation(g_ovenStation);
					g_sel = std::clamp(g_sel, 0, (std::max)(0, static_cast<int>(g_book.size()) - 1));
					break;
				case Act::Done:
					Leave();
					return;
				case Act::Help:
					g_help = !g_help;
					break;
				case Act::Mute:
					g_soundBox = !g_soundBox;
					g_help = false;
					if (!g_soundBox) {
						Config::Save();
					}
					break;
				case Act::Knife:
					NextKnife();
					break;
				}
			}
		}

		// The next knife the game has (then Hearthcooking's own, then the one found by itself), kept in the INI
		void NextKnife()
		{
			if (P.knife < 0) {
				return;
			}
			std::vector<std::string> list = Kit::Knives();
			list.push_back("ours");
			const auto& now = Kit::Get(Kit::Role::Knife);
			const std::string cur = now.vanilla ? now.path : std::string("ours");
			int at = -1;
			for (int k = 0; k < static_cast<int>(list.size()); ++k) {
				if (_stricmp(list[k].c_str(), cur.c_str()) == 0) {
					at = k;
				}
			}
			const std::string next = list[(at + 1) % list.size()];
			Kit::SetMesh(Kit::Role::Knife, next);
			Scene::Remove(P.knife);
			P.knife = KitAdd(Kit::Role::Knife);
			std::string shown = next == "ours" ? std::string("Hearthcooking's own") : next.substr(next.find_last_of("\\/") + 1);
			Toast(fmt::format("Knife: {}", shown));
		}

		void HandleKeys()
		{
			for (int key : Overlay::TakeKeys()) {
				if (key == 0x01) {  // Esc
					if (g_soundBox) {
						g_soundBox = false;
						Config::Save();
					} else if (g_help) {
						g_help = false;
					} else if (g_confirm) {
						g_confirm = false;
					} else if (g_hold != Hold::None) {
						Cancel();
					} else if (g_phase == Phase::Book || g_phase == Phase::Result) {
						Post(Act::Leave);
					} else if (g_phase == Phase::Cooking) {
						g_confirm = true;
					}
				} else if (key == 0x0F) {  // Tab
					if (g_phase == Phase::Cooking) {
						g_confirm = !g_confirm;
					} else if (g_phase == Phase::Book) {
						Post(Act::Leave);
					}
				} else if (key == 0x13 && g_phase == Phase::Cooking && (g_flow == hc::Flow::Pot || g_flow == hc::Flow::Oven)) {  // R
					g_knifeAlong = !g_knifeAlong;
				} else if (key == 0x31 && g_phase == Phase::Cooking) {  // N: the next knife
					NextKnife();
				} else if (key == 0x23) {  // H
					g_help = !g_help;
				} else if (key == 0x32 || key == Overlay::kPadSoundKey) {  // M: the sound panel
					g_soundBox = !g_soundBox;
					g_help = false;
					if (!g_soundBox) {
						Config::Save();
					}
				} else if (g_phase == Phase::Book && (key == 0xC8 || key == 0x11)) {  // up / W
					g_sel = (std::max)(0, g_sel - 1);
				} else if (g_phase == Phase::Book && (key == 0xD0 || key == 0x1F)) {  // down / S
					g_sel = (std::min)(static_cast<int>(g_book.size()) - 1, g_sel + 1);
				} else if (g_phase == Phase::Book && (key == 0x1C || key == 0x12)) {  // Enter / E
					Post(Act::CookByHand);
				}
			}
		}

		// ---------------------------------------------------------------- the interface
		struct CardLine
		{
			std::string text;
			bool        done = false;
		};

		std::vector<CardLine> Steps()
		{
			std::vector<CardLine> s;
			if (!g_recipe) {
				return s;
			}
			auto unitsDone = [&](Role a_role, std::initializer_list<Unit::At> a_where, const char* a_pre, const char* a_post) {
				std::map<std::string, std::pair<int, int>> by;
				for (const auto& u : g_units) {
					if (u.role != a_role) {
						continue;
					}
					auto& e = by[u.name];
					++e.first;
					for (auto w : a_where) {
						if (u.at == w) {
							++e.second;
							break;
						}
					}
				}
				// in the order they go in (a dough's own order, where it has one), else by name
				std::vector<std::pair<std::string, std::pair<int, int>>> list(by.begin(), by.end());
				if (a_role == Role::Chop) {
					std::stable_sort(list.begin(), list.end(), [](const auto& a_a, const auto& a_b) { return ChopRank(IngKey(a_a.first)) < ChopRank(IngKey(a_b.first)); });
				}
				if (a_role == Role::Dough) {
					const auto order = DoughOrder();
					auto rank = [&](const std::string& a_n) {
						std::string l = a_n;
						std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
						for (int g = 0; g < static_cast<int>(order.size()); ++g) {
							for (const auto& w : order[g]) {
								if (l.find(w) != std::string::npos) {
									return g;
								}
							}
						}
						return 99;
					};
					std::stable_sort(list.begin(), list.end(), [&](const auto& a_a, const auto& a_b) { return rank(a_a.first) < rank(a_b.first); });
				}
				for (const auto& [name, c] : list) {
					s.push_back({ fmt::format("{}{}{}{}", a_pre, name, a_post, c.first > 1 ? fmt::format(" ({})", c.first) : ""), c.second >= c.first });
				}
			};
			if (g_flow == hc::Flow::Pot) {
				bool anyChop = false;
				for (const auto& u : g_units) {
					anyChop |= u.role == Role::Chop;
				}
				if (anyChop) {
					std::map<std::string, std::pair<int, int>> by;
					for (const auto& u : g_units) {
						if (u.role == Role::Chop) {
							auto& e = by[u.name];
							++e.first;
							e.second += u.cut.cutCount > 0 || u.at == Unit::At::Pot;
						}
					}
					// (in the order they go in: a sauce's tomato first, to cook down, the garlic after it)
					std::vector<std::pair<std::string, std::pair<int, int>>> list(by.begin(), by.end());
					std::stable_sort(list.begin(), list.end(), [](const auto& a_a, const auto& a_b) { return ChopRank(IngKey(a_a.first)) < ChopRank(IngKey(a_b.first)); });
					// each diced and tipped in before the next is cut
					for (const auto& [name, c] : list) {
						s.push_back({ DiceLine(name, c.first), c.second >= c.first });
						int n = 0, in = 0;
						for (const auto& u : g_units) {
							if (u.role == Role::Chop && u.name == name) {
								++n;
								in += u.at == Unit::At::Pot;
							}
						}
						s.push_back({ fmt::format("Tip in the {}{}", name, n > 1 ? fmt::format(" ({})", n) : ""), in >= n });
					}
				}
				if (SauceBowl()) {
					// its liquids into the bowl, then the bowl into the pot
					bool anyL = false, allIn = true;
					for (const auto& u : g_units) {
						if (u.role == Role::Toss && Liquid(u)) {
							anyL = true;
							s.push_back({ fmt::format("Into the bowl: the {}", u.name), u.byBowl });
							allIn &= u.at == Unit::At::Pot;
						}
					}
					if (anyL) {
						s.push_back({ "Pour the bowl into the pot", allIn });
					}
					for (const auto& u : g_units) {
						if (u.role == Role::Toss && !Liquid(u)) {
							s.push_back({ fmt::format("Add the {}", u.name), u.at == Unit::At::Pot });
						}
					}
				} else {
					unitsDone(Role::Toss, { Unit::At::Pot }, "Add the ", "");
				}
				if (g_recipe->salted) {
					s.push_back({ "A pinch of salt", g_pot && g_pot->Seasoning().amount >= 0.7 });
				}
				s.push_back({ "Firewood under the pot", g_fire.Logs() > 0 || (g_pot && g_pot->Cooked() > 0.05) });
				s.push_back({ "Keep it at a simmer, and stir", g_pot && g_pot->Cooked() >= 0.92 });
				s.push_back({ "Ladle it into the bowl", g_served });
			} else if (g_flow == hc::Flow::Spit) {
				if (g_recipe->salted) {
					s.push_back({ "Salt the meat", g_spit && g_spit->Seasoning().amount >= 0.7 });
				}
				s.push_back({ "Firewood onto the fire", g_fire.Logs() > 0 || Mounted() > 0 });
				unitsDone(Role::Meat, { Unit::At::Spit }, "Put the ", " on the spit");
				unitsDone(Role::Toss, { Unit::At::Gone }, "Rub in the ", "");
				s.push_back({ "Turn it over the fire", g_spit && g_spit->Mean() >= 0.9 });
				s.push_back({ "Take it off onto the plate", g_served });
			} else {
				// a pie's fruit is cut first and waits in a bowl
				bool anyDice = false, allInBowl = true, allOnDough = true;
				std::map<std::string, std::pair<int, int>> diced;
				for (const auto& u : g_units) {
					if (!u.dice) {
						continue;
					}
					anyDice = true;
					auto& e = diced[u.name];
					++e.first;
					e.second += u.cut.cutCount + u.cut.alongCount > 0 || u.at == Unit::At::Pot || u.at == Unit::At::Dough;
					allInBowl &= u.at == Unit::At::Pot || u.at == Unit::At::Dough;
					allOnDough &= u.at == Unit::At::Dough;
				}
				for (const auto& [name, c] : diced) {
					s.push_back({ DiceLine(name, c.first), c.second >= c.first });
				}
				if (anyDice) {
					s.push_back({ BreadCuts() ? "Sweep it into the right bowl" : "Sweep it into the bowl", allInBowl });
				}
				if (g_doughNeeded > 0) {
					const std::size_t from = s.size();
					unitsDone(Role::Dough, { Unit::At::Dough }, g_bowlWater ? "Into the water: the " : "Into the bowl: the ", g_bowlWater ? "" : "");
					if (anyDice && BreadCuts()) {
						// a bread's cut things (a potato bread's potato) poured in after the flour
						std::size_t at = s.size();
						for (std::size_t k = from; k < s.size(); ++k) {
							std::string l = s[k].text;
							std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							if (l.find("flour") != std::string::npos) {
								at = k + 1;
							}
						}
						s.insert(s.begin() + static_cast<std::ptrdiff_t>(at), CardLine{ "Pour the right bowl into the left bowl", g_cutInMix });
					}
				}
				if (g_recipe->salted) {
					s.push_back({ "A pinch of salt", g_oven && g_oven->Seasoning().amount >= 0.7 });
				}
				if (g_doughNeeded > 0) {
					s.push_back({ "Stir it into a dough", g_ball });
					s.push_back({ "Onto the board", g_doughOnBoard });
					s.push_back({ "Knead the dough", g_oven && g_oven->Dough() >= 0.9 });
					if (g_recipe->pie) {
						s.push_back({ "Pick up rolling pin and flatten to shape", g_flat >= 1.0 });
					}
				}
				if (g_recipe->pie) {
					if (anyDice) {
						s.push_back({ "Heap the filling on the dough", allOnDough });
					}
					bool other = false;
					for (const auto& u : g_units) {
						other |= u.role == Role::Filling && !u.dice;
					}
					if (other) {
						for (const auto& u : g_units) {
							if (u.role == Role::Filling && !u.dice) {
								s.push_back({ fmt::format("Add the {}", u.name), u.at == Unit::At::Dough });
							}
						}
						(void)anyDice;  // (no spreading: the filling covers the inside as it goes in)
					}
					s.push_back({ Dumpling() ? "Fold the four corners in over it" : Crostata() ? "Fold the edge in over the fruit" : "Crimp the edge", g_oven && g_oven->Crimped() >= 0.9 });
				} else {
					if (g_topping) {
						bool topped = true;
						for (const auto& u : g_units) {
							topped &= !u.dice || u.at == Unit::At::Dough;
						}
						s.push_back({ g_recipe->makes > 1 ? fmt::format("Cut the bread into {} slices", g_recipe->makes) : std::string("Cut the bread into slices"), BreadCut() });
						s.push_back({ "Butter it", g_buttered });
						s.push_back({ "The garlic on top", topped });
					} else {
						for (const auto& u : g_units) {
							if (u.role == Role::Filling && !u.dice) {
								s.push_back({ fmt::format("Work in the {}", u.name), u.at == Unit::At::Dough });
							}
						}
					}
				}
				s.push_back({ "Firewood into the oven", g_fire.Logs() > 0 || (g_oven && (g_oven->InOven() || g_oven->Out())) });
				s.push_back({ "Into the oven with the peel", g_oven && (g_oven->InOven() || g_oven->Out()) });
				s.push_back({ "Close the oven door", g_doorShut });
				s.push_back({ "Bake it golden", g_oven && g_oven->Brown() >= 0.85 });
				s.push_back({ "Open the oven door", g_doorReopened });
				s.push_back({ "Out with the peel onto the plate", g_served });
			}
			return s;
		}

		std::string Hint()
		{
			if (!Config::Get().hints) {
				return {};
			}
			switch (g_hold) {
			case Hold::Ingredient:
				return g_flow == hc::Flow::Pot ? "Onto the board to chop it, or into the pot as it is" : g_flow == hc::Flow::Spit ? "Onto the spit, or rub it on what's there" : "Onto the board";
			case Hold::Log:
				return "Into the fire for more heat";
			case Hold::FireLog:
				return "Drop it anywhere to take the heat down (back on the fire to keep it)";
			case Hold::Board:
				return g_flow == hc::Flow::Oven ? "Its right end over the empty bowl: it tips in" : "Drag right to tip it into the pot; up and down to line it up";
			case Hold::Ladle:
				return Overlay::UsingPad() ? "Circle the right stick to stir. Left stick lifts it out to the bowl when it's done" : "Round and round. Lift it out to the bowl when it's done";
			case Hold::LadleOut:
				return "Over the bowl to serve, or back into the pot";
			case Hold::Salt:
				return "Hold for a bigger pinch. Let go over the dish";
			case Hold::Crank:
				return Overlay::UsingPad() ? "Circle the right stick to turn it: a quarter turn every few seconds is plenty" : "Turn it round: a quarter turn every few seconds is plenty";
			case Hold::Meat:
				return "Onto the plate to serve, or back on the spit";
			case Hold::Dough:
				return g_oven && g_oven->Pie() && g_oven->Dough() >= 0.9 ? "Roll it out with the rolling pin" : "A long push to knead";
			case Hold::Roll:
				return Dumpling() ? "Hold and roll it about over the dough, out square" : "Hold and roll it about over the dough, out round";
			case Hold::Mix:
				return Overlay::UsingPad() ? "Circle the right stick, round and round until it comes together" : "Round and round until it comes together";
			case Hold::Ball:
				return "Onto the board";
			case Hold::Apples:
				return "Over the dough: it tips and pours";
			case Hold::Peel:
				return g_peelBread ? "Onto the plate" : g_peelLoaded ? "Into the open oven" : g_oven && g_oven->InOven() ? "Into the open oven to take it out" : "Slide it under the dough";
			case Hold::Spread:
				return "Spread it out to the edge";
			case Hold::Crimp:
				return g_foldSide >= 0 ? "Draw the corner in, over the filling" : "Pinch your way all round the edge";
			case Hold::Door:
				return "Left to open, right to shut";
			case Hold::Bread:
				return "Onto the plate";
			default:
				break;
			}
			if (g_flow == hc::Flow::Pot && g_pot && !g_served && g_hold == Hold::None && g_pot->Cooked() >= 0.92) {
				return "It's ready: drag the ladle out of the pot and let go over the bowl";
			}
			if (g_flow == hc::Flow::Oven && g_hold == Hold::None && AnyOnBoard()) {
				bool cut = true;
				for (const auto& u : g_units) {
					cut &= u.at != Unit::At::Board || u.cut.cutCount + u.cut.alongCount > 0;
				}
				if (cut && !g_knifeOn) {
					return "Lift the board by its left end and carry it over the empty bowl";
				}
				if (cut) {
					return "Cut more, or lift the board by its left end and carry it over the empty bowl";
				}
			}
			if (g_knifeOn) {
				if (Overlay::UsingPad()) {
					return g_knifeAlong ? fmt::format("{} to cut along it. {} turns the knife back across", Btn("A"), Btn("RB"))
					                    : fmt::format("{} to chop across. {} turns the knife to cut along: both ways makes dice", Btn("A"), Btn("RB"));
				}
				return g_knifeAlong ? "Click to cut along it. Right-click (or R) turns the knife back across"
				                    : "Click to chop across. Right-click (or R) turns the knife to cut along: both ways makes dice";
			}
			for (const auto& st : Steps()) {
				if (!st.done) {
					return st.text;
				}
			}
			return {};
		}

		void DrawBook(float W, float H)
		{
			auto*       fg = ImGui::GetBackgroundDrawList();
			Vignette(fg, W, H);
			const float pw = S(470), ph = (std::min)(H - S(160), S(760));
			const ImVec2 lp(W * 0.5f - pw - S(18), (H - ph) * 0.5f - S(20)), rp(W * 0.5f + S(18), lp.y);
			// our version: the book's cover, dark leather and gold, the pages on it, a spine between them
			Panel(fg, ImVec2(lp.x - S(30), lp.y - S(30)), ImVec2(rp.x + pw + S(30), lp.y + ph + S(30)));
			fg->AddRectFilledMultiColor(ImVec2(W * 0.5f - S(16), lp.y - S(6)), ImVec2(W * 0.5f, lp.y + ph + S(6)), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170),
				IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 0));
			fg->AddRectFilledMultiColor(ImVec2(W * 0.5f, lp.y - S(6)), ImVec2(W * 0.5f + S(16), lp.y + ph + S(6)), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 0),
				IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170));
			fg->AddLine(ImVec2(W * 0.5f, lp.y - S(6)), ImVec2(W * 0.5f, lp.y + ph + S(6)), Alpha(kTrim, 0.6f), S(1.5f));
			// the left page: what can be made here
			BeginArea("##bookL", lp, ImVec2(pw, ph));
			auto* dl = ImGui::GetWindowDrawList();
			Sheet(dl, lp, ImVec2(pw, ph), 0);
			float y = lp.y + S(28);
			TextC(dl, F.bookSC, lp.x + pw * 0.5f, y, kInk, g_ovenStation ? "The Oven" : "The Cooking Pot");
			y += S(42);
			FadeRule(dl, lp.x + S(40), lp.x + pw - S(40), y, 0.5f, kInkSoft);
			y += S(12);
			const float rowH = S(30), listTop = y, listBot = lp.y + ph - S(36);
			const int   visible = (std::max)(1, static_cast<int>((listBot - listTop) / rowH));
			static int  s_first = 0;
			if (ImGui::IsWindowHovered()) {
				s_first -= static_cast<int>(ImGui::GetIO().MouseWheel * 2);
			}
			if (g_sel < s_first) {
				s_first = g_sel;
			}
			if (g_sel >= s_first + visible) {
				s_first = g_sel - visible + 1;
			}
			s_first = std::clamp(s_first, 0, (std::max)(0, static_cast<int>(g_book.size()) - visible));
			if (g_book.empty()) {
				Para(dl, F.bookIt, lp.x + S(40), y + S(10), pw - S(80), kInkSoft, "You know no recipes for this, yet.");
			}
			for (int i = s_first; i < static_cast<int>(g_book.size()) && i < s_first + visible; ++i) {
				const auto*  r = g_book[i];
				const ImVec2 a(lp.x + S(34), y), b(lp.x + pw - S(34), y + rowH);
				bool         hov = false;
				ImGui::PushID(i);
				if (Clicked("row", a, b, &hov)) {
					Post(Act::Select, i);
				}
				ImGui::PopID();
				const bool sel = i == g_sel, can = Recipes::CanMake(*r) > 0;
				if (sel || hov) {
					dl->AddRectFilledMultiColor(a, b, IM_COL32(142, 47, 27, sel ? 60 : 30), IM_COL32(142, 47, 27, 8), IM_COL32(142, 47, 27, 8), IM_COL32(142, 47, 27, sel ? 60 : 30));
				}
				if (sel) {
					Diamond(dl, ImVec2(a.x + S(4), a.y + rowH * 0.5f), S(3.5f), kRubric, 0);
				}
				Text(dl, F.book, ImVec2(a.x + S(14), a.y + (rowH - F.book->FontSize) * 0.5f), can ? kInk : Alpha(kInkSoft, 0.6f), r->name);
				bool              any = false;
				const auto        best = Recipes::Best(*r, any);
				const char*       mark = r->flow == hc::Flow::Pot ? "pot" : r->flow == hc::Flow::Spit ? "spit" : "oven";
				const std::string tag = any ? fmt::format("{} - {}", mark, hc::TierName(best)) : mark;
				const float       tagX = b.x - Measure(F.bookIt, tag, 0) - S(4);
				Text(dl, F.bookIt, ImVec2(tagX, a.y + (rowH - F.bookIt->FontSize) * 0.5f), Alpha(any && best == hc::Tier::Masterwork ? kRubric : kInkSoft, can ? 1.0f : 0.6f), tag);
				// where it's from, small after the name, when there's room before the tag
				if (!r->source.empty()) {
					const std::string src = fmt::format("({})", r->source);
					const float       sx = a.x + S(14) + Measure(F.book, r->name, 0) + S(8);
					if (sx + Measure(F.bookIt, src, 0) < tagX - S(10)) {
						Text(dl, F.bookIt, ImVec2(sx, a.y + (rowH - F.bookIt->FontSize) * 0.5f), Alpha(kInkSoft, can ? 0.7f : 0.45f), src);
					}
				}
				y += rowH;
			}
			ImGui::End();

			// the right page: the chosen recipe
			BeginArea("##bookR", rp, ImVec2(pw, ph));
			dl = ImGui::GetWindowDrawList();
			Sheet(dl, rp, ImVec2(pw, ph), 1);
			if (g_sel >= 0 && g_sel < static_cast<int>(g_book.size())) {
				const auto* r = g_book[g_sel];
				float       ry = rp.y + S(30);
				TextC(dl, F.bookSC, rp.x + pw * 0.5f, ry, kInk, r->name);
				ry += S(40);
				const char* how = r->flow == hc::Flow::Pot ? "In the pot: chopped, simmered and stirred" : r->flow == hc::Flow::Spit ? "On the spit, turned over the fire"
				                : r->pie                    ? "A pie: dough, filling, a crimped edge, baked"
				                                            : "Kneaded and baked in the oven";
				TextC(dl, F.bookIt, rp.x + pw * 0.5f, ry, kInkSoft, how);
				if (!r->source.empty()) {
					ry += S(22);
					TextC(dl, F.bookIt, rp.x + pw * 0.5f, ry, Alpha(kInkSoft, 0.75f), "From " + r->source);
				}
				ry += S(34);
				FadeRule(dl, rp.x + S(40), rp.x + pw - S(40), ry, 0.5f, kInkSoft);
				ry += S(18);
				Text(dl, F.bookSCsm, ImVec2(rp.x + S(44), ry), kRubric, "You will need", 0.06f);
				ry += S(28);
				for (const auto& in : r->ings) {
					const int   have = Recipes::Held(in.form);
					const bool  ok = have >= in.count;
					const auto  t = in.count > 1 ? fmt::format("{} x{}", in.name, in.count) : in.name;
					Text(dl, F.book, ImVec2(rp.x + S(52), ry), ok ? kInk : kRubric, t);
					const auto h = fmt::format("{} carried", have);
					Text(dl, F.bookIt, ImVec2(rp.x + pw - S(44) - Measure(F.bookIt, h, 0), ry + S(2)), ok ? kGreenInk : kRubric, h);
					ry += S(27);
				}
				ry += S(10);
				if (r->makes > 1) {
					Text(dl, F.bookIt, ImVec2(rp.x + S(52), ry), kInkSoft, fmt::format("Makes {}", r->makes));
					ry += S(26);
				}
				bool       any = false;
				const auto best = Recipes::Best(*r, any);
				Text(dl, F.bookIt, ImVec2(rp.x + S(52), ry), kInkSoft, any ? fmt::format("Your best: {}", hc::TierName(best)) : "Not made by hand yet");
				const bool can = Recipes::CanMake(*r) > 0;
				const float by = rp.y + ph - S(118);
				if (can) {
					// our version: how many at once, as far as the ingredients go
					const int   maxP = MaxPortions(*r);
					const int   cur = std::clamp(g_portions, 1, maxP);
					const float sy = (std::max)(ry + S(34), by - S(84));
					Text(dl, F.bookSC, ImVec2(rp.x + S(52), sy), kInk, fmt::format("Portions: {}", cur), 0.02f);
					const float x0 = rp.x + S(250), x1 = rp.x + pw - S(64);
					const float ly = sy + F.bookSC->FontSize * 0.5f;
					dl->AddLine(ImVec2(x0, ly), ImVec2(x1, ly), Alpha(kInkSoft, 0.7f), S(2));
					for (int k = 1; k <= maxP; ++k) {
						const float x = maxP > 1 ? x0 + (x1 - x0) * static_cast<float>(k - 1) / static_cast<float>(maxP - 1) : x0;
						bool        hov = false;
						const auto  id = fmt::format("portion{}", k);
						if (Clicked(id.c_str(), ImVec2(x - S(16), ly - S(16)), ImVec2(x + S(16), ly + S(16)), &hov, true)) {
							Post(Act::Portions, k);
						}
						Diamond(dl, ImVec2(x, ly), k == cur ? S(9) : S(5), k == cur ? kRubric : (hov ? kInkSoft : 0), kInk);
						TextC(dl, F.bookIt, x, ly + S(12), k == cur ? kRubric : kInkSoft, std::to_string(k));
					}
				}
				if (MenuBtnInk("cook", "Cook by hand", rp.x + pw * 0.5f, by, can)) {
					Post(Act::CookByHand);
				}
				if (MenuBtnInk("quick", "Quick cook (plain)", rp.x + pw * 0.5f, by + S(44), true)) {
					Post(Act::QuickCook);
				}
			}
			ImGui::End();
			// leave, bottom centre
			BeginArea("##leave", ImVec2(W * 0.5f - S(120), H - S(70)), ImVec2(S(240), S(50)));
			if (MenuButton("leave", "Leave", W * 0.5f, H - S(62))) {
				Post(Act::Leave);
			}
			ImGui::End();
		}

		// A button in ink on the parchment
		bool MenuBtnInk(const char* a_id, const std::string& a_label, float a_cx, float a_y, bool a_enabled)
		{
			auto*       dl = ImGui::GetWindowDrawList();
			const float w = Measure(F.bookSC, a_label, 0.04f) + S(40), h = F.bookSC->FontSize + S(8);
			bool        hov = false;
			const bool  r = Clicked(a_id, ImVec2(a_cx - w * 0.5f, a_y), ImVec2(a_cx + w * 0.5f, a_y + h), &hov, a_enabled);
			const ImU32 c = !a_enabled ? Alpha(kInkSoft, 0.45f) : hov ? kRubric : kInk;
			if (hov) {
				Diamond(dl, ImVec2(a_cx - w * 0.5f + S(6), a_y + h * 0.5f), S(4), kRubric, 0);
				Diamond(dl, ImVec2(a_cx + w * 0.5f - S(6), a_y + h * 0.5f), S(4), kRubric, 0);
			}
			TextC(dl, F.bookSC, a_cx, a_y + S(3), c, a_label, 0.04f);
			return r;
		}

		void RecipeCard(float W, float H)
		{
			const auto steps = Steps();
			const float w = (std::min)(S(380), W * 0.36f), rowH = S(26);
			const float head = S(30) + S(44) + S(14), foot = S(26);
			// never more than about two fifths of the screen, however large the interface: the steps from the current
			// one on, with the done ones counted above and the rest below
			const int n = static_cast<int>(steps.size());
			int       cur = n;
			for (int i = 0; i < n; ++i) {
				if (!steps[i].done) {
					cur = i;
					break;
				}
			}
			const int fit = (std::max)(3, static_cast<int>((H * 0.42f - head - foot) / rowH));
			int       from = 0, to = n;
			if (n > fit) {
				from = (std::max)(0, (std::min)(cur - 1, n - (fit - 2)));
				to = (std::min)(n, from + fit - 2);
				if (from == 0) {
					to = (std::min)(n, fit - 1);
				}
			}
			const bool  above = from > 0, below = to < n;
			const float h = head + rowH * static_cast<float>(to - from + (above ? 1 : 0) + (below ? 1 : 0)) + foot;
			const ImVec2 pos(S(22), S(22));
			// it's only to read: clicks go through it to the table, and it fades while the pointer is over it
			static float s_fade = 1.0f;
			const ImVec2 m = ImGui::GetIO().MousePos;
			const bool   over = m.x >= pos.x && m.x <= pos.x + w && m.y >= pos.y && m.y <= pos.y + h;
			s_fade += ((over ? 0.22f : 1.0f) - s_fade) * (std::min)(1.0f, ImGui::GetIO().DeltaTime * 10.0f);
			BeginArea("##card", pos, ImVec2(w, h), ImGuiWindowFlags_NoInputs);
			auto*     dl = ImGui::GetWindowDrawList();
			const int v0 = dl->VtxBuffer.Size;
			dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
			Sheet(dl, pos, ImVec2(w, h), 0);
			Frame(dl, ImVec2(pos.x - S(2), pos.y - S(2)), ImVec2(pos.x + w + S(2), pos.y + h + S(2)));
			float y = pos.y + S(24);
			Text(dl, F.bookSC, ImVec2(pos.x + S(30), y), kInk, g_recipe ? g_recipe->name : "", 0.02f);
			y += S(42);
			FadeRule(dl, pos.x + S(26), pos.x + w - S(26), y, 0.45f, kInkSoft);
			y += S(12);
			if (above) {
				Text(dl, F.bookIt, ImVec2(pos.x + S(54), y + (rowH - F.bookIt->FontSize) * 0.5f), Alpha(kInkSoft, 0.7f),
					from == 1 ? std::string("1 step done") : fmt::format("{} steps done", from));
				y += rowH;
			}
			for (int i = from; i < to; ++i) {
				const auto& st = steps[i];
				const bool  now = i == cur;
				const ImVec2 c(pos.x + S(38), y + rowH * 0.5f);
				if (st.done) {
					dl->AddLine(ImVec2(c.x - S(5), c.y), ImVec2(c.x - S(1), c.y + S(4)), kGreenInk, S(2));
					dl->AddLine(ImVec2(c.x - S(1), c.y + S(4)), ImVec2(c.x + S(6), c.y - S(5)), kGreenInk, S(2));
				} else {
					Diamond(dl, c, S(3.5f), now ? kRubric : 0, Alpha(kInkSoft, 0.8f));
				}
				Text(dl, now ? F.book : F.bookIt, ImVec2(pos.x + S(54), y + (rowH - F.book->FontSize) * 0.5f), st.done ? Alpha(kInkSoft, 0.7f) : now ? kInk : kInkSoft, st.text);
				y += rowH;
			}
			if (below) {
				Text(dl, F.bookIt, ImVec2(pos.x + S(54), y + (rowH - F.bookIt->FontSize) * 0.5f), Alpha(kInkSoft, 0.7f),
					n - to == 1 ? std::string("1 more") : fmt::format("{} more", n - to));
			}
			dl->PopClipRect();
			if (s_fade < 0.995f) {
				for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
					auto&      col = dl->VtxBuffer[i].col;
					const auto a = static_cast<ImU32>(((col >> IM_COL32_A_SHIFT) & 0xFF) * s_fade);
					col = (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
				}
			}
			ImGui::End();
		}

		void KeyBar(float W, float H)
		{
			struct Key
			{
				const char* key;
				const char* label;
				Act         act;
			};
			const bool pad = Overlay::UsingPad();
			// the knife can be swapped from here while there's one in the kitchen
			const bool knife = g_phase == Phase::Cooking && P.knife >= 0;
			const Key  keys[4] = { { pad ? Btn("LB") : "N", "Knife", Act::Knife }, { pad ? Btn("X") : "H", "How to cook", Act::Help },
				 { pad ? Btn("Back") : "M", Audio::Muted() ? "Sound (muted)" : "Sound", Act::Mute },
				 { pad ? Btn("Y") : "Tab", g_phase == Phase::Cooking ? "Put it aside" : "Leave", g_phase == Phase::Cooking ? Act::Abandon : Act::Leave } };
			const int first = knife ? 0 : 1, count = 4;
			float     widths[4]{}, total = 0;
			for (int k = first; k < count; ++k) {
				widths[k] = (std::max)(S(26), Measure(F.sansSemi, keys[k].key, 0.04f) + S(10)) + S(9) + Measure(F.sans, Upper(keys[k].label), 0.14f);
				total += widths[k];
			}
			total += S(26) * (count - first - 1);
			const float  right = W - S(30), bottom = H - S(22), h = S(24);
			const ImVec2 area(right - total - S(120), bottom - h - S(26));
			BeginArea("##keys", area, ImVec2(W - area.x, H - area.y));
			auto* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilledMultiColor(area, ImVec2(area.x + (W - area.x) * 0.3f, H), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 158), IM_COL32(0, 0, 0, 158), IM_COL32(0, 0, 0, 0));
			dl->AddRectFilled(ImVec2(area.x + (W - area.x) * 0.3f, area.y), ImVec2(W, H), IM_COL32(0, 0, 0, 158));
			float x = right - total;
			for (int k = first; k < count; ++k) {
				bool hov = false;
				ImGui::PushID(k);
				if (Clicked("key", ImVec2(x, bottom - h - S(2)), ImVec2(x + widths[k], bottom + S(2)), &hov)) {
					if (keys[k].act == Act::Help) {
						g_help = !g_help;
					} else {
						Post(keys[k].act);
					}
				}
				ImGui::PopID();
				const ImU32  col = hov ? kWhite : kGrey;
				const float  kw = (std::max)(S(26), Measure(F.sansSemi, keys[k].key, 0.04f) + S(10));
				const ImVec2 ka(x, bottom - h), kb(x + kw, bottom);
				dl->AddRectFilled(ka, kb, IM_COL32(0, 0, 0, 90), S(4));
				dl->AddRect(ka, kb, col, S(4), 0, 1.5f * g_ui);
				TextC(dl, F.sansSemi, x + kw * 0.5f + S(0.5f), ka.y + (h - F.sansSemi->FontSize) * 0.5f, col, keys[k].key, 0.04f);
				Text(dl, F.sans, ImVec2(kb.x + S(9), ka.y + (h - F.sans->FontSize) * 0.5f), col, Upper(keys[k].label), 0.14f, 0.7f);
				x += widths[k] + S(26);
			}
			ImGui::End();
		}

		// A controller button's name, as the controller in hand calls it (Xbox, or PlayStation's)
		const char* Btn(const char* a_xbox)
		{
			if (Config::Get().padStyle != 1) {
				return a_xbox;
			}
			static const std::pair<const char*, const char*> kPs[] = { { "A", "Cross" }, { "B", "Circle" }, { "X", "Square" }, { "Y", "Triangle" }, { "LB", "L1" },
				{ "RB", "R1" }, { "LT", "L2" }, { "RT", "R2" }, { "Back", "Share" }, { "Start", "Options" } };
			for (const auto& [x, p] : kPs) {
				if (std::strcmp(x, a_xbox) == 0) {
					return p;
				}
			}
			return a_xbox;
		}
		// "click", or the controller's own
		std::string ClickWord() { return Overlay::UsingPad() ? fmt::format("press {}", Btn("A")) : std::string("click"); }
		std::string HoldWord() { return Overlay::UsingPad() ? fmt::format("hold {}", Btn("A")) : std::string("hold the button"); }

		// With a controller: while stirring or turning, the right stick goes round the pot, the bowl or the crank
		void PadOrbit()
		{
			Vec3   c, rim;
			bool   on = false;
			if (Overlay::UsingPad() && g_phase == Phase::Cooking) {
				if (g_hold == Hold::Ladle) {
					c = { L::kPotX, L::kPotY, L::kLiquidZ };
					rim = c + Vec3{ L::kPotR - 3.0, 0, 0 };
					on = true;
				} else if (g_hold == Hold::Mix) {
					c = kMixAt + Vec3{ 0, 0, 1.8 - kMixAt.z };
					rim = c + Vec3{ 2.6, 0, 0 };
					on = true;
				} else if (g_hold == Hold::Crank) {
					c = { L::kFireX + L::kCrankOff, L::kFireY, L::kSpitZ };
					rim = CrankGrip();
					on = true;
				}
			}
			float cx = 0, cy = 0, rx = 0, ry = 0;
			if (on && Scene::Project(c, cx, cy) && Scene::Project(rim, rx, ry)) {
				Overlay::SetStickOrbit(true, cx, cy, (std::max)(S(40), std::hypot(rx - cx, ry - cy)));
			} else {
				Overlay::SetStickOrbit(false);
			}
		}

		void HintLine(float W, float H)
		{
			const auto hint = Hint();
			if (hint.empty()) {
				return;
			}
			auto*       dl = ImGui::GetForegroundDrawList();
			const float tw = Measure(F.sans, hint, 0.07f);
			const float y = H - S(26) - S(25);
			dl->AddRectFilledMultiColor(ImVec2(W * 0.5f - tw * 0.5f - S(60), y - S(6)), ImVec2(W * 0.5f, y + S(31)), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 120),
				IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 0));
			dl->AddRectFilledMultiColor(ImVec2(W * 0.5f, y - S(6)), ImVec2(W * 0.5f + tw * 0.5f + S(60), y + S(31)), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 0),
				IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 120));
			Text(dl, F.sans, ImVec2(W * 0.5f - tw * 0.5f, y + (S(25) - F.sans->FontSize) * 0.5f), kWhite, hint, 0.07f, 0.9f);
		}

		// Our version (DISENO.md): a slim vertical bar by each dish, filling as it cooks: grey to its time, then the
		// masterwork second (bright gold), good (gold), plain (tan) and burnt (red) above; the seconds left beside it.
		// The words for each portion served stand under its bowl or plate.
		std::string CounterText(const hc::timing::Piece& a_p, double a_rate, ImU32& a_col)
		{
			const double left = a_p.Remaining();
			bool         raw = false;
			const auto   now = a_p.TierNow(raw);
			if (left > 0.0) {
				a_col = kWhite;
				return fmt::format("{:.1f}", a_rate > 0.0 ? left / a_rate : left);
			}
			a_col = TierColour(now, false);
			return fmt::format("+{:.1f}", -left);
		}

		void TimerBar(ImDrawList* dl, float x, float yBottom, const hc::timing::Piece& a_p, double a_rate)
		{
			namespace T = hc::timing;
			const float  w = S(11), h = S(96);
			const double top = a_p.time + T::kPlainEnd + 1.5;  // the scale, in cooking seconds
			auto Y = [&](double a_s) { return yBottom - h * static_cast<float>(Clamp(a_s / top, 0.0, 1.0)); };
			Panel(dl, ImVec2(x - w * 0.5f - S(4), Y(top) - S(4)), ImVec2(x + w * 0.5f + S(4), yBottom + S(4)), 1.0f, false);
			const struct
			{
				double from, to;
				ImU32  col;
			} zones[] = {
				{ 0.0, a_p.time, IM_COL32(95, 90, 82, 220) },
				{ a_p.time, a_p.time + T::kMasterWin, IM_COL32(255, 214, 120, 255) },
				{ a_p.time + T::kMasterWin, a_p.time + T::kHeartyEnd, IM_COL32(196, 160, 96, 235) },
				{ a_p.time + T::kHeartyEnd, a_p.time + T::kPlainEnd, IM_COL32(150, 112, 78, 230) },
				{ a_p.time + T::kPlainEnd, top, IM_COL32(150, 52, 38, 235) },
			};
			for (const auto& z : zones) {
				dl->AddRectFilled(ImVec2(x - w * 0.5f, Y(z.to)), ImVec2(x + w * 0.5f, Y(z.from)), z.col);
			}
			// how far it's come: the bar below it lit, a marker at it
			const float my = Y(a_p.cooked);
			dl->AddRectFilled(ImVec2(x - w * 0.5f, my), ImVec2(x + w * 0.5f, yBottom), IM_COL32(255, 255, 255, 60));
			dl->AddTriangleFilled(ImVec2(x - w * 0.5f - S(9), my - S(6)), ImVec2(x - w * 0.5f - S(9), my + S(6)), ImVec2(x - w * 0.5f - S(1), my), IM_COL32(250, 240, 220, 255));
			dl->AddLine(ImVec2(x - w * 0.5f - S(2), my), ImVec2(x + w * 0.5f + S(2), my), IM_COL32(250, 240, 220, 255), S(2));
			ImU32      col = kWhite;
			const auto text = CounterText(a_p, a_rate, col);
			Text(dl, F.sansSemi, ImVec2(x + w * 0.5f + S(7), my - F.sansSemi->FontSize * 0.5f), col, text, 0.02f, 0.9f);
		}

		void Glowing(ImDrawList* dl, const Vec3& a_at)
		{
			float bx = 0, by = 0;
			if (Scene::Project(a_at + Vec3{ 0, 0, 2.0 }, bx, by)) {
				const float p = 0.5f + 0.5f * static_cast<float>(std::sin(g_clock * 4.0));
				const float r = (std::max)(S(26), Scene::PixelsPerUnit(a_at + Vec3{ 0, 0, 2.0 }) * (ServeCount() > 1 ? 4.2f : 5.5f));
				dl->AddCircle(ImVec2(bx, by), r + S(6) * p, IM_COL32(255, 214, 120, static_cast<int>(120 + 100 * p)), 48, S(3));
			}
		}

		// Each portion served: its word under its bowl or plate (the free ones glow while there's something to serve)
		void SlotWords(ImDrawList* dl, bool a_glowFree)
		{
			for (int k = 0; k < static_cast<int>(g_slots.size()); ++k) {
				const auto& sl = g_slots[k];
				if (sl.count == 0) {
					if (a_glowFree) {
						Glowing(dl, SlotAt(k));
					}
					continue;
				}
				float sx = 0, sy = 0;
				if (Scene::Project(SlotAt(k) + Vec3{ 0, -5.5, 0.0 }, sx, sy)) {
					const auto word = Upper(TierWord(sl.tier, sl.raw));
					const float w = Measure(F.sansSemi, word, 0.1f) + S(24);
					Panel(dl, ImVec2(sx - w * 0.5f, sy - S(3)), ImVec2(sx + w * 0.5f, sy + F.sansSemi->FontSize + S(7)), 1.0f, false);
					TextC(dl, F.sansSemi, sx, sy + S(1), TierColour(sl.tier, sl.raw), word, 0.1f, 0.9f);
				}
			}
		}

		void Meter()
		{
			auto* dl = ImGui::GetForegroundDrawList();
			// the firewood left, by the pile
			if (g_recipe && !g_served) {
				float px = 0, py = 0;
				if (Scene::Project({ L::kPileX, L::kPileY, 4.0 }, px, py)) {
					const int pack = (std::max)(0, PackLogs() - g_invLogsUsed);
					const auto t = pack > 0 ? fmt::format("Firewood {} (+{} in your pack)", g_stationLogs, pack) : fmt::format("Firewood {}", g_stationLogs);
					TextC(dl, F.sansSemi, px, py - S(44), LogsLeft() > 0 ? kWhite : IM_COL32(230, 150, 130, 255), t, 0.05f, 0.9f);
					// our version: how long the fire has left, a bar per log burning
					const int burning = g_fire.Logs();
					float     by = py - S(44) - S(10);
					if (burning > 0) {
						double most = 0.0;
						for (int k = 0; k < burning; ++k) {
							most = (std::max)(most, g_fire.Fuel(k));
						}
						const float bw = S(150), bh = S(9);
						for (int k = burning - 1; k >= 0; --k) {
							const double f = Clamp(g_fire.Fuel(k), 0.0, 1.0);
							by -= bh + S(4);
							Panel(dl, ImVec2(px - bw * 0.5f - S(3), by - S(3)), ImVec2(px + bw * 0.5f + S(3), by + bh + S(3)), 1.0f, false);
							const ImU32 c = f > 0.5 ? IM_COL32(236, 150, 60, 240) : f > 0.2 ? IM_COL32(220, 110, 45, 240) : IM_COL32(200, 60, 40, 250);
							dl->AddRectFilled(ImVec2(px - bw * 0.5f, by), ImVec2(px - bw * 0.5f + bw * static_cast<float>(f), by + bh), c, S(2));
						}
						const double secs = most * hc::timing::kLogBurn;
						TextC(dl, F.sansSemi, px, by - F.sansSemi->FontSize - S(6), secs < 5.0 ? IM_COL32(240, 130, 110, 255) : IM_COL32(236, 180, 110, 255),
							fmt::format("Fire: {:.0f} s", secs), 0.05f, 0.9f);
					} else {
						TextC(dl, F.sansSemi, px, by - F.sansSemi->FontSize, IM_COL32(170, 160, 150, 255), "Fire: out", 0.05f, 0.9f);
					}
				}
			}
			if (g_served) {
				return;
			}
			const double rate = hc::timing::Rate(g_fire.Logs());
			const char*  warn = nullptr;
			bool         glow = false;
			// the spit: every meat its own bar
			if (g_flow == hc::Flow::Spit && g_spit) {
				glow = g_hold == Hold::Meat;
				for (const auto& u : g_units) {
					if (u.role != Role::Meat || u.at != Unit::At::Spit) {
						continue;
					}
					float mx0 = 0, my0 = 0;
					if (Scene::Project(SpitAxis(u.spitX) + Vec3{ 0, 0, 4.0 }, mx0, my0)) {
						TimerBar(dl, mx0, my0 - S(8), u.timer, rate);
					}
					bool raw = false;
					glow |= u.timer.TierNow(raw) == hc::Tier::Masterwork;
				}
				if (Mounted() > 0 && rate <= 0.0) {
					warn = "The fire is out: it isn't cooking";
				}
			} else {
				Vec3 at;
				bool show = false;
				if (g_flow == hc::Flow::Pot && g_pot && g_chunks.CountIn(hc::Chunks::State::Pot) + g_tossedIn > 0) {
					show = true;
					at = { L::kPotX + L::kPotR + 2.5, L::kPotY, L::kPotRim };
					if (g_pot->Burn() > 0.25) {
						warn = "Catching on the bottom: stir!";
					}
				} else if (g_flow == hc::Flow::Oven && g_oven && g_oven->InOven()) {
					show = true;
					at = { L::kOvenX + 8.0, L::kOvenY - 2.0, OvenFloor() + 6.0 };
				}
				if (show) {
					float sx = 0, sy = 0;
					bool  raw = false;
					if (Scene::Project(at, sx, sy)) {
						if (g_flow == hc::Flow::Oven && !g_batch.empty()) {
							for (int i = 0; i < static_cast<int>(g_batch.size()); ++i) {
								TimerBar(dl, sx + S(70) * static_cast<float>(i), sy, g_batch[i], rate);
								glow |= g_batch[i].TierNow(raw) == hc::Tier::Masterwork;
							}
						} else {
							TimerBar(dl, sx, sy, g_timer, rate);
						}
					}
					glow |= (g_batch.empty() && g_timer.TierNow(raw) == hc::Tier::Masterwork) || g_hold == Hold::LadleOut || g_hold == Hold::Bread || g_peelBread;
					if (!warn && rate <= 0.0) {
						warn = "The fire is out: it isn't cooking";
					}
				}
			}
			if (warn) {
				float fx = 0, fy = 0;
				if (Scene::Project(FireCenter() + Vec3{ 0, 0, 2.0 }, fx, fy)) {
					TextC(dl, F.sansSemi, fx, fy + S(34), IM_COL32(240, 170, 140, 255), warn, 0.05f, 0.9f);
				}
			}
			// our version: how far the dough is kneaded (till it's cut, or rolled, or goes in)
			if (g_flow == hc::Flow::Oven && g_oven && g_doughNeeded > 0 && g_doughOnBoard && !g_topping && !g_rolled && g_flat <= 0.0 && !g_oven->InOven() &&
				!g_peelLoaded && g_doughParts >= g_doughNeeded && g_cuts.empty() && !g_batchCut) {
				float dx = 0, dy = 0;
				if (Scene::Project(g_doughAt + Vec3{ 0, 0, DoughRadius() * 1.4 + 1.0 }, dx, dy)) {
					const double k = g_oven->Dough();
					const float  w = S(200), h = S(12), x0 = dx - w * 0.5f, y0 = dy - S(30);
					const double top = 1.6;
					auto X = [&](double a_v) { return x0 + w * static_cast<float>(Clamp(a_v / top, 0.0, 1.0)); };
					Panel(dl, ImVec2(x0 - S(10), y0 - S(30)), ImVec2(x0 + w + S(10), y0 + h + S(8)), 1.0f, false);
					dl->AddRectFilled(ImVec2(x0, y0), ImVec2(X(0.9), y0 + h), IM_COL32(95, 90, 82, 220));
					dl->AddRectFilled(ImVec2(X(0.9), y0), ImVec2(X(1.3), y0 + h), IM_COL32(222, 190, 110, 240));
					dl->AddRectFilled(ImVec2(X(1.3), y0), ImVec2(x0 + w, y0 + h), IM_COL32(150, 52, 38, 235));
					dl->AddRectFilled(ImVec2(x0, y0), ImVec2(X(k), y0 + h), IM_COL32(255, 255, 255, 70));
					dl->AddLine(ImVec2(X(k), y0 - S(3)), ImVec2(X(k), y0 + h + S(3)), IM_COL32(250, 240, 220, 255), S(2));
					const char* word = k < 0.9 ? "Kneading" : k <= 1.3 ? "Kneaded: smooth" : "Overworked";
					Text(dl, F.sansSemi, ImVec2(x0, y0 - S(26)), k < 0.9 ? kWhite : k <= 1.3 ? kTrimBright : IM_COL32(240, 130, 110, 255),
						fmt::format("{} {:.0f}%", word, (std::min)(100.0, k / 0.9 * 100.0)), 0.04f, 0.9f);
				}
			}
			if (CutStage()) {
				float dx = 0, dy = 0;
				if (Scene::Project(g_doughAt + Vec3{ 0, 0, DoughRadius() * 1.4 + 1.0 }, dx, dy)) {
					const auto t = fmt::format("Cut it into {} with the knife ({} of {} cuts)", ServeCount(), g_cuts.size(), ServeCount() - 1);
					const float w = Measure(F.sansSemi, t, 0.04f) + S(30);
					Panel(dl, ImVec2(dx - w * 0.5f, dy - S(44)), ImVec2(dx + w * 0.5f, dy - S(12)), 1.0f, false);
					TextC(dl, F.sansSemi, dx, dy - S(40), kTrimBright, t, 0.04f, 0.9f);
				}
			}
			// the bowls and plates: what's been served, and where the next goes
			SlotWords(dl, glow);
		}

		void ToastLine(float W, float H)
		{
			const double t = g_clock - g_toastAt;
			if (g_toast.empty() || t < 0 || t > 2.4) {
				return;
			}
			auto*       dl = ImGui::GetForegroundDrawList();
			const float a = static_cast<float>(t < 0.3 ? t / 0.3 : t > 2.0 ? (2.4 - t) / 0.4 : 1.0);
			const auto  text = Upper(g_toast);
			const float w = Measure(F.toastLong, text, 0.16f) + S(100);
			const float y = H * 0.2f;
			Panel(dl, ImVec2(W * 0.5f - w * 0.5f, y - S(14)), ImVec2(W * 0.5f + w * 0.5f, y + F.toastLong->FontSize + S(14)), a, false);
			TextC(dl, F.toastLong, W * 0.5f, y, Alpha(kTrimBright, a), text, 0.16f, 0.9f * a);
		}

		// M: the sounds, each with a slider (0 off, 1 as it comes, 2 twice as loud), and all of them off at once; kept
		// in Hearthcooking.ini when it's closed
		void SoundBox(float W, float H)
		{
			auto& v = Config::Edit();
			struct Row
			{
				const char* label;
				float*      value;
			};
			std::vector<Row> rows;
			rows.push_back({ "All the kitchen's sounds", &v.volume });
			rows.push_back({ "Fire and boiling", &v.ambience });
			for (std::size_t k = 0; k < std::size(Config::kSoundNames); ++k) {
				if (!v.soundLevels.count(Config::kSoundNames[k])) {
					v.soundLevels[Config::kSoundNames[k]] = 1.0f;
				}
				rows.push_back({ Config::kSoundLabels[k], &v.soundLevels[Config::kSoundNames[k]] });
			}
			const float rowH = S(30), w = S(560), h = S(130) + rowH * (rows.size() + 1), x = W * 0.5f - w * 0.5f, y = (std::max)(S(40), H * 0.5f - h * 0.5f);
			BeginArea("##sound", ImVec2(x, y), ImVec2(w, h));
			auto* dl = ImGui::GetWindowDrawList();
			MsgBox(dl, ImVec2(x, y), ImVec2(x + w, y + h));
			TextC(dl, F.menu, W * 0.5f, y + S(22), kWhite, "SOUND", 0.26f);
			float yy = y + S(70);
			// all off at once
			{
				bool hov = false;
				const bool muted = Audio::Muted();
				if (Clicked("mute", ImVec2(x + S(32), yy), ImVec2(x + w - S(32), yy + rowH), &hov)) {
					Audio::SetMuted(!muted);
				}
				Text(dl, F.sans, ImVec2(x + S(32), yy + (rowH - F.sans->FontSize) * 0.5f), hov ? kWhite : kGrey, muted ? "Muted: click to hear it again" : "Mute everything", 0.0f, 0.7f);
				yy += rowH + S(6);
			}
			const float bx0 = x + S(300), bx1 = x + w - S(84);
			for (std::size_t r = 0; r < rows.size(); ++r) {
				ImGui::PushID(static_cast<int>(r));
				const float cy = yy + rowH * 0.5f;
				Text(dl, F.sans, ImVec2(x + S(32), yy + (rowH - F.sans->FontSize) * 0.5f), r < 2 ? kWhite : kGrey, rows[r].label, 0.0f, 0.7f);
				ImGui::SetCursorScreenPos(ImVec2(bx0 - S(8), yy));
				ImGui::InvisibleButton("bar", ImVec2(bx1 - bx0 + S(16), rowH));
				const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
				if (act) {
					const float f = std::clamp((ImGui::GetIO().MousePos.x - bx0) / (bx1 - bx0), 0.0f, 1.0f);
					float       val = f * 2.0f;
					if (std::abs(val - 1.0f) < 0.04f) {
						val = 1.0f;  // (it catches at "as it comes")
					}
					*rows[r].value = val;
				}
				const float f = std::clamp(*rows[r].value / 2.0f, 0.0f, 1.0f);
				dl->AddRectFilled(ImVec2(bx0, cy - S(2)), ImVec2(bx1, cy + S(2)), IM_COL32(255, 255, 255, 40), S(2));
				dl->AddRectFilled(ImVec2(bx0, cy - S(2)), ImVec2(bx0 + (bx1 - bx0) * f, cy + S(2)), hov || act ? kGold : kGrey, S(2));
				dl->AddLine(ImVec2((bx0 + bx1) * 0.5f, cy - S(6)), ImVec2((bx0 + bx1) * 0.5f, cy + S(6)), IM_COL32(255, 255, 255, 60), 1.0f * g_ui);  // (1: as it comes)
				dl->AddCircleFilled(ImVec2(bx0 + (bx1 - bx0) * f, cy), S(act ? 8 : 7), hov || act ? kWhite : kGrey);
				Text(dl, F.sans, ImVec2(bx1 + S(14), yy + (rowH - F.sans->FontSize) * 0.5f), kGrey, fmt::format("{:.0f}%", *rows[r].value * 100.0f), 0.0f, 0.7f);
				ImGui::PopID();
				yy += rowH;
			}
			TextC(dl, F.sans, W * 0.5f, y + h - S(34), kDim, "M or Esc to close", 0.1f);
			ImGui::End();
		}

		void HelpBox(float W)
		{
			const float w = S(520), x = W * 0.5f - w * 0.5f, y = S(110);
			std::vector<std::string> lines;
			if (Overlay::UsingPad()) {
				lines.push_back(fmt::format("The left stick moves the hand (the right one finely). Hold {} or {} to pick things up; let go to put them down. {} puts back what you're holding.",
					Btn("A"), Btn("RT"), Btn("LT")));
				lines.push_back(fmt::format("Stirring or turning the spit: circle the right stick. {} turns the knife to cut along; {} swaps the knife.", Btn("RB"), Btn("LB")));
			} else {
				lines.push_back("Hold the left button to pick things up; let go to put them down. The right button puts back what you're holding.");
			}
			if (g_flow == hc::Flow::Pot) {
				lines.push_back("Drag ingredients onto the board and click to chop across them; right-click (or R) turns the knife to cut along them. Both ways makes dice: small and even scores best.");
				lines.push_back("Grab the board by its left end and drag right to tip it into the pot. Line it up with the pot first.");
				lines.push_back("Hold the ladle in the pot and stir round. When the counter reaches zero, drag the ladle out of the pot and let go over the bowl to serve.");
			} else if (g_flow == hc::Flow::Spit) {
				lines.push_back("Drag the meat onto the spit. Hold the crank's handle and turn it round so every side faces the fire.");
				lines.push_back("Drag the roast onto the plate when it's brown all over.");
			} else {
				lines.push_back("A pie's fruit first: dice it on the board, then drag the board by its left end over the bowl behind it to sweep it in.");
				lines.push_back("Drag the flour, butter and the rest into the bowl of water and stir round until it's a ball; lift it onto the board and knead: long pushes, in a steady rhythm.");
				if (g_recipe && g_recipe->pie) {
					lines.push_back("Drag the bowl of filling over the dough to heap it on. A dumpling: take each side of the dough and draw it in over the filling. A pie: pinch round the edge to crimp it.");
				}
				lines.push_back("Open the door (drag it left). Drag the peel under the dough, carry it into the mouth, shut the door. Peek, but each peek lets the heat out.");
			}
			lines.push_back("The hourglass and its counter show the seconds left. Take it off in the very second it reaches zero for a masterwork; every second after makes it worse, and past seven it burns. Each log burns 20 seconds; while one burns it cooks, more logs cook a little faster. The station's firewood runs out, then your own pack's.");
			float h = S(80);
			for (const auto& l : lines) {
				h += F.sans->CalcTextSizeA(F.sans->FontSize, FLT_MAX, w - S(64), l.c_str()).y + S(10);
			}
			BeginArea("##help", ImVec2(x, y), ImVec2(w, h));
			auto* dl = ImGui::GetWindowDrawList();
			MsgBox(dl, ImVec2(x, y), ImVec2(x + w, y + h));
			TextC(dl, F.menu, W * 0.5f, y + S(22), kWhite, "HOW TO COOK", 0.26f);
			float yy = y + S(64);
			for (const auto& l : lines) {
				yy += Para(dl, F.sans, x + S(32), yy, w - S(64), kGrey, l) + S(10);
			}
			ImGui::End();
		}

		void ConfirmBox(float W, float H)
		{
			const float w = S(480), h = S(170), x = W * 0.5f - w * 0.5f, y = H * 0.38f;
			BeginArea("##confirm", ImVec2(x, y), ImVec2(w, h));
			auto* dl = ImGui::GetWindowDrawList();
			MsgBox(dl, ImVec2(x, y), ImVec2(x + w, y + h));
			TextC(dl, F.menu, W * 0.5f, y + S(26), kWhite, "PUT THIS DISH ASIDE?", 0.2f);
			TextC(dl, F.sans, W * 0.5f, y + S(66), kGrey, "Nothing is used up. You can start again.");
			switch (MenuButtons("confirm", { { "Put it aside", true }, { "Keep cooking", false } }, W * 0.5f, y + h - S(56))) {
			case 0:
				Post(Act::AbandonYes);
				break;
			case 1:
				Post(Act::AbandonNo);
				break;
			default:
				break;
			}
			ImGui::End();
		}

		// What to do differently next time, for a line of the verdict that held it back
		std::string Advice(const hc::Line& a_l)
		{
			const std::string lab = a_l.label, v = a_l.verdict;
			auto has = [&](const char* w) { return v.find(w) != std::string::npos; };
			if (lab == "The cuts") {
				return "Space your cuts evenly, for small pieces of a size";
			}
			if (lab == "Into the pot") {
				return "Line the board up over the pot before you tip it";
			}
			if (lab == "The heat" || lab == "The fire" || lab == "The oven") {
				return has("hot") ? "Fewer logs: let the fire settle before adding more" : "Add a log sooner, keep the flames lively";
			}
			if (lab == "The stirring") {
				return has("Caught") ? "Stir more often, so nothing catches on the bottom" : "Keep the ladle moving round the pot";
			}
			if (lab == "The salt") {
				return has("salty") ? "A shorter pinch of salt" : "A pinch of salt: hold it about half a second";
			}
			if (lab == "All the way round") {
				return "Give the crank a quarter turn every few seconds, so no side sits too long facing the fire";
			}
			if (lab == "The timing") {
				if (has("raw")) {
					return "Wait for the hourglass: take it off when the counter reaches zero";
				}
				return "Take it off the moment the counter reaches zero: that second is the masterwork";
			}
			if (lab == "Done") {
				if (has("raw") || has("bloody") || has("Not quite") || has("Underdone")) {
					return g_flow == hc::Flow::Spit ? "Leave it over the fire a little longer" : "Give it longer: wait for the meter's gold";
				}
				return g_flow == hc::Flow::Spit ? "Take it off sooner, in the gold" : "Serve it sooner, while the meter's in the gold";
			}
			if (lab == "The dough") {
				return has("fits") ? "Knead in a steady rhythm" : has("Over") ? "Stop kneading once it's smooth" : "Knead longer, with long steady pushes";
			}
			if (lab == "The filling") {
				return has("open") ? "Close the whole edge before it goes in" : "Heap the filling right out to the edge";
			}
			if (lab == "The bake") {
				return has("Dark") ? "Open the door and take it out sooner" : "Leave it in the oven a little longer";
			}
			return {};
		}

		// Several portions: nothing over the kitchen; each one's word under its plate, a line and the buttons below
		void DrawResultMany(float W, float H)
		{
			auto* dl = ImGui::GetForegroundDrawList();
			SlotWords(dl, false);
			int counts[4] = {};
			for (const auto& [t, raw] : g_portionTiers) {
				++counts[raw ? 0 : static_cast<int>(t)];
			}
			std::string sum;
			for (int t = 3; t >= 0; --t) {
				if (counts[t]) {
					sum += fmt::format("{}{} {}", sum.empty() ? "" : "   ", counts[t], Upper(t == 0 ? "Burnt / raw" : hc::TierName(static_cast<hc::Tier>(t))));
				}
			}
			const float y = H - S(150);
			const float w = (std::max)(Measure(F.menu, Upper(g_givenName), 0.2f), Measure(F.sansSemi, sum, 0.08f)) + S(80);
			Panel(dl, ImVec2(W * 0.5f - w * 0.5f, y - S(16)), ImVec2(W * 0.5f + w * 0.5f, y + S(64)));
			TextC(dl, F.menu, W * 0.5f, y, kGold, Upper(g_givenName), 0.2f, 0.8f);
			TextC(dl, F.sansSemi, W * 0.5f, y + S(28), kWhite, sum, 0.08f, 0.8f);
			BeginArea("##again", ImVec2(W * 0.5f - S(300), H - S(90)), ImVec2(S(600), S(60)));
			switch (MenuButtons("result", { { "Cook another", false }, { "Done", false } }, W * 0.5f, H - S(80))) {
			case 0:
				Post(Act::Again);
				break;
			case 1:
				Post(Act::Done);
				break;
			default:
				break;
			}
			ImGui::End();
		}

		void DrawResult(float W, float H)
		{
			if (g_portionTiers.size() > 1) {
				DrawResultMany(W, H);
				return;
			}
			auto* bg = ImGui::GetBackgroundDrawList();
			Vignette(bg, W, H);
			const double t = g_clock - g_serveAt;
			const float  a = static_cast<float>(Clamp(t / 0.6, 0.0, 1.0));
			const ImU32  tierCol = g_result.tier == hc::Tier::Masterwork ? kGold : g_result.tier == hc::Tier::Burnt ? IM_COL32(170, 150, 140, 255) : kWhite;
			// the tier, big, as the game announces a level up
			const auto title = Upper(hc::TierName(g_result.tier));
			const float ty = S(70);
			if (g_result.tier == hc::Tier::Masterwork) {
				Glow(bg, F.title, ImVec2(W * 0.5f - Measure(F.title, title, 0.12f) * 0.5f, ty), Alpha(kGold, 0.08f * a), title, 0.12f, S(8));
			}
			TextC(bg, F.title, W * 0.5f, ty, Alpha(tierCol, a), title, 0.12f, 0.9f * a);
			DiamondRule(bg, W * 0.5f, ty + F.title->FontSize + S(12), S(520));
			TextC(bg, F.menu, W * 0.5f, ty + F.title->FontSize + S(26), Alpha(kGrey, a), Upper(g_givenName + (g_recipe && g_recipe->makes > 1 ? fmt::format(" x{}", g_recipe->makes) : "")), 0.2f, 0.8f);
			if (g_result.tier == hc::Tier::Burnt && *g_result.burntWhy) {
				TextC(bg, F.sans, W * 0.5f, ty + F.title->FontSize + S(64), Alpha(IM_COL32(230, 170, 150, 255), a), g_result.burntWhy, 0.05f, 0.8f);
			}
			// how it went, on a card: each line marked as it helped or held it back; then what to do next time
			std::vector<std::pair<double, std::string>> tips;
			for (int i = 0; i < g_result.count; ++i) {
				if (g_result.lines[i].score < 0.8) {
					if (auto adv = Advice(g_result.lines[i]); !adv.empty()) {
						tips.emplace_back(g_result.lines[i].score, std::move(adv));
					}
				}
			}
			std::sort(tips.begin(), tips.end(), [](const auto& a_a, const auto& a_b) { return a_a.first < a_b.first; });
			if (tips.size() > 2) {
				tips.resize(2);  // (the two that cost it most)
			}
			const float  w = S(470), rowH = S(30);
			float        tipsH = 0.0f;
			for (const auto& tp : tips) {
				tipsH += F.hand->CalcTextSizeA(F.hand->FontSize, FLT_MAX, w - S(60), tp.second.c_str()).y + S(6);
			}
			const float  h = S(30) + S(40) + rowH * static_cast<float>(g_result.count) + (tips.empty() ? 0.0f : S(44) + tipsH) + S(28);
			const ImVec2 pos(W - w - S(40), H * 0.5f - h * 0.5f);
			BeginArea("##verdict", pos, ImVec2(w, h));
			auto* dl = ImGui::GetWindowDrawList();
			Sheet(dl, pos, ImVec2(w, h), 1);
			Frame(dl, ImVec2(pos.x - S(2), pos.y - S(2)), ImVec2(pos.x + w + S(2), pos.y + h + S(2)));
			float y = pos.y + S(24);
			TextC(dl, F.bookSC, pos.x + w * 0.5f, y, kInk, "How it went");
			y += S(42);
			for (int i = 0; i < g_result.count; ++i) {
				const auto& l = g_result.lines[i];
				const ImU32 c = l.score >= 0.8 ? kGreenInk : l.score >= 0.5 ? kInk : kRubric;
				// a mark: it helped (+), it was all right, or it held the dish back (-)
				const char* mark = l.score >= 0.8 ? "+" : l.score >= 0.5 ? "~" : "-";
				TextC(dl, F.bookSC, pos.x + S(26), y - S(1), c, mark);
				Text(dl, F.bookSCsm, ImVec2(pos.x + S(44), y + S(4)), kRubric, l.label, 0.04f);
				Text(dl, F.hand, ImVec2(pos.x + w - S(30) - Measure(F.hand, l.verdict, 0), y), c, l.verdict);
				y += rowH;
			}
			if (!tips.empty()) {
				y += S(10);
				dl->AddLine(ImVec2(pos.x + S(30), y), ImVec2(pos.x + w - S(30), y), Alpha(kInkSoft, 0.35f), 1.0f * g_ui);
				y += S(10);
				Text(dl, F.bookSCsm, ImVec2(pos.x + S(30), y + S(2)), kRubric, "Next time", 0.04f);
				y += S(24);
				for (const auto& tp : tips) {
					y += Para(dl, F.hand, pos.x + S(30), y, w - S(60), kInkSoft, tp.second) + S(6);
				}
			}
			ImGui::End();
			BeginArea("##again", ImVec2(W * 0.5f - S(300), H - S(90)), ImVec2(S(600), S(60)));
			switch (MenuButtons("result", { { "Cook another", false }, { "Done", false } }, W * 0.5f, H - S(80))) {
			case 0:
				Post(Act::Again);
				break;
			case 1:
				Post(Act::Done);
				break;
			default:
				break;
			}
			ImGui::End();
		}

		void DebugMarks()
		{
			if (!Config::Get().debugOverlay || g_phase != Phase::Cooking) {
				return;
			}
			auto* dl = ImGui::GetForegroundDrawList();
			auto  mark = [&](const Vec3& p, ImU32 c) {
				float sx = 0, sy = 0;
				if (Scene::Project(p, sx, sy)) {
					dl->AddCircle(ImVec2(sx, sy), S(10), c, 16, 2.0f);
				}
			};
			for (const auto& u : g_units) {
				mark(u.clothAt + Vec3{ 0, 0, 1.0 }, IM_COL32(120, 255, 120, 200));
			}
			mark({ L::kSaltX, L::kSaltY, 1.5 }, IM_COL32(255, 255, 255, 200));
			mark({ L::kServeX, L::kServeY, 1.0 }, IM_COL32(255, 220, 120, 200));
			mark({ L::kPileX, L::kPileY, 1.5 }, IM_COL32(255, 140, 80, 200));
			if (g_flow == hc::Flow::Spit) {
				mark(CrankGrip(), IM_COL32(120, 180, 255, 200));
			}
			if (g_flow == hc::Flow::Oven) {
				mark(DoorCentre(), IM_COL32(120, 180, 255, 200));
				mark(MouthAt(), IM_COL32(255, 120, 120, 200));
			}
		}

		// ---------------------------------------------------------------- the station: the game's crafting menu at a cooking pot or oven
		bool IsCookingStation(RE::TESFurniture* a_f, bool& a_oven)
		{
			if (!a_f) {
				return false;
			}
			a_oven = a_f->HasKeywordString("BYOHCraftingOven");
			return a_oven || a_f->HasKeywordString("CraftingCookpot");
		}

		void OnCraftingOpened()
		{
			if (!Config::Get().enabled || Scene::IsOpen()) {
				return;
			}
			if (g_quick) {
				g_quick = false;  // they asked for the game's own menu
				return;
			}
			auto menu = Crafting();
			auto* sub = menu ? menu->GetCraftingSubMenu() : nullptr;
			bool  oven = false;
			if (!sub || !IsCookingStation(sub->furniture, oven)) {
				return;
			}
			std::lock_guard lk(g_mtx);
			g_ovenStation = oven;
			g_book = Recipes::ForStation(oven);
			g_sel = 0;
			ShowCrafting(false);
			Scene::Open();
			ShowStation();
			g_phase = Phase::Book;
			g_clock = 0;
			g_toast.clear();  // (the clock starts again: a message left from the last cook would come back partway through this one)
			g_toastAt = -10.0;
			Audio::Ready();
			spdlog::info("At the {}: {} recipes", oven ? "oven" : "cooking pot", g_book.size());
		}

		class MenuWatch final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_e || std::string_view(a_e->menuName.c_str()) != RE::CraftingMenu::MENU_NAME) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_e->opening) {
					SKSE::GetTaskInterface()->AddTask([] { OnCraftingOpened(); });
				} else {
					SKSE::GetTaskInterface()->AddTask([] {
						g_quick = false;
						if (Scene::IsOpen()) {
							std::lock_guard lk(g_mtx);
							CloseKitchen(false);  // the station went away under us
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void OnDataLoaded()
	{
		Kit::Resolve();
		Recipes::OnDataLoaded();
		Recipes::InstallEatWatch();
		Scene::Register();
		{
			// the kitchen's own meshes read in now, not as it opens
			std::vector<std::string> ours;
			for (const char* n : { "Board", "Bowl", "Broth", "Cap", "Chunk", "Cloth", "Dough", "DoughDisk", "FirePit", "Hull", "Knife", "Ladle", "Log", "OvenBase", "OvenDome",
					 "OvenDoor", "OvenDoorArch", "Peel", "PieCrimp", "Plate", "Pot", "RollingPin", "Room", "SaltBowl", "SpitCrank", "SpitPosts", "SpitRod", "Spoon", "Worktop", "Leek" }) {
				ours.push_back(fmt::format("Hearthcooking\\{}.nif", n));
			}
			for (int k = 0; k < 9; ++k) {
				ours.push_back(fmt::format("Hearthcooking\\DumplingFold{}.nif", k));
				ours.push_back(fmt::format("Hearthcooking\\LeafBit{}.nif", k));
			}
			Scene::Preload(ours);
		}
		static MenuWatch s_watch;
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&s_watch);
		}
	}

	void OnGameLoaded()
	{
		std::lock_guard lk(g_mtx);
		g_quick = false;
		if (Scene::IsOpen()) {
			CloseKitchen(false);
		}
	}

	void Tick(float a_dt)
	{
		std::lock_guard lk(g_mtx);
		const double    dt = std::clamp(static_cast<double>(a_dt), 0.001, 0.05);
		if (g_phase != Phase::Idle && Overlay::Away()) {
			// the Steam overlay is up or the game is in the background: the kitchen waits, its clock stopped -
			// nothing cooks or burns
			Overlay::TakePresses();
			return;
		}
		g_clock += dt;
		ProcessActs();
		if (g_phase == Phase::Idle) {
			return;
		}
		HandleKeys();
		PadOrbit();
		if (g_phase == Phase::Cooking && !g_confirm && !g_help && !g_soundBox) {
			HandleInput(dt);
		} else {
			Overlay::TakePresses();  // the buttons are ImGui's
			if (g_hold != Hold::None) {
				Cancel();
			}
		}
		if (g_phase != Phase::Result) {
			Step(dt);
		} else {
			g_fire.Step(dt);
			FireLoop(Clamp(g_fire.Heat() * 0.5, 0.0, 0.6));
			Audio::SetLoop("loop_boil", 0.0f);
		}
		Look3D();
	}

	void Abort()
	{
		std::lock_guard lk(g_mtx);
		spdlog::error("The kitchen hit a problem and was closed; nothing was used up");
		CloseKitchen(true);
	}

	void SetupUi(float a_screenH) { Ui::Setup(a_screenH); }

	void DrawUi()
	{
		std::lock_guard lk(g_mtx);
		if (!Scene::IsOpen() || g_phase == Phase::Idle) {
			return;
		}
		Ui::LoadArt();
		auto& io = ImGui::GetIO();
		io.MouseDrawCursor = false;
		const float W = io.DisplaySize.x, H = io.DisplaySize.y;
		switch (g_phase) {
		case Phase::Book:
			DrawBook(W, H);
			break;
		case Phase::Cooking:
			Vignette(ImGui::GetBackgroundDrawList(), W, H);
			RecipeCard(W, H);
			Meter();
			HintLine(W, H);
			if (g_confirm) {
				ConfirmBox(W, H);
			}
			break;
		case Phase::Result:
			DrawResult(W, H);
			break;
		default:
			break;
		}
		KeyBar(W, H);
		if (g_help && g_phase == Phase::Cooking) {
			HelpBox(W);
		}
		if (g_soundBox) {
			SoundBox(W, H);
		}
		ToastLine(W, H);
		DebugMarks();
		// the pointer: the game's own cursor (or a cursor mod's) when it's up; ours, tinted while carrying, with a controller
		if (!Overlay::GameCursorShown()) {
			Ui::Cursor(g_hold != Hold::None ? IM_COL32(255, 226, 170, 245) : IM_COL32(241, 238, 232, 245));
		}
	}

	void Save(SKSE::SerializationInterface* a_intfc) { Recipes::Save(a_intfc); }
	void Load(SKSE::SerializationInterface* a_intfc) { Recipes::Load(a_intfc); }
	void Revert(SKSE::SerializationInterface*) { Recipes::Revert(); }
}
