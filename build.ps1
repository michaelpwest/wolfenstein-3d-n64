# Build the ROM: "Wolfenstein 3D.z64", "Spear of Destiny.z64", or -Name
# whatever you like.
#
#   .\build.ps1              # this help, and what the build needs
#   .\build.ps1 -Rom         # make
#   .\build.ps1 -Assets      # regenerate assets/wolf3d.dat first
#   .\build.ps1 -Clean       # make clean, then build
#   .\build.ps1 -Sim         # host build + capture instead of the ROM
#   .\build.ps1 -Rom -Name x # build x.z64 instead of the usual name
#
# build-gui.cmd opens a window that runs this script, with what it needs
# shown before it starts.
#
# The assets come from your own copy of the game - the shareware (*.WL1),
# the full game (*.WL6) or Spear of Destiny (*.SOD), with its WOLF3D.EXE or
# SPEAR.EXE - in gamedata\ here unless -Data names another folder; naming
# one always extracts from it. See README.md.
#
# -Source, left out of the help on purpose, is for maintaining the port: it
# points at the WOLFSRC folder of id's source release, which checks the
# tables in tools\wolf_tables.py and can stand in for the executable's
# palette. A build never needs it (see docs\TECHNICAL_DETAILS.md).
#
# Everything else runs in Docker: the n64-libdragon image, which this builds
# from tools\Dockerfile.libdragon the first time it is missing.

