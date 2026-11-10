using System.Runtime.InteropServices;

namespace Atrium.Settings;

// Uses the same monitor-interface ID as the core, rather than a changeable screen number.
internal sealed record DesktopDisplay(string Id, string Name, string Details, bool Primary,
    int Left = 0, int Top = 0, int Width = 0, int Height = 0, int WorkWidth = 0, int WorkHeight = 0, double Scale = 1)
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Rect { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MonitorInfo
    {
        public int Size;
        public Rect Bounds, Work;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Device;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DisplayDevice
    {
        public int Size;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public uint StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceId;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }

    private delegate bool MonitorCallback(IntPtr monitor, IntPtr dc, ref Rect bounds, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonitorCallback callback, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetMonitorInfoW")] private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "EnumDisplayDevicesW")] private static extern bool EnumDisplayDevices(string device, uint index, ref DisplayDevice info, uint flags);
    [DllImport("shcore.dll")] private static extern int GetDpiForMonitor(IntPtr monitor, int type, out uint x, out uint y);

    public static List<DesktopDisplay> Read()
    {
        var displays = new List<DesktopDisplay>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr monitor, IntPtr dc, ref Rect bounds, IntPtr data) =>
        {
            var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>(), Device = "" };
            if (!GetMonitorInfo(monitor, ref info)) return true;
            var device = new DisplayDevice { Size = Marshal.SizeOf<DisplayDevice>(), DeviceName = "", DeviceString = "", DeviceId = "", DeviceKey = "" };
            bool found = EnumDisplayDevices(info.Device, 0, ref device, 1);
            string id = found && device.DeviceId.Length > 0 ? device.DeviceId : info.Device;
            double scale = GetDpiForMonitor(monitor, 0, out uint dpi, out _) == 0 ? dpi / 96.0 : 1;
            bool primary = (info.Flags & 1) != 0;
            int w = info.Bounds.Right - info.Bounds.Left, h = info.Bounds.Bottom - info.Bounds.Top;
            string name = $"{info.Device.Replace(@"\\.\", "")} · {(found ? device.DeviceString : "显示器")}";
            displays.Add(new(id, name, $"{w} × {h} · {scale:P0} 缩放{(primary ? " · 主显示器" : "")}", primary,
                info.Bounds.Left, info.Bounds.Top, w, h, info.Work.Right - info.Work.Left, info.Work.Bottom - info.Work.Top, scale));
            return true;
        }, IntPtr.Zero);
        return displays.OrderByDescending(d => d.Primary).ThenBy(d => d.Left).ThenBy(d => d.Top).ToList();
    }
}
