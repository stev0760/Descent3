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


if __name__ == "__main__":
    unittest.main()
