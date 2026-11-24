// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Ui.h"

#include "Audio.h"
#include "Config.h"
#include "Overlay.h"
#include "Recipes.h"

namespace Ui
{
	Fonts F;
	float g_ui = 1.0f;

	namespace
	{
		void*   g_parchment[2]{};
		bool    g_artTried = false;
		ImGuiID g_hoverId = 0;

		void Tick(const char* a_name) { Audio::Play({ a_name, 1.0f }); }
	}

	float S(float a_px) { return a_px * g_ui; }

	ImU32 Alpha(ImU32 a_col, float a_a)
	{
		const int a = static_cast<int>(((a_col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a_a, 0.0f, 1.0f));
		return (a_col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
	}

	std::string Upper(std::string a_s)
	{
		for (auto& c : a_s) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		return a_s;
	}

	void Setup(float a_screenH)
	{
		g_ui = (std::max)(0.6f, a_screenH / 1080.0f) * Config::Get().uiScale;
		auto& io = ImGui::GetIO();
		char  windir[MAX_PATH]{};
		::GetWindowsDirectoryA(windir, MAX_PATH);
		const std::string ours = "Data\\SKSE\\Plugins\\Hearthcooking\\Fonts\\";
		const std::string sys = std::string(windir) + "\\Fonts\\";
		std::error_code   ec;
		static const ImWchar kText[] = { 0x0020, 0x00FF, 0x2018, 0x201E, 0x2026, 0x2026, 0 };
		static const ImWchar* kCaps = kText;
		// The game's names in any language (translations keep them as UTF-8): the letters beyond Latin-1 they use,
		// drawn from a Windows font that has them, merged into each of ours. Only the letters in use are built, so
		// even Chinese or Korean names cost a few hundred glyphs, not tens of thousands.
		std::vector<ImWchar>     extra;
		{
			std::set<ImWchar> cps;
			for (const auto& n : Recipes::AllNames()) {
				const int wn = ::MultiByteToWideChar(CP_UTF8, 0, n.data(), static_cast<int>(n.size()), nullptr, 0);
				if (wn <= 0) {
					continue;
				}
				std::wstring w(static_cast<size_t>(wn), L'\0');
				::MultiByteToWideChar(CP_UTF8, 0, n.data(), static_cast<int>(n.size()), w.data(), wn);
				for (const wchar_t c : w) {
					if (c > 0xFF && !(c >= 0xD800 && c <= 0xDFFF) && !(c >= 0x2018 && c <= 0x201E) && c != 0x2026) {
						cps.insert(static_cast<ImWchar>(c));
					}
				}
			}
			extra.assign(cps.begin(), cps.end());
		}
		// a Windows font for each script: Korean, Chinese and Japanese, Thai, and the rest (Cyrillic, Greek, Latin Extended,
		// Arabic, Hebrew...) from Arial / Segoe UI
		struct Fallback
		{
			std::vector<const char*> files;
			ImVector<ImWchar>        ranges;
		};
		std::vector<Fallback> fallbacks;
		{
			auto script = [](ImWchar c) -> int {
				if ((c >= 0xAC00 && c <= 0xD7A3) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F)) {
					return 0;  // Hangul
				}
				if ((c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF)) {
					return 1;  // CJK, kana, full-width forms
				}
				if (c >= 0x0E00 && c <= 0x0E7F) {
					return 2;  // Thai
				}
				return 3;
			};
			const std::vector<const char*> files[4] = { { "malgun.ttf", "gulim.ttc" },
				{ "msyh.ttc", "YuGothM.ttc", "meiryo.ttc", "simsun.ttc", "msgothic.ttc" }, { "tahoma.ttf", "leelawui.ttf" },
				{ "arial.ttf", "segoeui.ttf", "tahoma.ttf" } };
			ImFontGlyphRangesBuilder b[4];
			bool                     any[4]{};
			for (const ImWchar c : extra) {
				const int k = script(c);
				b[k].AddChar(c);
				any[k] = true;
			}
			for (int k = 0; k < 4; ++k) {
				if (any[k]) {
					Fallback f;
					f.files = files[k];
					b[k].BuildRanges(&f.ranges);
					fallbacks.push_back(std::move(f));
				}
			}
		}
		static std::vector<ImVector<ImWchar>> s_ranges;  // the atlas reads the ranges when it builds
		s_ranges.clear();
		s_ranges.reserve(fallbacks.size());
		for (auto& f : fallbacks) {
			s_ranges.push_back(f.ranges);
		}
		int merged = 0;
		auto load = [&](const char* a_file, const char* a_fallback, float a_size, const ImWchar* a_ranges) -> ImFont* {
			std::string p = ours + a_file;
			if (!std::filesystem::exists(p, ec)) {
				p = sys + a_fallback;
				if (!std::filesystem::exists(p, ec)) {
					return nullptr;
				}
			}
			ImFontConfig fc;
			fc.OversampleH = 2;
			fc.OversampleV = 1;
			ImFont* font = io.Fonts->AddFontFromFileTTF(p.c_str(), a_size * g_ui, &fc, a_ranges);
			if (font) {
				for (size_t i = 0; i < fallbacks.size(); ++i) {
					for (const char* file : fallbacks[i].files) {
						const std::string fp = sys + file;
						if (!std::filesystem::exists(fp, ec)) {
							continue;
						}
						ImFontConfig mc;
						mc.MergeMode = true;
						mc.OversampleH = 1;
						mc.OversampleV = 1;
						mc.PixelSnapH = true;
						if (io.Fonts->AddFontFromFileTTF(fp.c_str(), a_size * g_ui, &mc, s_ranges[i].Data)) {
							++merged;
						}
						break;
					}
				}
			}
			return font;
		};
		F.sans = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 20.0f, kText);
		F.sansSemi = load("BarlowCondensed-SemiBold.ttf", "arialnb.ttf", 13.0f, kText);
		F.tiny = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 15.0f, kText);
		F.menu = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 24.0f, kText);
		F.toastLong = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 26.0f, kCaps);
		F.toast = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 44.0f, kCaps);
		F.titleMid = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 64.0f, kCaps);
		F.title = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 92.0f, kCaps);
		F.book = load("IMFellEnglish-Regular.ttf", "georgia.ttf", 19.0f, kText);
		F.bookIt = load("IMFellEnglish-Italic.ttf", "georgiai.ttf", 17.0f, kText);
		F.bookSC = load("IMFellEnglishSC-Regular.ttf", "georgia.ttf", 28.0f, kText);
		F.bookSCsm = load("IMFellEnglishSC-Regular.ttf", "georgia.ttf", 17.0f, kText);
		F.hand = load("Fondamento-Regular.ttf", "georgiai.ttf", 21.0f, kText);
		F.handBig = load("Fondamento-Regular.ttf", "georgiai.ttf", 34.0f, kText);
		ImFont* any = F.sans ? F.sans : io.Fonts->AddFontDefault();
		for (ImFont** f : { &F.sans, &F.sansSemi, &F.tiny, &F.menu, &F.toastLong, &F.toast, &F.titleMid, &F.title, &F.book, &F.bookIt, &F.bookSC, &F.bookSCsm,
				 &F.hand, &F.handBig }) {
			if (!*f) {
				*f = any;
			}
		}
		io.FontDefault = F.sans;
		auto& st = ImGui::GetStyle();
		st.WindowRounding = 0.0f;
		st.WindowBorderSize = 0.0f;
		st.WindowPadding = ImVec2(0, 0);
		st.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
		st.Colors[ImGuiCol_Text] = ImVec4(0.945f, 0.933f, 0.91f, 1.0f);
		spdlog::info("Interface: scale {:.2f}, fonts {}; {} letters beyond Latin-1 in the game's names, from {} Windows font(s)", g_ui,
			std::filesystem::exists(ours + "BarlowCondensed-Medium.ttf", ec) ? "ours" : "Windows'", extra.size(), fallbacks.size());
		(void)merged;
	}

	void LoadArt()
	{
		if (g_artTried) {
			return;
		}
		g_artTried = true;
		for (int k = 0; k < 2; ++k) {
			std::ifstream f(k == 0 ? "Data\\SKSE\\Plugins\\Hearthcooking\\UI\\parchment_b.rgba" : "Data\\SKSE\\Plugins\\Hearthcooking\\UI\\parchment_a.rgba", std::ios::binary);
			std::uint32_t wh[2]{};
			if (f && f.read(reinterpret_cast<char*>(wh), 8) && wh[0] > 0 && wh[0] <= 4096 && wh[1] > 0 && wh[1] <= 4096) {
				std::vector<char> px(static_cast<std::size_t>(wh[0]) * wh[1] * 4);
				if (f.read(px.data(), static_cast<std::streamsize>(px.size()))) {
					g_parchment[k] = Overlay::CreateTexture(static_cast<int>(wh[0]), static_cast<int>(wh[1]), px.data());
				}
			}
		}
		spdlog::info("Interface: parchment {}", g_parchment[0] && g_parchment[1] ? "loaded" : "missing (plain sheets)");
	}

	void* Parchment(int a_k) { return g_parchment[std::clamp(a_k, 0, 1)]; }

	float Measure(ImFont* a_f, const std::string& a_s, float a_track)
	{
		if (!a_f) {
			return 0.0f;
		}
		const float size = a_f->FontSize;
		float       w = 0.0f;
		const char* p = a_s.c_str();
		const char* end = p + a_s.size();
		while (p < end) {
			const unsigned char c = static_cast<unsigned char>(*p);
			const int           len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
			w += a_f->CalcTextSizeA(size, FLT_MAX, 0.0f, p, (std::min)(p + len, end)).x + a_track * size;
			p += len;
		}
		return w;
	}

	float Text(ImDrawList* a_dl, ImFont* a_f, ImVec2 a_pos, ImU32 a_col, const std::string& a_s, float a_track, float a_shadow)
	{
		if (!a_f) {
			return 0.0f;
		}
		const float size = a_f->FontSize;
		float       x = a_pos.x;
		const char* p = a_s.c_str();
		const char* end = p + a_s.size();
		while (p < end) {
			const unsigned char c = static_cast<unsigned char>(*p);
			const int           len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
			const char*         q = (std::min)(p + len, end);
			if (a_shadow > 0.0f) {
				a_dl->AddText(a_f, size, ImVec2(x + 1, a_pos.y + S(1.5f)), Alpha(IM_COL32(0, 0, 0, 255), a_shadow), p, q);
			}
			a_dl->AddText(a_f, size, ImVec2(x, a_pos.y), a_col, p, q);
			x += a_f->CalcTextSizeA(size, FLT_MAX, 0.0f, p, q).x + a_track * size;
			p = q;
		}
		return x - a_pos.x;
	}

	void TextC(ImDrawList* a_dl, ImFont* a_f, float a_cx, float a_y, ImU32 a_col, const std::string& a_s, float a_track, float a_shadow)
	{
		const float w = Measure(a_f, a_s, a_track) - (a_f ? a_track * a_f->FontSize : 0.0f);
		Text(a_dl, a_f, ImVec2(a_cx - w * 0.5f, a_y), a_col, a_s, a_track, a_shadow);
	}

	void Glow(ImDrawList* a_dl, ImFont* a_f, ImVec2 a_pos, ImU32 a_col, const std::string& a_s, float a_track, float a_r)
	{
		for (int k = 0; k < 8; ++k) {
			const float a = k * 0.785f;
			Text(a_dl, a_f, ImVec2(a_pos.x + std::cos(a) * a_r, a_pos.y + std::sin(a) * a_r), a_col, a_s, a_track);
		}
	}

	float Para(ImDrawList* a_dl, ImFont* a_f, float a_x, float a_y, float a_wrap, ImU32 a_col, const std::string& a_s)
	{
		const ImVec2 sz = a_f->CalcTextSizeA(a_f->FontSize, FLT_MAX, a_wrap, a_s.c_str());
		a_dl->AddText(a_f, a_f->FontSize, ImVec2(a_x, a_y), a_col, a_s.c_str(), nullptr, a_wrap);
		return sz.y;
	}

	void FadeRule(ImDrawList* a_dl, float a_x0, float a_x1, float a_y, float a_alpha, ImU32 a_col)
	{
		const ImU32 c = Alpha(a_col, a_alpha), t = Alpha(a_col, 0.0f);
		const float a = a_x0 + (a_x1 - a_x0) * 0.18f, b = a_x0 + (a_x1 - a_x0) * 0.82f;
		a_dl->AddRectFilledMultiColor(ImVec2(a_x0, a_y), ImVec2(a, a_y + 1), t, c, c, t);
		a_dl->AddRectFilled(ImVec2(a, a_y), ImVec2(b, a_y + 1), c);
		a_dl->AddRectFilledMultiColor(ImVec2(b, a_y), ImVec2(a_x1, a_y + 1), c, t, t, c);
	}

	void Diamond(ImDrawList* a_dl, ImVec2 a_c, float a_r, ImU32 a_fill, ImU32 a_line)
	{
		const ImVec2 p[4] = { { a_c.x, a_c.y - a_r }, { a_c.x + a_r, a_c.y }, { a_c.x, a_c.y + a_r }, { a_c.x - a_r, a_c.y } };
		if (a_fill) {
			a_dl->AddQuadFilled(p[0], p[1], p[2], p[3], a_fill);
		}
		if (a_line) {
			a_dl->AddQuad(p[0], p[1], p[2], p[3], a_line, 1.0f);
		}
	}

	void DiamondRule(ImDrawList* a_dl, float a_cx, float a_y, float a_w)
	{
		FadeRule(a_dl, a_cx - a_w * 0.5f, a_cx + a_w * 0.5f, a_y);
		Diamond(a_dl, ImVec2(a_cx, a_y + 0.5f), S(5), IM_COL32(0, 0, 0, 255), kWhite);
	}

	void Shadow(ImDrawList* a_dl, ImVec2 a_a, ImVec2 a_b, float a_r, float a_alpha)
	{
		for (int k = 6; k >= 1; --k) {
			const float g = a_r * k / 6.0f;
			a_dl->AddRectFilled(ImVec2(a_a.x - g * 0.4f, a_a.y + g * 0.3f), ImVec2(a_b.x + g * 0.4f, a_b.y + g), Alpha(IM_COL32(0, 0, 0, 255), a_alpha / 6.0f), g);
		}
	}

	void MsgBox(ImDrawList* dl, ImVec2 a, ImVec2 b) { Panel(dl, a, b, 1.0f, true); }

	namespace
	{
		// a corner bracket: an L in gold with a jewel at its elbow; sx, sy point it inward
		void Corner(ImDrawList* dl, ImVec2 c, float sx, float sy, float len, float a)
		{
			const ImU32 g = Alpha(kTrim, a), hi = Alpha(kTrimBright, a), dk = Alpha(IM_COL32(0, 0, 0, 255), 0.6f * a);
			const float t = S(2.0f);
			// a dark line under it so it reads on anything
			dl->AddLine(ImVec2(c.x + sx * S(1), c.y + sy * S(1)), ImVec2(c.x + sx * len, c.y + sy * S(1)), dk, t + S(1.5f));
			dl->AddLine(ImVec2(c.x + sx * S(1), c.y + sy * S(1)), ImVec2(c.x + sx * S(1), c.y + sy * len), dk, t + S(1.5f));
			dl->AddLine(c, ImVec2(c.x + sx * len, c.y), g, t);
			dl->AddLine(c, ImVec2(c.x, c.y + sy * len), g, t);
			// a second, shorter bracket inside it
			const float in = S(5);
			dl->AddLine(ImVec2(c.x + sx * in, c.y + sy * in), ImVec2(c.x + sx * len * 0.55f, c.y + sy * in), Alpha(kTrimDark, a), S(1.2f));
			dl->AddLine(ImVec2(c.x + sx * in, c.y + sy * in), ImVec2(c.x + sx * in, c.y + sy * len * 0.55f), Alpha(kTrimDark, a), S(1.2f));
			// the jewel at the elbow, and a tick at each arm's end
			Diamond(dl, c, S(4.5f), Alpha(IM_COL32(60, 16, 12, 255), a), hi);
			Diamond(dl, ImVec2(c.x + sx * len, c.y), S(2.2f), g, 0);
			Diamond(dl, ImVec2(c.x, c.y + sy * len), S(2.2f), g, 0);
		}

		// a jewel on a rule: the ornament at the middle of a panel's top or bottom edge
		void Crest(ImDrawList* dl, float cx, float y, float w, float a)
		{
			FadeRule(dl, cx - w, cx + w, y, 0.9f * a, kTrim);
			Diamond(dl, ImVec2(cx - S(14), y + 0.5f), S(2.5f), Alpha(kTrim, a), 0);
			Diamond(dl, ImVec2(cx + S(14), y + 0.5f), S(2.5f), Alpha(kTrim, a), 0);
			Diamond(dl, ImVec2(cx, y + 0.5f), S(7), Alpha(IM_COL32(24, 20, 16, 255), a), Alpha(kTrimBright, a));
			Diamond(dl, ImVec2(cx, y + 0.5f), S(3), Alpha(IM_COL32(150, 36, 24, 255), a), 0);
		}
	}

	void Frame(ImDrawList* dl, ImVec2 a, ImVec2 b, float al)
	{
		const float in = S(4);
		dl->AddRect(a, b, Alpha(IM_COL32(0, 0, 0, 255), 0.55f * al), S(2), 0, S(2.5f));
		dl->AddRect(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), Alpha(kTrim, 0.8f * al), 0.0f, 0, S(1.2f));
		dl->AddRect(ImVec2(a.x + in + S(3), a.y + in + S(3)), ImVec2(b.x - in - S(3), b.y - in - S(3)), Alpha(kTrimDark, 0.6f * al), 0.0f, 0, S(1.0f));
		const float len = (std::min)(S(26), (std::min)(b.x - a.x, b.y - a.y) * 0.3f);
		Corner(dl, ImVec2(a.x + in, a.y + in), 1, 1, len, al);
		Corner(dl, ImVec2(b.x - in, a.y + in), -1, 1, len, al);
		Corner(dl, ImVec2(a.x + in, b.y - in), 1, -1, len, al);
		Corner(dl, ImVec2(b.x - in, b.y - in), -1, -1, len, al);
	}

	void Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float al, bool ornate)
	{
		Shadow(dl, a, b, S(ornate ? 26 : 14), 0.65f * al);
		// smoke: warm black, lighter at the top, with a faint grain of lines
		dl->AddRectFilledMultiColor(a, b, Alpha(IM_COL32(34, 29, 24, 255), 0.94f * al), Alpha(IM_COL32(34, 29, 24, 255), 0.94f * al),
			Alpha(IM_COL32(12, 10, 9, 255), 0.94f * al), Alpha(IM_COL32(12, 10, 9, 255), 0.94f * al));
		for (float y = a.y + S(3); y < b.y - S(2); y += S(3)) {
			dl->AddLine(ImVec2(a.x + S(2), y), ImVec2(b.x - S(2), y), Alpha(IM_COL32(255, 230, 190, 255), 0.018f * al));
		}
		// light falling in from the top edge
		dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + (b.y - a.y) * 0.35f), Alpha(IM_COL32(120, 96, 60, 255), 0.12f * al), Alpha(IM_COL32(120, 96, 60, 255), 0.12f * al),
			Alpha(IM_COL32(120, 96, 60, 255), 0.0f), Alpha(IM_COL32(120, 96, 60, 255), 0.0f));
		if (!ornate) {
			// small: a single gold line and corner ticks
			dl->AddRect(a, b, Alpha(IM_COL32(0, 0, 0, 255), 0.6f * al), S(3), 0, S(1.5f));
			dl->AddRect(ImVec2(a.x + S(2), a.y + S(2)), ImVec2(b.x - S(2), b.y - S(2)), Alpha(kTrim, 0.55f * al), S(2), 0, S(1.0f));
			const float t = S(7);
			for (int k = 0; k < 4; ++k) {
				const ImVec2 c(k & 1 ? b.x - S(2) : a.x + S(2), k & 2 ? b.y - S(2) : a.y + S(2));
				const float  sx = k & 1 ? -1.0f : 1.0f, sy = k & 2 ? -1.0f : 1.0f;
				dl->AddLine(c, ImVec2(c.x + sx * t, c.y), Alpha(kTrimBright, al), S(2));
				dl->AddLine(c, ImVec2(c.x, c.y + sy * t), Alpha(kTrimBright, al), S(2));
			}
			return;
		}
		Frame(dl, a, b, al);
		const float cx = (a.x + b.x) * 0.5f, w = (std::min)(S(120), (b.x - a.x) * 0.3f);
		Crest(dl, cx, a.y + S(4), w, al);
		Crest(dl, cx, b.y - S(4), w, al);
	}

	void Sheet(ImDrawList* dl, ImVec2 pos, ImVec2 size, int a_art, float a_dim)
	{
		void* art = Parchment(a_art);
		dl->PushClipRectFullScreen();
		if (art) {
			// the torn edge's shadow: the parchment itself, in black, smeared in rings
			const float  r = S(13), a = 0.5f;
			const ImVec2 drop(S(4), S(7));
			for (int ring = 1; ring <= 3; ++ring) {
				const float rr = r * ring / 3.0f;
				for (int k = 0; k < 8; ++k) {
					const float  ang = k * 0.785398f + ring * 0.39f;
					const ImVec2 o(drop.x + std::cos(ang) * rr, drop.y + std::sin(ang) * rr);
					dl->AddImage(reinterpret_cast<ImTextureID>(art), ImVec2(pos.x + o.x, pos.y + o.y), ImVec2(pos.x + size.x + o.x, pos.y + size.y + o.y), ImVec2(0, 0),
						ImVec2(1, 1), Alpha(IM_COL32(0, 0, 0, 255), a / 8.0f * (1.4f - ring * 0.25f)));
				}
			}
		} else {
			Shadow(dl, ImVec2(pos.x + S(6), pos.y + S(8)), ImVec2(pos.x + size.x - S(6), pos.y + size.y - S(4)), S(20), 0.7f);
		}
		dl->PopClipRect();
		const int   v = static_cast<int>(255 * std::clamp(a_dim, 0.0f, 1.0f));
		const ImU32 tint = IM_COL32(v, v, v, 255);
		if (art) {
			dl->AddImage(reinterpret_cast<ImTextureID>(art), pos, ImVec2(pos.x + size.x, pos.y + size.y), ImVec2(0, 0), ImVec2(1, 1), tint);
		} else {
			dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(214 * v / 255, 191 * v / 255, 149 * v / 255, 255));
		}
	}

	void Vignette(ImDrawList* a_dl, float W, float H)
	{
		const ImU32 b = IM_COL32(10, 6, 3, 255), t = IM_COL32(10, 6, 3, 0);
		a_dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H * 0.16f), Alpha(b, 0.6f), Alpha(b, 0.6f), t, t);
		a_dl->AddRectFilledMultiColor(ImVec2(0, H * 0.8f), ImVec2(W, H), t, t, Alpha(b, 0.7f), Alpha(b, 0.7f));
		a_dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W * 0.16f, H), Alpha(b, 0.5f), t, t, Alpha(b, 0.5f));
		a_dl->AddRectFilledMultiColor(ImVec2(W * 0.84f, 0), ImVec2(W, H), t, Alpha(b, 0.5f), Alpha(b, 0.5f), t);
	}

	void Cursor(ImU32 a_fill)
	{
		auto&        io = ImGui::GetIO();
		auto*        dl = ImGui::GetForegroundDrawList();
		const ImVec2 m = io.MousePos;
		const float  s = S(1.0f);
		const ImVec2 p[7] = { { m.x, m.y }, { m.x, m.y + 22 * s }, { m.x + 5.5f * s, m.y + 17 * s }, { m.x + 9.5f * s, m.y + 25 * s },
			{ m.x + 12.5f * s, m.y + 23.5f * s }, { m.x + 8.5f * s, m.y + 15.5f * s }, { m.x + 15.5f * s, m.y + 15.5f * s } };
		dl->AddTriangleFilled(p[0], p[1], p[6], a_fill);
		dl->AddQuadFilled(p[2], p[3], p[4], p[5], a_fill);
		dl->AddPolyline(p, 7, IM_COL32(20, 16, 12, 255), ImDrawFlags_Closed, 1.4f * s);
	}

	namespace
	{
		const ImGuiWindowFlags kBare = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
		                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
	}

	bool BeginArea(const char* a_id, ImVec2 a_pos, ImVec2 a_size, ImGuiWindowFlags a_extra)
	{
		ImGui::SetNextWindowPos(a_pos);
		ImGui::SetNextWindowSize(a_size);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		const bool open = ImGui::Begin(a_id, nullptr, kBare | a_extra);
		ImGui::PopStyleVar(2);
		return open;
	}

	bool Clicked(const char* a_id, ImVec2 a_min, ImVec2 a_max, bool* a_hovered, bool a_enabled)
	{
		ImGui::SetCursorScreenPos(a_min);
		const bool r = ImGui::InvisibleButton(a_id, ImVec2((std::max)(1.0f, a_max.x - a_min.x), (std::max)(1.0f, a_max.y - a_min.y)));
		const bool h = ImGui::IsItemHovered();
		const auto id = ImGui::GetItemID();
		if (h && a_enabled) {
			if (g_hoverId != id) {
				g_hoverId = id;
				Tick("ui_hover");
			}
		} else if (g_hoverId == id) {
			g_hoverId = 0;
		}
		if (a_hovered) {
			*a_hovered = h && a_enabled;
		}
		if (r && a_enabled) {
			Tick("ui_select");
		} else if (r) {
			Tick("ui_deny");
		}
		return r && a_enabled;
	}

	bool MenuButton(const char* a_id, const std::string& a_label, float a_cx, float a_y, bool a_danger, bool a_enabled)
	{
		auto*       dl = ImGui::GetWindowDrawList();
		const auto  text = Upper(a_label);
		const float w = Measure(F.menu, text, 0.24f) + S(52);
		const float h = F.menu->FontSize + S(12);
		bool        hov = false;
		const bool  r = Clicked(a_id, ImVec2(a_cx - w * 0.5f, a_y), ImVec2(a_cx + w * 0.5f, a_y + h), &hov, a_enabled);
		const ImU32 col = !a_enabled ? kDim : hov ? (a_danger ? IM_COL32(244, 185, 169, 255) : kTrimBright) : IM_COL32(214, 196, 160, 255);
		// a dark plate, gold-edged, with a jewel at each end; lit from within when the pointer is on it
		const ImVec2 pa(a_cx - w * 0.5f, a_y), pb(a_cx + w * 0.5f, a_y + h);
		dl->AddRectFilledMultiColor(pa, pb, IM_COL32(36, 30, 24, 230), IM_COL32(36, 30, 24, 230), IM_COL32(14, 12, 10, 235), IM_COL32(14, 12, 10, 235));
		if (hov && a_enabled) {
			const ImU32 gl = a_danger ? IM_COL32(208, 106, 82, 255) : kTrim;
			dl->AddRectFilledMultiColor(pa, ImVec2(pb.x, a_y + h * 0.5f), Alpha(gl, 0.0f), Alpha(gl, 0.0f), Alpha(gl, 0.16f), Alpha(gl, 0.16f));
			dl->AddRectFilledMultiColor(ImVec2(pa.x, a_y + h * 0.5f), pb, Alpha(gl, 0.16f), Alpha(gl, 0.16f), Alpha(gl, 0.0f), Alpha(gl, 0.0f));
		}
		const ImU32 edge = !a_enabled ? kTrimDark : hov ? kTrimBright : kTrim;
		dl->AddRect(pa, pb, IM_COL32(0, 0, 0, 200), S(2), 0, S(2.5f));
		dl->AddRect(ImVec2(pa.x + S(2), pa.y + S(2)), ImVec2(pb.x - S(2), pb.y - S(2)), Alpha(edge, hov ? 1.0f : 0.7f), S(1), 0, S(1.2f));
		Diamond(dl, ImVec2(pa.x + S(2), a_y + h * 0.5f), S(hov ? 6.0f : 4.5f), IM_COL32(24, 20, 16, 255), edge);
		Diamond(dl, ImVec2(pb.x - S(2), a_y + h * 0.5f), S(hov ? 6.0f : 4.5f), IM_COL32(24, 20, 16, 255), edge);
		if (hov) {
			Glow(dl, F.menu, ImVec2(a_cx - (w - S(52)) * 0.5f, a_y + S(6)), Alpha(a_danger ? IM_COL32(208, 106, 82, 255) : kTrimBright, 0.07f), text, 0.24f, S(3));
		}
		TextC(dl, F.menu, a_cx + S(2), a_y + S(6), col, text, 0.24f, 0.6f);
		return r;
	}

	int MenuButtons(const char* a_id, std::initializer_list<std::pair<std::string, bool>> a_items, float a_cx, float a_y)
	{
		std::vector<float> ws;
		float              total = 0;
		for (const auto& [label, danger] : a_items) {
			ws.push_back(Measure(F.menu, Upper(label), 0.24f) + S(52));
			total += ws.back();
		}
		total += S(46) * static_cast<float>(ws.size() - 1);
		float x = a_cx - total * 0.5f;
		int   k = 0, hit = -1;
		for (const auto& [label, danger] : a_items) {
			ImGui::PushID(k);
			if (MenuButton(a_id, label, x + ws[k] * 0.5f, a_y, danger)) {
				hit = k;
			}
			ImGui::PopID();
			x += ws[k] + S(46);
			++k;
		}
		return hit;
	}
}
