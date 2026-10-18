"""Source contracts and isolated command-intent models; no C++/game claim."""
import argparse
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
MOVEMENT = "firgune/project/core/features/movement/"


def body(source, signature):
    start = source.find(signature)
    if start < 0:
        return ""
    opening = source.find("{", start)
    depth = 1
    index = opening + 1
    while depth and index < len(source):
        depth += (source[index] == "{") - (source[index] == "}")
        index += 1
    return source[opening + 1:index - 1]


def compact(source):
    return re.sub(r"\s+", "", source)


class JumpIntentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        read = lambda path: (ROOT / path).read_text(encoding="utf-8-sig")
        cls.header = read(MOVEMENT + "movement.hpp")
        cls.bhop = read(MOVEMENT + "impl/bunnyhop.cpp")
        cls.jump = read(MOVEMENT + "impl/jumpbug.cpp")
        cls.hook = read("firgune/project/core/hooks/impl/cheat.cpp")
        cls.lua = read("firgune/project/core/features/scripting/impl/lua_engine.cpp")
        cls.early = body(cls.bhop, "void bhop::on_create_move(")
        cls.getter = body(cls.bhop, "bool bhop::suppressed_jump(")
        cls.jump_early = body(cls.jump, "void jumpbug::on_create_move(")
        cls.jump_finish = body(cls.jump, "void jumpbug::finish_prediction(")

    def require_getter(self):
        self.assertTrue(self.getter, "missing command-scoped suppressed_jump getter")

    def evaluate_getter(self, pointer, number, saved_pointer, saved_number, changed):
        self.require_getter()
        expression = re.search(r"return\s+(.+?);", self.getter, re.S)
        self.assertIsNotNone(expression)
        expression = expression.group(1)
        for old, new in [
            ("cmd->buttons.value_changed", "changed"),
            ("cmd->command_number", "number"),
            ("this->m_suppressed_jump_command", "saved_pointer"),
            ("this->m_suppressed_jump_number", "saved_number"),
        ]:
            expression = expression.replace(old, new)
        expression = re.sub(r"\bcmd\b", "pointer", expression)
        expression = expression.replace("&&", " and ").replace("||", " or ")
        expression = re.sub(r"\s+", " ", expression).strip()
        return bool(eval(expression, {"__builtins__": {}}, dict(
            pointer=pointer, number=number, saved_pointer=saved_pointer,
            saved_number=saved_number, changed=changed, k_jump=2)))

    def test_01_command_scoped_declaration(self):
        self.assertIn("bool suppressed_jump( const systems::input::usercmd* cmd ) const;", self.header)
        self.assertIn("const systems::input::usercmd* m_suppressed_jump_command{};", self.header)
        self.assertIn("std::intptr_t m_suppressed_jump_number{ -1 };", self.header)

    def test_02_every_early_return_resets_intent(self):
        first_return = self.early.find("return;")
        self.assertGreater(first_return, 0)
        prefix = self.early[:first_return]
        self.assertIn("this->m_suppressed_jump_command = nullptr;", prefix)
        self.assertIn("this->m_suppressed_jump_number = -1;", prefix)

    def test_03_capture_original_jump_only_before_airborne_clear(self):
        capture = "if ( cmd->buttons.value & k_jump )"
        self.assertIn(capture, self.early)
        start = self.early.index(capture)
        clearing = self.early.index("cmd->buttons.value &= ~k_jump;")
        self.assertLess(start, clearing)
        self.assertGreater(start, self.early.index("if ( pre.flags & cstypes::entity_flags::on_ground )"))
        captured = self.early[start:clearing]
        self.assertIn("this->m_suppressed_jump_command = cmd;", captured)
        self.assertIn("this->m_suppressed_jump_number = cmd->command_number;", captured)

    def test_04_original_jump_survives_bhop_consumption(self):
        self.assertTrue(self.evaluate_getter(101, 44, 101, 44, 0))

    def test_05_no_original_jump_does_not_activate(self):
        self.assertFalse(self.evaluate_getter(101, 44, 0, -1, 0))

    def test_06_command_switch_and_null_never_reuse_intent(self):
        for pointer, number in [(0, 44), (102, 44), (101, 45)]:
            self.assertFalse(self.evaluate_getter(pointer, number, 101, 44, 0))

    def test_07_lua_explicit_release_cancels_intent(self):
        setter = body(self.lua, "int api_cmd_set_button(")
        release = setter[setter.index("else"):]
        self.assertIn("g_current_cmd->buttons.value &= ~button;", release)
        self.assertIn("g_current_cmd->buttons.value_changed |= button;", release)
        self.assertFalse(self.evaluate_getter(101, 44, 101, 44, 2))

    def test_08_unrelated_buttons_do_not_cancel(self):
        for changed in [1, 4, 8, 16, 1 | 4 | 8 | 16]:
            self.assertTrue(self.evaluate_getter(101, 44, 101, 44, changed))

    def test_09_jumpbug_both_gates_accept_consumed_intent(self):
        self.assertIn("!g_bhop.suppressed_jump(cmd)", compact(self.jump_early))
        self.assertIn("!g_bhop.suppressed_jump(cmd)", compact(self.jump_finish))
        self.assertIn("buttons.value&cstypes::command_buttons::in_jump", compact(self.jump_early))
        self.assertIn("cmd->buttons.value&landing::jump", compact(self.jump_finish))
        self.assertNotIn("GetAsyncKeyState", self.jump)

    def test_10_bhop_key_and_three_channel_clear_unchanged(self):
        self.assertIn("const auto holding = perfect ? space_held( ) : ( cmd->buttons.value & k_jump ) != 0 || space_held( );", self.early)
        for field in ["value", "value_changed", "value_scroll"]:
            # The existing ground branch also clears the jump scroll edge.
            expected = 2 if field == "value_scroll" else 1
            self.assertEqual(self.early.count(f"cmd->buttons.{field} &= ~k_jump;"), expected)
        getter = compact(self.getter)
        self.assertNotIn("cmd->buttons.value=", getter)
        self.assertNotIn("GetAsyncKeyState", getter)

    def test_11_trial_endpoint_and_failed_trial_rollback(self):
        finish = self.jump_finish
        self.assertIn("const auto restore_jump =", finish)
        self.assertIn("g_bhop.suppressed_jump( cmd )", finish)
        apply_start = finish.index("const auto apply =")
        apply_end = finish.index("const auto accept =", apply_start)
        apply = finish[apply_start:apply_end]
        self.assertIn("if ( restore_jump )", apply)
        for field in ["value", "value_changed", "value_scroll"]:
            self.assertIn(f"cmd->buttons.{field} |= landing::jump;", apply)
        self.assertLess(apply.index("candidate.reset( );"), apply.index("if ( restore_jump )"))
        self.assertLess(apply.index("if ( restore_jump )"), apply.index("candidate.encode_angles( );"))
        landing = (ROOT / (MOVEMENT + "impl/landing.hpp")).read_text(encoding="utf-8-sig")
        self.assertIn("~transaction( ) { if ( !committed ) restore( ); }", landing)
        self.assertIn("cmd->buttons = buttons;", landing)
        for accepted in [False, True]:
            original = dict(value=0, value_changed=0, value_scroll=0)
            trial = {field: value | 2 for field, value in original.items()}
            final = trial if accepted else original
            self.assertEqual(final, dict.fromkeys(original, 2 if accepted else 0))

    def test_12_final_pipeline_and_shared_switch_unchanged(self):
        terms = ["g_movement_fix.finish( current_cmd )", "dt( ).finalize_command( current_cmd )",
                 "g_edgebug.finish_prediction( current_cmd )", "g_jumpbug.finish_prediction( current_cmd )",
                 "g_bhop.finish_prediction( current_cmd )", "g_rage.finalize_no_spread_state( current_cmd, local )"]
        positions = [self.hook.index(term) for term in terms]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(self.jump.count("settings::g_movement.landing_assist.value"), 2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    ROOT = args.root.resolve()
    result = unittest.TestResult()
    unittest.defaultTestLoader.loadTestsFromTestCase(JumpIntentTests).run(result)
    print(f"JUMP_INTENT STATIC/MODEL tests={result.testsRun} failures={len(result.failures)} errors={len(result.errors)}")
    for case, detail in result.failures + result.errors:
        print(case.id().split(".")[-1])
        print(detail)
    raise SystemExit(0 if result.wasSuccessful() else 1)
