# Presses Ctrl+Alt+Shift+Y / E for real (injected key presses) in a small test
# window and measures what the running UzAlpha does with the selected text:
# was it converted, how long did it take, and did the window that was being
# typed in stop responding in the meantime.
#
#   powershell -ExecutionPolicy Bypass -File tools\convert-shortcut-test.ps1
#
# Windows PowerShell (powershell.exe), not pwsh: the test window uses a classic
# menu bar, which newer .NET no longer has. UzAlpha must be running. The test
# window has to stay in front: do not type or click while it is open (it gives
# up if another window comes forward). Keep this file plain ASCII.
param([int]$Rounds = 3, [int]$FreezeLimitMs = 250, [switch]$Rich, [switch]$SkipIfNoForeground)
$ErrorActionPreference = 'Stop'

Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

public class ShortcutTest : Form {
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public InputUnion u; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern IntPtr GetKeyboardLayout(uint thread);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] static extern IntPtr GetClipboardOwner();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    static string OwnerClass() { IntPtr o = GetClipboardOwner(); if (o == IntPtr.Zero) return "nobody"; var s = new System.Text.StringBuilder(80); GetClassNameW(o, s, 80); string c = s.ToString(); return c.StartsWith("UzAlpha") ? c : "another program"; }

    const ushort VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12;
    readonly TextBoxBase box;
    readonly System.Windows.Forms.Timer pulse = new System.Windows.Forms.Timer();
    readonly Stopwatch clock = Stopwatch.StartNew();
    long lastPulse, maxGap;
    readonly int rounds, freezeLimit;
    public readonly List<string> Lines = new List<string>();
    public int Failures;
    public bool NoForeground;
    string savedText;

    public ShortcutTest(int rounds, int freezeLimit, bool rich) {
        if (rich) box = new RichTextBox(); else box = new TextBox();
        this.rounds = rounds; this.freezeLimit = freezeLimit;
        Text = "UzAlpha shortcut test - please do not type" + (rich ? " (rich text)" : "");
        Width = 560; Height = 220; StartPosition = FormStartPosition.CenterScreen; TopMost = true;
        // A classic menu bar: a stray Alt would open it and swallow the copy.
        Menu = new MainMenu(new[] { new MenuItem("&File"), new MenuItem("&Edit") });
        box.Multiline = true; box.Dock = DockStyle.Fill; box.Font = new System.Drawing.Font("Segoe UI", 12);
        Controls.Add(box);
        pulse.Interval = 15;
        pulse.Tick += delegate { long now = clock.ElapsedMilliseconds; if (now - lastPulse > maxGap) maxGap = now - lastPulse; lastPulse = now; };
        Shown += delegate {
            try { if (Clipboard.ContainsText()) savedText = Clipboard.GetText(); } catch { }
            try { Clipboard.SetText(Marker); } catch { }
            lastPulse = clock.ElapsedMilliseconds; pulse.Start();
            Thread t = new Thread(Scenario); t.IsBackground = true; t.Start();
        };
    }

    // Stands for whatever the user had copied before using the shortcut.
    const string Marker = "UzAlpha test: what the user had copied";
    // After a conversion has settled, the clipboard must hold the user's content again.
    void CheckClipboardPutBack(string when) {
        string now = Ui(delegate { try { return Clipboard.GetText(); } catch { return "(clipboard busy)"; } });
        if (now == Marker) { Say("ok    " + when + ": the clipboard holds the user's own content again"); return; }
        Failures++;
        Say("FAIL  " + when + ": the clipboard holds \"" + now + "\" instead of the user's content");
    }

    T Ui<T>(Func<T> f) { return (T)Invoke(f); }
    void UiDo(Action a) { Invoke(a); }
    void Say(string s) { lock (Lines) Lines.Add(s); }

    static void Key(ushort vk, bool up) {
        INPUT[] i = new INPUT[1];
        i[0].type = 1; i[0].u.ki.wVk = vk; i[0].u.ki.dwFlags = up ? 2u : 0u;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
        Thread.Sleep(25);
    }

    // The three modifiers in the given order, the letter, then everything up again.
    static void Press(ushort[] mods, char letter, bool lettersFirstUp) {
        foreach (ushort m in mods) Key(m, false);
        Key((ushort)letter, false);
        Thread.Sleep(40);
        if (lettersFirstUp) Key((ushort)letter, true);
        for (int k = mods.Length - 1; k >= 0; --k) Key(mods[k], true);
        if (!lettersFirstUp) Key((ushort)letter, true);
    }

    bool InFront() { return GetForegroundWindow() == Ui(() => Handle); }

    bool ComeToFront() {
        if (InFront()) return true;
        UiDo(delegate {
            uint fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero), me = GetCurrentThreadId();
            if (fg != 0 && fg != me) AttachThreadInput(me, fg, true);
            SetForegroundWindow(Handle); Activate(); box.Focus();
            if (fg != 0 && fg != me) AttachThreadInput(me, fg, false);
        });
        Thread.Sleep(300);
        return InFront();
    }

    void Scenario() {
        try {
            Thread.Sleep(400);
            if (FindWindowW("UzAlphaMainWindow", "UzAlpha") == IntPtr.Zero) { Say("FAIL  UzAlpha is not running"); Failures++; return; }
            if (!ComeToFront()) { Say("the test window could not come to the front"); NoForeground = true; return; }
            IntPtr layoutBefore = GetKeyboardLayout(Ui(() => GetWindowThreadProcessId(Handle, IntPtr.Zero)));

            var orders = new[] {
                new { name = "Ctrl, Alt, Shift", mods = new ushort[] { VK_CONTROL, VK_MENU, VK_SHIFT } },
                new { name = "Alt, Shift, Ctrl", mods = new ushort[] { VK_MENU, VK_SHIFT, VK_CONTROL } },
                new { name = "Shift, Ctrl, Alt", mods = new ushort[] { VK_SHIFT, VK_CONTROL, VK_MENU } },
            };
            const string sample = "shahar o'zbek choy g'isht";
            int press = 0;
            for (int r = 0; r < rounds; ++r) {
                var o = orders[r % orders.Length];
                UiDo(delegate { box.Text = sample; });
                foreach (char letter in new[] { 'Y', 'E', 'Y', 'E' }) {
                    ++press;
                    if (!InFront() && !ComeToFront()) { Say("stopped: another window came to the front"); NoForeground = true; return; }
                    string before = Ui(delegate { box.Focus(); box.SelectAll(); return box.Text; });
                    string owner = OwnerClass();
                    Thread.Sleep(60);
                    maxGap = 0; lastPulse = clock.ElapsedMilliseconds;
                    long start = clock.ElapsedMilliseconds;
                    Press(o.mods, letter, press % 2 == 0);
                    string after = before; long took = -1;
                    while (clock.ElapsedMilliseconds - start < 3500) {
                        Thread.Sleep(20);
                        after = Ui(() => box.Text);
                        if (after != before) { took = clock.ElapsedMilliseconds - start; break; }
                    }
                    Thread.Sleep(150);  // anything still arriving (a second paste, a stray letter)?
                    after = Ui(() => box.Text);
                    bool wantNew = letter == 'Y';
                    // Code points, not letters: Windows PowerShell reads a script without
                    // a byte order mark in the system code page.
                    bool hasNewS = after.IndexOf((char)0x015F) >= 0, hasNewO = after.IndexOf((char)0x00F6) >= 0;
                    bool right = wantNew ? (hasNewS && hasNewO && !after.Contains("sh"))
                                         : (after.Contains("sh") && after.Contains("ch") && !hasNewS);
                    bool frozen = maxGap > freezeLimit;
                    bool ok = took >= 0 && right && !frozen;
                    if (!ok) Failures++;
                    Say(String.Format("{0}  press {1,2}: {2} + {3}  {4,-12} window stalled {5,4} ms  clipboard owner before: {6}  -> \"{7}\"",
                        ok ? "ok  " : "FAIL", press, o.name, letter,
                        took >= 0 ? "changed in " + took + " ms" : "NOT CHANGED", maxGap, owner, after.Replace((char)10, ' ').Replace((char)13, ' ')));
                    // Odd rounds press again at once, even rounds let UzAlpha settle first.
                    Thread.Sleep(r % 2 == 0 ? 1300 : 250);
                }
                if (r % 2 != 0) Thread.Sleep(1200);
                CheckClipboardPutBack("after round " + (r + 1));
            }

            // Nothing selected: nothing may be typed, pasted or left on the clipboard.
            if (InFront() || ComeToFront()) {
                string before = Ui(delegate { box.Focus(); box.Select(box.TextLength, 0); return box.Text; });
                maxGap = 0; lastPulse = clock.ElapsedMilliseconds;
                Press(orders[0].mods, 'Y', true);
                Thread.Sleep(1700);
                string after = Ui(() => box.Text);
                bool ok = after == before && maxGap <= freezeLimit;
                if (!ok) Failures++;
                Say(String.Format("{0}  nothing selected: text {1}, window stalled {2} ms", ok ? "ok  " : "FAIL",
                    after == before ? "left alone" : "CHANGED", maxGap));
                CheckClipboardPutBack("after a press with nothing selected");
            }
            IntPtr layoutAfter = GetKeyboardLayout(Ui(() => GetWindowThreadProcessId(Handle, IntPtr.Zero)));
            if (layoutAfter != layoutBefore) { Say(String.Format("FAIL  the keyboard changed during the test: {0:x8} -> {1:x8}", layoutBefore.ToInt64() & 0xffffffff, layoutAfter.ToInt64() & 0xffffffff)); Failures++; }
            Thread.Sleep(1300);
        } catch (Exception e) {
            Say("FAIL  " + e.Message); Failures++;
        } finally {
            try { BeginInvoke((Action)delegate {
                try { if (savedText != null) Clipboard.SetText(savedText); } catch { }
                Close();
            }); } catch { }
        }
    }

    public static ShortcutTest RunTest(int rounds, int freezeLimit, bool rich) {
        ShortcutTest form = null;
        Thread ui = new Thread(delegate () { form = new ShortcutTest(rounds, freezeLimit, rich); Application.Run(form); });
        ui.SetApartmentState(ApartmentState.STA);
        ui.Start(); ui.Join();
        return form;
    }
}
'@

$result = [ShortcutTest]::RunTest($Rounds, $FreezeLimitMs, [bool]$Rich)
$result.Lines | ForEach-Object { $_ }
if ($result.NoForeground) {
    if ($SkipIfNoForeground) { 'shortcut test skipped: no window can be brought to the front here'; exit 0 }
    'shortcut test did not run to the end'; exit 2
}
if ($result.Failures) { "$($result.Failures) problem(s)"; exit 1 }
'shortcut test passed'
