// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
//
// The settings page in SKSE Menu Framework (if it's installed): every INI setting, changed live, saved to the INI.
#include "Menu.h"
#include "Config.h"
#include "Kit.h"
#include "Recipes.h"

#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#include "SKSEMenuFramework.h"

namespace Menu
{
	namespace
	{
		namespace UI = ImGuiMCP;

		bool g_dirty = false;

		void Hint(const char* a_text)
		{
			UI::TextDisabled("%s", a_text);
			UI::Spacing();
		}
		void Changed(bool a_changed) { g_dirty |= a_changed; }

		void __stdcall Settings()
		{
			auto& v = Config::Edit();
			UI::SeparatorText("Hearthcooking");
			Changed(UI::Checkbox("Cook by hand at pots and ovens", &v.enabled));
			Hint("Off: the cooking pot and the oven open the game's own menu.");
			UI::SeparatorText("The kitchen");
			Changed(UI::SliderFloat("Tilt", &v.tiltDeg, 30.0f, 80.0f, "%.0f degrees"));
			Hint("How far you look down on it.");
			Changed(UI::SliderFloat("Zoom", &v.zoom, 0.6f, 1.6f, "%.2f"));
			{
				// the knife, from every one the game has (for trying them)
				const auto&  knives = Kit::Knives();
				const auto&  now = Kit::Get(Kit::Role::Knife);
				const std::string shown = now.vanilla ? now.path : std::string("Hearthcooking's own");
				if (UI::BeginCombo("Knife", shown.c_str())) {
					if (UI::Selectable("The one it finds by itself", false)) {
						Kit::SetMesh(Kit::Role::Knife, "");
					}
					if (UI::Selectable("Hearthcooking's own", !now.vanilla)) {
						Kit::SetMesh(Kit::Role::Knife, "ours");
					}
					for (const auto& k : knives) {
						if (UI::Selectable(k.c_str(), now.vanilla && _stricmp(now.path.c_str(), k.c_str()) == 0)) {
							Kit::SetMesh(Kit::Role::Knife, k);
						}
					}
					UI::EndCombo();
				}
				Hint("Takes effect the next time the kitchen opens; kept in Hearthcooking.ini.");
			}
			UI::SeparatorText("Graphics");
			{
				static const char* kQ[] = { "Low (fastest)", "Medium", "High" };
				if (UI::BeginCombo("Kitchen quality", kQ[std::clamp(v.quality, 0, 2)])) {
					for (int q = 2; q >= 0; --q) {
						if (UI::Selectable(kQ[q], v.quality == q)) {
							v.quality = q;
							Changed(true);
						}
					}
					UI::EndCombo();
				}
				Hint("High: 4x anti-aliasing and sharp shadows. Medium: 2x. Low: none, for older cards. Shadows change at the next game start.");
			}
			UI::SeparatorText("Controls and sound");
			Changed(UI::SliderFloat("Mouse speed", &v.mouseSpeed, 0.2f, 3.0f, "%.2f"));
			Changed(UI::SliderFloat("Interface size", &v.uiScale, 0.6f, 2.0f, "%.2f"));
			Changed(UI::Checkbox("Sound", &v.sound));
			Changed(UI::SliderFloat("Kitchen sounds", &v.volume, 0.0f, 2.0f, "%.2f"));
			Changed(UI::SliderFloat("Fire and simmer", &v.ambience, 0.0f, 2.0f, "%.2f"));
			Changed(UI::SliderFloat("Menu clicks", &v.effectsVolume, 0.0f, 2.0f, "%.2f"));
			if (UI::CollapsingHeader("Each sound on its own")) {
				Hint("1 is as it comes, 0 is off. On top of the sliders above.");
				for (std::size_t k = 0; k < std::size(Config::kSoundNames); ++k) {
					const std::string name = Config::kSoundNames[k];
					float lv = v.soundLevels.count(name) ? v.soundLevels[name] : 1.0f;
					if (UI::SliderFloat(Config::kSoundLabels[k], &lv, 0.0f, 2.0f, "%.2f")) {
						v.soundLevels[name] = lv;
						g_dirty = true;
					}
				}
			}
			Changed(UI::Checkbox("Show what to do next", &v.hints));
			{
				const char* styles[] = { "Xbox", "PlayStation" };
				if (UI::BeginCombo("Controller buttons", styles[v.padStyle == 1 ? 1 : 0])) {
					for (int k = 0; k < 2; ++k) {
						if (UI::Selectable(styles[k], v.padStyle == k)) {
							v.padStyle = k;
							g_dirty = true;
						}
					}
					UI::EndCombo();
				}
			}
			Changed(UI::Checkbox("Debug: mark what the mouse can grab", &v.debugOverlay));
			UI::SeparatorText("Debug (testing)");
			if (UI::Button("Give 20 random ingredients")) {
				Recipes::DebugGiveRandom(20, 10);
			}
			Hint("20 different cooking ingredients picked at random, 1 to 10 of each. The list is in Hearthcooking.log.");
			UI::Spacing();
			UI::Separator();
			if (UI::Button("Save to Hearthcooking.ini")) {
				Config::Save();
				g_dirty = false;
			}
			UI::SameLine();
			if (UI::Button("Undo changes")) {
				Config::Load();
				g_dirty = false;
			}
			if (g_dirty) {
				UI::SameLine();
				UI::TextDisabled("(not saved yet: changes last until you quit)");
			}
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			spdlog::info("SKSE Menu Framework not installed: settings are in Hearthcooking.ini");
			return;
		}
		SKSEMenuFramework::SetSection("Hearthcooking");
		SKSEMenuFramework::AddSectionItem("Settings", Settings);
		spdlog::info("Settings page added to SKSE Menu Framework");
	}
}
