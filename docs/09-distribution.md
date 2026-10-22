# Windows Store and macOS Distribution

Verified against official documentation on 2026-10-12. Recheck the selected SDK, Partner Center account capabilities, and policies at submission. This is a preparation guide; no store product, application identity, package, certificate, or release has been created by this documentation task.

## 1. Windows publishing route

Use the **GDK Game** route for the new Win32 PC title. Microsoft currently recommends it for new PC games; packaging uses MSIXVC and submission is managed in Partner Center. Xbox network services are optional for this PC-only design. General non-game EXE/MSI installer guidance must not be assumed to apply to this game. [Microsoft's PC game publishing overview](https://learn.microsoft.com/en-us/windows/apps/publish/whats-new-game-publishing).

Unreal 5.8 documents Win64 GDK plugins; the exact required plugin combination and installed GDK version must be proven in M0. Keep Mac builds free of Windows platform dependencies. [Epic's GDK plugin documentation](https://dev.epicgames.com/documentation/unreal-engine/microsoft-gdk-plugins-for-unreal-engine?lang=en-US).

The original MSIXVC route remains the working baseline. Microsoft's packaging page also announces MSIXVC2 for October 2026 GDK; adopt it only after confirming SDK/account support and the revised upload workflow. Do not mix formats, tools, or credentials from different versions. [PC packaging guide](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/packaging/overviews/packaging-getting-started-for-pc?view=gdk-2604).

## 2. M0 packaging spike

1. On a Windows development machine, obtain a matching released Unreal installation, build tools, and public GDK from official sources.
2. Confirm the owner-controlled Partner Center account can create a GDK Game product; reserve the localized working titles only after name availability and rights review.
3. Read product Identity values from Game setup. Keep publisher/account details in a private release configuration. Never invent identifiers.
4. Produce a minimal Win64 packaged build and configure the applicable Epic GDK plugin path. Include only runtime game content and approved runtime prerequisites.
5. Generate `MicrosoftGame.config` with real product identity, supported language resources, shell icons, and executable paths. The repository template is deliberately not submit-ready.
6. Package, install locally, launch, save, quit, restart, and upgrade a second package version. Preserve command output and environment versions privately as necessary.
7. Record the proven workflow in a release runbook before M1. If it fails, resolve the platform issue before expanding content.

For this game without Xbox services, omit `MSAAppId`, `TitleId`, and `ExtendedAttributeList` from the shipping game config; Microsoft's upload guide identifies an unwanted Xbox sign-in failure when these are included. Upload artifacts can include the package, EKB, symbol archive, and validator output depending on the packaging route. Keep package keys out of public Git. [Game package and upload guide](https://learn.microsoft.com/en-us/gaming/game-publishing/tutorial-xbox-managed/how-to-create-a-package?pivots=msow).

## 3. Submission checklist

- Product category and genre accurately identify an action game.
- Price is Free; no trials, paid add-ons, or purchase claims.
- Only measured supported Windows hardware/OS capabilities are listed.
- English and Simplified Chinese listings match the shipped UI.
- Screenshots show actual packaged gameplay; include combat, loadout, hub, and tribulation views in both languages.
- Logos and screenshots satisfy the current device-family dimensions shown in Partner Center; don't reuse an outdated Xbox-only asset checklist.
- Age-rating answers match fantasy combat, flashes, and actual content; obtain IARC results rather than invent a target rating.
- Provide a public privacy URL and support contact chosen by the owner.
- Credits and license notices match the actual shipping asset manifest.
- Certification notes explain offline play, controls, profiles, supported devices, and how to reach representative gameplay without developer tools.
- Validate with the applicable GDK submission tooling; use Windows App Certification Kit where applicable to the chosen package route, not as a substitute for GDK validation.
- Verify package version, identity, payload, symbols, and final manifest against the release record.

Official references: [age-rating questionnaire](https://learn.microsoft.com/en-us/gaming/game-publishing/tutorial-xbox-managed/how-to-set-age-ratings?pivots=msow) and [game store listings](https://learn.microsoft.com/en-us/gaming/game-publishing/tutorial-xbox-managed/how-to-create-a-store-listing?pivots=msow).

## 4. Privacy and policy timing

Provide a privacy statement even for the offline Win32 title: Microsoft's Win32 product policy requires one. The public draft in `PRIVACY.md` must be verified against Shipping behavior, completed with an owner-selected support channel, and made available at a stable URL. [Store policies, section 10.5](https://learn.microsoft.com/en-us/windows/apps/publish/store-policies).

The retrieved policy page labels version 7.20 as published 2026-09-15 and effective 2026-10-22. On the baseline date it is upcoming, not yet effective. Compare the effective version at submission and use the policy history where needed. Do not assume Store approval resolves every market's local release requirements; evaluate selected markets and avoid claiming worldwide clearance without evidence.

## 5. macOS channel

The Mac build is a native arm64 `.app`, distributed as a direct GitHub Release with the same gameplay content version as Windows. Microsoft Store is not a macOS distribution channel. Mac App Store submission is not part of 1.0.

For the 0.1.0 alpha the owner has chosen not to enroll in the paid Apple Developer Program. The public alpha is therefore **ad-hoc signed and not notarized**, not Developer ID signed. This is an explicit alpha channel, not the final trusted deliverable; Gatekeeper will block the first launch, and the release must not describe the build as signed, notarized, or Store-ready.

Prepare the build on macOS, verify all embedded plugins/frameworks have arm64 support, and apply a real bundle identifier. `tools/release/package-mac-unsigned.sh` re-signs the staged app ad hoc with the `app-sandbox` entitlement and without the debug-only `get-task-allow` entitlement, then creates the DMG and checksum file. Owner-controlled Developer ID signing, hardened runtime configuration, notarization, and stapling remain planned release tasks for a later signed build. Credentials remain in secure local/CI storage, not this repository.

Release instructions must be explicit: verify `SHA256SUMS`; open the app once; use **System Settings > Privacy & Security > Open Anyway**; if macOS reports the app is damaged, remove the quarantine flag for that app only with `xattr -dr com.apple.quarantine <app>`. Never instruct users to run `sudo spctl --master-disable`, disable Gatekeeper globally, or bypass warnings for a build they did not verify. Publish only through GitHub Releases and include the alpha EULA, credits, and install notes in the DMG. [Apple notarization documentation](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).

The direct Mac download location, bundle ID, and support contact remain release configuration decisions. The unsigned DMG is published only through GitHub Releases; it is not a Mac App Store deliverable.

## 6. Listing copy draft

English short description: **Master elemental techniques, explore mountain sanctuaries, and survive Heavenly Tribulations in a completely free 3D cultivation adventure.**

Draft feature bullets: offline single-player cultivation; sword combat and elemental techniques; realistic scalable spell effects; English and Simplified Chinese UI; permanent progression and local save profiles; no advertisements or purchases.

Chinese listing text belongs in the localized copy resource and must be reviewed as native prose. Before submission, remove any feature that did not pass acceptance. Do not advertise universal recent-device compatibility, AAA-equivalent production, cloud saves, Intel Mac support, or future content as shipped functionality.

## 7. Release, updates, and recovery

Tag the matched source commit, preserve both verified artifacts/checksums, and keep a compatible previous release for diagnosis. Windows updates follow Store servicing; Mac updates in 1.0 use manual replacement instructions with verified save preservation. Do not install a custom Windows game updater.

A bad Store version may require a new higher version containing the repair; do not promise rollback by decreasing its package version. Preserve save backward-compatibility information. Review Windows and Mac known issues separately while keeping shared gameplay fixes on the same content version.
