using UnrealBuildTool;

public class AlePCGModule: ModuleRules
{
    public AlePCGModule(ReadOnlyTargetRules Target) : base(Target)
    {
        PrivateDependencyModuleNames.AddRange(new string[] {"Core", "CoreUObject", "Engine"});
    }
}
