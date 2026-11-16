using Ryujinx.Common.Logging;
using System;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace Ryujinx.Common.Helper
{
    public static partial class ConsoleHelper
    {
        [SupportedOSPlatform("windows")]
        [LibraryImport("kernel32")]
        private static partial nint GetConsoleWindow();

        [SupportedOSPlatform("windows")]
        [LibraryImport("kernel32", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static partial bool FreeConsole();

        public static bool SetConsoleWindowStateSupported => OperatingSystem.IsWindows();
        public static bool HasConsoleWindow => OperatingSystem.IsWindows() && GetConsoleWindow() != nint.Zero;

        public static void SetConsoleWindowState(bool show)
        {
            if (OperatingSystem.IsWindows())
            {
                SetConsoleWindowStateWindows(show);
            }
            else if (show == false)
            {
                Logger.Warning?.Print(LogClass.Application, "OS doesn't support hiding console window");
            }
        }

        [SupportedOSPlatform("windows")]
        private static void SetConsoleWindowStateWindows(bool show)
        {
            if (show)
            {
                if (GetConsoleWindow() != nint.Zero)
                {
                    Logger.SetConsoleTargetEnabled(true);
                }
                return;
            }

            Logger.SetConsoleTargetEnabled(false);
            DetachConsole();
        }

        [SupportedOSPlatform("windows")]
        private static void DetachConsole()
        {
            if (GetConsoleWindow() == nint.Zero)
            {
                return;
            }

            if (!FreeConsole())
            {
                Logger.Warning?.Print(LogClass.Application, "Attempted to detach console window but the operation failed");
            }
        }
    }
}
