// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

#include <map>
#include <set>
#include <string>

// Data\SKSE\Plugins\Hearthcooking.ini
namespace Config
{
	// the sounds that can be turned up or down one by one: their names (a file's name without its take number) and
	// what the settings page calls them
	inline constexpr const char* kSoundNames[] = { "chop", "stir", "knead", "flour", "egg", "salt", "pour", "splash", "log", "crank", "hiss",
		"loop_fire", "loop_oven", "loop_boil", "done", "burnt" };
	inline constexpr const char* kSoundLabels[] = { "Knife on the board", "Stirring", "Kneading", "Flour", "Egg cracking", "Salt", "Pouring",
		"Splashing into the pot", "Firewood", "Turning the spit", "Meat hissing", "Campfire", "Oven fire", "Pot boiling", "Done chime",
		"Burnt" };

	struct Values
	{
		bool  enabled = true;       // off: the cooking pot and the oven open the game's own menu, as if we weren't here

		// The kitchen on screen
		float tiltDeg = 56.0f;      // how far you look down on it
		float zoom = 1.0f;          // > 1 closer, < 1 further back
		int   quality = 2;          // the kitchen's drawing: 2 high (4x MSAA, sharp shadows), 1 medium (2x), 0 low (none)

		// Play
		float mouseSpeed = 1.0f;
		float uiScale = 1.3f;
		bool  sound = true;         // all of the kitchen's sounds
		float volume = 0.5f;        // the kitchen: chopping, the fire, the pot
		float ambience = 1.0f;      // the loops: the fire's crackle, the simmer, the sizzle
		float effectsVolume = 1.0f; // menu clicks
		// each sound on its own (by its file's name, without the take number): 1 as it is, 0 off, up to 2
		std::map<std::string, float> soundLevels;
		bool  hints = true;         // the line at the bottom saying what to do next
		bool  tips = true;          // a short tip the first time each step comes up
		int   padStyle = 0;         // the controller's button names: 0 Xbox, 1 PlayStation

		// Diagnostics
		bool debugOverlay = false;  // draws where the mouse can grab things
		bool logInput = false;
	};

	const Values& Get();
	Values&       Edit();  // the in-game settings page changes these live
	void          Load();
	void          Save();  // writes them back to the INI
	// The first-time tips already shown (kept in the INI under [Tips], apart from the settings)
	bool          TipSeen(const char* a_name);
	void          MarkTipSeen(const char* a_name);
	void          ResetTips();
}
