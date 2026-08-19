# What a screen reader can actually SEE of Trace's floating transport.
#
# Spec phase 14 builds an accessibility proxy tree over the composited overlay
# (plan section 19.7), and section 31.5 item 4 has said since phase 6 that "the
# overlay is not final until a screen reader has driven one". This is the part
# of that which can be automated: it walks the SAME UI Automation tree Narrator
# reads and prints every element it finds, with its name, control type and
# bounding rectangle.
#
# WHY THIS IS NOT A SUBSTITUTE FOR RUNNING NARRATOR, said plainly so the result
# is not over-claimed: UIA is the interface Narrator consumes, so an element
# absent here is certainly not announced -- but an element present here could
# still be announced badly, in the wrong order, or with a name that reads as
# nonsense aloud. This answers "is it exposed at all", which is the question
# that was open, and it answers it without a human having to listen. The
# listening is still owed.
#
# The NEGATIVE CONTROL is built in: run it on a build without the proxy tree (or
# with TRACE_TRANSPORT_BAR=1, where the transport is real widgets) and the
# output differs. Without that comparison a list of elements proves only that
# UIA works, which was never in doubt.
param(
    [string]$ProcName = "Trace",
    [int]$MaxDepth = 12
)

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no Trace window"; exit 1 }

$root = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
if (-not $root) { Write-Output "UIA could not attach to the window"; exit 1 }

Write-Output "UI Automation tree for $ProcName (pid $($p.Id))"
Write-Output ""

$script:count = 0
$script:named = 0

function Walk($element, [int]$depth) {
    if ($depth -gt $MaxDepth) { return }
    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $child = $walker.GetFirstChild($element)
    while ($child) {
        $script:count++
        $name = ""
        $type = ""
        $rect = ""
        $desc = ""
        try { $name = $child.Current.Name } catch {}
        try { $type = $child.Current.ControlType.ProgrammaticName -replace "ControlType\.", "" } catch {}
        try { $desc = $child.Current.HelpText } catch {}
        try {
            $r = $child.Current.BoundingRectangle
            if (-not [double]::IsInfinity($r.Width)) {
                $rect = "{0},{1} {2}x{3}" -f [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height
            }
        } catch {}
        if ($name) { $script:named++ }

        $indent = "  " * $depth
        Write-Output ("{0}{1,-14} {2,-34} {3}" -f $indent, $type, ("`"" + $name + "`""), $rect)
        if ($desc) { Write-Output ("{0}               help: {1}" -f $indent, $desc) }

        Walk $child ($depth + 1)
        $child = $walker.GetNextSibling($child)
    }
}

Walk $root 0

Write-Output ""
Write-Output ("elements: {0}, of which named: {1}" -f $script:count, $script:named)
Write-Output ""
Write-Output "The eight transport controls to look for, in reading order:"
Write-Output "  Rewind, Play, Fast-forward, Mute, Loop, Timeline,"
Write-Output "  Toggle Fullscreen, Share."
Write-Output "(Ten from roadmap step 5 until owner item 15, 2026-08-18, removed"
Write-Output " Go to Start and Go to End -- Home/End remain the keyboard route.)"
Write-Output "LOOP shows its state by COLOUR alone -- the package ships one loop"
Write-Output "glyph, not a pair -- so its CheckBox role is the only thing carrying"
Write-Output "that state to a screen reader. Check it reads checked/unchecked."
Write-Output "The MENU BAR and its five items appear only while the top chrome is"
Write-Output "revealed -- it is a real QMenuBar in a strip that auto-hides, so a walk"
Write-Output "taken more than kAutoHideMs after the last pointer move will not show it."
Write-Output "Absent here = certainly not announced. Present here = exposed, not yet"
Write-Output "proven to READ well; that still needs Narrator and a person listening."
