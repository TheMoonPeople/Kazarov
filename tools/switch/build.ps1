param(
    [Parameter(Mandatory)] [string]$Root,
    [Parameter(ValueFromRemainingArguments)] [string[]]$BuildArguments
)
$ErrorActionPreference = 'Stop'
$checkout = (Resolve-Path -LiteralPath $Root).Path
$sharedScript = Join-Path $PSScriptRoot 'build.sh'
$wslRoot = & wsl.exe --exec wslpath -a $checkout.Replace('\', '/')
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($wslRoot)) {
    throw 'Could not resolve the game checkout in WSL'
}
$wslScript = & wsl.exe --exec wslpath -a $sharedScript.Replace('\', '/')
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($wslScript)) {
    throw 'Could not resolve the shared Switch build script in WSL'
}
if ($BuildArguments.Count -gt 0 -and $BuildArguments[0] -eq 'switch') {
    $BuildArguments = @($BuildArguments | Select-Object -Skip 1)
}
$wslArguments = @('--exec', 'env', ('KZ_SWITCH_ROOT=' + $wslRoot.Trim()))
foreach ($name in @('BUILDER', 'CLEAN', 'JOBS', 'CMAKE_BUILD_PARALLEL_LEVEL', 'SWITCH_PRESET', 'KZ_SWITCH_IMAGE')) {
    $value = [Environment]::GetEnvironmentVariable($name)
    if ($null -ne $value) { $wslArguments += "$name=$value" }
}
$wslArguments += @('bash', $wslScript.Trim()) + $BuildArguments
& wsl.exe @wslArguments
exit $LASTEXITCODE
