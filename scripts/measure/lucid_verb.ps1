# Drives ONLY the Lucid shell extension, not the merged Explorer context menu.
#
# The merged menu loads every registered handler into the calling process --
# Adobe, OneDrive, PowerToys, Tailscale were all present on this box -- which is
# not something a media player should do to itself. CoCreateInstance on the one
# CLSID that owns the LucidLink integration loads exactly one DLL, and its menu
# then contains only Lucid's own items, which is also what makes identifying
# "Copy link" unambiguous instead of positional. `Pin` is the item next to it and
# pinning writes to the mount, so positional identification is not acceptable.
#
# -Invoke actually runs the command. Without it this is a pure read: it builds
# the menu, reports the items, and destroys it.
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$Clsid = "{b5fd958e-0119-49bc-a0b0-24bb917d6a27}",
    [switch]$Invoke
)

$src = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LucidVerb {
    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown),
     Guid("000214E8-0000-0000-C000-000000000046")]
    public interface IShellExtInit {
        [PreserveSig] int Initialize(IntPtr pidlFolder, IntPtr dataObject, IntPtr hkeyProgID);
    }

    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown),
     Guid("000214E4-0000-0000-C000-000000000046")]
    public interface IContextMenu {
        [PreserveSig] int QueryContextMenu(IntPtr menu, uint indexMenu, uint idFirst, uint idLast, uint flags);
        [PreserveSig] int InvokeCommand(IntPtr ici);
        [PreserveSig] int GetCommandString(IntPtr idCmd, uint uType, IntPtr res, IntPtr commandString, int cch);
    }

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

    [StructLayout(LayoutKind.Sequential)]
    struct CMINVOKECOMMANDINFO {
        public int cbSize; public int fMask; public IntPtr hwnd; public IntPtr lpVerb;
        public IntPtr lpParameters; public IntPtr lpDirectory; public int nShow;
        public int dwHotKey; public IntPtr hIcon;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    static extern int SHParseDisplayName(string name, IntPtr bc, out IntPtr pidl, uint inAtt, out uint outAtt);
    [DllImport("shell32.dll")]
    static extern int SHBindToParent(IntPtr pidl, ref Guid riid, out IntPtr ppv, out IntPtr pidlLast);
    [DllImport("ole32.dll")] static extern void CoTaskMemFree(IntPtr p);
    [DllImport("ole32.dll")]
    static extern int CoCreateInstance(ref Guid clsid, IntPtr outer, uint ctx, ref Guid iid, out IntPtr ppv);
    [DllImport("user32.dll")] static extern IntPtr CreatePopupMenu();
    [DllImport("user32.dll")] static extern bool DestroyMenu(IntPtr h);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(IntPtr h);
    [DllImport("user32.dll")] static extern int GetMenuItemID(IntPtr h, int pos);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetMenuStringW(IntPtr h, int id, StringBuilder buf, int n, int flags);

    const uint CLSCTX_INPROC_SERVER = 1;
    const uint GCS_VERBW = 4;
    const uint ID_FIRST = 1;

    public static string[] Run(string path, string clsidText, bool invoke) {
        var log = new System.Collections.Generic.List<string>();
        IntPtr pidl; uint att;
        int hr = SHParseDisplayName(path, IntPtr.Zero, out pidl, 0, out att);
        if (hr != 0) return new[] { "ERROR SHParseDisplayName 0x" + hr.ToString("X8") };

        Guid iidFolder = new Guid("000214E6-0000-0000-C000-000000000046");
        IntPtr folderPtr, childPidl;
        hr = SHBindToParent(pidl, ref iidFolder, out folderPtr, out childPidl);
        if (hr != 0) return new[] { "ERROR SHBindToParent 0x" + hr.ToString("X8") };
        var folder = (IShellFolder)Marshal.GetObjectForIUnknown(folderPtr);

        // IDataObject for the single selected file -- what IShellExtInit wants.
        Guid iidData = new Guid("0000010E-0000-0000-C000-000000000046");
        IntPtr dataPtr;
        folder.GetUIObjectOf(IntPtr.Zero, 1, new[] { childPidl }, ref iidData, IntPtr.Zero, out dataPtr);

        Guid clsid = new Guid(clsidText);
        Guid iidInit = new Guid("000214E8-0000-0000-C000-000000000046");
        IntPtr initPtr;
        hr = CoCreateInstance(ref clsid, IntPtr.Zero, CLSCTX_INPROC_SERVER, ref iidInit, out initPtr);
        if (hr != 0) return new[] { "ERROR CoCreateInstance 0x" + hr.ToString("X8") };
        var init = (IShellExtInit)Marshal.GetObjectForIUnknown(initPtr);
        hr = init.Initialize(IntPtr.Zero, dataPtr, IntPtr.Zero);
        log.Add("IShellExtInit::Initialize -> 0x" + hr.ToString("X8"));

        var ctx = (IContextMenu)init;
        IntPtr menu = CreatePopupMenu();
        hr = ctx.QueryContextMenu(menu, 0, ID_FIRST, 0x7FFF, 0);
        log.Add("QueryContextMenu -> 0x" + hr.ToString("X8") + "  items added: " + (hr & 0xFFFF));

        int count = GetMenuItemCount(menu);
        int copyLinkId = -1;
        for (int i = 0; i < count; i++) {
            int id = GetMenuItemID(menu, i);
            var text = new StringBuilder(512);
            GetMenuStringW(menu, i, text, text.Capacity, 0x0400);
            string plain = text.ToString().Replace("&", "").Trim();
            string verb = "--";
            if (id > 0) {
                IntPtr buf = Marshal.AllocHGlobal(1024);
                try {
                    int r = ctx.GetCommandString((IntPtr)(id - ID_FIRST), GCS_VERBW, IntPtr.Zero, buf, 512);
                    if (r == 0) { string v = Marshal.PtrToStringUni(buf); if (!string.IsNullOrEmpty(v)) verb = v; }
                } catch { } finally { Marshal.FreeHGlobal(buf); }
            }
            log.Add("  item[" + i + "] id=" + id + " verb=" + verb + " text='" + plain + "'");
            if (string.Equals(plain, "Copy link", StringComparison.OrdinalIgnoreCase)) copyLinkId = id;
        }

        if (invoke) {
            if (copyLinkId < 0) { log.Add("NOT INVOKED: no item whose text is exactly 'Copy link'"); }
            else {
                var ici = new CMINVOKECOMMANDINFO();
                ici.cbSize = Marshal.SizeOf(typeof(CMINVOKECOMMANDINFO));
                ici.hwnd = IntPtr.Zero;
                ici.lpVerb = (IntPtr)(copyLinkId - ID_FIRST);
                ici.nShow = 1;
                IntPtr p = Marshal.AllocHGlobal(ici.cbSize);
                Marshal.StructureToPtr(ici, p, false);
                int r = ctx.InvokeCommand(p);
                Marshal.FreeHGlobal(p);
                log.Add("InvokeCommand(offset " + (copyLinkId - ID_FIRST) + ") -> 0x" + r.ToString("X8"));
            }
        }

        DestroyMenu(menu);
        CoTaskMemFree(pidl);
        return log.ToArray();
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

Write-Output "path : $Path"
Write-Output "clsid: $Clsid"
[LucidVerb]::Run($Path, $Clsid, [bool]$Invoke) | ForEach-Object { Write-Output $_ }
