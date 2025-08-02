using UnrealBuildTool;

public class AlePCGModule : ModuleRules
{
    public AlePCGModule(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] {
    "Core", "CoreUObject", "Engine", "InputCore", "Landscape", "PCG", "RHI", "RenderCore"
        });
    }
}
