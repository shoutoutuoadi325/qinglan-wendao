# Example Data Contracts

These JSON examples illustrate stable IDs and reviewable fields. They are not a complete runtime database, Unreal data assets, or a production save serializer.

| File | Contract |
| --- | --- |
| [techniques.json](techniques.json) | Eight technique IDs, localization keys, elements, costs, cooldowns, and damage coefficients |
| [realm.json](realm.json) | Seven-room Bamboo template; base payout sums to 180; distinct first-clear bonus |
| [save.json](save.json) | New profile at Qi Refining rank 0; permanent state; optional checkpoint |

`schema_version` identifies the file shape; `content_version` identifies gameplay content. Runtime technique data additionally requires target rules, cast phases, cue references, animations, effects, and unlock conditions from the combat document. Non-damaging techniques use zero damage fields; their shield/heal/movement payloads are defined separately during implementation.

The first-clear materials in the realm example represent that expedition's total guaranteed first-clear allocation. Repeat runs use their own totals from the economy document; do not add the first-clear total to a second material payout.

Save IDs are synthetic and contain no user information. A future checkpoint must serialize seed/stream states, cleared room index, temporary inventory, blessings, attributes, and committed rewards. A null checkpoint means the profile is in the sanctuary.
