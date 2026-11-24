// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Recipes.h"

#include "EspIds.h"
#include "../sim/Timing.h"

namespace Recipes
{
	namespace
	{
		std::vector<std::unique_ptr<Recipe>> g_recipes;
		std::mutex                           g_mtx;

		// the tiered dishes: (the game's dish, tier) -> one of our empty dishes
		struct Slot
		{
			RE::FormID base = 0;
			hc::Tier   tier = hc::Tier::Plain;
			bool       used = false;
			std::vector<RE::Effect*> ours;  // the effects we made for it
			std::vector<RE::BGSKeyword*> keywords;
		};
		std::array<Slot, EspIds::kDishSlots> g_slots;
		std::unordered_map<RE::FormID, std::uint8_t> g_best;  // the game's dish -> the best tier made (+1; 0 none)

		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}
		bool Has(const std::string& a_lower, std::initializer_list<const char*> a_words)
		{
			for (const char* w : a_words) {
				if (a_lower.find(w) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		RE::AlchemyItem* SlotForm(int a_k)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			return dh ? dh->LookupForm<RE::AlchemyItem>(EspIds::kFirstDish + static_cast<RE::FormID>(a_k), EspIds::kPlugin) : nullptr;
		}

		// A plugin's short name: the game and its DLC, Creation Club and the unofficial patches are "Vanilla"; the
		// well-known food mods by the names people call them; anything else by its file name
		std::string ShortName(const RE::TESFile* a_f)
		{
			if (!a_f) {
				return "Vanilla";
			}
			const std::string file = a_f->GetFilename().data();
			const std::string low = Lower(file);
			for (const char* v : { "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm" }) {
				if (low == v) {
					return "Vanilla";
				}
			}
			if ((low.rfind("cc", 0) == 0 && low.find("sse0") != std::string::npos) || low.find("unofficial skyrim") != std::string::npos) {
				return "Vanilla";
			}
			static const std::pair<const char*, const char*> kKnown[] = { { "complete alchemy & cooking overhaul", "CACO" }, { "gourmet", "Gourmet" },
				{ "legacyofthedragonborn", "LotD" }, { "hunterborn", "Hunterborn" }, { "bsheartland", "Bruma" }, { "bsassets", "Bruma" },
				{ "campfire", "Campfire" }, { "frostfall", "Frostfall" }, { "sunhelm", "SunHelm" }, { "ineed", "iNeed" },
				{ "realisticneeds", "RND" }, { "hearthcooking", "Hearthcooking" } };
			for (const auto& [k, v] : kKnown) {
				if (low.find(k) != std::string::npos) {
					return v;
				}
			}
			const auto dot = file.find_last_of('.');
			return dot == std::string::npos ? file : file.substr(0, dot);
		}

		// Merged patches made by tools: they carry everything, so they don't count as an edit
		bool IsMergePatch(const RE::TESFile* a_f)
		{
			const std::string low = a_f ? Lower(a_f->GetFilename().data()) : std::string();
			return low.find("bashed patch") != std::string::npos || low.find("smashed patch") != std::string::npos ||
			       low.find("synthesis") != std::string::npos || low == "zpatch.esp";
		}

		// Where a recipe is from, and the last mod that changed it, if one did
		std::string SourceOf(const RE::TESForm* a_form)
		{
			const auto* files = a_form ? a_form->sourceFiles.array : nullptr;
			const std::string origin = ShortName(a_form ? a_form->GetFile(0) : nullptr);
			if (!files || files->size() < 2) {
				return origin;
			}
			for (auto i = static_cast<std::int64_t>(files->size()) - 1; i >= 1; --i) {
				const auto* f = (*files)[static_cast<std::uint32_t>(i)];
				if (!f || IsMergePatch(f)) {
					continue;
				}
				const std::string by = ShortName(f);
				return by == origin ? origin : fmt::format("{}, edited by {}", origin, by);
			}
			return origin;
		}

		// The qualities' multipliers live with the rules (src/sim/Timing.cpp, DISENO.md): masterwork 3.5x strength and value,
		// 2.5x duration; good 2x; burnt or raw half
		float ValueMult(hc::Tier t) { return static_cast<float>(hc::timing::Multipliers(t).value); }
		float MagMult(hc::Tier t) { return static_cast<float>(hc::timing::Multipliers(t).magnitude); }
		float DurMult(hc::Tier t) { return static_cast<float>(hc::timing::Multipliers(t).duration); }

		// Our version: the lists other mods keep food on (needs mods such as iNeed sort food by FormLists): our dish goes
		// on every list the game's dish is on, so it's known as that food rather than asked about. Built once, at the
		// first dish; re-done at every load (the lists' runtime additions are the game's to save or not).
		void MirrorLists(RE::AlchemyItem* a_base, RE::TESForm* a_ours)
		{
			static std::unordered_map<RE::FormID, std::vector<RE::BGSListForm*>> s_on;
			static bool s_built = false;
			if (!s_built) {
				s_built = true;
				if (auto* dh = RE::TESDataHandler::GetSingleton()) {
					for (auto* l : dh->GetFormArray<RE::BGSListForm>()) {
						if (!l) {
							continue;
						}
						l->ForEachForm([&](RE::TESForm* a_f) {
							if (a_f && a_f->Is(RE::FormType::AlchemyItem)) {
								s_on[a_f->GetFormID()].push_back(l);
							}
							return RE::BSContainer::ForEachResult::kContinue;
						});
					}
				}
				spdlog::info("Food lists: {} foods are on other mods' lists", s_on.size());
			}
			const auto it = s_on.find(a_base->GetFormID());
			if (it == s_on.end()) {
				return;
			}
			for (auto* l : it->second) {
				if (!l->HasForm(a_ours)) {
					l->AddForm(a_ours);
					const auto* file = l->GetFile(0);
					spdlog::info("  {} put on list {:08X} ({})", a_ours->GetName(), l->GetFormID(), file ? file->GetFilename() : "?");
				}
			}
		}

		// Makes slot a_k a copy of the game's dish at a tier
		bool Bind(int a_k, RE::AlchemyItem* a_base, hc::Tier a_tier)
		{
			auto* s = SlotForm(a_k);
			if (!s || !a_base) {
				return false;
			}
			auto& slot = g_slots[a_k];
			const std::string name = fmt::format("{} {}", hc::TierName(a_tier), a_base->GetName());
			s->fullName = name.c_str();
			s->SetModel(a_base->GetModel());
			s->alternateTextures = a_base->alternateTextures;
			s->numAlternateTextures = a_base->numAlternateTextures;
			s->weight = a_base->weight;
			s->data = a_base->data;
			const auto value = (std::max)(1, static_cast<int>(std::lround(a_base->GetGoldValue() * ValueMult(a_tier))));
			s->data.costOverride = value;
			s->data.flags.set(RE::AlchemyItem::AlchemyFlag::kCostOverride);
			s->pickupSound = a_base->pickupSound;
			s->putdownSound = a_base->putdownSound;
			s->equipSlot = a_base->equipSlot;
			// the effects: stronger and longer, or weaker and shorter
			s->effects.clear();
			for (auto* e : slot.ours) {
				delete e;
			}
			slot.ours.clear();
			// (burnt: half as strong and half as long. Never none: the game's item card reads a potion's strongest
			// effect without checking, and a dish with no effects crashes the inventory)
			for (auto* e : a_base->effects) {
				if (!e || !e->baseEffect) {
					continue;
				}
				auto* n = new RE::Effect();
				n->effectItem = e->effectItem;
				n->effectItem.magnitude = e->effectItem.magnitude * MagMult(a_tier);
				n->effectItem.duration = static_cast<std::uint32_t>(std::lround(e->effectItem.duration * DurMult(a_tier)));
				n->baseEffect = e->baseEffect;
				n->cost = e->cost * ValueMult(a_tier);
				s->effects.push_back(n);
				slot.ours.push_back(n);
			}
			if (s->effects.empty()) {
				// a dish from a food mod with no effect the game can read: borrow the first food's that has one, weak
				// and short, so the item card never meets an empty list
				static RE::Effect* s_any = [] {
					if (auto* dh = RE::TESDataHandler::GetSingleton()) {
						for (auto* f : dh->GetFormArray<RE::AlchemyItem>()) {
							if (f && f->IsFood() && !f->effects.empty() && f->effects[0] && f->effects[0]->baseEffect) {
								return f->effects[0];
							}
						}
					}
					return static_cast<RE::Effect*>(nullptr);
				}();
				if (s_any) {
					auto* n = new RE::Effect();
					n->effectItem = s_any->effectItem;
					n->effectItem.magnitude = (std::max)(1.0f, s_any->effectItem.magnitude * 0.25f);
					n->effectItem.duration = 0;
					n->baseEffect = s_any->baseEffect;
					n->cost = 0.0f;
					s->effects.push_back(n);
					slot.ours.push_back(n);
					spdlog::warn("Dish {:03}: {} has no effects; gave it a small one so the inventory can show it", a_k, a_base->GetName());
				}
			}
			s->hostileCount = a_base->hostileCount;
			s->avEffectSetting = a_base->avEffectSetting;
			// its keywords (VendorItemFood and the rest), so merchants and other mods see food
			if (!slot.keywords.empty()) {
				s->RemoveKeywords(slot.keywords);
				slot.keywords.clear();
			}
			std::vector<RE::BGSKeyword*> add;
			for (std::uint32_t i = 0; i < a_base->numKeywords; ++i) {
				if (auto* kw = a_base->keywords[i]; kw && !s->HasKeyword(kw)) {
					add.push_back(kw);
				}
			}
			if (!add.empty()) {
				s->AddKeywords(add);
				slot.keywords = add;
			}
			MirrorLists(a_base, s);
			slot.base = a_base->GetFormID();
			slot.tier = a_tier;
			slot.used = true;
			spdlog::info("Dish {:03}: {} ({:08X}), worth {}", a_k, name, slot.base, value);
			return true;
		}

		int SlotFor(RE::FormID a_base, hc::Tier a_tier)
		{
			for (int k = 0; k < EspIds::kDishSlots; ++k) {
				if (g_slots[k].used && g_slots[k].base == a_base && g_slots[k].tier == a_tier) {
					return k;
				}
			}
			return -1;
		}

		Role RoleOf(const Recipe& a_r, RE::TESBoundObject* a_form, const std::string& a_key)
		{
			const auto& low = a_key;  // the name and the model's words, so a translated name is still known
			if (Has(low, { "salt" })) {
				return Role::Salt;
			}
			if (a_r.flow == hc::Flow::Oven) {
				if (Has(low, { "flour", "butter", "milk", "egg", "yeast", "water", "lard", "sack" })) {
					return Role::Dough;
				}
				return Role::Filling;
			}
			// poured or sprinkled in, never cut (a jug of milk is "raw food" to the game)
			if (Has(low, { "milk", "ale", "wine", "mead", "water", "jug", "bottle", "honey", "sugar", "berries", "grapes", "juice", "broth" })) {
				return Role::Toss;
			}
			// solid food: anything raw, and the vegetables, cheeses and meats by name
			bool solid = false;
			if (auto* kwf = a_form ? a_form->As<RE::BGSKeywordForm>() : nullptr) {
				solid = kwf->HasKeywordString("VendorItemFoodRaw");
			}
			solid = solid || Has(low, { "leek", "potato", "cabbage", "carrot", "tomato", "gourd", "apple", "pear", "garlic", "onion", "yam",
										 "cheese", "beef", "venison", "horker", "mammoth", "goat", "horse", "chicken", "pheasant", "rabbit",
										 "dog", "mudcrab", "salmon", "fish", "clam", "meat", "leg", "haunch", "breast", "steak", "boar",
										 "slaughterfish", "eggplant", "pumpkin", "turnip", "radish", "corn", "larva" });
			if (a_r.flow == hc::Flow::Spit) {
				return solid ? Role::Meat : Role::Toss;
			}
			return solid ? Role::Chop : Role::Toss;
		}

		std::string ModelOf(RE::TESBoundObject* a_form)
		{
			auto* m = a_form ? a_form->As<RE::TESModel>() : nullptr;
			const char* p = m ? m->GetModel() : nullptr;
			return p ? p : "";
		}

		void Build()
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}
			g_recipes.clear();
			int pot = 0, spit = 0, oven = 0;
			for (auto* cobj : dh->GetFormArray<RE::BGSConstructibleObject>()) {
				if (!cobj || !cobj->benchKeyword || !cobj->createdItem) {
					continue;
				}
				const std::string bench = cobj->benchKeyword->GetFormEditorID();
				const bool        isOven = bench == "BYOHCraftingOven";
				if (bench != "CraftingCookpot" && !isOven) {
					continue;
				}
				auto* result = cobj->createdItem->As<RE::TESBoundObject>();
				if (!result || !result->GetName() || !*result->GetName()) {
					continue;
				}
				// recipes left out (Hearthcooking.ini [Recipes] Skip, names separated by commas): they cook the game's way
				{
					char buf[1024]{};
					::GetPrivateProfileStringA("Recipes", "Skip", "Cookie,Muffin", buf, sizeof(buf), "Data\\SKSE\\Plugins\\Hearthcooking.ini");
					const std::string skip = Lower(buf), name = Lower(result->GetName());
					bool skipped = false;
					std::size_t from = 0;
					while (from <= skip.size()) {
						auto to = skip.find(',', from);
						if (to == std::string::npos) {
							to = skip.size();
						}
						std::string w = skip.substr(from, to - from);
						while (!w.empty() && w.front() == ' ') {
							w.erase(w.begin());
						}
						while (!w.empty() && (w.back() == ' ' || w.back() == ';')) {
							w.pop_back();
						}
						skipped |= !w.empty() && name == w;
						from = to + 1;
					}
					if (skipped) {
						continue;
					}
				}
				auto r = std::make_unique<Recipe>();
				r->cobj = cobj;
				r->result = result;
				r->name = result->GetName();
				r->source = SourceOf(cobj);
				r->key = MatchKey(r->name, ModelOf(result));
				r->makes = (std::max)(1, static_cast<int>(cobj->data.numConstructed));
				const auto& low = r->key;
				if (low.find("garlic bread") != std::string::npos) {
					r->makes = (std::max)(r->makes, 5);  // a loaf cut into five slices: five of it
				}
				if (low.find("stew") != std::string::npos || low.find("soup") != std::string::npos || low.find("sauce") != std::string::npos) {
					r->makes = (std::max)(r->makes, 3);  // a whole pot of it: three
				}
				// a pie is baked, wherever the game has it made (its Chaurus Pie is a cooking pot's)
				const bool toOven = isOven || Has(low, { " pie", "pie " }) || low.rfind("pie", 0) == 0 || (low.size() >= 3 && low.compare(low.size() - 3, 3, "pie") == 0);
				r->oven = toOven;
				if (toOven) {
					r->flow = hc::Flow::Oven;
					r->pie = Has(low, { "pie", "crostata", "tart", "pastry", "dumpling" });
				} else {
					r->flow = Has(low, { "grilled", "cooked", "roast", "steak", "haunch", "chop", "seared", "skewer", "leg of", "charred", "baked", "spit", "larva" }) &&
					                  !Has(low, { "stew", "soup", "chowder", "fondue" })
					              ? hc::Flow::Spit
					              : hc::Flow::Pot;
				}
				cobj->requiredItems.ForEachContainerObject([&](RE::ContainerObject& a_e) {
					if (a_e.obj && a_e.count > 0) {
						Ingredient in;
						in.form = a_e.obj;
						in.name = a_e.obj->GetName() ? a_e.obj->GetName() : "?";
						in.model = ModelOf(a_e.obj);
						in.key = MatchKey(in.name, in.model);
						if (const char* ed = a_e.obj->GetFormEditorID(); ed && *ed) {
							in.key += " " + Lower(ed);  // (its Editor ID too, when the game keeps them)
						}
						in.count = a_e.count;
						r->ings.push_back(std::move(in));
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
				if (r->ings.empty()) {
					continue;
				}
				for (auto& in : r->ings) {
					in.role = RoleOf(*r, in.form, in.key);
					r->salted |= in.role == Role::Salt;
					r->dough |= in.role == Role::Dough;
				}
				// a roast with nothing to put on the spit is a stew after all
				if (r->flow == hc::Flow::Spit && std::none_of(r->ings.begin(), r->ings.end(), [](const Ingredient& i) { return i.role == Role::Meat; })) {
					r->flow = hc::Flow::Pot;
					for (auto& in : r->ings) {
						in.role = RoleOf(*r, in.form, in.key);
					}
				}
				if (r->flow == hc::Flow::Pot) {
					++pot;
				} else if (r->flow == hc::Flow::Spit) {
					++spit;
				} else {
					++oven;
				}
				std::string list;
				for (const auto& in : r->ings) {
					static constexpr const char* kRole[] = { "chop", "toss", "salt", "spit", "dough", "filling" };
					list += fmt::format("{}{} x{} ({})", list.empty() ? "" : ", ", in.name, in.count, kRole[static_cast<int>(in.role)]);
				}
				spdlog::info("Recipe: {} x{} [{}{}] <- {}", r->name, r->makes, r->flow == hc::Flow::Pot ? "pot" : r->flow == hc::Flow::Spit ? "spit" : "oven",
					r->pie ? ", pie" : "", list);
				g_recipes.push_back(std::move(r));
			}
			spdlog::info("Recipes: {} for the pot, {} for the spit, {} for the oven", pot, spit, oven);
		}

		class EatWatch final : public RE::BSTEventSink<RE::TESEquipEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_e, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (!a_e || !a_e->equipped || !a_e->actor || a_e->actor.get() != RE::PlayerCharacter::GetSingleton() || !IsBurnt(a_e->baseObject)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				SKSE::GetTaskInterface()->AddTask([] {
					if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
						pc->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina, 20.0f);
						RE::SendHUDMessage::ShowHUDMessage("That was burnt.");
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void OnDataLoaded()
	{
		std::lock_guard lk(g_mtx);
		Build();
		int found = 0;
		for (int k = 0; k < EspIds::kDishSlots; ++k) {
			found += SlotForm(k) != nullptr;
		}
		if (found < EspIds::kDishSlots) {
			spdlog::error("Only {} of the {} dishes are in {} - is the plugin enabled?", found, EspIds::kDishSlots, EspIds::kPlugin);
		}
	}

	std::string MatchKey(const std::string& a_name, const std::string& a_model)
	{
		// the model's file name in words: "Clutter\\Ingredients\\SaltPile.nif" -> "salt pile", "RawBeef01" -> "raw beef"
		std::string stem = a_model;
		if (const auto sl = stem.find_last_of("\\/"); sl != std::string::npos) {
			stem = stem.substr(sl + 1);
		}
		if (const auto dot = stem.find_last_of('.'); dot != std::string::npos) {
			stem = stem.substr(0, dot);
		}
		std::string words;
		for (std::size_t i = 0; i < stem.size(); ++i) {
			const unsigned char c = static_cast<unsigned char>(stem[i]);
			if (std::isupper(c) && i > 0 && std::islower(static_cast<unsigned char>(stem[i - 1]))) {
				words += ' ';
			}
			words += (c == '_' || c == '-' || std::isdigit(c)) ? ' ' : static_cast<char>(std::tolower(c));
		}
		return Lower(a_name) + " | " + words;
	}

	std::vector<std::string> AllNames()
	{
		std::lock_guard          lk(g_mtx);
		std::vector<std::string> out;
		for (const auto& r : g_recipes) {
			out.push_back(r->name);
			out.push_back(r->source);
			for (const auto& in : r->ings) {
				out.push_back(in.name);
			}
		}
		return out;
	}

	void DebugGiveRandom(int a_kinds, int a_most)
	{
		// every ingredient any cooking recipe asks for, once each, sorted into kinds
		using Pool = std::vector<std::pair<RE::TESBoundObject*, std::string>>;
		Pool meat, veg, bake, other;
		auto seen = [&](RE::TESBoundObject* f) {
			for (const Pool* p : { &meat, &veg, &bake, &other }) {
				for (const auto& e : *p) {
					if (e.first == f) {
						return true;
					}
				}
			}
			return false;
		};
		for (const auto& r : g_recipes) {
			for (const auto& in : r->ings) {
				if (!in.form || seen(in.form) || in.role == Role::Salt) {
					continue;
				}
				Pool& p = in.role == Role::Meat ? meat : in.role == Role::Chop ? veg : (in.role == Role::Dough || in.role == Role::Filling || r->oven) ? bake : other;
				p.emplace_back(in.form, in.name);
			}
		}
		std::mt19937 rng{ std::random_device{}() };
		for (Pool* p : { &meat, &veg, &bake, &other }) {
			std::shuffle(p->begin(), p->end(), rng);
		}
		// in turn from each kind until there are enough: baking, vegetables, meat, the rest
		std::vector<std::pair<RE::TESBoundObject*, std::string>> pick;
		std::size_t at[4] = {};
		Pool*       order[4] = { &bake, &veg, &meat, &other };
		for (bool more = true; more && static_cast<int>(pick.size()) < a_kinds;) {
			more = false;
			for (int k = 0; k < 4 && static_cast<int>(pick.size()) < a_kinds; ++k) {
				if (at[k] < order[k]->size()) {
					pick.push_back((*order[k])[at[k]++]);
					more = true;
				}
			}
		}
		if (pick.empty()) {
			RE::SendHUDMessage::ShowHUDMessage("Hearthcooking: no recipes found");
			return;
		}
		std::uniform_int_distribution<int> amount(1, (std::max)(1, a_most));
		std::vector<std::pair<RE::TESBoundObject*, int>> give;
		for (const auto& [f, name] : pick) {
			const int c = amount(rng);
			give.emplace_back(f, c);
			spdlog::info("Debug: gave {} x{}", name, c);
		}
		const int n = static_cast<int>(give.size());
		// on the game's own thread (the settings page runs on the render thread)
		SKSE::GetTaskInterface()->AddTask([give, n] {
			auto* pc = RE::PlayerCharacter::GetSingleton();
			if (!pc) {
				return;
			}
			for (const auto& [f, c] : give) {
				pc->AddObjectToContainer(f, nullptr, c, nullptr);
			}
			RE::SendHUDMessage::ShowHUDMessage(fmt::format("Hearthcooking debug: {} random ingredients added", n).c_str());
		});
	}

	std::vector<const Recipe*> ForStation(bool a_oven)
	{
		std::lock_guard            lk(g_mtx);
		std::vector<const Recipe*> out;
		for (const auto& r : g_recipes) {
			if (r->oven == a_oven && Known(*r)) {
				out.push_back(r.get());
			}
		}
		std::vector<std::pair<int, const Recipe*>> keyed;
		for (auto* r : out) {
			keyed.emplace_back(CanMake(*r) > 0 ? 0 : 1, r);
		}
		std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : a.second->name < b.second->name; });
		out.clear();
		for (const auto& [k, r] : keyed) {
			out.push_back(r);
		}
		return out;
	}

	bool Known(const Recipe& a_r)
	{
		auto* pc = RE::PlayerCharacter::GetSingleton();
		return pc && a_r.cobj && (!a_r.cobj->conditions.head || a_r.cobj->conditions.IsTrue(pc, pc));
	}

	// What stands in for a thing in a recipe: the thing itself, and every dish of ours made from it (a Masterwork
	// Bread is still bread), the plainer first - the order they're used up in
	std::vector<RE::TESBoundObject*> StandIns(RE::TESBoundObject* a_form)
	{
		std::vector<RE::TESBoundObject*> out;
		if (!a_form) {
			return out;
		}
		out.push_back(a_form);
		for (int t = static_cast<int>(hc::Tier::Burnt); t <= static_cast<int>(hc::Tier::Masterwork); ++t) {
			for (int k = 0; k < EspIds::kDishSlots; ++k) {
				if (g_slots[k].used && g_slots[k].base == a_form->GetFormID() && static_cast<int>(g_slots[k].tier) == t) {
					if (auto* f = SlotForm(k)) {
						out.push_back(f);
					}
				}
			}
		}
		return out;
	}

	int Held(RE::TESBoundObject* a_form)
	{
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc || !a_form) {
			return 0;
		}
		const auto ins = StandIns(a_form);
		const auto counts = pc->GetInventoryCounts([&ins](RE::TESBoundObject& a_o) { return std::find(ins.begin(), ins.end(), &a_o) != ins.end(); });
		int n = 0;
		for (const auto& [f, c] : counts) {
			n += (std::max)(0, c);
		}
		return n;
	}

	int CanMake(const Recipe& a_r)
	{
		int n = 1 << 20;
		for (const auto& in : a_r.ings) {
			n = (std::min)(n, Held(in.form) / (std::max)(1, in.count));
		}
		return n == (1 << 20) ? 0 : n;
	}

	void Consume(const Recipe& a_r)
	{
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc) {
			return;
		}
		for (const auto& in : a_r.ings) {
			// the thing itself first, then our dishes of it, plainest first
			int left = in.count;
			for (auto* f : StandIns(in.form)) {
				if (left <= 0) {
					break;
				}
				const auto counts = pc->GetInventoryCounts([f](RE::TESBoundObject& a_o) { return &a_o == f; });
				const auto it = counts.find(f);
				const int  have = it == counts.end() ? 0 : (std::max)(0, it->second);
				const int  take = (std::min)(have, left);
				if (take > 0) {
					pc->RemoveItem(f, take, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
					left -= take;
				}
			}
		}
	}

	RE::TESBoundObject* Give(const Recipe& a_r, hc::Tier a_tier)
	{
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc || !a_r.result) {
			return nullptr;
		}
		RE::TESBoundObject* item = a_r.result;
		{
			std::lock_guard lk(g_mtx);
			auto*           base = a_r.result->As<RE::AlchemyItem>();
			if (a_tier != hc::Tier::Plain && base) {
				int k = SlotFor(base->GetFormID(), a_tier);
				if (k < 0) {
					for (int j = 0; j < EspIds::kDishSlots; ++j) {
						if (!g_slots[j].used) {
							k = j;
							break;
						}
					}
					if (k < 0 || !Bind(k, base, a_tier)) {
						spdlog::warn("No dish left for {} {}: giving the plain one", hc::TierName(a_tier), a_r.name);
						k = -1;
					}
				}
				if (k >= 0) {
					item = SlotForm(k);
				}
			}
			auto& best = g_best[a_r.result->GetFormID()];
			best = static_cast<std::uint8_t>((std::max)(static_cast<int>(best), static_cast<int>(a_tier) + 1));
		}
		pc->AddObjectToContainer(item, nullptr, a_r.makes, nullptr);
		return item;
	}

	hc::Tier Best(const Recipe& a_r, bool& a_any)
	{
		std::lock_guard lk(g_mtx);
		const auto      it = g_best.find(a_r.result ? a_r.result->GetFormID() : 0);
		a_any = it != g_best.end() && it->second > 0;
		return a_any ? static_cast<hc::Tier>(it->second - 1) : hc::Tier::Plain;
	}

	bool IsBurnt(RE::FormID a_form)
	{
		std::lock_guard lk(g_mtx);
		for (int k = 0; k < EspIds::kDishSlots; ++k) {
			if (g_slots[k].used && g_slots[k].tier == hc::Tier::Burnt) {
				if (auto* f = SlotForm(k); f && f->GetFormID() == a_form) {
					return true;
				}
			}
		}
		return false;
	}

	void InstallEatWatch()
	{
		static EatWatch s_watch;
		if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) {
			src->AddEventSink<RE::TESEquipEvent>(&s_watch);
		}
	}