param(
    [switch]$Rom,
    [switch]$Assets,
    [switch]$Clean,
    [switch]$Sim,
    [switch]$Help,
    [string]$Data,
    [string]$Source,
    [string]$Name
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# The requirement checks, which build-gui.ps1 shows too, and Invoke-Native,
# which keeps Windows PowerShell 5.1 from ending the script when Docker or
# Python write to stderr (tools\requirements.ps1)
. (Join-Path $root "tools\requirements.ps1")

# ---------------------------------------------------------------------------
# What the ROM is called. Each game gets a name of its own, because a
# cartridge is given its SRAM by the name of the ROM: emulators and flash
# carts keep the saves beside it, so two games sharing a name would share a
# cartridge - and its ten save slots and its high score table (see
# src\save.c). The game is the blob's version, a big-endian word at byte 10
# of assets\wolf3d.dat (tools\extract_assets.py).

function Test-Spear {
    $blob = Join-Path $root "assets\wolf3d.dat"
    (Test-Path $blob) -and [IO.File]::ReadAllBytes($blob)[11] -eq 2
}

function Get-RomName {
    if ($Name) {
        return ConvertTo-RomName $Name
    }
    Get-DefaultRomName (Test-Spear)
}

# ---------------------------------------------------------------------------
# With nothing to do, say what there is to do, and whether it can be done.

function Show-Help {
    $req = Get-BuildRequirements -Root $root -Data $Data -Source $Source -Name $Name `
        -Extract ([bool]($Assets -or $Data))
    $marks = @{ ok = "  ok     "; missing = "  MISSING"; later = "  later  " }
    $version = Get-PortVersion $root

    @"

Wolfenstein 3D for Nintendo 64 v$version - build script

  .\build.ps1 -Rom       build the ROM (extracting the assets first,
                         if that has not been done)
  .\build.ps1 -Assets    extract the assets from your game files again,
                         then build what that changed
  .\build.ps1 -Clean     throw away build\ and the ROMs and compile the
                         lot again; the extracted assets are kept, so
                         add -Assets to redo those as well
  .\build.ps1 -Sim       build the game for this PC instead and run its
                         checks; frames go to build\sim, see sheet.png
  .\build.ps1 -Help      this help screen

  -Data <folder>         where your game files are - the shareware, the
                         full game or Spear of Destiny (default: gamedata\
                         here); naming a folder extracts from it again
  -Name <name>           what to call the ROM, instead of Wolfenstein 3D.z64
                         or Spear of Destiny.z64 - in quotes if it has spaces.
                         Each game is named apart because a cartridge's
                         saves and high scores follow the ROM's name

Examples:

  .\build.ps1 -Rom
      build what is in gamedata\ - Wolfenstein 3D.z64, or Spear of
      Destiny.z64 if those are Spear of Destiny's files

  .\build.ps1 -Rom -Data D:\SPEAR
      build from another folder instead. Naming one always extracts it
      again, so this is how to switch from one game to another

  .\build.ps1 -Rom -Data D:\WOLF6 -Name "Wolfenstein 3D (full)"
      call it Wolfenstein 3D (full).z64, so that the full game has a
      cartridge of its own - its saves and its high scores - beside the
      shareware's

  .\build.ps1 -Assets
      read the game files in gamedata\ again and build - after putting
      other files there, say

  .\build.ps1 -Sim
      no ROM: build the game for this PC and run its checks instead

What the build needs:
"@
    # (the compiler image is left out until Docker is running to ask it)
    foreach ($item in $req.Items | Where-Object { $_.State -ne "unknown" }) {
        "{0} {1}" -f $marks[$item.State], $item.Text
    }
    ""
    if ($req.RomNameError) { throw $req.RomNameError }
    "The ROM will be {0}." -f $req.RomName
    ""
    "See README.md for the details."
    ""
}

if ($Help -or -not ($Rom -or $Assets -or $Clean -or $Sim)) {
    Show-Help
    exit 0
}

# ---------------------------------------------------------------------------

# A program that is not there leaves $LASTEXITCODE as the last one that ran
# set it, so each is looked for first rather than judged by its exit code
if ($Assets -or $Data -or -not (Test-Path "$root\assets\wolf3d.dat")) {
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
        throw "Python 3 is needed to extract the game's data - install it and try again"
    }
    $extract = @()
    if ($Data)   { $extract += @("--data", $Data) }
    if ($Source) { $extract += @("--source", $Source) }
    Invoke-Native { python "$root\tools\extract_assets.py" @extract }
    if ($LASTEXITCODE -ne 0) { throw "asset extraction failed" }
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw "Docker is not installed - see README.md"
}
Invoke-Native { docker info *> $null }
if ($LASTEXITCODE -ne 0) {
    throw "Docker is not running - start Docker Desktop and try again"
}
Invoke-Native { docker image inspect n64-libdragon *> $null }
if ($LASTEXITCODE -ne 0) {
    "building the n64-libdragon image (once; it compiles libdragon, so it takes a while)"
    Invoke-Native { docker build -f "$root\tools\Dockerfile.libdragon" -t n64-libdragon "$root\tools" }
    if ($LASTEXITCODE -ne 0) { throw "building the n64-libdragon image failed" }
}

if ($Sim) {
    & "$root\tools\sim.ps1"
    exit $LASTEXITCODE
}

# The container is labeled with the project folder, so that build-gui.ps1's
# Stop can stop this project's build, and only this project's.
function Invoke-Make {
    param([string[]]$MakeArgs)
    Invoke-Native {
        docker run --rm --label "wolf3d-n64.project=$root" -v "${root}:/src" -w /src `
            n64-libdragon make @MakeArgs
    }
    if ($LASTEXITCODE -ne 0) { throw "make $($MakeArgs -join ' ') failed" }
}

# make builds the ROM inside build\ under the game's own name, and it is
# copied out as the game's full name or -Name's - which make, with its
# spaces, could not build (see the Makefile). The Makefile reads the game
# from the blob too, for the ROM header's title.
$romName = Get-RomName
$internal = Get-InternalRom (Test-Spear)

if ($Clean) { Invoke-Make @("clean") }
Invoke-Make @("ROM=$internal")
Copy-Item -LiteralPath (Join-Path $root $internal) -Destination (Join-Path $root $romName) -Force

$romFile = Get-Item -LiteralPath (Join-Path $root $romName)
"{0}  {1:N0} bytes" -f $romFile.Name, $romFile.Length
