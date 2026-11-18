# Descent 3 movement physics

This document lists the engine's movement physics that bot movement is built on: the drag model, the thrust formula,
the afterburner ramp, the packet flags clients use for thruster effects, and the constants the bots set. The bots'
thrust code is `BotApplyThrust()` and `BotConfigureAI()` in `Descent3/bot.cpp`. The rulings that constrain bot
movement are in [NAVIGATION.md](NAVIGATION.md) §1.3.

---

## 1. The physics model: exponential drag

Descent 3 uses an **exponential drag model**: velocity decays toward an equilibrium set by the applied thrust, with a
time constant set by mass and drag.

```
v_eq = force / drag         (terminal velocity under constant thrust)
τ    = mass / drag          (time constant: how fast v approaches v_eq)
v(t) = v_eq + (v₀ - v_eq) × exp(-t / τ)
```

With no thrust, `v_eq` is 0 and the ship slows to a stop on its own. The integration is `PhysicsApplyConstantForce()`
in `physics/physics.cpp`. When drag or mass is zero, it integrates the force with no drag instead.

---

## 2. Thrust construction: tri-chording

```cpp
// DoFlyingControl() in Descent3/object.cpp
objp->mtype.phys_info.thrust =
    speed_scalar * Players[objp->id].movement_scalar *
    (
        (objp->orient.fvec * controls.forward_thrust  * phys_info.full_thrust) +
        (objp->orient.uvec * controls.vertical_thrust * phys_info.full_thrust) +
        (objp->orient.rvec * controls.sideways_thrust * phys_info.full_thrust)
    );
```

The three axes are added **without normalization**, so thrusting on all three at once (tri-chording) gives
`√3 ≈ 1.732×` the thrust of one axis. `BotApplyThrust()` uses the same formula: the same three axes, the ship's
`full_thrust`, the player's `movement_scalar` and the outdoor scalar, with the afterburner multiplier on the forward
axis only (§3).

### Speed scalars

| Condition | `speed_scalar` in `DoFlyingControl` | In `BotApplyThrust` |
|---|---|---|
| Normal | 1.0 | 1.0 |
| Outside, on terrain | 1.3 | 1.3 |
| `EF_FREEZE` effect | `effect_info->freeze_scalar` | not applied |

A freeze hit (`ApplyFreezeDamageEffect` in `damage.cpp`, for `WF_FREEZE` weapons) lowers `freeze_scalar` by 0.2 per
hit, to no less than 0.3. It recovers by one third per second (`ObjDoEffects()` in `object.cpp`), and the effect ends
when it reaches 1.0. A player's thrust is scaled by it, and a bot's is not.

---

## 3. Afterburner: the thrust multiplier ramp

`DoPlayerAfterburnControl()` (`Descent3/object.cpp`), abridged:

```cpp
float punch_scalar = 1.0;
if (Players[slot].afterburn_time_left > AFTERBURN_TIME * 0.90f)
    punch_scalar = 1.8f;
else if (Players[slot].afterburn_time_left > AFTERBURN_TIME * 0.80f &&
         Players[slot].afterburn_time_left < AFTERBURN_TIME * 0.90f) {
    float norm = (Players[slot].afterburn_time_left - AFTERBURN_TIME * 0.80f)
               / (AFTERBURN_TIME * 0.10f);
    punch_scalar = 1.0f + norm * 0.8f;  // 1.0 rising to 1.8
}
controls->forward_thrust = controls->afterburn_thrust * 1.6f * punch_scalar;
```

| Fuel remaining | `punch_scalar` | Total multiplier |
|----------------|-------------|-----------------|
| over 90% | 1.8 | **2.88×** normal |
| 80 to 90% | 1.0 to 1.8 (linear) | **1.6 to 2.88×** |
| under 80% | 1.0 | **1.6×** |

`AFTERBURN_TIME` is 5.0 seconds (`Descent3/player.h`).

`DoFlyingControl`, which calls `DoPlayerAfterburnControl`, runs for an object under `CT_FLYING` control, and a bot's
ship is under `CT_AI` control. `BotApplyThrust()` therefore replicates the ramp with its own fuel counter
(`Bots[i].afterburner_fuel`, `BOT_AFTERBURNER_FUEL_MAX` 5.0 s, `BOT_AFTERBURNER_THRUST_MULT` 1.6). Burning drains fuel
and energy at one unit per second each. When the bot is not burning and has more than 20 energy
(`BOT_AB_RECHARGE_ENERGY_MIN`), fuel recharges from energy at the same rate.

---

## 4. Multiplayer packet flags

These flags drive the clients' visual effects for a ship. They are defined in `Descent3/multi.cpp` and set in
`MultiStuffPosition` for the position packet.

