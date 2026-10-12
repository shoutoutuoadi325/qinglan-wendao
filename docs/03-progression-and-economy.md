# Cultivation, Progression, and Economy

## 1. Permanent realms

Cultivation is deterministic. There is no random breakthrough failure, real-time waiting, offline accrual, or loss of a completed realm. Each realm has three cultivation ranks. A rank unlock costs banked Spirit Shards and the listed material; cultivation cannot consume temporary expedition loot.

| Stage | Rank costs in Spirit Shards | Material per rank | Breakthrough requirement |
| --- | --- | --- | --- |
| Qi Refining | 80, 120, 180 | 1 Spirit Herb | Clear Bamboo Guardian; complete all three ranks |
| Foundation Establishment | 220, 300, 400 | 1 Vein Crystal | Clear Ashen Guardian; complete all three ranks |
| Golden Core | Ending achievement | None | Pass Golden Core trial |

Start at Qi Refining rank 0. Rank purchases give +5 MaxHealth and +2 TechniquePower each. Foundation breakthrough gives +15 MaxHealth, +20 MaxEnergy, and unlocks technique upgrades to tier 2. Golden Core gives +20 MaxHealth and access to normal post-ending replay. Total pre-final rank costs: 1,300 Spirit Shards; six purchased ranks produce +30 health and +12 power. Golden Core is the campaign culmination, not another three-rank grind.

The Foundation trial unlocks when Qi ranks are complete and Bamboo Guardian's first-clear flag is banked. The Golden Core trial unlocks when Foundation ranks are complete and Ashen Guardian's first-clear flag is banked. Trial entry is free. Check requirements atomically before a rank purchase; insufficient resources change nothing.

## 2. Target progression pace

| Activity | Target payout | Expected use |
| --- | --- | --- |
| Guided tutorial | 80 Shards + 1 Herb | First cultivation rank |
| Bamboo first clear | 180 base + 120 first-clear bonus; 3 Herbs; 1 Crystal | Learn core builds; Foundation preparation |
| Bamboo repeat clear | 180 base; 2 Herbs; 1 Crystal | Optional experimentation |
| Ashen first clear | 300 base + 200 first-clear bonus; 3 Crystals | Late cultivation and technique upgrades |
| Ashen repeat clear | 300 base; 2 Crystals | Recovery or optional upgrades |

Base payout is the expedition total, including room rewards, not an additional boss payout. First-clear bonus is separate and idempotent. The initial example reward table allocates Bamboo's 180 base Shards across its rooms.

The baseline campaign may require approximately 3–5 full expeditions beyond the guided tutorial if the player buys optional technique upgrades. This is a tuning estimate. If testers repeat a realm more than twice solely because progression materials are missing, increase guaranteed quest rewards or reduce mandatory costs.

Always display the next mandatory requirement separately from optional purchases. A first-time player must not permanently lock progression by spending currency: repeat expeditions replenish all required materials, and known materials have guaranteed sources.

## 3. Techniques and loadouts

Jade Sword Arc and Flowing Ward unlock during the tutorial; Ember Lance unlocks after the guided expedition. Thunder Seal unlocks after Bamboo's first clear. Stone Aegis and Verdant Pulse unlock at Foundation. Mist Step and Orbiting Blades unlock through optional lore encounters with explicit journal directions.

Equip up to four techniques, two passives, two talismans, and one relic at a sanctuary or entrance shrine. Swapping is free. Do not allow loadout changes mid-combat or during cooldowns to reset availability. Unequipped techniques retain their progression.

Tier 1 is unlocked automatically. Optional tier 2 costs 100 Shards + 1 Crystal and grants +15% damage or healing magnitude; cooldown and cast timing stay unchanged. Barrier upgrades increase barrier amount by 15%. For damage, multiply the complete raw-damage expression before mitigation. No tier 3 at launch.

## 4. Passive manuals

Eight passives with two equipped: Energy Recovery (+1 energy/s), Steady Foundation (+10 health), Sword Discipline (+10% sword damage), Quiet Mind (-10% technique energy cost), Grounded Steps (+10 stamina), Herbal Knowledge (+10% healing), Elemental Insight (+10% reaction damage), and Measured Breath (+10% stamina recovery).

Never multiply several unrelated bonuses without an explicit rule. Add bonuses within a category, then apply category totals once. Energy cost has a minimum of 1 for an energy-spending skill, rounded up. Reaction damage modifiers affect Conduct only. There is no hidden stacking with temporary blessings.

## 5. Talismans, relics, and blessings

Talismans have expedition charges rather than permanent consumable inventory: Healing Talisman (25 health, 2 charges), Cleansing Talisman (remove Burn/Root, 2), Protective Talisman (20 barrier, 2), Focus Talisman (restore 25 energy, 2), Shock Talisman (interrupt ordinary enemies in 300 cm, 1), and Recall Talisman (request retreat from a cleared room, 1). Refill at the sanctuary for free. They cannot create infinite materials or restore their own charges.

Six permanent relics: Jade Pendant, Ember Bead, River Pearl, Thunder Charm, Mountain Token, and Verdant Seed. Each grants one modest element-related bonus and a clear tradeoff or cooldown. Implement two in the slice; defer additional relic upgrades until basic balance passes.

Twelve temporary blessings form four groups of three: sword offense, casting economy, protection, and elemental interactions. Offer three distinct eligible blessings after rooms 2 and 4. A blessing cannot be offered twice if it has a one-stack cap. If fewer than three options remain, show the available count without inventing duplicates. Blessings expire at expedition end and are serialized at checkpoints.

## 6. Economy controls

Only Spirit Shards and two material types are mandatory progression resources. No cash-equivalent currency, trading, loot boxes, or purchase screen. Inventory counts use bounded 64-bit integers; reject negative grants and overflow. A reward transaction has a stable ID derived from profile, expedition, room, and reward category.

UI previews show the exact transaction before confirmation. Bank once on return; trial wins and story proof commit once. Never let an autosave callback re-award a reward. Failed save writes preserve the in-memory transaction and offer retry; they do not grant a second payout.

## 7. Balance review

Track voluntarily observed clear time, deaths, damage sources, chosen techniques, and mandatory repeat count in supervised tests. The game itself has no network analytics. Test basic sword-only completion, all defensive slots, burst combinations, and Water/Lightning reactions. One efficient build is acceptable; a build that permanently denies boss actions is a bug.
