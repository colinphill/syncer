# Syncer.Client

`Syncer.Client` is the managed, asynchronous client for syncer protocol 6. It initializes and
mirrors host directories into managed Android shared-storage destinations over ADB.

```csharp
var client = new SyncerClient();
await client.InitializeAsync("/sdcard/MyMirror");
var result = await client.PushAsync("assets", "/sdcard/MyMirror");
```

The package embeds the matching Android server for all supported ABIs. Set
`SyncerClientOptions.ServerDirectory` to override those packaged binaries.
