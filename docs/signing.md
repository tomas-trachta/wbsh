# Code signing

Releases are unsigned today; every release carries `SHA256SUMS.txt` instead.
The release workflow already has a signing step that switches itself on once
the secrets below exist. It uses [SignPath Foundation](https://signpath.org),
which signs open-source projects for free with its own certificate.

## What the workflow does

In `.github/workflows/release.yml`, after the installers and ZIPs are built
and before the checksums and package manifests are written:

1. **Check for code signing** turns the next steps on only when both
   `SIGNPATH_API_TOKEN` and `SIGNPATH_ORGANIZATION_ID` are set. Without them
   the log says "the release ships unsigned" and the release goes on as before.
2. **Upload the files to sign** uploads `installer/output/*.exe` and
   `*.zip` (both architectures) as a workflow artifact.
3. **Sign with SignPath** (`signpath/github-action-submit-signing-request@v1`)
   submits that artifact and waits for the signed copy.
4. **Use the signed files** copies the signed files over the unsigned ones,
   so `SHA256SUMS.txt`, the winget and Scoop manifests and the GitHub release
   all describe the signed files.

## What Tomas has to do

1. **Apply to SignPath Foundation** at <https://signpath.org/apply> with the
   repository URL. Approval takes weeks, so apply early. They check that the
   project is open source (MIT here), actively maintained, and that releases
   are built by CI from the public repository, which the release workflow
   already does on GitHub-hosted runners.
2. **Set up the SignPath project** once approved, in the SignPath web app:
   - connect GitHub as the trusted build system for the repository;
   - a project with slug `wbsh`;
   - an artifact configuration with slug `release` that signs, inside the
     uploaded ZIP artifact, each `wbsh-setup-*.exe` / `wbshterm-setup-*.exe`
     and every `.exe` and `.dll` inside the `*-portable-*.zip` files (the SDK
     and skill ZIPs hold no executables);
   - a signing policy with slug `release-signing`.

   Other slugs work too: set them as repository **variables**
   `SIGNPATH_PROJECT_SLUG`, `SIGNPATH_POLICY_SLUG` and
   `SIGNPATH_ARTIFACT_CONFIGURATION_SLUG`.
3. **Add two repository secrets** (Settings > Secrets and variables > Actions):

   | Secret | Value |
   | --- | --- |
   | `SIGNPATH_API_TOKEN` | API token of a SignPath CI user that may submit signing requests for the project |
   | `SIGNPATH_ORGANIZATION_ID` | the organization ID shown in the SignPath web app |

4. **Publish the code signing policy** SignPath Foundation's terms ask open
   source projects for (who may approve signing, which files are signed, and
   a credit to SignPath); this page can grow into it.
5. Run the release workflow. The signed files' publisher shows as
   *SignPath Foundation*.

## Not covered yet

- The programs **inside** the Inno Setup installers are packed before the
  signing step, so only the installer itself is signed; what it installs is
  not. The portable ZIPs are signed inside. Signing the payload too needs a
  second round: sign the staged binaries (`installer/stage*`) before ISCC
  runs, then sign the installers.
- The bundled `conpty.dll` and `OpenConsole.exe` are Microsoft's and already
  carry Microsoft's signature; the artifact configuration should leave them
  as they are.
