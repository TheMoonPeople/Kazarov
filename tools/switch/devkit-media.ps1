<#
.SYNOPSIS
Uses the SDK's Target Manager 2 client from the command line for media capture.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SdkRoot,
    [Parameter(Mandatory = $true)][string]$Target,
    [ValidateSet('Status', 'Record', 'Screenshot')][string]$Action = 'Status',
    [ValidateRange(1, 10)][int]$Seconds = 8,
    [string]$OutputDirectory = '',
    [string]$FileName = '',
    [string]$StartLogPath = '',
    [string]$StartPattern = '',
    [ValidateRange(1, 600)][int]$StartTimeoutSeconds = 240
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($StartLogPath) -ne [string]::IsNullOrWhiteSpace($StartPattern)) {
    throw '-StartLogPath and -StartPattern must be supplied together.'
}
$client_directory = Join-Path $SdkRoot 'Resources/Firmwares/NX/NintendoTargetManager2'
$client_library = Join-Path $client_directory 'Nintendo.Htc.Rpc.Client.dll'
if (-not (Test-Path -LiteralPath $client_library -PathType Leaf)) {
    throw "Target Manager 2 client was not found: $client_library"
}
if ($Action -ne 'Status') {
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        throw 'Media capture requires -OutputDirectory.'
    }
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    [IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
    if ([string]::IsNullOrWhiteSpace($FileName)) {
        $FileName = '{0}-{1}' -f $Target, [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
    }
    if ([IO.Path]::GetFileName($FileName) -ne $FileName) {
        throw '-FileName must be a filename without directory components.'
    }
    foreach ($suffix in @('', '.mp4', '.mkv', '.png', '.jpg')) {
        $candidate = Join-Path $OutputDirectory ($FileName + $suffix)
        if (Test-Path -LiteralPath $candidate) {
            throw "Capture output already exists: $candidate"
        }
    }
}

if ($Action -eq 'Screenshot') {
    $capture_path = Join-Path $OutputDirectory ($FileName + '.png')
    if (Test-Path -LiteralPath $capture_path) {
        throw "Capture output already exists: $capture_path"
    }
    $control = Join-Path $SdkRoot 'Tools/CommandLineTools/ControlTarget.exe'
    & $control take-screenshot --target $Target --full-path $capture_path | Out-Null
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $capture_path -PathType Leaf)) {
        throw 'The SDK Target Manager screenshot command failed.'
    }
    @{kind='screenshot';target=$Target;path=$capture_path;backend='SDK ControlTarget / Target Manager 2'} | ConvertTo-Json -Compress
    return
}

$source = @'
using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;

internal static class Program
{
    private static void Write(object value)
    {
        var fields = JsonSerializer.Deserialize<Dictionary<string, JsonElement>>(JsonSerializer.Serialize(value))!;
        fields["host_time_utc"] = JsonSerializer.SerializeToElement(DateTimeOffset.UtcNow);
        Console.WriteLine(JsonSerializer.Serialize(fields));
        Console.Out.Flush();
    }

