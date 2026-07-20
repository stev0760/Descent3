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

#define BOT_CHAT_REPLY_COOLDOWN 2.0f

void BotOnChatMessage(int from_pnum, int towho, const char *message);

// Stage 6: order lifecycle reports ("In position." / "Can't get there!") — DM'd to the player
// who issued the bot's current order (order_issuer_slot). Subject to the per-bot reply throttle.
void BotOrderReport(int bot_index, const char *text);

// Co-op: broadcast a bot-voiced line to everyone ("<callsign>: <text>", retail clients see a
// normal HUD chat message). Subject to the per-bot reply throttle.
void BotBroadcastAnnounce(int bot_index, const char *text);

#endif // BOT_CHAT_H
