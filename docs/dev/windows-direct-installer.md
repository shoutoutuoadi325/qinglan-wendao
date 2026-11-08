# Windows direct-download packaging

The desktop installer and portable ZIP both use the exact same verified Win64
Shipping payload. They are separate from the encrypted Microsoft Store MSIXVC
submission package. The packaging script never rebuilds Unreal or modifies its
input artifact.

Run on a Windows runner with PowerShell 7 after downloading the successful
Windows build artifact, keeping its Windows/ tree and build-evidence.json
together. For the v0.1.3 source run:

~~~powershell
./tools/ci/windows-direct-installer.ps1 -StagedPath "$env:RUNNER_TEMP/payload/Windows" -SourceEvidencePath "$env:RUNNER_TEMP/payload/build-evidence.json" -OutputPath "$env:RUNNER_TEMP/direct" -Version '0.1.3' -ExpectedProjectCommit '93f91dc3cc7ef42924e8e1756691a6e28671b84f' -SourceRunId '42474888842'
~~~

The caller must first verify through GitHub's API that the source run succeeded
and its head_sha equals the expected commit. The script independently requires
that commit in the Win64 Shipping manifest, validates every recorded source
file's size and SHA-256, and rejects unverified game files. The Shipping EXE and
nonempty PAK, UTOC and UCAS content are mandatory. Injected Store shell assets,
manifests and build-status files are omitted from the direct package.

The existing Store repack workflow also supports a direct_install mode. Dispatch
it at the packaging-tools branch with source_run_id, version and
expected_project_commit. That mode requires a successful source run whose SHA
also matches the release tag. It downloads the existing Shipping artifact,
compiles the installer and runs windows-direct-install-smoke.ps1 before uploading
the public-download outputs. It does not rebuild the game.

The smoke check runs both language selections, compares all installed game files
with the payload inventory, checks Start menu and desktop launch paths, starts
the installed Shipping executable with NullRHI, and uninstalls it while checking
that a unique save-file sentinel remains intact. The authored screenshot mode
provides a bounded exit after world startup. NullRHI does not verify DX12 visuals.

## Outputs

- Qinglan-0.1.3-Windows-x64-Setup.exe: an Inno Setup installer with Start menu
  shortcuts, an optional desktop shortcut and a Windows uninstaller.
- Qinglan-0.1.3-Windows-x64.zip: the same game tree with a launcher, installation
  guide and bundled Microsoft runtime for portable use.
- SHA256SUMS: SHA-256 entries for both downloadable files.
- direct-package-evidence.json: the source run and commit, complete payload
  inventory, compiler provenance, prerequisite signature and downloadable hashes.

The portable archive is reopened and every file's size and SHA-256 is compared
with the package inventory. Only completed outputs are copied from temporary
storage into the requested output directory. Merge the two Windows checksum
entries with the existing macOS entry when publishing a shared release.

## Compiler, runtime and installation behavior

Inno Setup 7.1.1 is locked to the official release installer and its GitHub
SHA-256 digest. A registered compiler is reused only when it has the matching
version and a valid official signature; otherwise the verified official installer
provides an isolated compiler. Its English and Simplified Chinese resources
provide the installer UI without maintaining copies of third-party translations.

The Microsoft x64 VC++ runtime is downloaded from its official aka.ms/vc14
endpoint. Windows Authenticode must validate its signature and Microsoft
Corporation signer before it is packaged. Its actual version and SHA-256 are
recorded, since Microsoft's supported-runtime endpoint advances over time.
Installation skips a matching or newer runtime by checking both registry views;
otherwise it installs silently with /norestart. Exit code 3010 requests a reboot
after game installation; other nonzero results fail preparation with a localized
Windows error. The portable guide directs users to the same bundled prerequisite.

The stable direct-install AppId is
028A5C95-3510-56CD-B9D9-29F884E79E3E. Keep it across updates. Installed files
reside in Program Files; saves remain in
%LOCALAPPDATA%\QinglanAscension\Saved\SaveGames\. The uninstaller has no
save-removal action and never removes the shared Microsoft runtime. When the
wizard offers to launch the game, it runs as the original user rather than the
elevated installer account.

## Verification limits

PowerShell parsing and repository validation can run on macOS. Inno compilation,
Authenticode trust, registry lookup and actual installation require Windows.
Before delivery, exercise install and uninstall on a Windows runner in both
languages, verify installed file hashes against the package inventory (the runtime
is a temporary installer input), and preserve a save-file sentinel across
uninstall. An already-installed equal or newer runtime must be accepted. Exercise
the missing-runtime and reboot-request paths on a clean Windows test environment.
DX12 gameplay, frame rate and a clean customer-machine install require their own
runtime evidence; packaging does not establish those results.
