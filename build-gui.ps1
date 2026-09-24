# A window for building: choose what to build and where your game files
# are, see what the build needs light up before you start, then build - with
# build.ps1's own output shown as it runs. It runs build.ps1 itself, and its
# lights come from tools\requirements.ps1, the same check build.ps1's help
# screen prints, so the window and the script cannot disagree. The choices
# are kept in build-gui.json, beside this file, for next time.
#
#   build-gui.cmd                   double-click it, or from a console:
#   powershell -ExecutionPolicy Bypass -File build-gui.ps1
#
# Written for Windows PowerShell 5.1, which every Windows has, as well as
# PowerShell 7; the build runs under whichever one opened the window.

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location -LiteralPath $root
[Environment]::CurrentDirectory = $root

# Windows Forms dialogs need a single-threaded apartment, which Windows
# PowerShell always has and PowerShell 7 may not; without one, open the
# window with Windows PowerShell instead.
if ([Threading.Thread]::CurrentThread.GetApartmentState() -ne "STA") {
    $ps51 = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    Start-Process $ps51 -WindowStyle Hidden -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    return
}

. (Join-Path $root "tools\requirements.ps1")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

if (-not ("Wolf3dBuildGui.Runner" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Wolf3dBuildGui {
    public static class Native {
        [DllImport("user32.dll")]
        public static extern bool SetProcessDPIAware();
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, string lParam);
        [DllImport("user32.dll", EntryPoint = "SendMessageW")]
        public static extern IntPtr SendMessagePtr(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
    }

    // The build, as a child process whose output is queued a line at a time
    // from .NET's own threads. The window's timer empties the queue on the
    // window's thread: a PowerShell script block cannot run on those threads.
    public class Runner {
        public readonly ConcurrentQueue<string> Lines = new ConcurrentQueue<string>();
        public Process Process;

        public void Start(string exe, string args, string dir) {
            ProcessStartInfo psi = new ProcessStartInfo(exe, args);
            psi.WorkingDirectory = dir;
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
            Process = new Process();
            Process.StartInfo = psi;
            Process.OutputDataReceived += delegate(object s, DataReceivedEventArgs e) {
                if (e.Data != null) Lines.Enqueue(e.Data);
            };
            Process.ErrorDataReceived += delegate(object s, DataReceivedEventArgs e) {
                if (e.Data != null) Lines.Enqueue(e.Data);
            };
            Process.Start();
            Process.BeginOutputReadLine();
            Process.BeginErrorReadLine();
        }
    }
}
"@
}

[void][Wolf3dBuildGui.Native]::SetProcessDPIAware()
[System.Windows.Forms.Application]::EnableVisualStyles()

# ---------------------------------------------------------------------------
# The choices, kept for next time. Written as UTF-8 either way, so Windows
# PowerShell and PowerShell 7 read each other's.

$settingsFile = Join-Path $root "build-gui.json"
$gamedata = Join-Path $root "gamedata"

function Read-Settings {
    # the first time, the game folder is the project's own gamedata
    $s = @{ Action = "rom"; Assets = $false; Data = $gamedata; Name = "" }
    if (Test-Path -LiteralPath $settingsFile) {
        try {
            $j = [IO.File]::ReadAllText($settingsFile, [Text.Encoding]::UTF8) | ConvertFrom-Json
            foreach ($k in @($s.Keys)) {
                if ($null -ne $j.$k) { $s[$k] = $j.$k }
            }
        } catch { }                             # a damaged file: the defaults
    }
    $s
}

function Save-Settings {
    $json = [pscustomobject]@{
        Action = Get-Action
        Assets = $cbAssets.Checked
        Data   = $txtData.Text.Trim()
        Name   = $txtName.Text.Trim()
    } | ConvertTo-Json
    try {
        [IO.File]::WriteAllText($settingsFile, $json, (New-Object Text.UTF8Encoding($false)))
    } catch { }                                 # a read-only folder: forget
}

# ---------------------------------------------------------------------------
# The window. Laid out in pixels at 96 dots per inch, and every one of them
# scaled here to the screen's own - rather than by Windows Forms' automatic
# scaling, which stretched the output box to twice its height, below the
# window, on a screen at 200%: it both scaled the box and stretched it with
# the window it is anchored to.

$screen = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero)
$scale = $screen.DpiX / 96.0
$screen.Dispose()
function Px([double]$v) { [int][Math]::Round($v * $scale) }
function Point([double]$x, [double]$y) { New-Object System.Drawing.Point((Px $x), (Px $y)) }
function Size([double]$w, [double]$h) { New-Object System.Drawing.Size((Px $w), (Px $h)) }
$TL   = [System.Windows.Forms.AnchorStyles]"Top, Left"
$TR   = [System.Windows.Forms.AnchorStyles]"Top, Right"
$TLR  = [System.Windows.Forms.AnchorStyles]"Top, Left, Right"
$ALL  = [System.Windows.Forms.AnchorStyles]"Top, Bottom, Left, Right"

