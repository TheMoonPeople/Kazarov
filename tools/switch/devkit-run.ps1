# Packages a libnx ELF or an existing NSO for an SDK development unit and records an isolated, bounded run.
# Arguments are written to romfs:/args.txt; no game-specific scene or save behavior is assumed.
[CmdletBinding(DefaultParameterSetName = 'Elf')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Elf')] [string]$Elf,
    [Parameter(Mandatory, ParameterSetName = 'Nso')] [string]$Nso,
    [Parameter(Mandatory)] [string]$SdkRoot,
    [Parameter(Mandatory)] [string]$Target,
    [Parameter(Mandatory)] [ValidatePattern('^0x[0-9a-fA-F]{16}$')] [string]$ProgramId,
    [Parameter(Mandatory)] [string]$Application,
    [string]$Arguments = '',
    [hashtable]$RomfsFiles = @{},
    [string]$OutputRoot = $env:TEMP,
    [string]$Image = 'kazarov-switch:latest',
    [ValidateSet('handheld', 'console')] [string]$Mode = 'handheld',
    [ValidateSet('normal', 'auto')] [string]$Performance = 'normal',
    [ValidateRange(1, 3600)] [int]$Seconds = 60,
    [string]$CompletePattern = '',
    [switch]$StageOnly
)
$ErrorActionPreference = 'Stop'
$Mode = $Mode.ToLowerInvariant()
$Performance = $Performance.ToLowerInvariant()
$applicationFile = $Application -replace '[^A-Za-z0-9_-]', '_'
$runRoot = Join-Path $OutputRoot ('{0}-Devkit-{1}-{2}' -f $applicationFile, (Get-Date -Format 'yyyyMMdd-HHmmss'), [guid]::NewGuid().ToString('N').Substring(0, 8))
$code = Join-Path $runRoot 'code'
$romfs = Join-Path $runRoot 'romfs'
New-Item -ItemType Directory -Path $code, $romfs | Out-Null
$tools = Join-Path $SdkRoot 'Tools\CommandLineTools'
$control = Join-Path $tools 'ControlTarget.exe'
$manifest = [ordered]@{
    application = $Application
    program_id = $ProgramId
    created = (Get-Date -Format o)
    target = $Target
    mode = $Mode
    performance = $Performance
    input_kind = $PSCmdlet.ParameterSetName.ToLowerInvariant()
    elf_source = $null
    elf_sha256 = $null
    nso_source = $null
    arguments = $Arguments
    seconds = $Seconds
    stage_only = [bool]$StageOnly
    result = 'staged'
    files = @()
}
if ($PSCmdlet.ParameterSetName -eq 'Elf') {
    $elfSource = (Resolve-Path -LiteralPath $Elf).Path
    Copy-Item -LiteralPath $elfSource -Destination (Join-Path $runRoot 'application.elf')
    $manifest.elf_source = $elfSource
    $manifest.elf_sha256 = (Get-FileHash -LiteralPath (Join-Path $runRoot 'application.elf') -Algorithm SHA256).Hash
    docker run --rm -v "${runRoot}:/stage" $Image elf2nso /stage/application.elf /stage/code/main
    if ($LASTEXITCODE -ne 0) { throw 'elf2nso failed' }
    Remove-Item -LiteralPath (Join-Path $runRoot 'application.elf')
} else {
    $nsoSource = (Resolve-Path -LiteralPath $Nso).Path
    $stream = [IO.File]::OpenRead($nsoSource)
    try {
        $magic = [byte[]]::new(4)
        if ($stream.Read($magic, 0, 4) -ne 4 -or [Text.Encoding]::ASCII.GetString($magic) -ne 'NSO0') {
            throw "Invalid NSO header: $nsoSource"
        }
    } finally { $stream.Dispose() }
    Copy-Item -LiteralPath $nsoSource -Destination (Join-Path $code 'main')
    $manifest.nso_source = $nsoSource
}
$manifest.nso_sha256 = (Get-FileHash -LiteralPath (Join-Path $code 'main') -Algorithm SHA256).Hash

foreach ($relative in $RomfsFiles.Keys) {
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[/\\])\.\.([/\\]|$)' -or $relative -eq 'args.txt') {
        throw "Invalid romfs relative path: $relative"
    }
    $source = (Resolve-Path -LiteralPath $RomfsFiles[$relative]).Path
    $destination = Join-Path $romfs $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    if (Test-Path -LiteralPath $source -PathType Container) {
        Copy-Item -LiteralPath $source -Destination $destination -Recurse
    } else {
        try { New-Item -ItemType HardLink -Path $destination -Target $source | Out-Null }
        catch { Copy-Item -LiteralPath $source -Destination $destination }
    }
    $manifest.files += [ordered]@{ path = $relative; source = $source }
}
[IO.File]::WriteAllText((Join-Path $romfs 'args.txt'), $Arguments, [Text.UTF8Encoding]::new($false))

