#!/usr/bin/env python3
"""Compile the production skeleton search/export against a small synthetic graph.

This tests waypoint order and capacity without linking the game. Geometry and room routing are
stubbed, so it does not test hull clearance or whether an actual map crossing can be flown.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SkeletonChainTests(unittest.TestCase):
    def test_production_chain_export(self):
        source = (ROOT / "Descent3/bot_steering.cpp").read_text()
        functions = []
        for signature in ("static int SkelBfs(", "static bool ExitPortalUsable(", "int BotSkelBuildChain("):
            start = source.index(signature)
            # Both production definitions end at an unindented closing brace.
            end = source.index("\n}", start) + 2
            functions.append(source[start:end])

        harness = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

constexpr int SKEL_MAX_NODES = 32;
constexpr int RF_EXTERNAL = 1;
constexpr float BOT_VIA_ARRIVE_DIST = 15.0f;
constexpr float BOT_PORTAL_IMPASSABLE = 1.0e6f;
struct vector { float x; };
struct object { int roomnum; vector pos; float size; };
struct portal { int croom; };
struct room { bool used; int flags; int num_portals; portal portals[32]; };
room Rooms[2]{};
int Highest_room_index = 1;
bool skel_built[2]{};
int skel_node_count[2]{};
vector skel_node_pos[2][32]{};
uint32_t skel_edges[2][32]{};
int visible_node = 0;

// Every portal is admitted by default; `portal_rejected[i]` lets a check reject one portal so the
// passability filter (ExitPortalUsable) can be exercised without the game's geometry.
bool portal_rejected[32]{};
bool BOA_PassablePortal(int, int portal) { return !portal_rejected[portal]; }
float BotPortalRouteCost(int, int, bool) { return 0.0f; }
int BotPortalWindDir(int, int) { return 0; }
void SkelLevelReset() {}
void SkelBuild(int) {}
int SkelPortalCount(const room &rm) { return rm.num_portals; }
int BotComputeRoute(int, int target_room) { return target_room; }
float vm_VectorDistanceQuick(const vector *a, const vector *b) { return std::fabs(a->x - b->x); }
bool BotSegmentClear(int, const vector &a, const vector &b, float) {
  // The bot can see only one node. Only the exit node sees the same-room final target.
  return a.x == -100.0f ? b.x == visible_node * 20.0f : a.x == 60.0f && b.x == 100.0f;
}
'''
        checks = r'''
int main() {
  Rooms[0].used = Rooms[1].used = true;
  Rooms[0].num_portals = 4;
  skel_built[0] = true;
  skel_node_count[0] = 4;
  for (int i = 0; i < 4; ++i) {
    Rooms[0].portals[i].croom = i == 3 ? 1 : -1;
    skel_node_pos[0][i] = {i * 20.0f};
    if (i > 0) skel_edges[0][i] |= 1u << (i - 1);
    if (i < 3) skel_edges[0][i] |= 1u << (i + 1);
  }
  object bot{0, {-100.0f}, 6.7f};
  vector output[32];
  // Routed callers pass a local aim together with the next room. Never append that aim behind
  // the exit: the old [near, ..., exit, near] chain turned a corrected crossing back on itself.
  assert(BotSkelBuildChain(&bot, 0, 1, {0.0f}, output, 32) == 4);
  assert(output[0].x == 0.0f && output[3].x == 60.0f);
  // Passability filter: an exit portal the router would not admit is not a chain candidate, even
  // when the skeleton graph connects it. (This is the aim/router agreement the 0.9.14 fix added.)
  portal_rejected[3] = true;
  assert(BotSkelBuildChain(&bot, 0, 1, {0.0f}, output, 32) == 0);
  portal_rejected[3] = false;
  for (int target_room : {0, 1}) {
    visible_node = 0;
    int n = BotSkelBuildChain(&bot, 0, target_room, {100.0f}, output, 32);
    const int expected = target_room == 0 ? 5 : 4;
    assert(n == expected);
    for (int i = 0; i < 4; ++i) assert(output[i].x == i * 20.0f);
    if (target_room == 0) assert(output[4].x == 100.0f);
    // Never truncate a crossing and append the destination across omitted legs.
    assert(BotSkelBuildChain(&bot, 0, target_room, {100.0f}, output, expected - 1) == 0);
    assert(BotSkelBuildChain(&bot, 0, target_room, {100.0f}, output, expected) == expected);
    // A directly visible exit is one skeleton hop; leave it to the existing single-hop caller.
    visible_node = 3;
    n = BotSkelBuildChain(&bot, 0, target_room, {100.0f}, output, 32);
    assert(n == 0);
  }
  // Two skeleton nodes are still a multi-hop crossing after removing the extra terminal point.
  visible_node = 2;
  assert(BotSkelBuildChain(&bot, 0, 1, {40.0f}, output, 2) == 2);
  assert(output[0].x == 40.0f && output[1].x == 60.0f);
  assert(BotSkelBuildChain(&bot, 0, 0, {100.0f}, output, 2) == 0);
  assert(BotSkelBuildChain(&bot, 0, 0, {100.0f}, output, 3) == 3);
  assert(output[2].x == 100.0f);
  visible_node = 0;
  skel_edges[0][1] &= ~(1u << 2);
  skel_edges[0][2] &= ~(1u << 1);
  assert(BotSkelBuildChain(&bot, 0, 1, {100.0f}, output, 32) == 0);
  std::cout << "production skeleton chain: order, endpoints, capacity, direct exit, disconnected passed\n";
}
'''
        with tempfile.TemporaryDirectory(prefix="bot-skel-test-") as directory:
            path = Path(directory)
            cpp = path / "chain_test.cpp"
            exe = path / "chain_test"
            cpp.write_text(harness + "\n".join(functions) + checks)
            command = shlex.split(os.environ.get("CXX", "c++"))
            subprocess.run(command + ["-std=c++17", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
