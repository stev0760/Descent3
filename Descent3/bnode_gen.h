/*
* Descent 3
* Copyright (C) 2024 Parallax Software
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Runtime BNode (in-room AI waypoint) generator for multiplayer maps.
//
// Multiplayer maps ship without baked BNodes — vanilla D3 MP had no AI players, so the editor's
// level-authoring BNode pass was never run on them (official or custom). Without BNodes the engine
// threads rooms with only the sparse BOA path (room path_pnt + portal points), which fails in complex
// rooms. This generator ports Outrage's own editor pass (editor/ebnode.cpp) and is invoked at MP level
// load — the matched twin of the existing MakeBOA() repair in MultiStartNewLevel(). The engine's native
// AIGenerateBNodePath then threads complex rooms exactly as on single-player maps.
//
// See matcen-docs/NAVIGATION.md §2.2.

#ifndef BNODE_GEN_H_
#define BNODE_GEN_H_

// Build the in-room BNode graph for the currently loaded level from room geometry. Sets BNode_allocated
// and BNode_verified. Call only when BNode_allocated is false (i.e. the map shipped without BNodes).
void BNodeGenerateForLevel(void);

#endif