function Add-Control {
    param($Parent, [string]$Type, [int]$X, [int]$Y, [int]$W, [int]$H,
          [string]$Text = "", $Anchor = $TL)
    $c = New-Object "System.Windows.Forms.$Type"
    $c.Location = Point $X $Y
    if ($W -gt 0) { $c.Size = Size $W $H } else { $c.AutoSize = $true }
    $c.Text = $Text
    $c.Anchor = $Anchor
    $Parent.Controls.Add($c)
    $c
}

$form = New-Object System.Windows.Forms.Form
$form.SuspendLayout()
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::None
$form.Font = New-Object System.Drawing.Font("Segoe UI", 9)       # points: sized for the screen already
$form.Text = "Wolfenstein 3D for Nintendo 64 v$(Get-PortVersion $root) - Build"
$form.ClientSize = Size 900 600
$form.StartPosition = "CenterScreen"

# -- your game files
$gbGame = Add-Control $form GroupBox 12 8 876 58 "Your game files" $TLR
$txtData = Add-Control $gbGame TextBox 12 23 560 23 "" $TLR
$btnData = Add-Control $gbGame Button 580 21 100 27 "Browse..." $TR
$btnDataReset = Add-Control $gbGame Button 686 21 178 27 "Use the gamedata folder" $TR

# -- what to build
$gbWhat = Add-Control $form GroupBox 12 74 430 212 "What to build"
$rbRom = Add-Control $gbWhat RadioButton 14 24 0 0 "Build the ROM"
$rbClean = Add-Control $gbWhat RadioButton 14 48 0 0 "Clean build: compile everything again"
$rbSim = Add-Control $gbWhat RadioButton 14 72 0 0 "No ROM: build for this PC and run the checks"
$cbAssets = Add-Control $gbWhat CheckBox 14 104 0 0 "Read the game files again first"
[void](Add-Control $gbWhat Label 14 142 0 0 "ROM name:")
$txtName = Add-Control $gbWhat TextBox 120 139 180 23

# -- what the build needs: a light and a line per requirement. The light is
# a circle painted in its color (kept in Tag), and named for its state for
# screen readers.
$gbReq = Add-Control $form GroupBox 454 74 434 212 "What the build needs" $TLR
$rows = @()
for ($i = 0; $i -lt 7; $i++) {
    $y = 22 + 22 * $i
    $dot = Add-Control $gbReq Panel 14 ($y + 3) 12 12
    $dot.Tag = [System.Drawing.Color]::Silver
    $dot.AccessibleName = "checking"
    $dot.Add_Paint({
        param($s, $e)
        $e.Graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $brush = New-Object System.Drawing.SolidBrush($s.Tag)
        $e.Graphics.FillEllipse($brush, 0, 0, $s.Width - 1, $s.Height - 1)
        $brush.Dispose()
    })
    $line = Add-Control $gbReq Label 32 $y 392 20 "checking..." $TLR
    $line.AutoEllipsis = $true
    $rows += [pscustomobject]@{ Dot = $dot; Line = $line }
}
$lblRom = Add-Control $gbReq Label 12 182 310 20 "" $TLR
$lblRom.AutoEllipsis = $true
$btnCheck = Add-Control $gbReq Button 326 178 96 27 "Check again" $TR

