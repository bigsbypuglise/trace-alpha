# Read-only probe of the installed LucidLink Explorer shell extension.
#
# Enumerates the context menu for a file through IContextMenu -- the shell
# interface Explorer itself uses -- and reports, for every item, its position,
# its display text and its CANONICAL VERB if it exposes one. Nothing is invoked.
#
# This exists to answer one question before any code is written: does the
# extension expose a stable verb (which the spec prefers), or only a localizable
# display string (which the spec permits but requires documenting as a risk)?
#
# Read-only. It opens no file, writes nothing, and invokes no command.
param(
    [Parameter(Mandatory = $true)][string]$Path,
    # Also enumerate the extended (shift-right-click) set.
    [switch]$Extended
)

Add-Type -Namespace Lucid -Name Probe -MemberDefinition @"
[DllImport("shell32.dll", CharSet=CharSet.Unicode)]
public static extern int SHParseDisplayName(string name, IntPtr bc, out IntPtr pidl, uint sfgaoIn, out uint sfgaoOut);
[DllImport("shell32.dll")]
public static extern int SHBindToParent(IntPtr pidl, ref Guid riid, out IntPtr ppv, out IntPtr pidlLast);
[DllImport("user32.dll")]
public static extern IntPtr CreatePopupMenu();
[DllImport("user32.dll")]
public static extern bool DestroyMenu(IntPtr h);
[DllImport("user32.dll")]
public static extern int GetMenuItemCount(IntPtr h);
[DllImport("user32.dll")]
public static extern int GetMenuItemID(IntPtr h, int pos);
[DllImport("user32.dll", CharSet=CharSet.Unicode)]
public static extern int GetMenuStringW(IntPtr h, int id, System.Text.StringBuilder buf, int n, int flags);
[DllImport("ole32.dll")]
public static extern void CoTaskMemFree(IntPtr p);
"@

