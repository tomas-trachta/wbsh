# wbsh in editors and terminals

How to make wbsh the shell of the terminal you already use. The paths below
are the installer's defaults (`%LOCALAPPDATA%\Programs\wbsh`); use your own
folder if you chose another one or unpacked a portable ZIP.

For coding agents (Claude Code, Codex, OpenCode, ...) see [agents.md](agents.md).

## Windows Terminal

The installer's **Add a wbsh profile to Windows Terminal** task (on by
default) places a profile fragment in
`%LOCALAPPDATA%\Microsoft\Windows Terminal\Fragments\wbsh\wbsh.json`. Windows
Terminal, Store or unpackaged, picks it up on its next start: a **wbsh** entry
appears in the new-tab menu with the bundled `wbsh` colour scheme.
`settings.json` is not touched, and uninstalling wbsh removes the fragment.

Without the installer, copy
[`installer/windows-terminal/wbsh.json`](../installer/windows-terminal/wbsh.json)
to that folder yourself, and change `commandline` and `icon` if `wbsh.exe` is
somewhere else. To make wbsh the default profile, pick it under
**Settings > Startup > Default profile**.

## VS Code (and Cursor, Windsurf, other VS Code builds)

Add a terminal profile to your user `settings.json`
(**Preferences: Open User Settings (JSON)**):

```jsonc
{
    "terminal.integrated.profiles.windows": {
        "wbsh": {
            "path": "${env:LOCALAPPDATA}\\Programs\\wbsh\\wbsh.exe",
            "args": ["-i"],
            "icon": "terminal-bash"
        }
    },
    // Optional: open new terminals in wbsh.
    "terminal.integrated.defaultProfile.windows": "wbsh"
}
```

Point `path` at `wbsh.exe` rather than at the `bash.exe` shim: VS Code
recognises an executable named `bash` and adds its own bash integration
arguments, which wbsh has not been tested with.

Tasks (`tasks.json`) with `"type": "shell"` use the default profile's shell
unless they set `options.shell`:

```jsonc
"options": {
    "shell": {
        "executable": "${env:LOCALAPPDATA}\\Programs\\wbsh\\wbsh.exe",
        "args": ["-c"]
    }
}
```

## JetBrains IDEs

**Settings > Tools > Terminal > Shell path**:

```
"%LOCALAPPDATA%\Programs\wbsh\wbsh.exe" -i
```

## Other tools: `SHELL` and the `bash.exe` shims

Two optional installer tasks cover programs that look for a shell rather than
being configured with one:

- **Install bash.exe and sh.exe shims** puts copies of wbsh named `bash.exe`
  and `sh.exe` in `%LOCALAPPDATA%\Programs\wbsh\bin` and adds that folder to
  the user `PATH`. As `sh.exe`, wbsh starts in POSIX mode. The user `PATH`
  comes after the system one, so where WSL is installed, a bare `bash` typed
  in cmd or PowerShell still starts WSL's `C:\Windows\System32\bash.exe`;
  tools that take a full path (for example Claude Code's
  `CLAUDE_CODE_GIT_BASH_PATH`) are not affected.
- **Set SHELL to wbsh for your user** sets `SHELL` to the full path of
  `wbsh.exe`, for editors, `make`, `script` and others that read it.
  Uninstalling removes it while it still names this wbsh.

## Command marks (shell integration)

wbsh marks each prompt and command with OSC 133 and reports its directory
with OSC 7. Terminals that read OSC 133 can use them without a shell script
of their own: Windows Terminal for its scrollbar marks and `scrollToMark`
actions (with `showMarksOnScrollbar` on), VS Code for its command
decorations. Nothing needs turning on in wbsh; a terminal that does not know
the sequences ignores them. See
[prompts.md](prompts.md#shell-integration) for what is sent, and for prompt
frameworks and completion menus.
