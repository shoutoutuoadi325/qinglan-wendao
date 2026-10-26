# Microsoft Store Submission Runbook

This guide details how to build and submit **Qinglan: Path of Ascension** to the
**Microsoft Store** using GitHub Actions' free Windows runner.

---

## 1. Why a Windows Build Host is Mandatory

Developing on macOS or Linux is fully supported for local asset creation and
native platform testing, but publishing to the Microsoft Store requires software
that **only runs on Windows**:

1. **Unreal Engine Win64 Cooker & HLSL Compiler**: Compiles C++ Win64 binaries and
   pre-cooks DirectX 12 shaders into `.pak`, `.ucas`, and `.utoc` IoStore containers.
2. **Microsoft GDK (`makepkg.exe`)**: Encrypts and bundles loose game files into
   the gaming-optimized `.msixvc` format and generates the required `.ekb` (encryption
   key block) file.
3. **Windows SDK (`MakeAppx.exe`)**: Packages Win32 applications into standard
   `.msix` app packages.
4. **Validation Tooling**: Runs `makepkg validate` to ensure compliance with
   Store ingestion rules.

By utilizing GitHub Actions' hosted `windows-2022` runner, all Windows-specific
build and packaging requirements are completed in CI at **zero runner cost** for
public repositories.

---

## 2. Microsoft Partner Center Setup

### 2.1 Reserve the Game Name
1. Sign in to the [Microsoft Partner Center](https://partner.microsoft.com/dashboard).
2. Go to **Apps and games** → **+ New product** → **Game**.
3. Reserve your title name (e.g. `Qinglan: Path of Ascension`).

### 2.2 Retrieve Product Identity
1. Inside your product page, navigate to **Product management** → **Product Identity**
   (or **Game setup**).
2. Note the following values:
   - **Package/Identity Name** (e.g. `45698YourName.QinglanPathofAscension`)
   - **Publisher ID** (e.g. `CN=A1B2C3D4-E5F6-7890-ABCD-EF1234567890`)
   - **Publisher display name** (e.g. `Your Studio Name`)
   - **Store ID** (a 12-character alphanumeric string, e.g. `9NBLGGH42THB`)

---

## 3. GitHub Secrets & Variables Configuration

In your GitHub repository, open **Settings → Secrets and variables → Actions**.

### Required for Engine Acquisition
| Name | Type | Value |
| --- | --- | --- |
| `UE_WINDOWS_ARCHIVE_URL` | Secret | HTTPS signed URL to a private precompiled UE 5.8.3 Windows installed-engine ZIP |
| `UE_WINDOWS_ARCHIVE_SHA256` | Variable | Full SHA-256 hash of the engine archive |
| *or* `UE_SOURCE_TOKEN` | Secret | PAT with read access to Epic's private repo (experimental) |

### Partner Center Submission Identity
| Name | Type | Value |
| --- | --- | --- |
| `STORE_IDENTITY_NAME` | Variable / Input | Your Partner Center Identity Name |
| `STORE_PUBLISHER_ID` | Secret / Input | Your Partner Center Publisher ID (`CN=...`) |
| `STORE_PUBLISHER_DISPLAY_NAME` | Variable / Input | Your readable publisher display name |
| `STORE_PRODUCT_ID` | Variable / Input | Your 12-char Store ID (optional during draft builds) |
| `STORE_PACKAGE_VERSION` | Variable / Input | Package version (e.g., `0.1.0.0`) |
| `GDK_ARCHIVE_URL` | Secret (optional) | Custom GDK archive URL (public GDK is auto-downloaded if omitted) |

> [!NOTE]
> If these variables are left empty, the pipeline will automatically generate a
> package using safe test default credentials (`QinglanCommunity.QinglanPathofAscension`).
> This allows you to test the packaging pipeline before completing Partner Center registration.

---

## 4. Building the Store Package in GitHub Actions

1. In GitHub, go to the **Actions** tab.
2. Under All workflows, select **Windows build**.
3. Click **Run workflow**:
   - **mode**: `package`
   - **engine_source**: `archive` (recommended)
   - **store_package**: `true`
   - **store_package_format**: `both` (or `msixvc` for GDK, `msix` for standard desktop)
   - Fill in your Partner Center identity fields (or leave blank to use defaults/configured secrets).
4. Click **Run workflow**.

The workflow will:
1. Run preflight checks on the Windows runner.
2. Fetch and extract the licensed Unreal Engine 5.8.3.
3. Build the Win64 editor and cook/package Win64 Shipping assets.
4. Stage the shell visual assets (`packaging/windows/assets/`).
5. Generate `MicrosoftGame.config` and `AppxManifest.xml`.
6. Acquire the Microsoft GDK tools and Windows SDK tools.
7. Run `makepkg pack /pc` to produce `.msixvc` and `.ekb`.
8. Run `MakeAppx.exe pack` to produce `.msix`.
9. Validate the package with `makepkg validate`.
10. Upload the artifacts for download.

---

## 5. Downloading Artifacts and Submitting to Partner Center

Once the workflow finishes, download **`Qinglan-MicrosoftStore-Package-<run_id>`**.

The archive contains:
- `msixvc/`:
  - `<PackageName>.msixvc` (The GDK game package)
  - `<PackageName>.ekb` (Encryption key block)
- `msix/`:
  - `QinglanAscension-<version>.msix` (Standard Windows app package)
- `MicrosoftGame.config` & `AppxManifest.xml` (The generated manifests)
- `store-package-evidence.json` (SHA-256 hashes and build metadata)
- `PARTNER-CENTER-UPLOAD-INSTRUCTIONS.txt` (Submission checklist)

### 5.1 Uploading to Partner Center

#### Route A: GDK Game Submission (`.msixvc` - Recommended for PC Games)
1. In Partner Center, open your Game title.
2. In the submission checklist, click **Packages**.
3. Drag and drop both the **`.msixvc`** file and the **`.ekb`** file from the `msixvc/` folder.
4. Wait for Partner Center's automated verification to complete.

#### Route B: Desktop MSIX Submission (`.msix`)
1. In Partner Center, open your MSIX product.
2. In the submission checklist, click **Packages**.
3. Upload the **`.msix`** file from the `msix/` folder.

---

## 6. Store Listing & Certification Checklist

Before submitting for certification, complete the listing requirements:

- **Pricing**: Free ($0.00). No trials or in-app purchases.
- **Languages**: English and Simplified Chinese (UI, narrative, and subtitle text).
- **Age Rating (IARC)**: Complete the IARC questionnaire based on fantasy combat and elemental effects.
- **Privacy Policy**: Provide a valid URL to `PRIVACY.md` hosted online (required by Microsoft Store policy 10.5).
- **Screenshots**: Upload actual packaged gameplay screenshots (from `docs/dev/evidence/rendered/`).
- **Certification Notes**: State that the game is offline-only single-player, supports keyboard and mouse, and does not require Xbox services.
