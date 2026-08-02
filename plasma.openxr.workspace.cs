// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaOpenXRWorkspace : WorkspaceRules
{
    public PlasmaOpenXRWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("OpenXRPlugin");
    }
}
