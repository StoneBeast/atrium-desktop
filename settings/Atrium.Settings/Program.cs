namespace Atrium.Settings;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        // Never the install folder: what this starts would inherit it, and a folder some process
        // is in cannot be replaced by an update.
        string profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (Directory.Exists(profile)) Environment.CurrentDirectory = profile;
        // A BITS completion launches the same executable without creating XAML,
        // a window, a graphics device, or the settings-window singleton.
        if (args.Length > 0 && args[0] is "--queue-update" or "--finish-update" or "--resume-update")
            return Task.Run(() => ReleaseInstaller.RunBackground(args)).GetAwaiter().GetResult();
        XamlGeneratedProgram.XamlGeneratedMain();
        return 0;
    }
}
