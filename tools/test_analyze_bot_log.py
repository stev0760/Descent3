#!/usr/bin/env python3
"""Regression checks for the route diagnostics used to judge navigation changes."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import tempfile
import unittest

import analyze_bot_log as analyzer


class RouteDiagnosticTests(unittest.TestCase):
    def test_stuck_context_and_reissue_semantics(self):
        lines = ["Opening level 'fixture.d3l'\n"]
        for chain, live, room, team, disp in (("stored", "yes", 0, 0, 9),
                                               ("none", "no", 30, 1, 10)):
            lines.append(f"BOT: 'Test[BOT]' STUCKSTATE room {room} chain={chain} len=5 cursor=0 "
                         f"chain_room={room} chain_target=51 via_live={live} carrier=no "
                         f"phase=preclear team={team} net_disp={disp}\n")
        lines += [
            "BOT: 'Old[BOT]' STUCKSTATE room 0 chain=held len=5 cursor=0 "
            "chain_room=0 chain_target=51 via_live=no carrier=no\n",
            "BOT NAV: 'Test[BOT]' chain built rm0 len5 (target room 51)\n",
            "BOT NAV: 'Test[BOT]' composed route rm0 len4 term=EXIT (target room 51)\n",
            "BOT NAV: 'Test[BOT]' chain complete rm0 -> rm80\n",
            "BOT: 'Test[BOT]' carrier nav room -1 -> wp 51 (home 38)\n",
            "BOT: 'Test[BOT]' carrier nav room -1 -> wp 51 (home 38)\n",
        ]
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "fixture.log"
            log.write_text("".join(lines))
            stats, total = analyzer.parse_log(log)
        s = stats["fixture"]
        self.assertEqual(s["stuckstate_chain"]["stored"], 2)
        self.assertEqual(s["stuckstate_chain_hard"]["stored"], 1)
        self.assertEqual(s["stuckstate_chain_hard"]["none"], 0)
        self.assertEqual(s["stuckstate_context"][(0, "0", "preclear", "stored", "yes")]["hard"], 1)
        self.assertEqual(s["stuckstate_context"][(0, "unknown", "postclear", "stored", "unknown")]["events"], 1)
        self.assertEqual(s["chains_built"], 1)
        self.assertEqual(s["composed_built"], 1)
        self.assertEqual(s["chains_done"], 1)
        self.assertEqual(s["carrier_nav_ticks"], 2)
        output = io.StringIO()
        with redirect_stdout(output):
            analyzer.print_report(stats, total, "fixture.log")
        text = output.getvalue()
        self.assertIn("no stored chain", text)
        self.assertNotIn("it was flying the single-hop", text)
        self.assertIn("not travel time or successful crossings", text)
        self.assertIn("do not establish a route-completion rate", text)
        self.assertIn("| fixture | 0 | Red (log team=0) | preclear | stored | yes | 1 | 1 |", text)

    def test_mechanism_telemetry_parsing(self):
        """0.9.14 lines: via-fail mechanism, hop outcome, arrival item distance, item-reach LOS pair."""
        lines = [
            "Opening level 'mech.d3l'\n",
            # New-format via fail with the mechanism suffix.
            "BOT NAV: 'A[BOT]' via search failed in room 8 (target room 84) — hit=1 face=146/3 "
            "tmap=207 breakable=1 forcefield=0 d=17 stage=rings\n",
            # Old-format via fail (no suffix) must still count, with no stage.
            "BOT NAV: 'A[BOT]' via search failed in room 12 (target room 6)\n",
            "BOT NAV: 'A[BOT]' hop outcome: CROSSED rm3 -> rm6 via portal 1 (1.2s)\n",
            "BOT NAV: 'A[BOT]' hop outcome: NOT-CROSSED rm35 -> rm84 via portal 2 (8.1s, now rm35)\n",
            # ARRIVED now carries item identity/distance/aim; the em dash is the separator.
            "BOT OBJ: 'A[BOT]' ARRIVED at objective room 6 (t=285s) — item='FlagBlue' obj=321 "
            "d_item=20 steer rm44 d=20\n",
            "BOT NAV: item-reach 'Shield' (room 8): REACHABLE (graph-connected) los=0 d=117\n",
            "BOT NAV: item-reach 'Seeker3pack' (room 120): UNREACHABLE (no hull-clear graph link) los=1 d=44\n",
        ]
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "mech.log"
            log.write_text("".join(lines))
            stats, _ = analyzer.parse_log(log)
        s = stats["mech"]
        self.assertEqual(s["via_fails"], 2)
        self.assertEqual(s["via_fail_stages"]["rings"], 1)
        self.assertEqual(s["via_fail_faces"][(146, 3)], 1)  # keyed by FACE room/index, not the bot's room
        self.assertEqual(s["via_fail_breakable"], 1)
        self.assertEqual(s["via_fail_forcefield"], 0)
        self.assertEqual(s["hop_crossed"], 1)
        self.assertEqual(s["hop_not_crossed"], 1)
        self.assertEqual(s["hop_not_crossed_portals"][(35, 84)], 1)
        self.assertEqual(s["arrived_obj"], 1)
        self.assertEqual(s["arrived_rooms"][6], 1)
        self.assertEqual(s["arrived_d_item"], [20.0])
        self.assertEqual(s["item_reach_events"], 2)
        self.assertEqual(s["item_reach_unreachable"], 1)
        self.assertEqual(s["item_reach_los_yes"], 1)
        self.assertEqual(s["item_reach_unreachable_los_yes"], 1)
        output = io.StringIO()
        with redirect_stdout(output):
            analyzer.print_report(stats, _ if False else 0, "mech.log")
        text = output.getvalue()
        self.assertIn("Mechanism Telemetry", text)
        self.assertIn("Hops Crossed", text)
        self.assertIn("UNREACHABLE but LOS clear", text)


if __name__ == "__main__":
    unittest.main()
