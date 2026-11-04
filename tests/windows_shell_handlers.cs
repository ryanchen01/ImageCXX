using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

public static class ImageCXXShellHandlers
{
    [ComImport, Guid("F04061AC-1659-4A3F-A954-775AA57FC083"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IAssocHandler
    {
        // Only the first vtable method is needed for this test.
        void GetName([MarshalAs(UnmanagedType.LPWStr)] out string name);
    }

    [ComImport, Guid("973810AE-9599-4B88-9E4D-6EE98C9552DA"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IEnumAssocHandlers
    {
        [PreserveSig]
        int Next(uint count, out IAssocHandler handler, out uint fetched);
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHAssocEnumHandlers(string extension, uint filter,
        out IEnumAssocHandlers handlers);

    public static string[] GetNames(string filename)
    {
        IEnumAssocHandlers handlers;
        Marshal.ThrowExceptionForHR(SHAssocEnumHandlers(Path.GetExtension(filename), 0, out handlers));
        var names = new List<string>();
        try
        {
            while (true)
            {
                IAssocHandler handler;
                uint fetched;
                int result = handlers.Next(1, out handler, out fetched);
                Marshal.ThrowExceptionForHR(result);
                if (fetched == 0) break;
                try
                {
                    string name;
                    handler.GetName(out name);
                    names.Add(name);
                }
                finally { Marshal.ReleaseComObject(handler); }
            }
        }
        finally { Marshal.ReleaseComObject(handlers); }
        return names.ToArray();
    }
}