# -- the buttons, and the build's own output
$btnBuild = Add-Control $form Button 12 294 120 32 "Build"
$btnBuild.Font = New-Object System.Drawing.Font("Segoe UI", 9, [System.Drawing.FontStyle]::Bold)
$btnStop = Add-Control $form Button 138 294 90 32 "Stop"
$btnShow = Add-Control $form Button 234 294 120 32 "Show in folder"
$lblStatus = Add-Control $form Label 364 301 524 20 "" $TLR
$lblStatus.AutoEllipsis = $true
$txtLog = Add-Control $form TextBox 12 334 876 254 "" $ALL
$txtLog.Multiline = $true
$txtLog.ReadOnly = $true
$txtLog.WordWrap = $false
$txtLog.ScrollBars = "Both"
$txtLog.MaxLength = [int]::MaxValue
$txtLog.BackColor = [System.Drawing.SystemColors]::Window
$txtLog.Font = New-Object System.Drawing.Font("Consolas", 9)

# names for the controls that matter, which UI Automation - screen readers,
# and scripts that drive the window - identifies them by
$names = @{ GameFolder = $txtData; ReadAgain = $cbAssets; BuildRom = $rbRom
            CleanBuild = $rbClean; RunChecks = $rbSim; RomName = $txtName
            Build = $btnBuild; Stop = $btnStop
            ShowInFolder = $btnShow; Status = $lblStatus; RomLine = $lblRom
            Output = $txtLog }
foreach ($n in $names.Keys) { $names[$n].Name = $n }
for ($i = 0; $i -lt $rows.Count; $i++) {
    $rows[$i].Dot.Name = "Light$i"
    $rows[$i].Line.Name = "Requirement$i"
}

$tips = New-Object System.Windows.Forms.ToolTip
$tips.SetToolTip($txtData, "The folder with your copy of the game - to begin with, the gamedata folder in this project (as it is if left empty)")
$tips.SetToolTip($cbAssets, "Extract the game's data again, even from the folder read last time (-Assets)")
$tips.SetToolTip($txtName, "Leave it empty for Wolfenstein 3D.z64 or Spear of Destiny.z64. Each name gets its own saves in an emulator.")
$tips.SetToolTip($btnStop, "Stops the build, and this project's build container with it")
$form.ResumeLayout($false)

$colors = @{
    ok      = [System.Drawing.Color]::FromArgb(34, 139, 34)
    missing = [System.Drawing.Color]::FromArgb(200, 30, 30)
    later   = [System.Drawing.Color]::FromArgb(215, 145, 0)
    unknown = [System.Drawing.Color]::Silver
}
$stateNames = @{ ok = "ok"; missing = "missing"; later = "not needed yet"; unknown = "unknown" }
$inputs = @($txtData, $btnData, $btnDataReset, $rbRom, $rbClean, $rbSim, $cbAssets,
            $txtName, $btnCheck)

# ---------------------------------------------------------------------------
# State

$script:req = $null             # the last requirement check's result
$script:check = $null           # the check under way, in a runspace of its own
$script:checkDue = $null        # when the next check is due, if one is
$script:lastCheck = [DateTime]::MinValue
$script:runner = $null          # the build under way
$script:stopped = $false
$script:result = ""             # what the last build came to
$script:resultColor = [System.Drawing.SystemColors]::ControlText
$script:lastFile = $null        # what "Show in folder" shows

function Get-Action {
    if ($rbClean.Checked) { "clean" } elseif ($rbSim.Checked) { "sim" } else { "rom" }
}

