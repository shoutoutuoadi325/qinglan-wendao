# Decision Log

Baseline: 2026-10-12. Status is either Confirmed Requirement, Accepted Design, or Pending Validation.

| ID | Status | Decision | Rationale / validation |
| --- | --- | --- | --- |
| ADR-001 | Confirmed Requirement | Completely free, no ads or in-app purchases | Owner selected completely free release |
| ADR-002 | Confirmed Requirement | Public repository | Owner selected public visibility; no reuse license chosen yet |
| ADR-003 | Confirmed Requirement | 3D with ambitious realistic hero effects | Owner requested contemporary AAA-like spell quality |
| ADR-004 | Confirmed Requirement | Windows and Apple Silicon macOS | Owner selected Apple Silicon; Windows requires GPU rendering |
| ADR-005 | Confirmed Requirement | English development; bilingual English/Simplified Chinese UI | Latest owner clarification supersedes English-only UI |
| ADR-006 | Confirmed Requirement | Engines and mandatory plugins must allow free individual use | No paid-only dependency |
| ADR-007 | Accepted Design | Unreal Engine 5.8 release family | Niagara, realistic 3D, native platform support; exact patch validated in M0 |
| ADR-008 | Accepted Design | Small third-person action game with authored realms | Focus quality budget on combat and hero spells |
| ADR-009 | Accepted Design | Offline, no server/account/telemetry | Free operation and reliable single-player access |
| ADR-010 | Accepted Design | Lightweight ability components before GAS | Eight techniques do not require multiplayer-scale complexity |
| ADR-011 | Accepted Design | CPU gameplay authority; GPU cosmetic effects | Platform and quality differences cannot change damage |
| ADR-012 | Accepted Design | GDK Game / MSIXVC Windows route; native Mac direct download | Store-compatible Windows plan; separate Mac distribution |
| ADR-013 | Accepted Design | Core visuals independent of hardware ray tracing | Metal parity and stable scaling |
| ADR-014 | Pending Validation | Stable 60 FPS recent gaming laptop; base Mac 30 FPS; preferred Mac 60 FPS | Must be measured; purchase year alone cannot establish performance |
| ADR-015 | Pending Validation | Free Unreal license eligibility and asset permissions | Verify accepted terms and applicable personal/indie exception |
| ADR-016 | Pending Validation | Store name, identity, Mac signing, support contact | Must come from owner-controlled release configuration |

Changes update the affected design tables, sample data, acceptance cases, and README in the same commit. Requirements may change through an explicit owner instruction; implementation preferences may change through documented evidence. Neither an example file nor a public issue silently overrides a confirmed requirement.