| Flag | Value | Effect |
|------|-------|--------|
| `MPF_AFTERBURNER` | 1 | blue afterburner flame |
| `MPF_OUTSIDE` | 2 | the player is on terrain |
| `MPF_DEAD` | 4 | the player is dead |
| `MPF_FIRED` | 8 | a weapon fired this frame |
| `MPF_SPRAY` | 16 | a spray weapon is active |
| `MPF_ON_OFF` | 32 | an on/off weapon is active |
| `MPF_HEADLIGHT` | 64 | headlight on |
| `MPF_THRUSTED` | 128 | forward thrust: the thruster plume |

`MPF_AFTERBURNER` and `MPF_THRUSTED` come from `PLAYER_FLAGS_*` (`Descent3/player_external.h`):

| `PLAYER_FLAGS` | Value |
|-------------|-------|
| `PLAYER_FLAGS_AFTERBURN_ON` | `1 << 15` = 32768 |
| `PLAYER_FLAGS_THRUSTED` | `1 << 17` = 131072 |

A client copies the two packet flags back into `Players[slot].flags`. `DoPlayerFrameForOne()` (`Descent3/Player.cpp`)
then ramps the thruster glow from `PLAYER_FLAGS_THRUSTED` and the afterburner glow and sound from
`PLAYER_FLAGS_AFTERBURN_ON`, and `renderobject.cpp` draws the plume from both. A player's afterburner sets both flags,
and `BotApplyThrust()` sets both while a bot burns.

---

## 5. The `physics_info` structure

`lib/object_external_struct.h`, abridged to the fields bot movement uses:

```cpp
struct physics_info {
    vector velocity;       // current linear velocity (units/sec)
    vector thrust;         // applied thrust force, integrated by PhysicsDoFrame when PF_USES_THRUST is set

    vector rotvel;         // angular velocity
    vector rotthrust;      // angular thrust

    float  mass;           // object mass
    float  drag;           // linear drag coefficient
    float  rotdrag;        // rotational drag coefficient
    float  full_thrust;    // maximum thrust magnitude
    float  full_rotthrust; // maximum rotational thrust

    uint32_t flags;        // PF_* physics flags
};
```

### Key physics flags

| Flag | Meaning |
|------|---------|
| `PF_USES_THRUST` | `PhysicsDoFrame()` integrates the thrust field; it must be set for bots |
| `PF_FIXED_VELOCITY` | velocity is frozen; `ResetPlayerObject()` sets it for players who are not the local player, and `BotConfigureAI()` **must clear it** |
| `PF_LEVELING` | the object levels itself toward the horizon |

**Important:** `PlayerSetControlToAI()` (`Descent3/Player.cpp`) clears `PF_USES_THRUST` and `PF_LEVELING`, sets
`drag = 0.1`, zeroes the ship's velocity and thrust, and zeroes the AI state. `BotConfigureAI()` then restores the ship template values and sets
`PF_USES_THRUST` again.

---

## 6. Quick reference: bot physics constants

From `BotConfigureAI()` and `BotCacheShipPhysics()` in `bot.cpp`:

```cpp
obj->ai_info->max_velocity       = 50.0f;   // used by the AI goal system to scale direction
obj->ai_info->max_delta_velocity = 0.0f;    // zero: AI goals cannot change velocity
obj->ai_info->max_turn_rate      = 16000;   // initial; BotApplyThrust sets it every frame
obj->ai_info->avoid_friends_distance = 40.0f; // PlayerSetControlToAI sets 0; must be set again

// Ship template values (restored after PlayerSetControlToAI):
obj->mtype.phys_info.mass          = Ships[ship_idx].phys_info.mass;
obj->mtype.phys_info.drag          = Ships[ship_idx].phys_info.drag;
obj->mtype.phys_info.rotdrag       = Ships[ship_idx].phys_info.rotdrag;
obj->mtype.phys_info.full_thrust   = Ships[ship_idx].phys_info.full_thrust;
obj->mtype.phys_info.full_rotthrust = Ships[ship_idx].phys_info.full_rotthrust;
obj->mtype.phys_info.flags &= ~PF_FIXED_VELOCITY;
obj->mtype.phys_info.flags |=  PF_USES_THRUST;
obj->flags |= OF_FORCE_CEILING_CHECK;   // ceiling collision (bots are CT_AI, normally excluded)
```

Every frame, `BotApplyThrust()` sets `max_turn_rate` by range to the target (65535 under 70 units, 40000 under 140,
26000 beyond) times the difficulty's turn-rate scale, capped at 65535 ([NAVIGATION.md](NAVIGATION.md) §1.3).

---

## 7. The physics rulings

The rulings that constrain bot movement (braking is not thrusting, and bots do not resist knockback), and the one place
bot code writes a velocity, are in [NAVIGATION.md](NAVIGATION.md) §1.3. They follow from the drag model in §1.
