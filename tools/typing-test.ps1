# Types words with real (injected) key presses through the installed UzAlpha
# "Avtomatik" keyboard into three kinds of text boxes, and compares what
# arrives with what should have been typed:
#
#   edit     a classic Windows edit box (talks to the keyboard the old IMM way)
#   rich     a rich-text box (talks to it the TSF way)
#   keydown  a box that takes each letter at key-down time and the keyboard's
#            own text as a separate message, the way Qt programs (Telegram) and
#            terminals built on xterm.js (Termius, VS Code) do. This is the one
#            where a letter the keyboard is still holding can be overtaken by
#            the next key: "server" arriving as "esrver".
#
#   powershell -ExecutionPolicy Bypass -File tools\typing-test.ps1
#
# Windows PowerShell (powershell.exe), not pwsh. UzAlpha must be installed. The
# test window has to stay in front: do not type or click while it is open. The
# keyboard that was active before is put back at the end. Keep this file ASCII.
param([string[]]$Hosts = @('edit', 'rich', 'keydown'), [int]$KeyGapMs = 35, [switch]$SkipIfNoForeground)
$ErrorActionPreference = 'Stop'

Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[StructLayout(LayoutKind.Sequential)]
public struct TF_INPUTPROCESSORPROFILE {
    public uint dwProfileType; public ushort langid; public Guid clsid; public Guid guidProfile; public Guid catid;
    public IntPtr hklSubstitute; public uint dwCaps; public IntPtr hkl; public uint dwFlags;
}

