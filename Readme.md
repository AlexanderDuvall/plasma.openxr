# plasma.openxr

Virtual and mixed reality for PlasmaEngine through [OpenXR](https://www.khronos.org/openxr/),
rendering via Direct3D 12 or Vulkan - the session binds to whichever renderer the engine runs. Moved
out of the engine tree to be built and distributed as a package.

Published as [`plasma.openxr`](https://plasmaengine.github.io/PlasmaPackages/index.plPackageIndex) in
the Plasma package registry.

## Layout

```
Package.plPackage      the manifest - identity, compatibility, components
OpenXR.plPluginBundle  legacy descriptor, kept until the engine reads manifests directly
Source/
  OpenXRPlugin/        runtime -> plOpenXRPlugin
ThirdParty/OpenXR/     the Apache-2.0 text that ships with the loader
Configs/               build rules for building against an installed engine SDK
Branding/              icon and banner
```

Runtime only: there is no editor plugin and no editor-engine-process plugin, so this package needs
no Qt and installs nothing into the editor UI.

## What you still need

The OpenXR **loader** ships with the package. An OpenXR **runtime** does not — that comes from the
headset vendor (SteamVR, Oculus, Windows Mixed Reality, and so on) and has to be installed and set
as the active runtime on the machine.

## Dependencies

None resolvable through the registry. It binds to GameEngine, RendererVulkan and RendererDX12, all
part of the engine rather than packages, so the Vulkan headers and the Agility SDK's D3D12 headers are
read out of the SDK rather than vendored here. The Agility headers exist only once the SDK's
RendererDX12 has been built.

## Building

Requires an engine SDK — a built engine checkout — named by `PLASMA_SDK_ROOT`:

```bash
PLASMA_SDK_ROOT=/path/to/engine plasmabuild build OpenXRPlugin
```

The build downloads the OpenXR SDK from NuGet on first run and extracts it under `Intermediate/`.
Output lands in `Bin/Windows_x64_Development/`.

The NuGet package declares Apache-2.0 as an SPDX expression and carries no licence text, so the
licence shipped alongside `openxr_loader.dll` comes from `ThirdParty/OpenXR/LICENSE.txt` in this
repository.

## Artwork

The icon and banner are the OpenXR mark, a Khronos trademark, used to identify an OpenXR
integration. Both already shipped in the engine under `Data/Plugins/Editor/Plugins`, but the bundle
had no `Icon`/`Banner` fields, so nothing ever displayed them. The icon is 48×48 — below the
128×128 the packaging notes ask for, adequate at the 36 px size the Plugin Manager list draws but
soft anywhere larger.

## Publishing a new version

Staging and publishing are driven from the engine and registry repositories:

```bash
# engine repo - stage and validate, emitting machine-readable output
python Utilities/PackageInspector.py <staged-package-dir> --json <out>.json

# registry repo - upload blobs and record the release
python tools/publish_package.py <out>.json
```

Versions are immutable. A mistake is fixed by publishing a new patch version, never by replacing an
existing one — every lock file that already trusts a version must keep resolving to the same bytes.
