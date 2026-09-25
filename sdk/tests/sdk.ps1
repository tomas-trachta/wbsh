#Requires -Version 5.1
<#
.SYNOPSIS
    Integration checks for the wbsh SDK.
.DESCRIPTION
    Drives the real binaries with the sample utils installed: they must be
    listed, a util's command must behave like any other command, a DLL that
    is not a util must be skipped without taking the good ones with it, and
    the pick and procs samples must take keys and draw inside a pseudoconsole.
#>
[CmdletBinding()]
param(
    [string]$Shell,
    [string]$PluginDir
)

$ErrorActionPreference = 'Stop'
$script:Failures = 0

function Assert-That {
    param([string]$Name, [bool]$Condition, [string]$Detail = '')

    if ($Condition) {
        Write-Host "ok   $Name"
        return
    }

    Write-Host "FAIL $Name"
    if ($Detail) { Write-Host "     $Detail" }
    $script:Failures++
}

# Stderr from a native program arrives as an error record, and a util that
# is skipped is expected to say so there, so this one call stops treating
# that as fatal.
function Invoke-Shell {
    param([string]$Script)

    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        return (& $Shell -r -c $Script 2>&1 | Out-String)
    }
    finally {
        $ErrorActionPreference = $previous
    }
}

if (-not $Shell) {
    $root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
    $Shell = Join-Path $root 'x64\Release\wbsh.exe'
}
if (-not $PluginDir) {
    $PluginDir = Join-Path (Split-Path -Parent $Shell) 'plugins'
}

if (-not (Test-Path $Shell)) { throw "wbsh not built: $Shell" }

Write-Host "shell:   $Shell"
Write-Host "plugins: $PluginDir"

$listed = Invoke-Shell 'utils'
Assert-That 'the sample util is listed by `utils`' ($listed -match 'hello') $listed
Assert-That 'the listing carries its version and summary' `
    (($listed -match '1\.0\.0') -and ($listed -match 'worked example')) $listed

$greeting = Invoke-Shell 'hello Tomas'
Assert-That 'a util command runs' ($greeting -match 'Hello, Tomas!') $greeting

$piped = Invoke-Shell 'hello Tomas | tr a-z A-Z'
Assert-That 'a util command works in a pipeline' ($piped -match 'HELLO, TOMAS!') $piped

$status = Invoke-Shell 'hello >nul; echo status=$?'
Assert-That 'a util command sets the exit status' ($status -match 'status=0') $status

$where = Invoke-Shell 'hello --where'
Assert-That 'a util can ask the host where it is' ($where -match 'You are in') $where

$shadow = Invoke-Shell 'echo marker | wc -l'
Assert-That 'the bundled commands are untouched by any of this' ($shadow -match '1') $shadow

# A DLL that exports nothing must be skipped, and the good util must still
# load: one bad file in the folder cannot cost the others.
$decoy = Join-Path $PluginDir 'not-a-util.dll'
Copy-Item (Join-Path (Split-Path -Parent $Shell) 'wbshsdk.dll') $decoy -Force
try {
    $withDecoy = Invoke-Shell 'utils'
    Assert-That 'a DLL that is not a util does not stop the others' `
        ($withDecoy -match 'hello') $withDecoy
}
finally {
    Remove-Item $decoy -Force -ErrorAction SilentlyContinue
}

# --- the pick sample: the terminal helpers ---------------------------------

$pickListed = Invoke-Shell 'utils'
Assert-That 'the pick sample is listed by `utils`' ($pickListed -match 'pick') $pickListed

$pickTty = Invoke-Shell 'echo x | pick --tty'
Assert-That 'wbshIsTerminal sees a piped stdin as not a terminal' ($pickTty -match 'stdin=0') $pickTty

$pickUsage = Invoke-Shell 'pick --help'
Assert-That 'pick prints its usage' ($pickUsage -match 'usage: pick') $pickUsage

