using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;

namespace Syncer.Tests;

[TestClass]
public sealed class CoreTests
{
    [TestMethod]
    public void EmbeddedServerSetIsCompleteWhenPresent()
    {
        var names = typeof(SyncerClient).Assembly.GetManifestResourceNames().Where(name => name.StartsWith("Syncer.Servers.", StringComparison.Ordinal)).Order().ToArray();
        if (Environment.GetEnvironmentVariable("SYNCER_REQUIRE_EMBEDDED_SERVERS") == "1") Assert.HasCount(4, names);
        if (names.Length != 0) CollectionAssert.AreEquivalent(new[] { "Syncer.Servers.arm64-v8a.syncerd", "Syncer.Servers.armeabi-v7a.syncerd", "Syncer.Servers.x86.syncerd", "Syncer.Servers.x86_64.syncerd" }, names);
    }

    [TestMethod]
    public void ManagedVersionAndProtocolMatchCMake()
    {
        var cmake = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "CMakeLists.txt"));
        Assert.AreEqual(ProtocolConstants.Version, Regex.Match(cmake, @"project\(syncer VERSION ([0-9.]+)").Groups[1].Value);
        Assert.AreEqual(ProtocolConstants.ProtocolVersion.ToString(), Regex.Match(cmake, @"SYNCER_PROTOCOL_VERSION=([0-9]+)").Groups[1].Value);
    }

    [TestMethod]
    public void AdbOutputParsingMatchesNativeSelectionRules()
    {
        CollectionAssert.AreEqual(new[] { "one", "three" }, AdbOutput.ReadyDevices("List of devices attached\none\tdevice\ntwo\toffline\nthree\tdevice product:x\n"));
        Assert.AreEqual("x86_64", AdbOutput.SelectAbi("riscv64,x86_64,x86")); Assert.IsNull(AdbOutput.SelectAbi("riscv64"));
        Assert.IsTrue(AdbOutput.TryParsePort("54321", out var port)); Assert.AreEqual(54321, port); Assert.IsFalse(AdbOutput.TryParsePort("70000", out _));
    }

    [TestMethod]
    public void AdbOutputEnumeratesAllStatesAndMetadata()
    {
        SyncerDevice[] devices = AdbOutput.Devices(
            "* daemon started successfully\n" +
            "List of devices attached\n" +
            "R5CT123\tdevice product:husky_beta model:Pixel_8_Pro device:husky transport_id:4 usb:1-2\n" +
            "emulator-5554\toffline product:sdk_phone model:Virtual_Device device:emu transport_id:7\n" +
            "192.0.2.4:5555\tunauthorized transport_id:9\n" +
            "USB123\tno permissions (missing udev rules)\n");

        Assert.HasCount(4, devices);
        Assert.AreEqual(new SyncerDevice(
            "Pixel_8_Pro|R5CT123", "R5CT123", "Pixel 8 Pro (R5CT123)", "device", "Pixel_8_Pro",
            "husky_beta", "husky", "4", "usb"), devices[0]);
        Assert.IsTrue(devices[0].IsReady);
        Assert.AreEqual("Virtual Device (emulator-5554)", devices[1].DisplayName);
        Assert.AreEqual("emulator", devices[1].Connection);
        Assert.IsFalse(devices[1].IsReady);
        Assert.AreEqual("network", devices[2].Connection);
        Assert.AreEqual("192.0.2.4:5555", devices[2].DisplayName);
        Assert.AreEqual("unknown|192.0.2.4:5555", devices[2].Id);
        Assert.AreEqual("192.0.2.4:5555", devices[2].Serial);
        Assert.AreEqual("no permissions", devices[3].State);
        Assert.AreEqual("unknown", devices[3].Connection);
    }

    [TestMethod]
    public void AdbOutputRejectsSpaceDelimitedDiagnostics()
    {
        SyncerDevice[] devices = AdbOutput.Devices(
            "List of devices attached\n" +
            "adb server version does not match this client\n" +
            "* daemon started successfully *\n" +
            "SERIAL123              device product:husky model:Pixel_8 transport_id:4\n");

        Assert.HasCount(1, devices);
        Assert.AreEqual("SERIAL123", devices[0].Serial);
        Assert.AreEqual("Pixel_8|SERIAL123", devices[0].Id);
    }

    [TestMethod]
    public void AdbOutputParsesCurrentWindowsFixedWidthColumns()
    {
        const string output =
            "List of devices attached\r\n" +
            "?                      device product:bengal_515 model:FIIO_M33_R2R device:bengal_515 transport_id:33\r\n" +
            "d                      device product:DX180 model:DX180 device:DX180 transport_id:32\r\n\r\n";

        SyncerDevice[] devices = AdbOutput.Devices(output);

        Assert.HasCount(2, devices);
        Assert.AreEqual("FIIO_M33_R2R|?", devices[0].Id);
        Assert.AreEqual("FIIO M33 R2R (?)", devices[0].DisplayName);
        Assert.AreEqual("DX180|d", devices[1].Id);
        Assert.IsTrue(devices.All(device => device.IsReady));
    }

    [TestMethod]
    public void AdbOutputUsesProductThenDeviceAsFriendlyNameFallback()
    {
        SyncerDevice[] devices = AdbOutput.Devices(
            "product-fallback\tdevice product:My_Tablet device:tablet\n" +
            "device-fallback\trecovery device:rescue_target\n");

        Assert.AreEqual("My Tablet (product-fallback)", devices[0].DisplayName);
        Assert.AreEqual("unknown|product-fallback", devices[0].Id);
        Assert.AreEqual("rescue target (device-fallback)", devices[1].DisplayName);
    }

    [TestMethod]
    public async Task DeviceEnumerationHonorsPreCancelledTokenBeforeResolvingAdb()
    {
        var client = new SyncerClient(new() { AdbPath = "an intentionally missing adb executable" });
        await Assert.ThrowsExactlyAsync<OperationCanceledException>(() =>
            client.EnumerateDevicesAsync(new CancellationToken(canceled: true)));
    }

    [TestMethod]
    public async Task ServerDirectoryOverrideAcceptsReleaseLayout()
    {
        var root = Path.Combine(Path.GetTempPath(), "syncer-servers-" + Guid.NewGuid().ToString("N")); var expected = Path.Combine(root, "servers", "x86_64", "syncerd");
        Directory.CreateDirectory(Path.GetDirectoryName(expected)!); await File.WriteAllTextAsync(expected, "server");
        try { Assert.AreEqual(Path.GetFullPath(expected), await new ServerBinaries(root).ResolveAsync("x86_64", default)); }
        finally { Directory.Delete(root, true); }
    }

    [TestMethod]
    public void Blake3MatchesOfficialVectors()
    {
        Assert.AreEqual("af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262", Blake3.Hex(new Blake3().FinalizeHash()));
        Assert.AreEqual("6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85", Blake3.Hex(Blake3.Hash(Encoding.UTF8.GetBytes("abc"))));
        var official = Enumerable.Range(0, 2048).Select(index => (byte)(index % 251)).ToArray();
        Assert.AreEqual("e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a", Blake3.Hex(Blake3.Hash(official)));
        var data = Enumerable.Range(0, 4097).Select(index => (byte)(index * 31)).ToArray(); var pieces = new Blake3();
        for (var index = 0; index < data.Length; index += 17) pieces.Update(data.AsSpan(index, Math.Min(17, data.Length - index)));
        CollectionAssert.AreEqual(Blake3.Hash(data), pieces.FinalizeHash());
    }

    [TestMethod]
    public void ProtocolEncodingMatchesNativeVectors()
    {
        var frame = new Frame(MessageType.Scan, 42, [1, 2, 3]);
        Assert.AreEqual(Vector("header_scan_request_42_payload_010203"), Convert.ToHexString(ProtocolCodec.EncodeHeader(frame)).ToLowerInvariant());
        var writer = new ProtocolWriter(); ProtocolCodec.WriteManifestEntry(writer, new("folder/file.bin", EntryType.File, 0x100000002, -123));
        Assert.AreEqual(Vector("manifest_folder_file"), Convert.ToHexString(writer.ToArray()).ToLowerInvariant());
        var decoded = ProtocolCodec.DecodeHeader(ProtocolCodec.EncodeHeader(frame)); Assert.AreEqual(MessageType.Scan, decoded.Type); Assert.AreEqual((uint)42, decoded.RequestId); Assert.AreEqual(3, decoded.Payload.Length);
    }

    [TestMethod]
    public void ProtocolRejectsMalformedData()
    {
        var header = ProtocolCodec.EncodeHeader(new(MessageType.Scan, 1, [])); header[0] ^= 0xff;
        Assert.ThrowsExactly<SyncerException>(() => ProtocolCodec.DecodeHeader(header));
        var invalid = ProtocolCodec.EncodeHeader(new(MessageType.Scan, 1, [])); invalid[12] = 0x7f;
        Assert.ThrowsExactly<SyncerException>(() => ProtocolCodec.DecodeHeader(invalid));
        try { var reader = new ProtocolReader([0, 0, 0]); _ = reader.ReadUInt32(); Assert.Fail("Expected a protocol exception."); }
        catch (SyncerException) { }
    }

    [TestMethod]
    public void GlobsAndPlansMatchNativeBehavior()
    {
        Assert.IsTrue(GlobSet.GlobMatch("**/*.txt", "readme.txt")); Assert.IsTrue(GlobSet.GlobMatch("**/*.txt", "docs/readme.txt"));
        Assert.IsFalse(GlobSet.GlobMatch("*.txt", "docs/readme.txt")); Assert.IsFalse(GlobSet.GlobMatch("README", "readme"));
        Assert.IsFalse(GlobSet.GlobMatch("?.txt", "α.txt"));
        var globs = new GlobSet(["build", "**/*.tmp"]); Assert.IsTrue(globs.Matches("build/deep/a.bin")); Assert.IsTrue(globs.Matches("a/b/cache.tmp"));
        var source = new Dictionary<string, ManifestEntry>(StringComparer.Ordinal)
        {
            ["dir"] = new("dir", EntryType.Directory, 0, 0), ["dir/new.txt"] = new("dir/new.txt", EntryType.File, 20, 11), ["same.bin"] = new("same.bin", EntryType.File, 7, 12),
        };
        var destination = new Dictionary<string, ManifestEntry>(StringComparer.Ordinal)
        {
            ["old.txt"] = new("old.txt", EntryType.File, 2, 1), ["same.bin"] = new("same.bin", EntryType.File, 7, 12),
        };
        var plan = Planner.MakePlan(source, destination, new GlobSet([]), true, 60);
        Assert.AreEqual(1, plan.Directories.Count); Assert.AreEqual(1, plan.Files.Count); Assert.AreEqual(1, plan.Deletions.Count); Assert.AreEqual((ulong)20, plan.TransferBytes);
        Assert.AreEqual(PlanAction.CreateDirectory, plan.Directories[0].Action); Assert.AreEqual(PlanAction.AddFile, plan.Files[0].Action); Assert.AreEqual(PlanAction.DeleteFile, plan.Deletions[0].Action);
        var unicode = Planner.MakePlan(new Dictionary<string, ManifestEntry>
        {
            ["\U00010000.txt"] = new("\U00010000.txt", EntryType.File, 1, 0), ["\uE000.txt"] = new("\uE000.txt", EntryType.File, 1, 0),
        }, new Dictionary<string, ManifestEntry>(), new GlobSet([]), true, 60);
        Assert.AreEqual("\uE000.txt", unicode.Files[0].Path); Assert.AreEqual("\U00010000.txt", unicode.Files[1].Path);
    }

    [TestMethod]
    [DataRow("a/b.txt", true)] [DataRow("../a", false)] [DataRow("/a", false)] [DataRow("a\\b", false)]
    [DataRow("", false)] [DataRow("a/", false)] [DataRow("a//b", false)] [DataRow("a/./b", false)]
    public void RelativePathValidation(string path, bool expected) => Assert.AreEqual(expected, Planner.IsValidRelativePath(path));

    [TestMethod]
    public async Task SourceScannerPreservesUnicodeAndExclusions()
    {
        var root = Path.Combine(Path.GetTempPath(), "syncer-managed-tests-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(Path.Combine(root, "nested"));
        try
        {
            await File.WriteAllTextAsync(Path.Combine(root, "nested", "file.txt"), "data"); await File.WriteAllTextAsync(Path.Combine(root, "unicode-α.txt"), "unicode"); await File.WriteAllTextAsync(Path.Combine(root, "skip.tmp"), "skip");
            var tree = await SourceScanner.ScanAsync(root, new GlobSet(["**/*.tmp"]), default);
            Assert.IsTrue(tree.Manifest.ContainsKey("nested")); Assert.IsTrue(tree.Manifest.ContainsKey("nested/file.txt")); Assert.IsTrue(tree.Manifest.ContainsKey("unicode-α.txt")); Assert.IsFalse(tree.Manifest.ContainsKey("skip.tmp"));
        }
        finally { Directory.Delete(root, true); }
    }

    [TestMethod]
    public void SourceScannerLeavesWindowsUncReparsePointsInTheNetworkNamespace()
    {
        if (!OperatingSystem.IsWindows()) return;
        Assert.IsFalse(SourceScanner.ShouldResolveReparsePoint(@"\\server\share\folder"));
        Assert.IsFalse(SourceScanner.ShouldResolveReparsePoint(@"\\?\UNC\server\share\folder"));
        Assert.IsFalse(SourceScanner.ShouldResolveReparsePoint(@"\\server\share\folder\link"));
        Assert.IsTrue(SourceScanner.ShouldResolveReparsePoint(@"C:\source-link"));
    }

    [TestMethod]
    public async Task SourceScannerRejectsReservedStateDirectory()
    {
        var root = Path.Combine(Path.GetTempPath(), "syncer-reserved-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(Path.Combine(root, ".syncer"));
        try { var error = await Assert.ThrowsExactlyAsync<SyncerException>(() => SourceScanner.ScanAsync(root, new GlobSet([]), default)); Assert.AreEqual(SyncerErrorCode.Safety, error.ErrorCode); }
        finally { Directory.Delete(root, true); }
    }

    [TestMethod]
    public async Task ChangeProgressReportsTerminalCompletionAndFailure()
    {
        var change = new PlannedChange(new("file.flac", EntryType.File, 10, 1),
            PlanAction.AddFile, "missing");
        var completed = new RecordingProgress<SyncerProgress>();
        await SyncerClient.RunChangeAsync(change, SyncerProgressStage.Applying, true,
            completed, () => Task.CompletedTask);
        CollectionAssert.AreEqual(
            new[] { SyncerProgressStage.Applying, SyncerProgressStage.CompletedChange },
            completed.Values.Select(value => value.Stage).ToArray());

        var failed = new RecordingProgress<SyncerProgress>();
        await Assert.ThrowsExactlyAsync<IOException>(() => SyncerClient.RunChangeAsync(
            change, SyncerProgressStage.Applying, true, failed,
            () => Task.FromException(new IOException("failed"))));
        CollectionAssert.AreEqual(
            new[] { SyncerProgressStage.Applying, SyncerProgressStage.FailedChange },
            failed.Values.Select(value => value.Stage).ToArray());
    }

    [TestMethod]
    public async Task SavedPlanRoundTripsWithStableDigest()
    {
        var entry = new ManifestEntry("a.txt", EntryType.File, 3, 42); var options = new PushOptions { Exclusions = ["**/*.tmp"], Direct = true, ModificationTimeTolerance = TimeSpan.Zero };
        var parts = new PlanParts([], [new(entry, PlanAction.AddFile, "destination path is missing")], [], 3);
        var sourcePath = Path.Combine(Path.GetTempPath(), "source-a.txt"); var sources = new Dictionary<string, SourceFile> { ["a.txt"] = new(entry, sourcePath) };
        var plan = SavedPlans.Create("source", "/sdcard/target", "serial", options, parts, 0, sources, [new("a.txt", false, null)]);
        Assert.AreEqual(Vector("plan_digest_reference"), plan.Digest);
        var path = Path.Combine(Path.GetTempPath(), "syncer-plan-" + Guid.NewGuid().ToString("N") + ".json");
        try
        {
            await SavedPlans.SaveAsync(plan, path, default); var loaded = await SavedPlans.LoadAsync(path, default);
            Assert.AreEqual(plan.Digest, loaded.Digest); Assert.AreEqual(plan.Files[0], loaded.Files[0]); Assert.AreEqual(sourcePath, loaded.SourceFiles["a.txt"].PhysicalPath);
        }
        finally { File.Delete(path); }
    }

    [TestMethod]
    public async Task LoadsNativeProducedPlanFixture()
    {
        var plan = await SavedPlans.LoadAsync(Path.Combine(AppContext.BaseDirectory, "native-plan-v1.json"), default);
        Assert.AreEqual(Vector("plan_digest_reference"), plan.Digest); Assert.AreEqual(PlanAction.AddFile, plan.Files.Single().Action);
        Assert.AreEqual("source-a.txt", plan.SourceFiles["a.txt"].PhysicalPath);
    }

    [TestMethod]
    public async Task SessionStreamsAndVerifiesStagedAndDirectUploads()
    {
        await VerifyUploadAsync(false); await VerifyUploadAsync(true);
    }

    private static async Task VerifyUploadAsync(bool direct)
    {
        var path = Path.Combine(Path.GetTempPath(), "syncer-upload-" + Guid.NewGuid().ToString("N")); var contents = Encoding.UTF8.GetBytes("managed upload contents");
        await File.WriteAllBytesAsync(path, contents); var info = new FileInfo(path); var entry = new ManifestEntry("folder/file.bin", EntryType.File, (ulong)info.Length, new DateTimeOffset(info.LastWriteTimeUtc).ToUnixTimeSeconds());
        var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start(); var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        var peer = Task.Run(async () =>
        {
            using var socket = await listener.AcceptTcpClientAsync(); await using var stream = socket.GetStream();
            var hello = await ProtocolCodec.ReceiveAsync(stream, default); var ack = new ProtocolWriter(); ack.WriteString(ProtocolConstants.Version); await ProtocolCodec.SendAsync(stream, new(MessageType.HelloAck, hello.RequestId, ack.ToArray()), default);
            var begin = await ProtocolCodec.ReceiveAsync(stream, default); var reader = new ProtocolReader(begin.Payload); Assert.AreEqual(entry.Path, reader.ReadString()); Assert.AreEqual(entry.Size, reader.ReadUInt64()); Assert.AreEqual(entry.ModifiedSeconds, reader.ReadInt64()); Assert.AreEqual(direct ? (byte)1 : (byte)0, reader.ReadByte()); reader.RequireEnd();
            await ProtocolCodec.SendAsync(stream, new(MessageType.OperationOk, begin.RequestId, []), default); using var received = new MemoryStream(); Frame frame;
            while ((frame = await ProtocolCodec.ReceiveAsync(stream, default)).Type == MessageType.PutChunk) received.Write(frame.Payload);
            Assert.AreEqual(MessageType.PutEnd, frame.Type); CollectionAssert.AreEqual(contents, received.ToArray()); CollectionAssert.AreEqual(Blake3.Hash(contents), frame.Payload);
            await ProtocolCodec.SendAsync(stream, new(MessageType.OperationOk, frame.RequestId, []), default);
        });
        try { await using var session = await ClientSession.ConnectAsync(port, "token", default); await session.UploadAsync(new(entry, path), direct, null, default); await peer; }
        finally { listener.Stop(); File.Delete(path); }
    }

    [TestMethod]
    public async Task SessionHandlesHandshakeAndManifestBatch()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start(); var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        var peer = Task.Run(async () =>
        {
            using var socket = await listener.AcceptTcpClientAsync(); await using var stream = socket.GetStream();
            var hello = await ProtocolCodec.ReceiveAsync(stream, default); var ack = new ProtocolWriter(); ack.WriteString(ProtocolConstants.Version);
            await ProtocolCodec.SendAsync(stream, new(MessageType.HelloAck, hello.RequestId, ack.ToArray()), default);
            var scan = await ProtocolCodec.ReceiveAsync(stream, default); var batch = new ProtocolWriter(); batch.WriteUInt32(1); ProtocolCodec.WriteManifestEntry(batch, new("file.txt", EntryType.File, 7, 9));
            await ProtocolCodec.SendAsync(stream, new(MessageType.ManifestBatch, scan.RequestId, batch.ToArray()), default);
            await ProtocolCodec.SendAsync(stream, new(MessageType.ManifestEnd, scan.RequestId, []), default);
        });
        try
        {
            await using var session = await ClientSession.ConnectAsync(port, "token", default); var manifest = await session.ScanAsync("/sdcard/test", default);
            Assert.AreEqual((ulong)7, manifest["file.txt"].Size); await peer;
        }
        finally { listener.Stop(); }
    }

    [TestMethod]
    public async Task SessionPropagatesServerErrorCodes()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start(); var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        var peer = Task.Run(async () =>
        {
            using var socket = await listener.AcceptTcpClientAsync(); await using var stream = socket.GetStream();
            var hello = await ProtocolCodec.ReceiveAsync(stream, default); var ack = new ProtocolWriter(); ack.WriteString(ProtocolConstants.Version); await ProtocolCodec.SendAsync(stream, new(MessageType.HelloAck, hello.RequestId, ack.ToArray()), default);
            var request = await ProtocolCodec.ReceiveAsync(stream, default); var error = new ProtocolWriter(); error.WriteUInt32((uint)SyncerErrorCode.Safety); error.WriteString("unsafe destination");
            await ProtocolCodec.SendAsync(stream, new(MessageType.Error, request.RequestId, error.ToArray()), default);
        });
        try
        {
            await using var session = await ClientSession.ConnectAsync(port, "token", default);
            var error = await Assert.ThrowsExactlyAsync<SyncerException>(() => session.InitializeAsync("/sdcard", false, default));
            Assert.AreEqual(SyncerErrorCode.Safety, error.ErrorCode); Assert.AreEqual("unsafe destination", error.Message); await peer;
        }
        finally { listener.Stop(); }
    }

    private static string Vector(string name) => File.ReadLines(Path.Combine(AppContext.BaseDirectory, "protocol-v6-vectors.txt"))
        .Where(line => !line.StartsWith('#') && line.Contains('='))
        .Select(line => line.Split('=', 2)).Single(parts => parts[0] == name)[1];

    private sealed class RecordingProgress<T> : IProgress<T>
    {
        public List<T> Values { get; } = [];
        public void Report(T value) => Values.Add(value);
    }
}
