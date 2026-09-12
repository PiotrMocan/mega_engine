param(
    [ValidateSet('ClangTidy','MSVC')][string]$Tool='ClangTidy',
    [string]$BuildTree='build/windows-debug',
    [string[]]$Sources=@()
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$tree=(Resolve-Path -LiteralPath $BuildTree).Path
$allowed=@('windows-debug','windows-release','windows-asan') | ForEach-Object { Join-Path $root "build/$_" }
if ($tree -notin $allowed) { throw 'Analysis requires one of the three native Windows build trees.' }
$commands=Get-Content -Raw (Join-Path $tree 'compile_commands.json') | ConvertFrom-Json
$entries=$commands | Where-Object {
    $_.file.Replace('\','/').StartsWith($root.Replace('\','/')+'/src/') -and
    $_.file -notmatch '/vma.cpp$' -and
    ($Sources.Count -eq 0 -or [IO.Path]::GetFileName($_.file) -in $Sources)
} | Sort-Object file -Unique
if (-not $entries) { throw 'No application translation units selected.' }
if ($Tool -eq 'ClangTidy') {
    $tidy=(Get-Command clang-tidy.exe -ErrorAction SilentlyContinue).Source
    if (-not $tidy) {
        foreach ($edition in @('Community','Professional','Enterprise','BuildTools')) {
            $candidate=Join-Path ${env:ProgramFiles} "Microsoft Visual Studio/2022/$edition/VC/Tools/Llvm/x64/bin/clang-tidy.exe"
            if (Test-Path -LiteralPath $candidate) { $tidy=$candidate; break }
        }
    }
    if (-not $tidy) { throw 'Install native clang-tidy (Visual Studio C++ LLVM tools).' }
    foreach ($entry in $entries) {
        & $tidy "-p=$tree" '-checks=-*,clang-analyzer-*,bugprone-*,performance-*,concurrency-*,cppcoreguidelines-owning-memory,cppcoreguidelines-pro-bounds-*' $entry.file
        if ($LASTEXITCODE -ne 0) { throw "clang-tidy failed: $($entry.file)" }
    }
} else {
    # Use the exact native compile command, including generated SDK includes.
    # /analyze:only does not replace the regular objects or link a new executable.
    foreach ($entry in $entries) {
        $match=[regex]::Match($entry.command,'^("[^"]+"|\S+)\s+(.*)$')
        if (-not $match.Success) { throw 'Unsupported compilation database command.' }
        $info=New-Object System.Diagnostics.ProcessStartInfo
        $info.FileName=$match.Groups[1].Value.Trim('"')
        $info.Arguments=$match.Groups[2].Value+' /analyze:only /analyze:plugin EspXEngine.dll'
        $info.WorkingDirectory=$entry.directory
        $info.UseShellExecute=$false
        $info.CreateNoWindow=$true
        $info.RedirectStandardOutput=$true
        $info.RedirectStandardError=$true
        Write-Output "MSVC Core Check: $($entry.file)"
        $process=[Diagnostics.Process]::Start($info)
        $stdout=$process.StandardOutput.ReadToEndAsync()
        $stderr=$process.StandardError.ReadToEndAsync()
        try {
            $process.WaitForExit()
            Write-Output $stdout.Result
            Write-Output $stderr.Result
            if ($process.ExitCode -ne 0) { throw "MSVC analysis failed: $($entry.file)" }
        } finally { $process.Dispose() }
    }
}