# The game folder as build.ps1's -Data: nothing for the project's own
# gamedata folder, so that choosing it does not mean extracting every time
function Get-DataArg {
    $d = $txtData.Text.Trim()
    if (-not $d) { return "" }
    try { $full = [IO.Path]::GetFullPath($d).TrimEnd("\") } catch { return $d }
    if ($full -ieq $gamedata) { return "" }
    $full
}

# An argument for the command line, quoted - a trailing backslash would
# escape the closing quote, so it goes (a drive's root keeps a dot instead)
function Format-Arg([string]$s) {
    $s = $s.TrimEnd("\")
    if ($s.EndsWith(":")) { $s += "\." }
    '"' + $s + '"'
}

function Update-Controls {
    $busy = $null -ne $script:runner
    foreach ($c in $inputs) { $c.Enabled = -not $busy }
    $txtName.Enabled = (-not $busy) -and -not $rbSim.Checked
    $btnStop.Enabled = $busy
    $btnShow.Enabled = -not $busy
    $btnBuild.Text = switch (Get-Action) { "clean" { "Clean build" } "sim" { "Run checks" } default { "Build" } }
    $btnBuild.Enabled = (-not $busy) -and $script:req -and $script:req.Ready

    if ($script:req) {
        if ((Get-Action) -eq "sim") {
            $lblRom.Text = "No ROM: the checks write to build\sim."
            $lblRom.ForeColor = [System.Drawing.SystemColors]::ControlText
        } elseif ($script:req.RomNameError) {
            $lblRom.Text = $script:req.RomNameError
            $lblRom.ForeColor = $colors.missing
        } else {
            $lblRom.Text = "The ROM will be $($script:req.RomName)."
            $lblRom.ForeColor = [System.Drawing.SystemColors]::ControlText
        }
    }
    if ($busy) { return }
    if ($script:req -and -not $script:req.Ready) {
        $needs = @($script:req.Missing)
        if ($script:req.RomNameError) { $needs += "a ROM name that is just a file name" }
        $lblStatus.Text = "Still needed: " + ($needs -join ", ")
        $lblStatus.ForeColor = $colors.missing
    } elseif ($script:result) {
        $lblStatus.Text = $script:result
        $lblStatus.ForeColor = $script:resultColor
    } elseif ($script:req) {
        $lblStatus.Text = "Ready to build."
        $lblStatus.ForeColor = $colors.ok
    }
}

# ---------------------------------------------------------------------------
# The requirement check runs in a runspace of its own - asking Docker can
# take seconds, which would freeze the window - and the timer collects it.

function Start-Check {
    $script:checkDue = $null
    if ($script:check) {                        # one at a time; again after
        $script:checkDue = [DateTime]::Now.AddMilliseconds(200)
        return
    }
    $ps = [PowerShell]::Create()
    # -Extract: when the folder holds another game than the one extracted,
    # the window reads the files again itself (Start-Build), so its build
    # always switches
    [void]$ps.AddScript('param($file, $root, $data, $name)
        . $file
        Get-BuildRequirements -Root $root -Data $data -Name $name -Extract $true')
    [void]$ps.AddArgument((Join-Path $root "tools\requirements.ps1"))
    [void]$ps.AddArgument($root)
    [void]$ps.AddArgument((Get-DataArg))
    [void]$ps.AddArgument($txtName.Text.Trim())
    $script:check = @{ PS = $ps; Handle = $ps.BeginInvoke() }
    $script:lastCheck = [DateTime]::Now
    $gbReq.Text = "What the build needs (checking...)"
}

function Receive-Check {
    try {
        $out = $script:check.PS.EndInvoke($script:check.Handle)
        $req = if ($out.Count) { $out[0] } else { $null }
    } catch {
        $req = $null
    }
    $script:check.PS.Dispose()
    $script:check = $null
    $gbReq.Text = "What the build needs"
    if (-not $req) { return }
    $script:req = $req
    for ($i = 0; $i -lt $rows.Count -and $i -lt $req.Items.Count; $i++) {
        $item = $req.Items[$i]
        $rows[$i].Dot.Tag = $colors[$item.State]
        $rows[$i].Dot.AccessibleName = $stateNames[$item.State]
        $rows[$i].Dot.Invalidate()
        $rows[$i].Line.Text = $item.Label
        $tips.SetToolTip($rows[$i].Line, $item.Text)
    }
    # waiting for Docker to start: look again every few seconds, so its
    # light comes on without a click
    $running = $req.Items | Where-Object { $_.Key -eq "running" }
    if ($running.State -ne "ok" -and -not $script:checkDue) {
        $script:checkDue = [DateTime]::Now.AddSeconds(5)
    }
    Update-Controls
}

# ---------------------------------------------------------------------------
# The build: build.ps1, run by the same PowerShell as this window

function Start-Build {
    if ($script:runner -or -not ($script:req -and $script:req.Ready)) { return }
    Save-Settings
    $action = Get-Action
    $argv = @("-NoProfile", "-ExecutionPolicy", "Bypass",
              "-File", (Format-Arg (Join-Path $root "build.ps1")))
    $argv += switch ($action) { "clean" { "-Clean" } "sim" { "-Sim" } default { "-Rom" } }
    $data = Get-DataArg
    # the folder holds another game than the one extracted: without reading
    # the files again, build.ps1 would build the old one
    $switch = $script:req.Switching -and -not $cbAssets.Checked -and -not $data
    if ($cbAssets.Checked -or $switch) { $argv += "-Assets" }
    if ($data) { $argv += @("-Data", (Format-Arg $data)) }
    $name = $txtName.Text.Trim()
    if ($name -and $action -ne "sim") { $argv += @("-Name", (Format-Arg $name)) }

    $exe = (Get-Process -Id $PID).Path
    $txtLog.Clear()
    $txtLog.AppendText("> build.ps1 " + (($argv | Select-Object -Skip 5) -join " ") + [Environment]::NewLine)
    if ($switch) {
        $txtLog.AppendText("(-Assets: the folder holds another game than the one extracted, so it is read again)" +
                           [Environment]::NewLine)
    }
    $script:stopped = $false
    $script:result = ""
    $script:lastFile = $null
    $script:runner = New-Object Wolf3dBuildGui.Runner
    try {
        $script:runner.Start($exe, ($argv -join " "), $root)
    } catch {
        $script:runner = $null
        $script:result = "Could not start the build: $($_.Exception.Message)"
        $script:resultColor = $colors.missing
        Update-Controls
        return
    }
    $lblStatus.Text = if ($action -eq "sim") { "Running the checks..." } else { "Building..." }
    $lblStatus.ForeColor = [System.Drawing.SystemColors]::ControlText
    Update-Controls
}

function Receive-Output {
    $line = $null
    $sb = New-Object System.Text.StringBuilder
    while ($script:runner.Lines.TryDequeue([ref]$line)) {
        # the compiler colors its messages for a terminal; not here
        $line = $line -replace "\x1b\[[0-9;]*[A-Za-z]", ""
        [void]$sb.AppendLine($line)
        # build.ps1's last line, "Wolfenstein 3D.z64  3,031,040 bytes" - two
        # spaces after the name, which may have single ones in it, and the
        # thousands marked as the language Windows is set to marks them
        if ($line -match '^\s*(\S.*?\.z64)  +(\S.*) bytes\s*$') {
            $script:lastFile = Join-Path $root $matches[1]
            $script:result = "Built $($matches[1]), $($matches[2]) bytes."
        }
    }
    if ($sb.Length) {
        $txtLog.AppendText($sb.ToString())
        # to the end, where the new lines are - AppendText only gets there
        # for sure while the box has the focus (WM_VSCROLL, SB_BOTTOM)
        [void][Wolf3dBuildGui.Native]::SendMessagePtr($txtLog.Handle, 0x115, [IntPtr]7, [IntPtr]::Zero)
    }
}

function Complete-Build {
    $script:runner.Process.WaitForExit()        # the last of its output
    Receive-Output
    $code = $script:runner.Process.ExitCode
    $action = Get-Action
    $script:runner = $null
    if ($script:stopped) {
        $script:result = "Stopped."
        $script:resultColor = $colors.later
        $script:lastFile = $null
    } elseif ($code -ne 0) {
        $script:result = "The build failed - see its output below."
        $script:resultColor = $colors.missing
        $script:lastFile = $null
    } else {
        if ($action -eq "sim") {
            $sheet = Join-Path $root "build\sim\sheet.png"
            $script:lastFile = $sheet
            $script:result = "The checks ran: frames in build\sim, and sheet.png."
        } elseif (-not $script:result) {
            $script:result = "Built."
        }
        $script:resultColor = $colors.ok
    }
    Start-Check                                  # the assets or image may be new
    Update-Controls
}

function Stop-Build {
    if (-not $script:runner) { return }
    $script:stopped = $true
    $lblStatus.Text = "Stopping..."
    $id = $script:runner.Process.Id
    # the build script, and everything it started...
    Invoke-Native { taskkill /T /F /PID $id *> $null }
    # ...and its container, which carries on otherwise: only this project's,
    # which build.ps1 labels with its folder
    $ids = @(Invoke-Native { docker ps -q --filter "label=wolf3d-n64.project=$root" 2>$null })
    if ($ids.Count) { Invoke-Native { docker kill $ids *> $null } }
}

# ---------------------------------------------------------------------------
# Events

function Invoke-Safely([scriptblock]$Block) {
    try { & $Block } catch {
        [void][System.Windows.Forms.MessageBox]::Show($form, $_.ToString(), $form.Text, "OK", "Error")
    }
}

function Select-Folder([System.Windows.Forms.TextBox]$Box, [string]$Description) {
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = $Description
    $dlg.ShowNewFolderButton = $false
    $start = $Box.Text.Trim()
    if ($start -and (Test-Path -LiteralPath $start)) { $dlg.SelectedPath = $start }
    if ($dlg.ShowDialog($form) -eq "OK") { $Box.Text = $dlg.SelectedPath }
}

$btnData.Add_Click({ Invoke-Safely { Select-Folder $txtData "The folder with your copy of the game" } })
$btnDataReset.Add_Click({ $txtData.Text = $gamedata })
$btnCheck.Add_Click({ Invoke-Safely { Start-Check } })
$btnBuild.Add_Click({ Invoke-Safely { Start-Build } })
$btnStop.Add_Click({ Invoke-Safely { Stop-Build } })
$btnShow.Add_Click({
    Invoke-Safely {
        # the ROM (or the checks' contact sheet) just built, picked out -
        # or, with nothing built yet, the project folder
        if ($script:lastFile -and (Test-Path -LiteralPath $script:lastFile)) {
            Start-Process explorer.exe "/select,`"$script:lastFile`""
        } else {
            Start-Process explorer.exe "`"$root`""
        }
    }
})

# a change to a folder or the name: check again once the typing stops
$changed = { $script:checkDue = [DateTime]::Now.AddMilliseconds(600); Update-Controls }
$txtData.Add_TextChanged($changed)
$txtName.Add_TextChanged($changed)
foreach ($rb in $rbRom, $rbClean, $rbSim) { $rb.Add_CheckedChanged({ Update-Controls }) }

# back from another window - where Docker may have been started, or the
# game files copied - look again
$form.Add_Activated({
    if (-not $script:runner -and ([DateTime]::Now - $script:lastCheck).TotalSeconds -gt 2) {
        $script:checkDue = [DateTime]::Now
    }
})

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 100
$timer.Add_Tick({
    try {
        if ($script:runner) {
            Receive-Output
            if ($script:runner.Process.HasExited) { Complete-Build }
        }
        if ($script:check -and $script:check.Handle.IsCompleted) { Receive-Check }
        if ($script:checkDue -and [DateTime]::Now -ge $script:checkDue -and -not $script:runner) {
            Start-Check
        }
    } catch {
        $txtLog.AppendText("[window] " + $_.ToString() + [Environment]::NewLine)
    }
})

$form.Add_Load({
    # the grey hints inside the empty boxes (EM_SETCUEBANNER)
    $cue = @{ $txtData = "the gamedata folder in this project"; $txtName = "automatic" }
    foreach ($box in $cue.Keys) {
        [void][Wolf3dBuildGui.Native]::SendMessage($box.Handle, 0x1501, [IntPtr]1, $cue[$box])
    }
    $form.MinimumSize = $form.Size
    $txtLog.Text = "The build's output will show here."
    Start-Check
    $timer.Start()
})

$form.Add_FormClosing({
    param($s, $e)
    if ($script:runner) {
        $answer = [System.Windows.Forms.MessageBox]::Show($form,
            "A build is still running. Stop it and close?", $form.Text, "YesNo", "Question")
        if ($answer -ne "Yes") { $e.Cancel = $true; return }
        Stop-Build
    }
    $timer.Stop()
    Save-Settings
})

# ---------------------------------------------------------------------------

$settings = Read-Settings
switch ($settings.Action) {
    "clean" { $rbClean.Checked = $true }
    "sim"   { $rbSim.Checked = $true }
    default { $rbRom.Checked = $true }
}
$cbAssets.Checked = [bool]$settings.Assets
$txtData.Text = $settings.Data
$txtName.Text = $settings.Name
$script:checkDue = $null                        # Load checks, once
Update-Controls

[void]$form.ShowDialog()
$form.Dispose()
