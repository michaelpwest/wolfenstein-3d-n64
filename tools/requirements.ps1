# What a build needs, checked without building anything: the game files,
# the executable, Python 3 and Docker. build.ps1 prints the list on its
# help screen and build-gui.ps1 shows it as lights - one check, so the two
# cannot disagree. Dot-source it:
#
#   . "$root\tools\requirements.ps1"
#
# Written for Windows PowerShell 5.1 as well as PowerShell 7.

# Windows PowerShell 5.1 turns a native program's stderr, once it is
# redirected, into error records - and under ErrorActionPreference Stop into
# a terminating error, which ends the script. Docker writes to stderr when an
# image is missing, when it is not running, and all through a build, so
# native programs run with that switched off; their exit code, in
# $LASTEXITCODE, says how it went.
function Invoke-Native {
    param([scriptblock]$Command)
    $ErrorActionPreference = "Continue"
    & $Command
}

# The ROM's file name from -Name: .z64 added if it is not there. Spaces are
# fine - make builds the ROM under a name of its own, inside build\, and the
# name only goes on the copy made of it - but not a folder, nor anything
# Windows does not allow in a file name.
function ConvertTo-RomName {
    param([string]$Name)
    $n = $Name.Trim()
    if ($n -notmatch '\.z64$') { $n = "$n.z64" }
    if ($n -match '[\\/:*?"<>|\x00-\x1f]' -or $n -eq ".z64") {
        throw ('-Name must be a file name, without a folder or any of \ / : * ? " < > | - not: ' + $Name)
    }
    $n
}

