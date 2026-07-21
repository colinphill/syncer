namespace Syncer.ToolTests;

using System.Text.Json.Nodes;
using Syncer;

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

    [TestMethod]
    public void DeviceJsonUsesCrossClientEnumerationSchema()
    {
        JsonObject json = Tool.DevicesJson([
            new SyncerDevice("Pixel_8|serial", "serial", "Pixel 8 (serial)", "device", "Pixel_8", "husky", "husky", "2", "usb"),
            new SyncerDevice("unknown|offline", "offline", "offline", "offline", Connection: "unknown"),
        ]);

        Assert.AreEqual("success", json["status"]!.GetValue<string>());
        JsonArray devices = json["devices"]!.AsArray();
        Assert.HasCount(2, devices);
        Assert.AreEqual("Pixel_8|serial", devices[0]!["id"]!.GetValue<string>());
        Assert.AreEqual("serial", devices[0]!["serial"]!.GetValue<string>());
        Assert.IsTrue(devices[0]!["ready"]!.GetValue<bool>());
        Assert.AreEqual("Pixel_8", devices[0]!["model"]!.GetValue<string>());
        Assert.IsFalse(devices[1]!["ready"]!.GetValue<bool>());
        Assert.AreEqual("unknown", devices[1]!["connection"]!.GetValue<string>());
    }

    [TestMethod]
    public void DeviceErrorJsonKeepsDevicesArrayStable()
    {
        JsonObject json = Tool.DevicesErrorJson(3, "adb unavailable");
        Assert.AreEqual("error", json["status"]!.GetValue<string>());
        Assert.IsEmpty(json["devices"]!.AsArray());
        Assert.AreEqual(3, json["error"]!["code"]!.GetValue<int>());
    }

    [TestMethod]
    public void DevicesCommandUsesEnumerationErrorSchema()
    {
        string missingAdb = Path.Combine(Path.GetTempPath(),
            "missing-syncer-adb-" + Guid.NewGuid().ToString("N"));
        Captured result = Capture(["devices", "--json", "--adb", missingAdb]);

        Assert.AreEqual((int)SyncerErrorCode.Adb, result.ExitCode);
        JsonObject json = JsonNode.Parse(result.Output)!.AsObject();
        Assert.AreEqual("error", json["status"]!.GetValue<string>());
        Assert.IsEmpty(json["devices"]!.AsArray());
        Assert.AreEqual((int)SyncerErrorCode.Adb,
            json["error"]!["code"]!.GetValue<int>());
        Assert.AreEqual(string.Empty, result.Error);
    }

    [TestMethod]
    public void DevicesParseFailureUsesEnumerationErrorSchema()
    {
        Captured result = Capture(["devices", "--json", "--not-a-real-option"]);

        Assert.AreEqual((int)SyncerErrorCode.Usage, result.ExitCode);
        JsonObject json = JsonNode.Parse(result.Output)!.AsObject();
        Assert.AreEqual("error", json["status"]!.GetValue<string>());
        Assert.IsEmpty(json["devices"]!.AsArray());
        Assert.AreEqual((int)SyncerErrorCode.Usage,
            json["error"]!["code"]!.GetValue<int>());
        Assert.IsNull(json["device"]);
        Assert.AreEqual(string.Empty, result.Error);
    }

    private static Captured Capture(string[] arguments)
    {
        var oldOut = Console.Out; var oldError = Console.Error; using var output = new StringWriter(); using var error = new StringWriter();
        try { Console.SetOut(output); Console.SetError(error); return new(Tool.Run(arguments), output.ToString(), error.ToString()); }
        finally { Console.SetOut(oldOut); Console.SetError(oldError); }
    }
    private sealed record Captured(int ExitCode, string Output, string Error);
}
