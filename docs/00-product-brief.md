# Product Brief

Baseline date: 2026-10-12. Document language: English. Working product name: **Qinglan: Path of Ascension**. Internal project name: **QinglanAscension**. Repository name: `qinglan-wendao`.

## 1. Product promise

Give players the feeling of becoming a cultivator through responsive 3D sword combat, readable elemental spells, exploration of atmospheric mountain sanctuaries, and memorable tribulation encounters. Chinese cultivation concepts must change play: spiritual energy powers techniques, cultivation advances permanent realms, talismans provide preparation choices, and breakthrough trials demand mastery.

The game is a small, complete experience that can be replayed. Photorealistic surface response and physically grounded spell lighting are priorities; a large open world, cinematic facial acting, and unlimited destructive simulation are outside the first-release budget.

## 2. Confirmed requirements versus design choices

Confirmed by the owner: completely free game; public repository; English development text and comments; English and Simplified Chinese game UI; 3D presentation with ambitious realistic effects; Windows and Mac support; Windows GPU use; Apple Silicon Mac target; engines usable for free by an individual developer. Performance evaluation focuses on recent gaming laptops and Macs, without a CPU-only Windows requirement.

Chosen design baseline: third-person action; Unreal Engine 5.8; offline play; one protagonist; two realms; three cultivation stages; no advertising, purchases, monetization SDK, server, or account login. These choices guide implementation and can be changed by a new recorded decision.

Windows Microsoft Store remains the required storefront. Microsoft Store does not distribute a macOS build; the Mac release uses a separate direct-download channel. Mac App Store publication is a later option requiring a separate packaging and sandbox assessment.

## 3. Audience and sessions

Target players enjoy action RPGs and cultivation fantasy but may have no prior knowledge of xianxia. Explain specialized terms through concise contextual tooltips. Intended campaign length: 4–6 hours for a first clear and 6–8 hours including all optional stories, subject to playtesting. The expanded authored narrative uses eight main chapters, eight side quests, and three informed ending choices. Realm expeditions target 12–18 minutes. Normal room encounters target 45–90 seconds. The player can pause and suspend at safe room boundaries.

No daily rewards, energy timers, required grinding, random paid rewards, or online retention incentives. Repetition should support experimentation rather than gate the ending.

## 4. Scope

| Feature | Vertical slice | Version 1.0 |
| --- | --- | --- |
| Character | One rig and sword style | Same rig; two cosmetic robe variations earned in play |
| Hub | Training courtyard and menu | Small sanctuary with three interactive stations |
| Realms | One 4-room expedition | Two 7-room expedition templates |
| Basic enemy archetypes | Three | Six |
| Bosses | One | Two realm bosses |
| Tribulations | One prototype | Foundation and Golden Core trials |
| Active techniques | Three | Eight; four equipped |
| Passives / talismans / relics | 3 / 2 / 2 | 8 / 6 / 6 |
| Temporary blessings | Four | Twelve |
| Settings / saving | Essential | Full controls, accessibility, three profile slots |
| Narrative | CH00–CH03 slice, one optional story | Eight main chapters, eight side quests, three endings, 24 records |
| Builds | Windows and native Mac feasibility | Both release builds with the same gameplay version |

The seven-room expedition count includes the entrance and exit shrine. It does not mean seven full combat encounters. Cosmetic variants and optional notes may be removed before reducing responsiveness, dual-platform support, save integrity, or spell readability. The two realms now carry distinct mandatory story revelations; removing either requires redesigning the complete narrative and must not silently strand the ending.

## 5. Out of scope for first release

Multiplayer, open world streaming, flying traversal, faction simulation, romance, housing, survival meters, elaborate alchemy minigames, procedural 3D geometry, voice acting, live AI dialogue, mod loaders, cloud saves, Xbox services, console support, and Intel Mac support. Apple Silicon is the explicitly selected Mac baseline. None of these features should appear as promised store features.

## 6. Quality and success criteria

- A new player understands movement, attacks, spiritual energy, and the return loop within ten minutes.
- At least four of five external first-time testers finish the tutorial without developer assistance.
- Every damage event has a visible cause and useful sound cue.
- Every shipped spell retains its telegraph and impact silhouette at Low quality.
- Windows and Mac produce equivalent progression and rewards from the same content version.
- The release gates in the QA document pass on actual hardware; editor FPS is insufficient evidence.

These are acceptance targets, not market forecasts. There is no analytics upload in version 1.0. Gather feedback through voluntary testing and public issue reports that exclude private logs or saves.

## 7. Cost boundaries

The player's price is zero. Unreal game development can use Epic's free game-development licensing path; license obligations are tracked separately. Mandatory paid engine seats and paid-only plugins are disallowed. Free licensed assets must still permit shipping and the required use.

Production is not guaranteed to cost zero: developer time, hardware, storage, and a trusted macOS signing/notarization workflow can incur costs. Do not enroll in paid services or purchase assets automatically. Use original assets, public-domain/CC0 sources, or suitably licensed free tools first. An unsigned Mac test build must never be represented as the final trusted release.
