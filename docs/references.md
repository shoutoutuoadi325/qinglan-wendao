# Verified External References

Checked on 2026-10-12. These sources support engine/platform/distribution facts. Gameplay content, budgets, and schedules are original planning choices. Dynamic documentation can change; the M0 spike and final release must recheck it.

| Topic | Official source | Use in this project |
| --- | --- | --- |
| Engine release | [Unreal 5.8 release announcement](https://www.unrealengine.com/news/unreal-engine-5-8-is-now-available) | Released engine family, not preview baseline |
| Free game-development access | [Unreal licensing](https://www.unrealengine.com/license) | Free-use policy and revenue-based obligations |
| Binding engine terms | [Unreal EULA](https://www.unrealengine.com/eula/unreal) | Eligibility, distribution, notices, exceptions |
| Windows tools and rendering | [Epic hardware/software requirements](https://dev.epicgames.com/documentation/en-us/unreal-engine/hardware-and-software-specifications-for-unreal-engine) | Version-specific developer and renderer prerequisites |
| Mac tooling/features | [Epic Mac requirements](https://dev.epicgames.com/documentation/en-us/unreal-engine/macos-development-requirements-for-unreal-engine) | Metal/Apple Silicon capability and tooling verification |
| Scalable lighting | [Lumen performance guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-performance-guide-for-unreal-engine) | Rendering profiling and scaling references |
| Engine Store integration | [Unreal GDK plugins](https://dev.epicgames.com/documentation/unreal-engine/microsoft-gdk-plugins-for-unreal-engine?lang=en-US) | Win64 integration to validate during M0 |
| PC game Store route | [Microsoft game publishing overview](https://learn.microsoft.com/en-us/windows/apps/publish/whats-new-game-publishing) | GDK Game route for new PC games |
| Game package preparation | [Package/upload guide](https://learn.microsoft.com/en-us/gaming/game-publishing/tutorial-xbox-managed/how-to-create-a-package?pivots=msow) | Package artifacts and no-Xbox-services config guidance |
| Packaging tools | [MSIXVC PC guide](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/packaging/overviews/packaging-getting-started-for-pc?view=gdk-2604) | Versioned SDK workflow; evaluate MSIXVC2 separately |
| Ratings | [Game age ratings](https://learn.microsoft.com/en-us/gaming/game-publishing/tutorial-xbox-managed/how-to-set-age-ratings?pivots=msow) | Actual content questionnaire |
| Store metadata | [Game listing guide](https://learn.microsoft.com/en-us/gaming/game-publishing/tutorial-xbox-managed/how-to-create-a-store-listing?pivots=msow) | Localized truthful copy and current asset checks |
| Store policies | [Microsoft Store policies](https://learn.microsoft.com/en-us/windows/apps/publish/store-policies) | Privacy and effective-date review |
| Market asset licenses | [Fab licensing](https://www.fab.com/eula) | Separate source redistribution rules |
| Mac trust | [Apple notarization guide](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution) | Trusted direct-download preparation |

Known documentation caveats: some Microsoft game pages expose both managed-Xbox and PC self-service prerequisite text; use the PC/self-service branch and actual account workflow. The Mac requirements page has inconsistent marketing OS wording in its general recommendation; use its version table and the selected engine's validated SDK rather than inferring a player minimum. Upcoming policy effective dates are tracked in the distribution guide.
