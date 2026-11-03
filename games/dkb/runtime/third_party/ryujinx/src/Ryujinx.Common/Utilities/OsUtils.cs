using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Ryujinx.Common.Utilities
{
    public partial class OsUtils
    {
        [LibraryImport("libc", SetLastError = true)]
        private static partial int setenv([MarshalAs(UnmanagedType.LPStr)] string name, [MarshalAs(UnmanagedType.LPStr)] string value, int overwrite);

        public static void SetEnvironmentVariableNoCaching(string key, string value)
        {
            // Set the value in the cached environment variables, too.
            Environment.SetEnvironmentVariable(key, value);

            if (!OperatingSystem.IsWindows())
            {
                int res = setenv(key, value, 1);
                Debug.Assert(res != -1);
            }
        }

        // "dumpable" attribute of the calling process
        private const int PR_GET_DUMPABLE = 3;
        private const int PR_SET_DUMPABLE = 4;

        [LibraryImport("libc", SetLastError = true)]
        private static partial int prctl(int option, int arg2);

        public static void SetCoreDumpable(bool dumpable)
        {
            if (OperatingSystem.IsLinux())
            {
                int dumpableInt = dumpable ? 1 : 0;
                int result = prctl(PR_SET_DUMPABLE, dumpableInt);
                Debug.Assert(result == 0);
            }
        }

        // Use the below line to display dumpable status in the console:
        // Console.WriteLine($"{OsUtils.IsCoreDumpable()}");
        public static bool IsCoreDumpable()
        {
            int result = prctl(PR_GET_DUMPABLE, 0);
            return result == 1;
        }
    }
}
