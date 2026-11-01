param(
    [ValidateSet("Ryujinx", "Atmosphere")]
    [string]$Target = "Ryujinx",
    [string]$ContentsRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$PackageRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$ManifestPath = Join-Path $PackageRoot "PACKAGE_MANIFEST.json"
$ChecksumsPath = Join-Path $PackageRoot "CHECKSUMS.txt"
foreach ($path in @($ManifestPath, $ChecksumsPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Invalid package; missing $path"
    }
}

$manifest = Get-Content -Raw -LiteralPath $ManifestPath | ConvertFrom-Json
if ($manifest.schema_version -ne 1 -or $manifest.program_id -ne "01007A3009184000") {
    throw "Unsupported package manifest."
}

foreach ($line in @(Get-Content -LiteralPath $ChecksumsPath)) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    if ($line -notmatch '^([0-9A-Fa-f]{64}) \*(.+)$') {
        throw "Malformed checksum line: $line"
    }
    $relative = $Matches[2].Replace('/', [IO.Path]::DirectorySeparatorChar)
    $source = [IO.Path]::GetFullPath((Join-Path $PackageRoot $relative))
    $prefix = $PackageRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $source.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Checksum path is invalid: $relative"
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -ne $Matches[1].ToUpperInvariant()) {
        throw "Package checksum mismatch: $relative"
    }
}

if ([string]::IsNullOrWhiteSpace($ContentsRoot)) {
    if ($Target -eq "Ryujinx") {
        if ([string]::IsNullOrWhiteSpace($env:APPDATA)) {
            throw "APPDATA environment variable is unavailable; specify -ContentsRoot."
        }
        $ContentsRoot = Join-Path $env:APPDATA "Ryujinx\mods\contents"
    } else {
        throw "Specify the SD card's atmosphere/contents path with -ContentsRoot."
    }
}

$ContentsRoot = [IO.Path]::GetFullPath($ContentsRoot)
$titleIdFolder = if ($Target -eq "Ryujinx") { "01007a3009184000" } else { "01007A3009184000" }
$TitleRoot = Join-Path $ContentsRoot $titleIdFolder
New-Item -ItemType Directory -Force -Path $TitleRoot | Out-Null
$backupRoot = Join-Path $TitleRoot (".pshowcoop-backup\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
$backupCount = 0

foreach ($record in @($manifest.files)) {
    $prefixText = "payload/contents/01007A3009184000/"
    if (-not ([string]$record.path).StartsWith($prefixText, [StringComparison]::Ordinal)) {
        throw "Unexpected payload path: $($record.path)"
    }
    $relativeTarget = ([string]$record.path).Substring($prefixText.Length).Replace('/', [IO.Path]::DirectorySeparatorChar)
    $source = Join-Path $PackageRoot ([string]$record.path).Replace('/', [IO.Path]::DirectorySeparatorChar)
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -ne [string]$record.sha256) {
        throw "Payload hash mismatch: $($record.path)"
    }
    $destination = [IO.Path]::GetFullPath((Join-Path $TitleRoot $relativeTarget))
    $titlePrefix = [IO.Path]::GetFullPath($TitleRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $destination.StartsWith($titlePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Payload path escapes the title directory: $relativeTarget"
    }
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $backup = Join-Path $backupRoot $relativeTarget
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $backup) | Out-Null
        Copy-Item -LiteralPath $destination -Destination $backup -Force
        $backupCount++
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    $temporary = "$destination.pshowcoop-new"
    Copy-Item -LiteralPath $source -Destination $temporary -Force
    Move-Item -LiteralPath $temporary -Destination $destination -Force
}

Write-Host "Princess Peach Showtime Local Co-op $($manifest.version) installed for $Target."
Write-Host "Destination: $TitleRoot"
if ($backupCount -gt 0) {
    Write-Host "Backed up $backupCount replaced file(s) under: $backupRoot"
}
if ($Target -eq "Atmosphere") {
    Write-Warning "Atmosphere / physical hardware support is experimental."
}
