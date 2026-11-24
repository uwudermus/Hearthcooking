// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

// Skyrim's own interface language, as Skyrim Yacht draws it: off-white condensed capitals on smoke-black gradients,
// thin rules that fade at the ends, key prompts in the corner; the recipe book and card are parchment written in ink.
// Laid out at 1080p sizes and scaled with the screen.
namespace Ui
{
	struct Fonts
	{
		ImFont *sans = nullptr, *sansSemi = nullptr, *tiny = nullptr, *menu = nullptr, *toast = nullptr, *toastLong = nullptr,
			   *title = nullptr, *titleMid = nullptr;
		ImFont *book = nullptr, *bookIt = nullptr, *bookSC = nullptr, *bookSCsm = nullptr, *hand = nullptr, *handBig = nullptr;
	};
	extern Fonts F;
	extern float g_ui;

	inline constexpr ImU32 kWhite = IM_COL32(241, 238, 232, 255);
	inline constexpr ImU32 kGrey = IM_COL32(162, 157, 148, 255);
	inline constexpr ImU32 kDim = IM_COL32(102, 98, 91, 255);
	inline constexpr ImU32 kGold = IM_COL32(222, 196, 140, 255);
	inline constexpr ImU32 kInk = IM_COL32(42, 28, 17, 255);
	inline constexpr ImU32 kInkSoft = IM_COL32(94, 70, 49, 255);
	inline constexpr ImU32 kRubric = IM_COL32(142, 47, 27, 255);
	inline constexpr ImU32 kGreenInk = IM_COL32(46, 84, 40, 255);
	// our version: the dark-and-gold look (a witcher's notebook rather than a flat box)
	inline constexpr ImU32 kTrim = IM_COL32(176, 140, 82, 255);        // worn gold
	inline constexpr ImU32 kTrimBright = IM_COL32(236, 204, 136, 255); // gold catching the light
	inline constexpr ImU32 kTrimDark = IM_COL32(92, 70, 40, 255);

	void  Setup(float a_screenH);  // the fonts, at the screen's size
	void  LoadArt();               // the parchment (on the render thread, once)
	void* Parchment(int a_k);      // 0, 1

	float       S(float a_px);
	ImU32       Alpha(ImU32 a_col, float a_a);
	std::string Upper(std::string a_s);

	float Measure(ImFont* a_f, const std::string& a_s, float a_track);
	float Text(ImDrawList* a_dl, ImFont* a_f, ImVec2 a_pos, ImU32 a_col, const std::string& a_s, float a_track = 0.0f, float a_shadow = 0.0f);
	void  TextC(ImDrawList* a_dl, ImFont* a_f, float a_cx, float a_y, ImU32 a_col, const std::string& a_s, float a_track = 0.0f, float a_shadow = 0.0f);
	void  Glow(ImDrawList* a_dl, ImFont* a_f, ImVec2 a_pos, ImU32 a_col, const std::string& a_s, float a_track, float a_r);
	float Para(ImDrawList* a_dl, ImFont* a_f, float a_x, float a_y, float a_wrap, ImU32 a_col, const std::string& a_s);
	void  FadeRule(ImDrawList* a_dl, float a_x0, float a_x1, float a_y, float a_alpha = 0.62f, ImU32 a_col = kWhite);
	void  Diamond(ImDrawList* a_dl, ImVec2 a_c, float a_r, ImU32 a_fill, ImU32 a_line);
	void  DiamondRule(ImDrawList* a_dl, float a_cx, float a_y, float a_w);
	void  Shadow(ImDrawList* a_dl, ImVec2 a_a, ImVec2 a_b, float a_r, float a_alpha);
	void  MsgBox(ImDrawList* a_dl, ImVec2 a_a, ImVec2 a_b);
	// A dark panel: shadow, a smoky gradient with a faint grain, a gold line set in from the edge and ornaments at
	// its corners (a_ornate: also a jewel top and bottom); a_alpha fades it all
	void  Panel(ImDrawList* a_dl, ImVec2 a_a, ImVec2 a_b, float a_alpha = 1.0f, bool a_ornate = true);
	// Only the gold: a frame of two lines with corner brackets and jewels, around something already drawn
	void  Frame(ImDrawList* a_dl, ImVec2 a_a, ImVec2 a_b, float a_alpha = 1.0f);
	void  Sheet(ImDrawList* a_dl, ImVec2 a_pos, ImVec2 a_size, int a_art, float a_dim = 1.0f);  // a parchment page with its shadow
	void  Vignette(ImDrawList* a_dl, float a_w, float a_h);
	void  Cursor(ImU32 a_fill = IM_COL32(241, 238, 232, 245));

	bool BeginArea(const char* a_id, ImVec2 a_pos, ImVec2 a_size, ImGuiWindowFlags a_extra = 0);  // an invisible window: its buttons take the mouse
	bool Clicked(const char* a_id, ImVec2 a_min, ImVec2 a_max, bool* a_hovered = nullptr, bool a_enabled = true);
	bool MenuButton(const char* a_id, const std::string& a_label, float a_cx, float a_y, bool a_danger = false, bool a_enabled = true);
	int  MenuButtons(const char* a_id, std::initializer_list<std::pair<std::string, bool>> a_items, float a_cx, float a_y);
}