$pickEmpty = Invoke-Shell "printf '' | pick 2>&1; echo status=`$?"
Assert-That 'pick with nothing to choose from fails cleanly' `
    (($pickEmpty -match 'nothing to choose from') -and ($pickEmpty -match 'status=1')) $pickEmpty

# The interactive path needs a console that keys can be typed into, so the
# shell is run on a pseudoconsole of its own and driven from here. The
# picker writes its choice through a redirection, which proves the drawing
# went to the console device and not down stdout.
$ptyDriver = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class PtyDriver {
    [StructLayout(LayoutKind.Sequential)]
    struct COORD { public short X; public short Y; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public IntPtr lpReserved; public IntPtr lpDesktop; public IntPtr lpTitle;
        public int dwX; public int dwY; public int dwXSize; public int dwYSize;
        public int dwXCountChars; public int dwYCountChars; public int dwFillAttribute; public int dwFlags;
        public short wShowWindow; public short cbReserved2; public IntPtr lpReserved2;
        public IntPtr hStdInput; public IntPtr hStdOutput; public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct STARTUPINFOEX { public STARTUPINFO StartupInfo; public IntPtr lpAttributeList; }

    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess; public IntPtr hThread; public int dwProcessId; public int dwThreadId; }

    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CreatePipe(out IntPtr read, out IntPtr write, IntPtr attributes, int size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern int CreatePseudoConsole(COORD size, IntPtr input, IntPtr output, uint flags, out IntPtr console);
    [DllImport("kernel32.dll", SetLastError = true)] static extern void ClosePseudoConsole(IntPtr console);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, int flags, ref IntPtr size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute, IntPtr value, IntPtr size, IntPtr previous, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool CreateProcessW(string application, StringBuilder commandLine, IntPtr processAttributes, IntPtr threadAttributes, bool inherit, uint flags, IntPtr environment, string directory, ref STARTUPINFOEX startup, out PROCESS_INFORMATION process);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool WriteFile(IntPtr handle, byte[] bytes, int count, out int written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool ReadFile(IntPtr handle, byte[] bytes, int count, out int read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr process, out int code);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr handle);

    const uint EXTENDED_STARTUPINFO_PRESENT = 0x00080000;
    const int STARTF_USESTDHANDLES = 0x00000100;
    static readonly IntPtr PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = (IntPtr)0x00020016;

    static byte[] KeyBytes(string token) {
        switch (token) {
            case "ENTER": return new byte[] { 13 };
            case "ESC":   return new byte[] { 27 };
            case "DOWN":  return new byte[] { 27, (byte)'[', (byte)'B' };
            case "CTRLC": return new byte[] { 3 };
            default:      return Encoding.UTF8.GetBytes(token);
        }
    }

    static IntPtr PseudoConsoleAttributes(IntPtr console) {
        IntPtr size = IntPtr.Zero;
        InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
        IntPtr list = Marshal.AllocHGlobal(size);
        InitializeProcThreadAttributeList(list, 1, 0, ref size);
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, (IntPtr)IntPtr.Size, IntPtr.Zero, IntPtr.Zero);
        return list;
    }

    public static string Run(string commandLine, string[] keys, int keyDelayMs, int timeoutMs, out int exitCode) {
        IntPtr inputRead, inputWrite, outputRead, outputWrite;
        CreatePipe(out inputRead, out inputWrite, IntPtr.Zero, 0);
        CreatePipe(out outputRead, out outputWrite, IntPtr.Zero, 0);

        IntPtr console;
        COORD size; size.X = 80; size.Y = 24;
        if (CreatePseudoConsole(size, inputRead, outputWrite, 0, out console) != 0) throw new InvalidOperationException("CreatePseudoConsole failed");

        STARTUPINFOEX startup = new STARTUPINFOEX();
        startup.StartupInfo.cb = Marshal.SizeOf(typeof(STARTUPINFOEX));
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.lpAttributeList = PseudoConsoleAttributes(console);

        PROCESS_INFORMATION process;
        if (!CreateProcessW(null, new StringBuilder(commandLine), IntPtr.Zero, IntPtr.Zero, false, EXTENDED_STARTUPINFO_PRESENT, IntPtr.Zero, null, ref startup, out process)) {
            throw new InvalidOperationException("CreateProcess failed: " + Marshal.GetLastWin32Error());
        }

        StringBuilder screen = new StringBuilder();
        Thread reader = new Thread(delegate () {
            byte[] buffer = new byte[4096];
            int read;
            while (ReadFile(outputRead, buffer, buffer.Length, out read, IntPtr.Zero) && read > 0) screen.Append(Encoding.UTF8.GetString(buffer, 0, read));
        });
        reader.Start();

        foreach (string key in keys) {
            Thread.Sleep(keyDelayMs);
            byte[] bytes = KeyBytes(key);
            int written;
            WriteFile(inputWrite, bytes, bytes.Length, out written, IntPtr.Zero);
        }

        if (WaitForSingleObject(process.hProcess, (uint)timeoutMs) != 0) TerminateProcess(process.hProcess, 9);
        GetExitCodeProcess(process.hProcess, out exitCode);

        ClosePseudoConsole(console);
        CloseHandle(outputWrite);
        reader.Join();
        CloseHandle(inputRead); CloseHandle(inputWrite); CloseHandle(outputRead);
        CloseHandle(process.hProcess); CloseHandle(process.hThread);
        return screen.ToString();
    }
}
'@
Add-Type -TypeDefinition $ptyDriver

$pickedFile = Join-Path $env:TEMP 'wbsh-sdk-picked.txt'
$pickedFileForShell = $pickedFile -replace '\\', '/'

function Invoke-InConsole {
    param([string]$Script, [string[]]$Keys)

    $exit = 0
    return [PtyDriver]::Run("`"$Shell`" -r -c `"$Script; echo rc=`$?`"", $Keys, 700, 8000, [ref]$exit)
}

function Invoke-Picker {
    param([string[]]$Keys)

    Remove-Item $pickedFile -Force -ErrorAction SilentlyContinue
    $screen = Invoke-InConsole "pick apple banana cherry > '$pickedFileForShell'" $Keys
    $picked = ''
    if (Test-Path $pickedFile) { $picked = [System.IO.File]::ReadAllText($pickedFile).Trim() }
    Remove-Item $pickedFile -Force -ErrorAction SilentlyContinue
    return @{ Screen = $screen; Picked = $picked }
}

$typed = Invoke-Picker @('ban', 'ENTER')
Assert-That 'typing narrows the list and Enter picks the match' `
    (($typed.Picked -eq 'banana') -and ($typed.Screen -match 'rc=0')) $typed.Screen