# The port's version, kept in one place: PORT_VERSION in src\wolf.h, which
# build.ps1's help and the build window's title show
function Get-PortVersion {
    param([string]$Root)
    $line = Select-String -LiteralPath (Join-Path $Root "src\wolf.h") `
        -Pattern '#define\s+PORT_VERSION\s+"([^"]+)"' | Select-Object -First 1
    if ($line) { $line.Matches[0].Groups[1].Value } else { "unknown" }
}

# Each game's ROM: the name people see, and the one make builds it under
# inside build\ (the Makefile says why the two differ)
function Get-DefaultRomName([bool]$Spear) {
    if ($Spear) { "Spear of Destiny.z64" } else { "Wolfenstein 3D.z64" }
}
function Get-InternalRom([bool]$Spear) {
    if ($Spear) { "build/spear.z64" } else { "build/wolf3d.z64" }
}

# Every requirement as an item: Key, State (ok, missing, later - not needed
# yet - or unknown), Required (whether a build needs it now), Text (a line
# for the help screen) and Label (a short one for the window). -Extract says
# the build reads the game files again anyway (build.ps1's -Assets or -Data).
function Get-BuildRequirements {
    param([string]$Root, [string]$Data, [string]$Source, [string]$Name,
          [bool]$Extract = $false)
    $ErrorActionPreference = "Continue"

    $dataDir = if ($Data) { $Data } else { Join-Path $Root "gamedata" }
    $games = @{ WL1 = "the shareware episode"; WL6 = "the full game";
                SOD = "Spear of Destiny" }
    $short = @{ WL1 = "shareware"; WL6 = "full game"; SOD = "Spear of Destiny" }
    $here = { param($f) Test-Path -LiteralPath (Join-Path $dataDir $f) }

    # whichever of the three games is there: the one with a VSWAP
    $found = @("WL1", "WL6", "SOD" | Where-Object { & $here "VSWAP.$_" })
    $ext = if ($found.Count -eq 1) { $found[0] } else { "WL1" }
    $files = "VSWAP", "GAMEMAPS", "MAPHEAD", "VGAGRAPH", "VGADICT", "VGAHEAD",
             "AUDIOT", "AUDIOHED" | ForEach-Object { "$_.$ext" }
    $count = @($files | Where-Object { & $here $_ }).Count
    # Spear of Destiny's is SPEAR.EXE, or WOLF3D.EXE in some releases
    $exeNames = if ($ext -eq "SOD") { "SPEAR.EXE", "WOLF3D.EXE" } else { , "WOLF3D.EXE" }
    $exeName = $exeNames -join " or "
    $exe = [bool]($exeNames | Where-Object { & $here $_ })

    # Python as build.ps1 will run it: "python" must be there and be Python
    # 3, not the Microsoft Store's stand-in, which only offers to install it
    $python = $false
    if (Get-Command python -ErrorAction SilentlyContinue) {
        python -c "import sys; sys.exit(sys.version_info[0] != 3)" *> $null
        $python = $LASTEXITCODE -eq 0
    }

    $dockerCmd = [bool](Get-Command docker -ErrorAction SilentlyContinue)
    $dockerUp = $false
    $image = $false
    if ($dockerCmd) {
        docker info *> $null
        $dockerUp = $LASTEXITCODE -eq 0
        if ($dockerUp) {
            docker image inspect n64-libdragon *> $null
            $image = $LASTEXITCODE -eq 0
        }
    }
    # Which game is extracted: the blob's version, the big-endian word at
    # byte 10 of assets\wolf3d.dat - 0, 1 or 2 (tools/extract_assets.py)
    $blob = Join-Path $Root "assets\wolf3d.dat"
    $extracted = Test-Path -LiteralPath $blob
    $extractedGame = $null
    if ($extracted) {
        try {
            $fs = [IO.File]::OpenRead($blob)
            try {
                $head = New-Object byte[] 12
                if ($fs.Read($head, 0, 12) -eq 12) {
                    $extractedGame = @{ 0 = "WL1"; 1 = "WL6"; 2 = "SOD" }[[int]$head[11]]
                }
            } finally { $fs.Dispose() }
        } catch { }
    }
    # the folder holds another game than the one extracted: until the files
    # are read again, a build would build the old one
    $selectedGame = if ($found.Count -eq 1) { $ext } else { $null }
    $switching = $extractedGame -and $selectedGame -and ($extractedGame -ne $selectedGame)

    $items = @()
    $item = {
        param($key, $state, $required, $text, $label)
        [pscustomobject]@{ Key = $key; State = $state; Required = $required
                           Text = $text; Label = $label }
    }

    if ($found.Count -gt 1) {
        $items += & $item "game" "missing" $true `
            ("more than one game in {0} ({1}): give each its own folder" -f $dataDir, ($found -join ", ")) `
            "Game files: more than one game in the folder"
    } elseif ($found.Count -eq 0) {
        $items += & $item "game" "missing" $true `
            ("your game files in {0}: none found (VSWAP.WL1, .WL6 or .SOD)" -f $dataDir) `
            "Game files: none found in the folder"
    } else {
        $items += & $item "game" $(if ($count -eq 8) { "ok" } else { "missing" }) $true `
            ("{0} in {1}: {2} of 8 .{3} files" -f $games[$ext], $dataDir, $count, $ext) `
            ("Game files: {0}, {1} of 8" -f $games[$ext], $count)
    }
    # without the executable, id's source release can stand in for the
    # palette, so it is only a must when there is no -Source
    $items += & $item "exe" $(if ($exe) { "ok" } else { "missing" }) (-not $Source) `
        ("{0} there (the palette and sign-on screen)" -f $exeName) `
        ("{0} (the palette and sign-on screen)" -f $exeName)
    $items += & $item "python" $(if ($python) { "ok" } else { "missing" }) $true `
        "Python 3 (the asset extractor)" "Python 3"
    $items += & $item "docker" $(if ($dockerCmd) { "ok" } else { "missing" }) $true `
        "Docker Desktop, installed" "Docker Desktop installed"
    $items += & $item "running" $(if ($dockerUp) { "ok" } else { "missing" }) $true `
        "Docker Desktop, running" "Docker Desktop running"
    # the N64 compiler: libdragon and its tools, in a Docker image that the
    # first build makes from tools\Dockerfile.libdragon
    if (-not $dockerUp) {
        $items += & $item "image" "unknown" $false `
            "the N64 compiler (the n64-libdragon Docker image) - Docker is not running to ask" `
            "N64 compiler (Docker image) - Docker isn't running"
    } elseif ($image) {
        $items += & $item "image" "ok" $false `
            "the N64 compiler (the n64-libdragon Docker image)" "N64 compiler (Docker image)"
    } else {
        $items += & $item "image" "later" $false `
            "the N64 compiler (the n64-libdragon Docker image) - the first build makes it; it takes a while" `
            "N64 compiler (Docker image) - the first build makes it"
    }
    # the game data: the game files converted for the ROM, in assets\
    if (-not $extracted) {
        $items += & $item "assets" "later" $false `
            "game data not extracted yet (-Rom or -Assets does it)" `
            "Game data not extracted yet - the build does it"
    } elseif ($switching) {
        $text = if ($Extract) {
            "game data extracted from {0}; this build reads {1} from the folder instead"
        } else {
            "game data extracted from {0}, but the folder holds {1} - -Assets reads it"
        }
        $items += & $item "assets" "later" $false `
            ($text -f $games[$extractedGame], $games[$selectedGame]) `
            ("Game data extracted: {0}, switching to {1}" -f $short[$extractedGame], $short[$selectedGame])
    } elseif ($extractedGame) {
        $items += & $item "assets" "ok" $false `
            ("game data extracted (assets\wolf3d.dat): {0}" -f $games[$extractedGame]) `
            ("Game data extracted: {0}" -f $games[$extractedGame])
    } else {
        $items += & $item "assets" "ok" $false `
            "game data extracted (assets\wolf3d.dat)" "Game data extracted"
    }

    # the game in the data folder decides the name, unless -Name says
    $romName = $null
    $romError = $null
    if ($Name) {
        try { $romName = ConvertTo-RomName $Name } catch { $romError = $_.Exception.Message }
    } else {
        $romName = Get-DefaultRomName ($ext -eq "SOD" -and $found.Count -eq 1)
    }

    $blocking = @($items | Where-Object { $_.Required -and $_.State -ne "ok" })
    [pscustomobject]@{
        Items        = $items
        Switching    = [bool]$switching
        RomName      = $romName
        RomNameError = $romError
        Ready        = ($blocking.Count -eq 0) -and -not $romError
        Missing      = @($blocking | ForEach-Object { $_.Label })
        DataDir      = $dataDir
    }
}
