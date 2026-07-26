param(
    [Parameter(Mandatory = $true)]
    [string]$ExePath,
    [string[]]$AppArguments = @(),
    [int]$Seconds = 3,
    [string]$ScreenshotPath = "",
    [string]$WindowTitle = "Vulkan Cube"
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class VulkanCubeWin32 {
    private delegate bool EnumWindowCallback(IntPtr handle, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    public struct Rect {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetWindowRect(IntPtr handle, out Rect rect);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string windowName);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(
        EnumWindowCallback callback, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr handle, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(
        IntPtr handle, StringBuilder text, int maximumCount);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessage(
        IntPtr handle, uint message, IntPtr wParam, IntPtr lParam);

    public static IntPtr FindProcessWindow(int processId, string titlePart) {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr handle, IntPtr parameter) {
            uint owner;
            GetWindowThreadProcessId(handle, out owner);
            if (owner != (uint)processId) {
                return true;
            }
            StringBuilder title = new StringBuilder(512);
            GetWindowText(handle, title, title.Capacity);
            if (title.ToString().Contains(titlePart)) {
                result = handle;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
"@

$startParameters = @{
    FilePath = $ExePath
    PassThru = $true
}
if ($AppArguments.Count -gt 0) {
    $startParameters.ArgumentList = $AppArguments
}
$process = Start-Process @startParameters

$windowHandle = [IntPtr]::Zero
$windowDeadline = [DateTime]::UtcNow.AddSeconds(10)
while (-not $process.HasExited -and
       $windowHandle -eq [IntPtr]::Zero -and
       [DateTime]::UtcNow -lt $windowDeadline) {
    Start-Sleep -Milliseconds 100
    $process.Refresh()
    $windowHandle = [VulkanCubeWin32]::FindProcessWindow(
        $process.Id, $WindowTitle)
}
if ($windowHandle -eq [IntPtr]::Zero -and -not $process.HasExited) {
    $process.Refresh()
    $windowHandle = $process.MainWindowHandle
}

Start-Sleep -Seconds $Seconds
$alive = -not $process.HasExited

if ($alive -and $ScreenshotPath -ne "") {
    $rect = New-Object VulkanCubeWin32+Rect
    if ([VulkanCubeWin32]::GetWindowRect(
            $windowHandle, [ref]$rect)) {
        $width = [Math]::Max(1, $rect.Right - $rect.Left)
        $height = [Math]::Max(1, $rect.Bottom - $rect.Top)
        $bitmap = New-Object System.Drawing.Bitmap($width, $height)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen(
                $rect.Left,
                $rect.Top,
                0,
                0,
                $bitmap.Size,
                [System.Drawing.CopyPixelOperation]::SourceCopy)
            $bitmap.Save(
                $ScreenshotPath,
                [System.Drawing.Imaging.ImageFormat]::Png)
        }
        finally {
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
}

if ($alive) {
    [void][VulkanCubeWin32]::PostMessage(
        $windowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $process.WaitForExit(5000)) {
        Stop-Process -Id $process.Id
        $process.WaitForExit()
    }
}

Write-Output "alive_after_${Seconds}s=$alive"
if (-not $alive -and $process.ExitCode -ne 0) {
    exit $process.ExitCode
}
