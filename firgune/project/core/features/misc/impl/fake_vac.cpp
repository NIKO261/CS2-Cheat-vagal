#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>

// ---------------------------------------------------------------------------
// Fake VacNet reveal — shows the hidden main-menu VacNet navbar button.
//
// The game ships the button, its icon and its tooltip, but the compiled
// stylesheet collapses it behind one class:
//
//     #VacNet                    { visibility: collapse; }
//     .show-vacnet-link #VacNet  { visibility: visible;  }
//
// mainmenu.js only adds that class after a server-side reviewer check
// (VacNet_OnReviewerInfoReceived). This adds it locally, from JavaScript,
// reusing the same RunScript path as scoreboard_weapons — no hardcoded RVAs,
// no byte patterns, nothing version-specific beyond the stock Panorama JS API.
//
// The class must sit on an ANCESTOR of #VacNet (the rule is a descendant
// selector), so the whole parent chain plus the button itself is tagged.
// AddClass and SetHasClass are both used — the native experiment behind this
// (VacNetReveal) showed a single call leaves the button hidden.
//
// This is a local UI unhide only: counter-strike.net/vacnet still decides
// entry server-side by account. Nothing here touches that.
// ---------------------------------------------------------------------------

namespace features::misc {

	namespace {

		constexpr const char* k_reveal_script = R"PANORAMA(
(function () {
	var root = $.GetContextPanel();
	if (!root || !root.IsValid()) return;
	var btn = root.FindChildTraverse("VacNet");
	if (!btn || !btn.IsValid()) return;

	var node = btn.GetParent();
	var depth = 0;
	while (node && node.IsValid() && depth < 32) {
		if (!node.BHasClass("show-vacnet-link")) {
			node.AddClass("show-vacnet-link");
			node.SetHasClass("show-vacnet-link", true);
		}
		node = node.GetParent();
		++depth;
	}
	if (!btn.BHasClass("show-vacnet-link")) {
		btn.AddClass("show-vacnet-link");
		btn.SetHasClass("show-vacnet-link", true);
	}
})();
)PANORAMA";

		constexpr const char* k_hide_script = R"PANORAMA(
(function () {
	var root = $.GetContextPanel();
	if (!root || !root.IsValid()) return;
	var btn = root.FindChildTraverse("VacNet");
	if (!btn || !btn.IsValid()) return;

	var node = btn.GetParent();
	var depth = 0;
	while (node && node.IsValid() && depth < 32) {
		if (node.BHasClass("show-vacnet-link")) {
			node.RemoveClass("show-vacnet-link");
			node.SetHasClass("show-vacnet-link", false);
		}
		node = node.GetParent();
		++depth;
	}
	if (btn.BHasClass("show-vacnet-link")) {
		btn.RemoveClass("show-vacnet-link");
		btn.SetHasClass("show-vacnet-link", false);
	}
})();
)PANORAMA";

	} // namespace

	c_ui_panel* fake_vac::find_main_menu_panel () const {
		if (!addresses::globals::main_menu_panel)
			return nullptr;

		const auto main_menu = memory::safe_read<std::uintptr_t> (addresses::globals::main_menu_panel).value_or (0);
		if (!main_menu)
			return nullptr;

		// Same slot model_preview_renderer uses: the global is a manager struct,
		// the panel is its +0x8 member.
		const auto panel = memory::safe_read<c_ui_panel*> (main_menu + 0x8).value_or (nullptr);
		if (!panel)
			return nullptr;

		const auto vtable = memory::safe_read<std::uintptr_t> (
			reinterpret_cast<std::uintptr_t> (panel));
		return vtable && *vtable ? panel : nullptr;
	}

	bool fake_vac::run_script (const std::string& script) {
		if (!m_ui_engine || !m_script_panel)
			return false;

		auto* panorama = reinterpret_cast<c_panorama_ui_engine*> (addresses::globals::panorama);
		if (!panorama || panorama->get_ui_engine () != m_ui_engine ||
			find_main_menu_panel () != m_script_panel) {
			// Main menu was rebuilt; drop the cached host and re-resolve next tick.
			m_ui_engine = nullptr;
			m_script_panel = nullptr;
			return false;
		}

		const auto engine = reinterpret_cast<std::uintptr_t> (m_ui_engine);
		const auto vtable = memory::safe_read<std::uintptr_t> (engine);
		const auto function = vtable ? memory::safe_read<std::uintptr_t> (
			*vtable + 77 * sizeof (std::uintptr_t)) : std::nullopt;
		const auto panorama_begin = addresses::modules::panorama;
		const auto panorama_end = panorama_begin + memory::get_module_size (panorama_begin);
		if (!function || *function < panorama_begin || *function >= panorama_end)
			return false;

		m_ui_engine->run_script (m_script_panel, script.c_str ());
		return true;
	}

	void fake_vac::try_initialize () {
		if (m_ui_engine && m_script_panel)
			return;

		if (!addresses::globals::panorama)
			return;

		auto* panorama = reinterpret_cast<c_panorama_ui_engine*> (addresses::globals::panorama);
		auto* ui_engine = panorama->get_ui_engine ();
		if (!ui_engine)
			return;

		const auto script_panel = find_main_menu_panel ();
		if (!script_panel)
			return;

		m_ui_engine = ui_engine;
		m_script_panel = script_panel;

		logging::console::print (xs ("[fake_vac] main menu host ready (engine={:p} panel={:p})\n"),
			static_cast<void*> (m_ui_engine),
			static_cast<void*> (m_script_panel));
	}

	void fake_vac::on_process_input_event () {
		const bool enabled = settings::g_misc.m_fake_vac.enabled.value;

		// Re-apply once a second rather than once: the main menu is torn down
		// and rebuilt (leaving a match, for one) and a fresh tree does not carry
		// the class. JS-side BHasClass checks keep this idempotent.
		const auto now = GetTickCount64 ();
		if (now - m_last_apply < 1000)
			return;
		m_last_apply = now;

		if (enabled) {
			if (!m_ui_engine || !m_script_panel)
				try_initialize ();
			if (!m_ui_engine || !m_script_panel)
				return;

			(void)run_script (k_reveal_script);
		}
		else if (m_ui_engine && m_script_panel) {
			(void)run_script (k_hide_script);
		}
	}

} // namespace features::misc
