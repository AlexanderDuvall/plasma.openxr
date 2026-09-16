using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaBuildOpenXR
{
    public const string Version = "1.1.47";
    private const string PackageName = "OpenXR." + Version + ".nupkg";
    private const string DownloadUrl = "https://www.nuget.org/api/v2/package/OpenXR/" + Version;

    public static bool IsSupported(BuildContext context, out string disabledReason)
    {
        disabledReason = string.Empty;

        if (context.Platform != TargetPlatform.Windows)
        {
            disabledReason = "OpenXR plugin is currently only supported on Windows desktop.";
            return false;
        }

        if (context.Architecture == TargetArchitecture.X86 || context.Architecture == TargetArchitecture.ARM64)
        {
            disabledReason = "OpenXR plugin PlasmaBuild support is currently configured for Windows x64 only.";
            return false;
        }

        return true;
    }

    public static string GetOpenXRPath(BuildContext context)
    {
        return Path.Combine(context.ProjectRoot, "Intermediate", "PlasmaBuild", "ThirdParty", "OpenXR-" + Version);
    }

    public static void EnsureOpenXR(BuildContext context)
    {
        if (!IsSupported(context, out var disabledReason))
        {
            throw new InvalidOperationException(disabledReason);
        }

        var openXrPath = GetOpenXRPath(context);
        if (HasExpectedOpenXRFiles(openXrPath))
        {
            return;
        }

        var thirdPartyRoot = Path.Combine(context.ProjectRoot, "Intermediate", "PlasmaBuild", "ThirdParty");
        var archivePath = Path.Combine(thirdPartyRoot, PackageName);
        var extractMarker = archivePath + ".extracted";

        Directory.CreateDirectory(thirdPartyRoot);
        Directory.CreateDirectory(openXrPath);

        if (!File.Exists(archivePath))
        {
            DownloadFile(DownloadUrl, archivePath);
        }

        if (File.Exists(extractMarker) && !HasExpectedOpenXRFiles(openXrPath))
        {
            File.Delete(extractMarker);
        }

        if (!File.Exists(extractMarker))
        {
            ZipFile.ExtractToDirectory(archivePath, openXrPath, overwriteFiles: true);
            File.WriteAllText(extractMarker, string.Empty);
        }

        if (!HasExpectedOpenXRFiles(openXrPath))
        {
            throw new InvalidOperationException($"OpenXR extraction finished, but '{openXrPath}' does not contain the expected files.");
        }
    }

    private static bool HasExpectedOpenXRFiles(string openXrPath)
    {
        return File.Exists(Path.Combine(openXrPath, "include", "openxr", "openxr.h")) &&
            File.Exists(Path.Combine(openXrPath, "native", "x64", "lib", "openxr_loader.lib")) &&
            File.Exists(Path.Combine(openXrPath, "native", "x64", "bin", "openxr_loader.dll"));
    }

    private static void DownloadFile(string url, string targetPath)
    {
        using var httpClient = new HttpClient();
        using var response = httpClient.GetAsync(url, HttpCompletionOption.ResponseHeadersRead).GetAwaiter().GetResult();
        response.EnsureSuccessStatusCode();

        Directory.CreateDirectory(Path.GetDirectoryName(targetPath)!);
        using var source = response.Content.ReadAsStreamAsync().GetAwaiter().GetResult();
        using var target = File.Create(targetPath);
        source.CopyTo(target);
    }
}

public class OpenXRPluginModule : ModuleRules
{
    public OpenXRPluginModule(BuildContext context) : base(context)
    {
        PlasmaBuildOpenXR.EnsureOpenXR(context);
        var openXrPath = PlasmaBuildOpenXR.GetOpenXRPath(context);

        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/OpenXRPlugin", "OpenXRPluginPCH.h",
            "BUILDSYSTEM_BUILDING_OPENXRPLUGIN_LIB");

        // Vulkan and Direct3D 12 headers stay in the engine - the renderers need them - so they are read
        // out of the SDK rather than vendored here. The session binds to whichever renderer is active.
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Code", "ThirdParty", "Vulkan-Headers", "include"));
        PrivateLibraries.Add(Path.Combine(PlasmaPackageSdk.SdkBinaryDirectory(context), "plRendererVulkan.lib"));
        PublicIncludePaths.Add(FindAgilitySdkInclude());
        PrivateLibraries.Add(Path.Combine(PlasmaPackageSdk.SdkBinaryDirectory(context), "plRendererDX12.lib"));

        PublicIncludePaths.Add(Path.Combine(openXrPath, "include"));
        PublicLibraries.Add(Path.Combine(openXrPath, "native", "x64", "lib", "openxr_loader.lib"));
        PublicDefinitions.Add("XR_USE_GRAPHICS_API_VULKAN");

        // The OpenXR loader ships with the package: it is Apache-2.0, and the plugin cannot load
        // without it.
        StageLoader(context, openXrPath);
    }

    /// \brief The Agility SDK include directory the engine's RendererDX12 headers were built against.
    ///
    /// RendererDX12 uses interfaces (ID3D12Device10, ID3D12GraphicsCommandList7) that the Windows SDK copy of
    /// d3d12.h may lack, so the SDK's Agility headers must come first. The engine build fetches them.
    private static string FindAgilitySdkInclude()
    {
        var thirdParty = Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "ThirdParty");
        if (Directory.Exists(thirdParty))
        {
            foreach (var dir in Directory.GetDirectories(thirdParty, "AgilitySDK-*").OrderByDescending(d => d, StringComparer.OrdinalIgnoreCase))
            {
                var include = Path.Combine(dir, "build", "native", "include");
                if (File.Exists(Path.Combine(include, "d3d12.h")))
                {
                    return include;
                }
            }
        }

        throw new DirectoryNotFoundException(
            $"No Agility SDK headers under '{thirdParty}'. Build the engine's RendererDX12 once so it fetches them.");
    }

    /// rief Copies openxr_loader.dll and its licence beside the plugin.
    ///
    /// The engine does this through PlasmaBuildRuntimeFiles, which does not exist out here.
    private static void StageLoader(BuildContext context, string openXrPath)
    {
        var target = PlasmaPackageSdk.PackageBinaryDirectory(context);
        Copy(Path.Combine(openXrPath, "native", "x64", "bin", "openxr_loader.dll"),
            Path.Combine(target, "openxr_loader.dll"));

        // The NuGet package declares Apache-2.0 as an SPDX expression and carries no licence text,
        // so the package supplies it. Staged beside the DLL and listed in PackageDependencies, which
        // puts both in the same component - a component is what gets unpacked as a unit.
        Copy(Path.Combine(context.ProjectRoot, "ThirdParty", "OpenXR", "LICENSE.txt"),
            Path.Combine(target, "openxr-LICENSE.txt"));
    }

    private static void Copy(string source, string target)
    {
        if (!File.Exists(source))
        {
            throw new FileNotFoundException($"Missing OpenXR file '{source}'.", source);
        }

        if (File.Exists(target))
        {
            var from = new FileInfo(source);
            var to = new FileInfo(target);

            if (from.Length == to.Length && from.LastWriteTimeUtc <= to.LastWriteTimeUtc)
            {
                return;
            }
        }

        Directory.CreateDirectory(Path.GetDirectoryName(target));
        File.Copy(source, target, overwrite: true);
    }
}