    private static int Run(string[] args)
    {
        string client_directory = Path.GetFullPath(args[0]);
        string target_id = args[1];
        string action = args[2];
        int seconds = int.Parse(args[3]);
        string output_directory = args.Length > 4 ? args[4] : "";
        string file_name = args.Length > 5 ? args[5] : "";
        string start_log = args.Length > 6 ? args[6] : "";
        string start_pattern = args.Length > 7 ? args[7] : "";
        int start_timeout_seconds = args.Length > 8 ? int.Parse(args[8]) : 240;
        AssemblyLoadContext.Default.Resolving += (context, name) =>
        {
            string path = Path.Combine(client_directory, name.Name + ".dll");
            return File.Exists(path) ? context.LoadFromAssemblyPath(path) : null;
        };
        Assembly assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(
            Path.Combine(client_directory, "Nintendo.Htc.Rpc.Client.dll"));
        Type client_type = assembly.GetType("Nintendo.Htc.Rpc.Client.HtcRpcClient", true)!;
        dynamic client = Activator.CreateInstance(client_type)!;
        using IDisposable client_lifetime = (IDisposable)client;
        dynamic handle = client.GetTargetFromUniqueIdentifier(target_id);
        dynamic info = client.GetTargetInfo(handle);
        string actual_id = info.UniqueIdentifier;
        if (!StringComparer.Ordinal.Equals(target_id, actual_id))
            throw new InvalidOperationException("Target Manager returned a different target identifier.");
        string status = client.GetTargetStatus(handle).ToString();
        dynamic recording = client.GetTargetVideoRecordingState(handle);
        string recording_state = recording.State.ToString();
        Write(new {
            kind = "status", target = actual_id, name = (string)info.Name, status,
            firmware = (string)info.FirmwareVersion, recording_state,
            recording_seconds = ((TimeSpan)recording.RecordingTime).TotalSeconds,
            client = assembly.Location, client_version = assembly.GetName().Version?.ToString()
        });
        if (action == "Status") return 0;
        if (status != "Connected")
            throw new InvalidOperationException("The selected target is not connected.");
        if (recording_state != "NotStarted")
            throw new InvalidOperationException("Target Manager already has an active or unknown recording state.");
        if (start_pattern.Length != 0)
        {
            var pattern = new System.Text.RegularExpressions.Regex(start_pattern,
                System.Text.RegularExpressions.RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
            Write(new { kind = "recording_waiting", target = actual_id, log = start_log, pattern = start_pattern });
            var waiting = System.Diagnostics.Stopwatch.StartNew();
            bool matched = false;
            while (waiting.Elapsed < TimeSpan.FromSeconds(start_timeout_seconds))
            {
                try
                {
                    using var input = new FileStream(start_log, FileMode.Open, FileAccess.Read,
                        FileShare.ReadWrite | FileShare.Delete);
                    using var reader = new StreamReader(input);
                    if (pattern.IsMatch(reader.ReadToEnd())) { matched = true; break; }
                }
                catch (IOException) { }
                Thread.Sleep(100);
            }
            if (!matched) throw new TimeoutException("The recording start condition was not observed.");
            recording = client.GetTargetVideoRecordingState(handle);
            if (recording.State.ToString() != "NotStarted")
                throw new InvalidOperationException("Another recording started while waiting for the scene.");
        }
        string output_path;
        client.StartTargetVideoRecording(out output_path, handle, output_directory,
            file_name, seconds, 10);
        Write(new { kind = "recording_started", target = actual_id, path = output_path, seconds });
        bool finished = false;
        try
        {
            var deadline = System.Diagnostics.Stopwatch.StartNew();
            while (deadline.Elapsed < TimeSpan.FromSeconds(seconds + 12))
            {
                Thread.Sleep(200);
                recording = client.GetTargetVideoRecordingState(handle);
                recording_state = recording.State.ToString();
                if (recording_state == "NotStarted")
                {
                    finished = true;
                    break;
                }
            }
            if (!finished)
                throw new TimeoutException("Target Manager did not finish its bounded recording.");
            string exit_reason = client.GetTargetLastVideoRecordingExitReason(handle).ToString();
            Write(new {
                kind = "recording_completed", target = actual_id, path = output_path,
                exit_reason, exists = File.Exists(output_path),
                bytes = File.Exists(output_path) ? new FileInfo(output_path).Length : 0
            });
            return File.Exists(output_path) && new FileInfo(output_path).Length > 0 ? 0 : 2;
        }
        finally
        {
            if (!finished) client.StopTargetVideoRecording(handle);
        }
    }

    public static async Task<int> Main(string[] args)
    {
        try
        {
            Task<int> operation = Task.Run(() => Run(args));
            int timeout_seconds = args.Length > 3 ? int.Parse(args[3]) + 30 : 40;
            if (args.Length > 8 && args[6].Length != 0) timeout_seconds += int.Parse(args[8]);
            if (await Task.WhenAny(operation, Task.Delay(TimeSpan.FromSeconds(timeout_seconds))) != operation)
                throw new TimeoutException("Target Manager media command timed out.");
            return await operation;
        }
        catch (Exception exception)
        {
            while (exception is TargetInvocationException && exception.InnerException != null)
                exception = exception.InnerException;
            Write(new { kind = "error", error = exception.GetType().Name, message = exception.Message });
            return 1;
        }
    }
}
'@

$project = @'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0-windows</TargetFramework>
    <UseWindowsForms>true</UseWindowsForms>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
  </PropertyGroup>
  <ItemGroup>
    <FrameworkReference Include="Microsoft.AspNetCore.App" />
  </ItemGroup>
</Project>
'@
$hash = [Security.Cryptography.SHA256]::Create()
try {
    $cache_key = [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($source + $project))).Replace('-', '').ToLowerInvariant()
}
finally { $hash.Dispose() }
$cache_directory = Join-Path $env:LOCALAPPDATA "Kazarov/devkit-media/$cache_key"
$program = Join-Path $cache_directory 'bin/Release/net10.0-windows/devkit-media.dll'
if (-not (Test-Path -LiteralPath $program -PathType Leaf)) {
    [IO.Directory]::CreateDirectory($cache_directory) | Out-Null
    [IO.File]::WriteAllText((Join-Path $cache_directory 'Program.cs'), $source)
    $project_path = Join-Path $cache_directory 'devkit-media.csproj'
    [IO.File]::WriteAllText($project_path, $project)
    & dotnet build $project_path --configuration Release --nologo --verbosity quiet --ignore-failed-sources
    if ($LASTEXITCODE -ne 0) { throw 'Unable to build the Target Manager command-line helper.' }
}
& dotnet $program $client_directory $Target $Action $Seconds $OutputDirectory $FileName $StartLogPath $StartPattern $StartTimeoutSeconds
if ($LASTEXITCODE -ne 0) { throw "Target Manager media command failed with exit code $LASTEXITCODE." }
