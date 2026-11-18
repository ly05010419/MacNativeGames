// The translation service as a library (NativeAOT NativeLib=Static): the iPhone / iPad app links it and calls these instead
// of starting `totk_xlate --service`, which iOS does not allow. Same code, same files written (Service.cs HandleLine).
using System;
using System.Runtime.InteropServices;
using System.Text;

static unsafe class LibExports
{
    [UnmanagedCallersOnly(EntryPoint = "totk_xlate_init")]
    static int Init(byte* outDir, byte* chain, int debugMode)
    {
        try { ShaderService.Configure(Marshal.PtrToStringUTF8((IntPtr)outDir), Marshal.PtrToStringUTF8((IntPtr)chain), debugMode); return 0; }
        catch (Exception) { return -1; }
    }

    // One request (a JSON line, as the service reads it); the reply goes into reply (size bytes, always terminated).
    // Returns 1 when the reply is {"ok": true}, 0 for an error reply, -1 when reply is too small.
    [UnmanagedCallersOnly(EntryPoint = "totk_xlate_request")]
    static int Request(byte* json, byte* reply, int size)
    {
        string r = ShaderService.HandleLine(Marshal.PtrToStringUTF8((IntPtr)json));
        byte[] b = Encoding.UTF8.GetBytes(r);
        if (size <= 0) return -1;
        int n = Math.Min(b.Length, size - 1);
        Marshal.Copy(b, 0, (IntPtr)reply, n); reply[n] = 0;
        return n < b.Length ? -1 : r == "{\"ok\": true}" ? 1 : 0;
    }
}
