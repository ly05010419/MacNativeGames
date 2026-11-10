using System;
using System.Runtime.InteropServices;
using System.Text;

static unsafe class RawWriteProofExports
{
    // Pure bounded shader analysis; no runtime shader compilation or game calls.
    [UnmanagedCallersOnly(EntryPoint = "oot_raw_write_proof")]
    static int Prove(byte* json, byte* reply, int capacity)
    {
        if (json == null || reply == null || capacity <= 0) return -1;
        string result = RawWriteProof.Handle(Marshal.PtrToStringUTF8((IntPtr)json));
        byte[] bytes = Encoding.UTF8.GetBytes(result);
        if (bytes.Length >= capacity) { reply[0] = 0; return -1; }
        Marshal.Copy(bytes, 0, (IntPtr)reply, bytes.Length); reply[bytes.Length] = 0;
        return bytes.Length;
    }
}