$template = Join-Path $SdkRoot 'Samples\Sources\Applications\MoviePlayerSimple\MoviePlayerSimple.aarch64.lp64.nmeta'
$meta = Join-Path $runRoot 'application.nmeta'
$text = [IO.File]::ReadAllText($template).Replace('0x01004b9000490000', $ProgramId)
$safeName = [Security.SecurityElement]::Escape($Application)
$text = $text.Replace('<Name>Name</Name>', "<Name>$safeName</Name>")
$text = $text.Replace('<Publisher>Publisher</Publisher>', '<Publisher>Kazarov</Publisher>')
$text = $text.Replace('../../../../Samples/Sources/Resources/Icon/NintendoSDK_Application.bmp', (Join-Path $SdkRoot 'Samples\Sources\Resources\Icon\NintendoSDK_Application.bmp'))
[IO.File]::WriteAllText($meta, $text, [Text.UTF8Encoding]::new($false))
$descriptor = Join-Path $SdkRoot 'Resources\SpecFiles\Desc\NX-NXFP2-a64\Application.autogen.desc'
& (Join-Path $tools 'MakeMeta\MakeMeta.exe') --platform NX "--meta=$meta" "--desc=$descriptor" "-o=$(Join-Path $code 'main.npdm')" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'MakeMeta failed' }
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runRoot 'run-manifest.json') -Encoding utf8
if ($StageOnly) { Write-Output $runRoot; return }

$package = Join-Path $runRoot 'application.nsp'
& (Join-Path $tools 'AuthoringTool\AuthoringTool.exe') creatensp --platform NX --meta $meta --type Application --desc $descriptor `
    --program $code $romfs --ignore-nss-nrs-option --no-check-dir-warning -o $package | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'AuthoringTool creatensp failed' }

function Invoke-TargetCleanup([string]$Step, [string[]]$CommandArguments) {
    try {
        $process = Start-Process -FilePath $control -ArgumentList $CommandArguments -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $runRoot "cleanup-$Step.out") -RedirectStandardError (Join-Path $runRoot "cleanup-$Step.err")
        if (!$process.WaitForExit(15000)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            Write-Warning "Target cleanup $Step timed out; see $runRoot"
        } elseif ($process.ExitCode -ne 0) {
            Write-Warning "Target cleanup $Step failed with exit $($process.ExitCode); see $runRoot"
        }
    } catch { Write-Warning "Target cleanup $Step failed: $_" }
}
$targetLock = $null
$locked = $false
$listener = $null
$launcher = $null
try {
    # Shared with Chronicle's existing SDK validation operations.
    $targetLock = [IO.File]::Open((Join-Path $env:TEMP 'chronicle-edev.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    $locked = $true
    & $control check-alive --target $Target | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$Target is not reachable" }
    & $control devmenu --target $Target -- debug set-operation-mode-policy $Mode | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not select operation mode' }
    & $control devmenu --target $Target -- debug set-performance-mode-policy $Performance | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not select performance policy' }
    $log = Join-Path $runRoot 'run.log'
    $listener = Start-Process -FilePath $control -ArgumentList @('read-log', '--target', $Target, '--print-host-time', '-v') `
        -RedirectStandardOutput $log -RedirectStandardError (Join-Path $runRoot 'read-log.err') -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2
    $launcher = Start-Process -FilePath (Join-Path $tools 'RunOnTarget.exe') `
        -ArgumentList @(('"' + $package + '"'), '--target', $Target, '--print-host-time', '--no-wait') `
        -RedirectStandardOutput (Join-Path $runRoot 'launch.log') -RedirectStandardError (Join-Path $runRoot 'launch.err') -PassThru -WindowStyle Hidden
    if (!$launcher.WaitForExit(120000)) { throw 'Application launch timed out' }
    if ($launcher.ExitCode -ne 0) { throw "Application launch failed; see $runRoot" }
    $deadline = (Get-Date).AddSeconds($Seconds)
    $manifest.result = 'timed_out'
    while ((Get-Date) -lt $deadline) {
        if ($CompletePattern -and (Select-String -LiteralPath $log -Pattern $CompletePattern -Quiet)) {
            $manifest.result = 'completed'
            break
        }
        if ($listener.HasExited) { throw "Target log stream ended; see $runRoot" }
        Start-Sleep -Milliseconds 500
    }
    if ($CompletePattern -and !(Select-String -LiteralPath $log -Pattern $CompletePattern -Quiet)) {
        throw "Completion pattern was not observed; see $runRoot"
    }
    $screenshot = Start-Process -FilePath $control `
        -ArgumentList @('take-screenshot', '--target', $Target, '--full-path', ('"' + (Join-Path $runRoot 'final.png') + '"')) `
        -RedirectStandardOutput (Join-Path $runRoot 'screenshot.out') -RedirectStandardError (Join-Path $runRoot 'screenshot.err') `
        -PassThru -WindowStyle Hidden
    if (!$screenshot.WaitForExit(15000)) {
        Stop-Process -Id $screenshot.Id -Force -ErrorAction SilentlyContinue
        $manifest.screenshot_result = 'timed_out'
    } else {
        $manifest.screenshot_result = $screenshot.ExitCode
    }
} catch {
    $manifest.result = 'failed'
    $manifest.error = $_.Exception.Message
    throw
} finally {
    $manifest.finished = (Get-Date -Format o)
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runRoot 'run-manifest.json') -Encoding utf8
    if ($locked) {
        Invoke-TargetCleanup 'terminate' @('terminate', '--target', $Target)
        Invoke-TargetCleanup 'operation-auto' @('devmenu', '--target', $Target, '--', 'debug', 'set-operation-mode-policy', 'auto')
        Invoke-TargetCleanup 'performance-auto' @('devmenu', '--target', $Target, '--', 'debug', 'set-performance-mode-policy', 'auto')
        $targetLock.Dispose()
    }
    foreach ($process in @($launcher, $listener)) {
        if ($null -ne $process -and !$process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
    }
}
Write-Output $runRoot
