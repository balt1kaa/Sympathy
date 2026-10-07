param([Parameter(Mandatory = $true)][string]$Exe)
$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class NoIcon {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr h);
    delegate bool NameProc(IntPtr module, IntPtr type, IntPtr name, IntPtr param);
    delegate bool LangProc(IntPtr module, IntPtr type, IntPtr name, ushort lang, IntPtr param);
    [DllImport("kernel32.dll")] static extern bool EnumResourceNames(IntPtr m, IntPtr type, NameProc cb, IntPtr p);
    [DllImport("kernel32.dll")] static extern bool EnumResourceLanguages(IntPtr m, IntPtr type, IntPtr name, LangProc cb, IntPtr p);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr BeginUpdateResource(string path, bool deleteAll);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UpdateResource(IntPtr h, IntPtr type, IntPtr name, ushort lang, IntPtr data, uint size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool EndUpdateResource(IntPtr h, bool discard);

    struct Res { public IntPtr Type; public long Id; public string Name; public ushort Lang; }

    public static int Strip(string path) {
        var found = new List<Res>();
        IntPtr m = LoadLibraryEx(path, IntPtr.Zero, 0x2); // LOAD_LIBRARY_AS_DATAFILE
        if (m == IntPtr.Zero) throw new Exception("cannot open " + path);
        foreach (int t in new[] { 14, 3 }) { // RT_GROUP_ICON, RT_ICON
            IntPtr type = (IntPtr)t;
            EnumResourceNames(m, type, (mod, ty, name, p) => {
                long v = name.ToInt64();
                string s = (v >> 16) == 0 ? null : Marshal.PtrToStringUni(name);
                EnumResourceLanguages(mod, ty, name, (mm, tt, nn, lang, pp) => {
                    found.Add(new Res { Type = ty, Id = v, Name = s, Lang = lang });
                    return true;
                }, IntPtr.Zero);
                return true;
            }, IntPtr.Zero);
        }
        FreeLibrary(m);
        if (found.Count == 0) return 0;

        IntPtr u = BeginUpdateResource(path, false);
        if (u == IntPtr.Zero) throw new Exception("cannot update " + path + ": " + Marshal.GetLastWin32Error());
        foreach (var r in found) {
            IntPtr name = r.Name == null ? (IntPtr)r.Id : Marshal.StringToHGlobalUni(r.Name);
            if (!UpdateResource(u, r.Type, name, r.Lang, IntPtr.Zero, 0)) {
                EndUpdateResource(u, true);
                throw new Exception("cannot remove an icon: " + Marshal.GetLastWin32Error());
            }
        }
        if (!EndUpdateResource(u, false)) throw new Exception("cannot write " + path + ": " + Marshal.GetLastWin32Error());
        return found.Count;
    }
}
"@

$n = [NoIcon]::Strip((Resolve-Path $Exe).Path)
"icons removed: $n"
