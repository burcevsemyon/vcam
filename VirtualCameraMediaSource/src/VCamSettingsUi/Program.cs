using System.Diagnostics;
using System.Runtime.InteropServices;

namespace VCamSettingsUi;

static class Program
{
    const string InstanceMutexName = "VCamSettingsUi.Instance";
    const int SW_RESTORE = 9;

    [DllImport("user32.dll")]
    static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    static extern bool IsIconic(IntPtr hWnd);

    [STAThread]
    static void Main()
    {
        using var instanceMutex = new Mutex(true, InstanceMutexName, out bool createdNew);
        if (!createdNew)
        {
            ActivateExistingInstance();
            Environment.Exit(0);
            return;
        }

        try
        {
            ApplicationConfiguration.Initialize();
            Application.Run(new MainForm());
        }
        finally
        {
            instanceMutex.ReleaseMutex();
        }
    }

    static void ActivateExistingInstance()
    {
        foreach (var process in Process.GetProcessesByName("VCamSettingsUi"))
        {
            try
            {
                var handle = process.MainWindowHandle;
                if (handle == IntPtr.Zero) continue;
                if (IsIconic(handle)) ShowWindow(handle, SW_RESTORE);
                SetForegroundWindow(handle);
            }
            catch
            {
                // process may have exited between enumeration and activation
            }
            finally
            {
                process.Dispose();
            }
        }
    }
}