	// ---------------------------------------------------------------- co-save
	void Save(SKSE::SerializationInterface* a_intfc)
	{
		std::lock_guard lk(g_mtx);
		if (a_intfc->OpenRecord('HCDS', 1)) {
			std::uint32_t n = 0;
			for (const auto& s : g_slots) {
				n += s.used;
			}
			a_intfc->WriteRecordData(n);
			for (int k = 0; k < EspIds::kDishSlots; ++k) {
				const auto& s = g_slots[k];
				if (!s.used) {
					continue;
				}
				const std::uint16_t slot = static_cast<std::uint16_t>(k);
				const std::uint8_t  tier = static_cast<std::uint8_t>(s.tier);
				a_intfc->WriteRecordData(slot);
				a_intfc->WriteRecordData(tier);
				a_intfc->WriteRecordData(s.base);
			}
		}
		if (a_intfc->OpenRecord('HCBT', 1)) {
			const std::uint32_t n = static_cast<std::uint32_t>(g_best.size());
			a_intfc->WriteRecordData(n);
			for (const auto& [id, t] : g_best) {
				a_intfc->WriteRecordData(id);
				a_intfc->WriteRecordData(t);
			}
		}
	}

	void Load(SKSE::SerializationInterface* a_intfc)
	{
		std::lock_guard lk(g_mtx);
		std::uint32_t   type = 0, version = 0, length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type == 'HCDS') {
				std::uint32_t n = 0;
				a_intfc->ReadRecordData(n);
				int bound = 0;
				for (std::uint32_t i = 0; i < n; ++i) {
					std::uint16_t slot = 0;
					std::uint8_t  tier = 0;
					RE::FormID    base = 0, fixed = 0;
					a_intfc->ReadRecordData(slot);
					a_intfc->ReadRecordData(tier);
					a_intfc->ReadRecordData(base);
					if (slot >= EspIds::kDishSlots || !a_intfc->ResolveFormID(base, fixed)) {
						continue;
					}
					auto* form = RE::TESForm::LookupByID<RE::AlchemyItem>(fixed);
					if (form && Bind(slot, form, static_cast<hc::Tier>((std::min)(tier, std::uint8_t(3))))) {
						++bound;
					} else {
						// the dish's mod is gone: keep the slot taken so it isn't given to another dish
						g_slots[slot].used = true;
						g_slots[slot].base = fixed;
						g_slots[slot].tier = static_cast<hc::Tier>(tier);
					}
				}
				spdlog::info("Loaded {} cooked dishes", bound);
			} else if (type == 'HCBT') {
				std::uint32_t n = 0;
				a_intfc->ReadRecordData(n);
				g_best.clear();
				for (std::uint32_t i = 0; i < n; ++i) {
					RE::FormID   id = 0, fixed = 0;
					std::uint8_t t = 0;
					a_intfc->ReadRecordData(id);
					a_intfc->ReadRecordData(t);
					if (a_intfc->ResolveFormID(id, fixed)) {
						g_best[fixed] = t;
					}
				}
			}
		}
	}

	void Revert()
	{
		std::lock_guard lk(g_mtx);
		for (int k = 0; k < EspIds::kDishSlots; ++k) {
			auto& s = g_slots[k];
			if (!s.used) {
				continue;
			}
			if (auto* f = SlotForm(k)) {
				f->fullName = "Hearthcooking Dish";
				f->effects.clear();
				if (!s.keywords.empty()) {
					f->RemoveKeywords(s.keywords);
				}
			}
			for (auto* e : s.ours) {
				delete e;
			}
			s = Slot{};
		}
		g_best.clear();
	}
}
