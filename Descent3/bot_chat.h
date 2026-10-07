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

#ifndef BOT_CHAT_H
#define BOT_CHAT_H

// A bot volunteers at most one report (arrival, blocked, a hunted player down, a co-op announcement)
// per this many seconds; a report inside the window waits its turn. Answers to an order are not paced.
#define BOT_CHAT_REPORT_SPACING 2.0f

// Every human chat line on the server, before it is relayed: reads a `!` order and carries it out.
void BotOnChatMessage(int from_pnum, int towho, const char *message);

// Once per server frame: sends the queued bot lines, reports hunted players who died or left, and
// delivers the level-change notices and the one-time tip.
void BotChatFrame();

// At a level change, before the orders are cleared: notes who gave the orders being cleared, so each
// of them is told once they are in the new level, and drops lines queued in the old one.
void BotChatLevelReset();

// Order lifecycle report ("In position." / "Can't get there!"), sent by direct message to the player
// who gave the bot its current order. A report the bot already gave since that order is not repeated.
void BotOrderReport(int bot_index, const char *text);

// Co-op: a bot-voiced line to everyone ("<callsign>: <text>"; retail clients see a normal chat line).
void BotBroadcastAnnounce(int bot_index, const char *text);

#endif // BOT_CHAT_H