$src = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LucidCtx {
    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown),
     Guid("000214E6-0000-0000-C000-000000000046")]
    public interface IShellFolder {
        void ParseDisplayName(IntPtr h, IntPtr bc, [MarshalAs(UnmanagedType.LPWStr)] string d, ref uint e, out IntPtr pidl, ref uint att);
        void EnumObjects(IntPtr h, int flags, out IntPtr en);
        void BindToObject(IntPtr pidl, IntPtr bc, ref Guid riid, out IntPtr ppv);
        void BindToStorage(IntPtr pidl, IntPtr bc, ref Guid riid, out IntPtr ppv);
        void CompareIDs(IntPtr l, IntPtr p1, IntPtr p2);
        void CreateViewObject(IntPtr h, ref Guid riid, out IntPtr ppv);
        void GetAttributesOf(uint c, [MarshalAs(UnmanagedType.LPArray)] IntPtr[] a, ref uint att);
        void GetUIObjectOf(IntPtr h, uint c, [MarshalAs(UnmanagedType.LPArray)] IntPtr[] a, ref Guid riid, IntPtr r, out IntPtr ppv);
        void GetDisplayNameOf(IntPtr pidl, uint f, IntPtr name);
        void SetNameOf(IntPtr h, IntPtr pidl, [MarshalAs(UnmanagedType.LPWStr)] string n, uint f, out IntPtr o);
    }

    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown),
     Guid("000214E4-0000-0000-C000-000000000046")]
    public interface IContextMenu {
        [PreserveSig] int QueryContextMenu(IntPtr menu, uint indexMenu, uint idFirst, uint idLast, uint flags);
        [PreserveSig] int InvokeCommand(ref CMINVOKECOMMANDINFOEX ici);
        [PreserveSig] int GetCommandString(IntPtr idCmd, uint uType, IntPtr res, IntPtr commandString, int cch);
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct CMINVOKECOMMANDINFOEX {
        public int cbSize; public int fMask; public IntPtr hwnd;
        public IntPtr lpVerb;
        [MarshalAs(UnmanagedType.LPStr)] public string lpParameters;
        [MarshalAs(UnmanagedType.LPStr)] public string lpDirectory;
        public int nShow; public int dwHotKey; public IntPtr hIcon;
        [MarshalAs(UnmanagedType.LPStr)] public string lpTitle;
        public IntPtr lpVerbW;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpParametersW;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpDirectoryW;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpTitleW;
        public int ptX; public int ptY;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    static extern int SHParseDisplayName(string name, IntPtr bc, out IntPtr pidl, uint inAtt, out uint outAtt);
    [DllImport("shell32.dll")]
    static extern int SHBindToParent(IntPtr pidl, ref Guid riid, out IntPtr ppv, out IntPtr pidlLast);
    [DllImport("ole32.dll")] static extern void CoTaskMemFree(IntPtr p);
    [DllImport("user32.dll")] static extern IntPtr CreatePopupMenu();
    [DllImport("user32.dll")] static extern bool DestroyMenu(IntPtr h);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(IntPtr h);
    [DllImport("user32.dll")] static extern int GetMenuItemID(IntPtr h, int pos);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetMenuStringW(IntPtr h, int id, StringBuilder buf, int n, int flags);

    const uint GCS_VERBW = 0x00000004;
    const uint CMF_NORMAL = 0x00000000;
    const uint CMF_EXTENDEDVERBS = 0x00000100;
    const uint ID_FIRST = 1;
    const uint ID_LAST = 0x7FFF;

    // Builds the context menu for `path` and returns one line per item:
    //   <cmdId>\t<verb or -->\t<display text>
    public static string[] Enumerate(string path, bool extended) {
        var lines = new System.Collections.Generic.List<string>();
        IntPtr pidl; uint att;
        int hr = SHParseDisplayName(path, IntPtr.Zero, out pidl, 0, out att);
        if (hr != 0) return new[] { "ERROR SHParseDisplayName 0x" + hr.ToString("X8") };

        Guid iidFolder = new Guid("000214E6-0000-0000-C000-000000000046");
        IntPtr folderPtr, childPidl;
        hr = SHBindToParent(pidl, ref iidFolder, out folderPtr, out childPidl);
        if (hr != 0) { CoTaskMemFree(pidl); return new[] { "ERROR SHBindToParent 0x" + hr.ToString("X8") }; }

        var folder = (IShellFolder)Marshal.GetObjectForIUnknown(folderPtr);
        Guid iidCtx = new Guid("000214E4-0000-0000-C000-000000000046");
        IntPtr ctxPtr;
        folder.GetUIObjectOf(IntPtr.Zero, 1, new[] { childPidl }, ref iidCtx, IntPtr.Zero, out ctxPtr);
        var ctx = (IContextMenu)Marshal.GetObjectForIUnknown(ctxPtr);

        IntPtr menu = CreatePopupMenu();
        uint flags = CMF_NORMAL | (extended ? CMF_EXTENDEDVERBS : 0);
        ctx.QueryContextMenu(menu, 0, ID_FIRST, ID_LAST, flags);

        int count = GetMenuItemCount(menu);
        for (int i = 0; i < count; i++) {
            int id = GetMenuItemID(menu, i);
            var text = new StringBuilder(512);
            GetMenuStringW(menu, i, text, text.Capacity, 0x0400 /*MF_BYPOSITION*/);
            string verb = "--";
            if (id > 0) {
                IntPtr buf = Marshal.AllocHGlobal(512);
                try {
                    int r = ctx.GetCommandString((IntPtr)(id - ID_FIRST), GCS_VERBW, IntPtr.Zero, buf, 256);
                    if (r == 0) { string v = Marshal.PtrToStringUni(buf); if (!string.IsNullOrEmpty(v)) verb = v; }
                } catch { } finally { Marshal.FreeHGlobal(buf); }
            }
            lines.Add(id + "\t" + verb + "\t" + text.ToString().Replace("\t", " "));
        }

        DestroyMenu(menu);
        Marshal.ReleaseComObject(ctx);
        Marshal.ReleaseComObject(folder);
        CoTaskMemFree(pidl);
        return lines.ToArray();
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

Write-Output "context menu for: $Path"
Write-Output "extended: $Extended"
Write-Output "id`tverb`ttext"
[LucidCtx]::Enumerate($Path, [bool]$Extended) | ForEach-Object { Write-Output $_ }
