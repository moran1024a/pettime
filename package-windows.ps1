param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$Toolchain
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$Toolchain = (Resolve-Path -LiteralPath $Toolchain).Path
$toolBin = Join-Path $Toolchain 'bin'
$env:PATH = "$toolBin;$env:PATH"
$deploy = Join-Path $toolBin 'windeployqt6.exe'
$objdump = Join-Path $toolBin 'objdump.exe'
foreach ($required in @($deploy, $objdump, (Join-Path $BuildDir 'pettime.exe'), (Join-Path $Toolchain 'share\licenses\qt6-base'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required file or directory missing: $required" }
}
# A fresh staging directory prevents stale DLLs from entering subsequent packages.
$stage = Join-Path $BuildDir ('package-' + [guid]::NewGuid().ToString('N'))
$packageName = 'Pettime-windows-x64'
$packageDir = Join-Path $stage $packageName
$archive = Join-Path $BuildDir ($packageName + '.zip')
try {
    New-Item -ItemType Directory -Path $packageDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $BuildDir 'pettime.exe') -Destination $packageDir
    & $deploy --release --compiler-runtime --no-translations --force-openssl --dir $packageDir (Join-Path $packageDir 'pettime.exe')
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)" }
    if (-not (Test-Path -LiteralPath (Join-Path $packageDir 'platforms\qwindows.dll'))) {
        throw 'Windows platform plugin was not deployed'
    }
    "[Paths]`r`nPrefix=.`r`nPlugins=.`r`n" | Set-Content -LiteralPath (Join-Path $packageDir 'qt.conf') -Encoding ASCII

    # The OpenSSL plugin loads these by name; they are absent from PE import tables.
    foreach ($sslDll in 'libssl-3-x64.dll', 'libcrypto-3-x64.dll') {
        Copy-Item -LiteralPath (Join-Path $toolBin $sslDll) -Destination $packageDir
    }

    # Qt deployment alone does not collect all MSYS2 third-party dependencies.
    # Scan every deployed plugin too, then recursively collect DLL imports.
    $queue = [Collections.Generic.Queue[string]]::new()
    Get-ChildItem -LiteralPath $packageDir -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' } | ForEach-Object { $queue.Enqueue($_.FullName) }
    $seen = @{}
    while ($queue.Count -gt 0) {
        $binary = $queue.Dequeue()
        if ($seen.ContainsKey($binary)) { continue }
        $seen[$binary] = $true
        $imports = & $objdump -p $binary
        if ($LASTEXITCODE -ne 0) { throw "Cannot read DLL imports: $binary" }
        foreach ($line in $imports) {
            if ($line -notmatch '^\s*DLL Name:\s*(\S+)\s*$') { continue }
            $dll = $Matches[1]
            if ($dll -match '^(api-ms-|ext-ms-)') { continue }
            $dest = Join-Path $packageDir $dll
            $source = Join-Path $toolBin $dll
            if (Test-Path -LiteralPath $dest) {
                $queue.Enqueue($dest)
            } elseif (Test-Path -LiteralPath $source) {
                Copy-Item -LiteralPath $source -Destination $dest
                $queue.Enqueue($dest)
            } elseif (-not (Test-Path -LiteralPath (Join-Path ([Environment]::SystemDirectory) $dll))) {
                throw "Unresolved DLL '$dll' required by '$binary'"
            }
        }
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE'), (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.md') -Destination $packageDir
    # Preserve the installed toolchain's license texts, including Qt's bundled components.
    Copy-Item -LiteralPath (Join-Path $Toolchain 'share\licenses') -Destination (Join-Path $packageDir 'licenses') -Recurse
    @(
        'Pettime for Windows x64', '',
        'Extract this entire folder, then double-click pettime.exe.',
        'Keep all DLLs, qt.conf and plugin folders beside the executable.',
        'No Qt or MSYS2 installation and no PATH changes are needed on the destination PC.',
        'Do not launch the EXE from inside the ZIP viewer.', '',
        'Shared dependencies were copied from the build machine MSYS2 UCRT64 installation.',
        'The licenses directory preserves installed license texts (including some build-only packages).',
        'See LICENSE and THIRD_PARTY_NOTICES.md for notices.',
        'MSYS2 package metadata and source build recipes: https://packages.msys2.org/',
        'Qt source archives: https://download.qt.io/official_releases/qt/'
    ) | Set-Content -LiteralPath (Join-Path $packageDir 'README.txt') -Encoding UTF8
    $manifest = Get-ChildItem -LiteralPath $packageDir -Recurse -File | Sort-Object FullName | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.FullName.Substring($packageDir.Length + 1)
    }
    $manifest | Set-Content -LiteralPath (Join-Path $packageDir 'SHA256SUMS.txt') -Encoding ASCII
    $temporaryArchive = Join-Path $stage ($packageName + '.zip')
    Compress-Archive -LiteralPath $packageDir -DestinationPath $temporaryArchive -CompressionLevel Optimal
    Move-Item -LiteralPath $temporaryArchive -Destination $archive -Force
    Write-Host "Distribution ZIP: $archive"
} finally {
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    $buildPrefix = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\') + '\'
    if (-not $resolvedStage.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolvedStage) -notmatch '^package-[0-9a-f]{32}$') {
        throw "Unsafe staging cleanup path: $resolvedStage"
    }
    if (Test-Path -LiteralPath $resolvedStage) { Remove-Item -LiteralPath $resolvedStage -Recurse -Force }
}