[ComImport, Guid("71C6E74C-0F28-11D8-A82A-00065B84435C"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ITfInputProcessorProfileMgr {
    [PreserveSig] int ActivateProfile(uint type, ushort langid, ref Guid clsid, ref Guid profile, IntPtr hkl, uint flags);
    [PreserveSig] int DeactivateProfile();   // the next six only hold their places in the interface
    [PreserveSig] int GetProfile();
    [PreserveSig] int EnumProfiles();
    [PreserveSig] int ReleaseInputProcessor();
    [PreserveSig] int RegisterProfile();
    [PreserveSig] int UnregisterProfile();
    [PreserveSig] int GetActiveProfile(ref Guid catid, out TF_INPUTPROCESSORPROFILE profile);
}

// Takes letters at key-down time and the keyboard's finished text from
// WM_IME_COMPOSITION, and ignores WM_CHAR: see the top of the script.
public class KeyDownBox : Control {
    [DllImport("user32.dll")] static extern bool GetKeyboardState(byte[] state);
    [DllImport("user32.dll")] static extern IntPtr GetKeyboardLayout(uint thread);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int ToUnicodeEx(uint vk, uint scan, byte[] state, StringBuilder buf, int size, uint flags, IntPtr hkl);
    [DllImport("imm32.dll")] static extern IntPtr ImmGetContext(IntPtr hwnd);
    [DllImport("imm32.dll")] static extern bool ImmReleaseContext(IntPtr hwnd, IntPtr himc);
    [DllImport("imm32.dll", CharSet = CharSet.Unicode)] static extern int ImmGetCompositionStringW(IntPtr himc, int index, byte[] buf, int len);

    public readonly StringBuilder Typed = new StringBuilder();
    public KeyDownBox() { SetStyle(ControlStyles.Selectable | ControlStyles.UserPaint, true); TabStop = true; BackColor = System.Drawing.Color.White; }
    protected override bool IsInputKey(Keys keyData) { return true; }
    protected override bool IsInputChar(char charCode) { return true; }
    protected override void OnPaint(PaintEventArgs e) { e.Graphics.DrawString(Typed.ToString(), Font, System.Drawing.Brushes.Black, 4, 4); }

    protected override void WndProc(ref Message m) {
        switch (m.Msg) {
            case 0x0087:  // WM_GETDLGCODE: all keys, please
                m.Result = (IntPtr)(0x0004 | 0x0080); return;
            case 0x0100:    // WM_KEYDOWN
            case 0x0290: {  // WM_IME_KEYDOWN: a key the keyboard hands back after its own text
                int vk = (int)m.WParam;
                if (vk == 0xE5) break;  // VK_PROCESSKEY: the keyboard is dealing with it
                if (vk == 0x08) { if (Typed.Length > 0) Typed.Length--; Invalidate(); return; }
                if (vk == 0x0D) { Typed.Append('\n'); Invalidate(); return; }
                byte[] state = new byte[256]; GetKeyboardState(state);
                StringBuilder buf = new StringBuilder(8);
                int n = ToUnicodeEx((uint)vk, (uint)(((long)m.LParam >> 16) & 0xFF), state, buf, 8, 0x4, GetKeyboardLayout(0));
                if (n >= 1 && buf[0] >= 0x20) { Typed.Append(buf.ToString(0, n)); Invalidate(); }
                return;
            }
            case 0x0102: case 0x0286:  // WM_CHAR, WM_IME_CHAR: already taken at key-down time
                return;
            case 0x010F: {  // WM_IME_COMPOSITION
                if (((long)m.LParam & 0x0800) != 0) {  // GCS_RESULTSTR: finished text
                    IntPtr himc = ImmGetContext(Handle);
                    int bytes = ImmGetCompositionStringW(himc, 0x0800, null, 0);
                    if (bytes > 0) { byte[] b = new byte[bytes]; ImmGetCompositionStringW(himc, 0x0800, b, bytes); Typed.Append(Encoding.Unicode.GetString(b)); }
                    ImmReleaseContext(Handle, himc);
                    Invalidate();
                }
                return;
            }
            case 0x010D: case 0x010E:  // WM_IME_STARTCOMPOSITION / ENDCOMPOSITION: no separate composition window
                return;
        }
        base.WndProc(ref m);
    }
}

public class TypingTest : Form {
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public InputUnion u; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern uint MapVirtualKeyW(uint code, uint type);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();

    static Guid ClsidUzAlpha = new Guid("D6C739FF-D990-418B-8DDE-DA75436B5F08");
    static Guid ProfileAuto = new Guid("81E0DDC7-68E8-4A0F-8CF3-4CBCCE454F13");
    static Guid CatKeyboard = new Guid("34745C63-B2F0-4784-8B67-5E12C8701A31");  // GUID_TFCAT_TIP_KEYBOARD
    const uint ForSession = 0x20000000;  // TF_IPPMF_FORSESSION: the way the UzAlpha window itself switches

    readonly string[] hosts;
    readonly int gap;
    public readonly List<string> Lines = new List<string>();
    public int Failures;
    public bool NoForeground;
    Control box;
    ITfInputProcessorProfileMgr profiles;
    TF_INPUTPROCESSORPROFILE before;
    bool haveBefore;

    public TypingTest(string[] hosts, int gap) {
        this.hosts = hosts; this.gap = gap;
        Text = "UzAlpha typing test - please do not type";
        Width = 560; Height = 200; StartPosition = FormStartPosition.CenterScreen; TopMost = true;
        Shown += delegate { Thread t = new Thread(Scenario); t.IsBackground = true; t.Start(); };
    }

    T Ui<T>(Func<T> f) { return (T)Invoke(f); }
    void UiDo(Action a) { Invoke(a); }
    void Say(string s) { lock (Lines) Lines.Add(s); }

    void Key(ushort vk, bool up) {
        INPUT[] i = new INPUT[1];
        i[0].type = 1; i[0].u.ki.wVk = vk; i[0].u.ki.wScan = (ushort)MapVirtualKeyW(vk, 0); i[0].u.ki.dwFlags = up ? 2u : 0u;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
        Thread.Sleep(gap / 2);
    }

    // Letters, space, apostrophe, grave; "<" is Backspace, "~" is Enter. Capitals are typed with Shift.
    void TypeKeys(string keys) {
        foreach (char c in keys) {
            ushort vk; bool shift = false;
            if (c >= 'a' && c <= 'z') vk = (ushort)(c - 'a' + 'A');
            else if (c >= 'A' && c <= 'Z') { vk = (ushort)c; shift = true; }
            else if (c >= '0' && c <= '9') vk = (ushort)c;
            else if (c == ' ') vk = 0x20;
            else if (c == '\'') vk = 0xDE;
            else if (c == '`') vk = 0xC0;
            else if (c == '.') vk = 0xBE;
            else if (c == '<') vk = 0x08;
            else if (c == '~') vk = 0x0D;
            else throw new ArgumentException("no key for " + c);
            if (shift) Key(0x10, false);
            Key(vk, false); Key(vk, true);
            if (shift) Key(0x10, true);
            Thread.Sleep(gap);
        }
    }

    bool InFront() { return GetForegroundWindow() == Ui(() => Handle); }
    bool ComeToFront() {
        if (InFront()) return true;
        UiDo(delegate {
            uint fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero), me = GetCurrentThreadId();
            if (fg != 0 && fg != me) AttachThreadInput(me, fg, true);
            SetForegroundWindow(Handle); Activate();
            if (fg != 0 && fg != me) AttachThreadInput(me, fg, false);
        });
        Thread.Sleep(300);
        return InFront();
    }

    string Read() {
        return Ui(delegate {
            KeyDownBox k = box as KeyDownBox;
            string s = k != null ? k.Typed.ToString() : box.Text;
            return s.Replace("\r\n", "\n").Replace('\r', '\n');
        });
    }
    void Clear() {
        UiDo(delegate {
            KeyDownBox k = box as KeyDownBox;
            if (k != null) { k.Typed.Length = 0; k.Invalidate(); } else box.Text = "";
            box.Focus();
        });
    }
    // {sh} {ch} {o} {g} {t} stand for the new letters and the tutuq mark, by code
    // point: this file stays ASCII (Windows PowerShell reads it in the system code page).
    static string L(int code) { return ((char)code).ToString(); }
    static string New(string s) {
        return s.Replace("{sh}", L(0x015F)).Replace("{SH}", L(0x015E)).Replace("{ch}", L(0x00E7)).Replace("{CH}", L(0x00C7))
                .Replace("{o}", L(0x00F6)).Replace("{g}", L(0x011F)).Replace("{t}", L(0x02BC));
    }
    static string Show(string s) {
        StringBuilder b = new StringBuilder();
        foreach (char c in s) { if (c == '\n') b.Append("\\n"); else if (c < 0x7F) b.Append(c); else b.AppendFormat("[{0:x4}]", (int)c); }
        return b.ToString();
    }

    void Scenario() {
        try {
            Thread.Sleep(400);
            if (!ComeToFront()) { Say("the test window could not come to the front"); NoForeground = true; return; }

            // What is typed, and what should arrive ("<" is Backspace, "~" is Enter).
            string[][] cases = new string[][] {
                new[] { "server ",        "server " },
                new[] { "shahar ",        "{sh}ahar " },
                new[] { "cho'l so'z ",    "{ch}{o}l s{o}z " },
                new[] { "g'isht ",        "{g}i{sh}t " },
                new[] { "Shahar CHoy ",   "{SH}ahar {CH}oy " },
                new[] { "class coo go ",  "class coo go " },
                new[] { "sos.",           "sos." },
                new[] { "ma'no ",         "ma{t}no " },
                new[] { "ish<x",          "ishx" },
                new[] { "as<x",           "ax" },
                new[] { "los~sa",         "los\nsa" },
                new[] { "tog~~x",         "tog\n\nx" },
            };

            foreach (string host in hosts) {
                UiDo(delegate {
                    if (box != null) { Controls.Remove(box); box.Dispose(); }
                    if (host == "rich") { RichTextBox r = new RichTextBox(); r.Multiline = true; box = r; }
                    else if (host == "keydown") box = new KeyDownBox();
                    else { TextBox t = new TextBox(); t.Multiline = true; t.AcceptsReturn = true; box = t; }
                    box.Dock = DockStyle.Fill; box.Font = new System.Drawing.Font("Segoe UI", 12);
                    Controls.Add(box); box.Focus();
                    Text = "UzAlpha typing test (" + host + ") - please do not type";
                });
                Thread.Sleep(250);
                // The keyboard is chosen from the window's own thread, as the user would with Win+Space.
                int hr = Ui(delegate {
                    if (profiles == null) {
                        profiles = (ITfInputProcessorProfileMgr)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("33C53A50-F456-4884-B049-85FD643ECFED")));
                        haveBefore = profiles.GetActiveProfile(ref CatKeyboard, out before) == 0;
                    }
                    return profiles.ActivateProfile(1, 0x0443, ref ClsidUzAlpha, ref ProfileAuto, IntPtr.Zero, ForSession);
                });
                if (hr != 0) { Say(String.Format("FAIL  the Avtomatik keyboard could not be chosen ({0:x8}); is UzAlpha installed?", hr)); Failures++; return; }
                Thread.Sleep(500);

                int bad = 0;
                foreach (string[] c in cases) {
                    if (!InFront() && !ComeToFront()) { Say("stopped: another window came to the front"); NoForeground = true; return; }
                    Clear();
                    Thread.Sleep(120);
                    TypeKeys(c[0]);
                    // Whatever is still held (a last s, c, o or g) is not text yet; let it settle.
                    Thread.Sleep(250);
                    string got = Read();
                    string want = New(c[1]);
                    bool ok = got == want;
                    if (!ok) { bad++; Failures++; }
                    Say(String.Format("{0}  {1,-8} typed {2,-16} -> {3}{4}", ok ? "ok  " : "FAIL", host, "\"" + c[0] + "\"", "\"" + Show(got) + "\"",
                        ok ? "" : "   expected \"" + Show(want) + "\""));
                }
            }
            // Which copy of the keyboard did the typing?
            foreach (System.Diagnostics.ProcessModule m in System.Diagnostics.Process.GetCurrentProcess().Modules) {
                if (String.Equals(m.ModuleName, "UzAlphaTip.dll", StringComparison.OrdinalIgnoreCase)) {
                    Say("      keyboard service used: " + m.FileName + " (" + m.FileVersionInfo.FileVersion + ")");
                }
            }
        } catch (Exception e) {
            Say("FAIL  " + e.Message); Failures++;
        } finally {
            try { Invoke((Action)delegate {
                // Back to the keyboard that was active before the test.
                if (profiles != null && haveBefore) {
                    Guid clsid = before.clsid, profile = before.guidProfile;
                    profiles.ActivateProfile(before.dwProfileType, before.langid, ref clsid, ref profile, before.hkl, ForSession);
                }
                Close();
            }); } catch { }
        }
    }

    public static TypingTest RunTest(string[] hosts, int gap) {
        TypingTest form = null;
        Thread ui = new Thread(delegate () { form = new TypingTest(hosts, gap); Application.Run(form); });
        ui.SetApartmentState(ApartmentState.STA);
        ui.Start(); ui.Join();
        return form;
    }
}
'@

$result = [TypingTest]::RunTest($Hosts, $KeyGapMs)
$result.Lines | ForEach-Object { $_ }
if ($result.NoForeground) {
    if ($SkipIfNoForeground) { 'typing test skipped: no window can be brought to the front here'; exit 0 }
    'typing test did not run to the end'; exit 2
}
if ($result.Failures) { "$($result.Failures) problem(s)"; exit 1 }
'typing test passed'