Assert-That 'the picker highlights and draws on the console, not stdout' `
    ($typed.Screen -match '1/3') $typed.Screen

$moved = Invoke-Picker @('DOWN', 'DOWN', 'ENTER')
Assert-That 'arrow keys move the selection' `
    (($moved.Picked -eq 'cherry') -and ($moved.Screen -match 'rc=0')) $moved.Screen

$escaped = Invoke-Picker @('ESC')
Assert-That 'Escape cancels with status 1 and prints nothing' `
    (($escaped.Picked -eq '') -and ($escaped.Screen -match 'rc=1')) $escaped.Screen

$interrupted = Invoke-Picker @('CTRLC')
Assert-That 'Ctrl+C reaches the util as a key and ends it with status 130' `
    (($interrupted.Picked -eq '') -and ($interrupted.Screen -match 'rc=130')) $interrupted.Screen

# --- the procs sample: both hosts, a timed refresh, a piped fallback -------

$procsListed = Invoke-Shell 'utils'
Assert-That 'the procs sample is listed by `utils`' ($procsListed -match 'procs') $procsListed

$procsOnce = Invoke-Shell 'procs --once | head -1'
Assert-That 'procs --once prints the table heading' ($procsOnce -match 'PID\s+PPID\s+MEM') $procsOnce

$procsPiped = Invoke-Shell 'procs | grep -c wbsh.exe'
Assert-That 'procs into a pipe prints once and lists the shell itself' `
    ([int]($procsPiped.Trim()) -ge 1) $procsPiped

$procsBad = Invoke-Shell 'procs --bogus; echo status=$?'
Assert-That 'procs rejects an unknown option with status 2' ($procsBad -match 'status=2') $procsBad

$monitor = Invoke-InConsole 'procs' @('n', 'DOWN', 'q')
Assert-That 'the monitor draws on the console, takes keys, and q quits with status 0' `
    (($monitor -match 'processes') -and ($monitor -match 'sorted by name') -and ($monitor -match 'rc=0')) $monitor

if ($script:Failures -gt 0) {
    Write-Host "$script:Failures check(s) failed"
    exit 1
}

Write-Host 'all checks passed'
