namespace Syncer.ToolTests;

[TestClass]
[DoNotParallelize]
public sealed class ToolTests
{
    [TestMethod]
    public void VersionCommandReportsManagedIdentity()
    {
        var result = Capture(["version"]);
        Assert.AreEqual(0, result.ExitCode); Assert.AreEqual("syncer-managed 0.2.0 (protocol 6)", result.Output.Trim()); Assert.AreEqual(string.Empty, result.Error);
    }

    [TestMethod]
    public void ParseFailureUsesUsageExitCode()
    {
        var result = Capture(["unknown"]); Assert.AreEqual(2, result.ExitCode); StringAssert.Contains(result.Error, "syncer-managed:");
    }

    [TestMethod]
    [DataRow("init")]
    [DataRow("push")]
    [DataRow("apply")]
    [DataRow("restore")]
    public void EveryOperationalCommandValidatesRequiredInputs(string command)
    {
        var result = Capture([command]); Assert.AreEqual(2, result.ExitCode);
    }

    [TestMethod]
    public void ParseFailureHonorsJsonMode()
    {
        var result = Capture(["push", "--json"]); Assert.AreEqual(2, result.ExitCode);
        StringAssert.Contains(result.Output, "\"status\":\"error\""); Assert.AreEqual(string.Empty, result.Error);
    }

    [TestMethod]
    public void JsonOperationFailureUsesNativeErrorSchema()
    {
        var result = Capture(["push", "--json", "--mtime-tolerance", "-1", "source", "destination"]);
        Assert.AreEqual(2, result.ExitCode); StringAssert.Contains(result.Output, "\"status\":\"error\""); StringAssert.Contains(result.Output, "\"code\":2");
    }

    private static Captured Capture(string[] arguments)
    {
        var oldOut = Console.Out; var oldError = Console.Error; using var output = new StringWriter(); using var error = new StringWriter();
        try { Console.SetOut(output); Console.SetError(error); return new(Tool.Run(arguments), output.ToString(), error.ToString()); }
        finally { Console.SetOut(oldOut); Console.SetError(oldError); }
    }
    private sealed record Captured(int ExitCode, string Output, string Error);
}
