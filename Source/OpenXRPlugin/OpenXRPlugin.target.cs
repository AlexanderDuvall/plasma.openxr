using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class OpenXRPluginTarget : TargetRules
{
    public OpenXRPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plOpenXRPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;

        ExtraModules.Add("OpenXRPluginModule");
    }
}
