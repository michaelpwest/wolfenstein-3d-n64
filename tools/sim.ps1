# Build the game for the host and run the capture script.
#
#   .\tools\sim.ps1
#
# src/ has no N64 dependencies, so the same sources the ROM is built from
# compile with the container's host gcc (tools/sim_run.sh, which
# tools/sim.sh runs too). The frames land in build/sim, and
# build/sim/sheet.png is the contact sheet.

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot "requirements.ps1")      # Invoke-Native

if (-not (Test-Path "$root\assets\wolf3d.dat")) {
    Invoke-Native { python "$root\tools\extract_assets.py" }
    if ($LASTEXITCODE -ne 0) { throw "asset extraction failed" }
}

New-Item -ItemType Directory -Force -Path "$root\build\sim" | Out-Null
Get-ChildItem "$root\build\sim" -Filter *.ppm -ErrorAction SilentlyContinue |
    Remove-Item

# labeled with the project folder, as build.ps1's are, for build-gui.ps1's Stop
Invoke-Native {
    docker run --rm --label "wolf3d-n64.project=$root" -v "${root}:/src" -w /src `
        n64-libdragon bash tools/sim_run.sh
}
if ($LASTEXITCODE -ne 0) { throw "host build or run failed" }

Invoke-Native { python "$root\tools\sheet.py" "$root\build\sim" 3 2 }
