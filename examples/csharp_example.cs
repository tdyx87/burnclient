using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

/* BurningDLL C# example (x64).
 *
 * Build (from an SDK whose include/ and lib/ sit next to bin/):
 *   csc /platform:x64 /out:csharp_example.exe csharp_example.cs
 * The DLL (bin/burning_dll.dll) must be on PATH or next to the exe.
 *
 * Two correctness points this file deliberately demonstrates:
 *  1. GetPlatformInfo()/GetSupportedFormats()/GetLastErrorMessage() and
 *     Burn_GetJobErrorMessage() return pointers into DLL-owned buffers
 *     (thread_local std::string or static literals). FreeStringMemory() is
 *     free(): calling it on those pointers corrupts the CRT heap and crashes
 *     with STATUS_HEAP_CORRUPTION (0xC0000374). Copy the text, never free.
 *  2. The C API's char* strings are UTF-8 (the backends convert with
 *     CP_UTF8), while .NET's ANSI marshalling uses the system code page.
 *     Marshal UTF-8 bytes explicitly - StringToHGlobalAnsi/LPStr would
 *     garble non-ASCII paths and labels.
 */
class BurningUtilityExample
{
    const string Dll = "burning_dll.dll";

    /* ---- legacy synchronous API ---- */

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern IntPtr GetPlatformInfo();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern IntPtr GetSupportedFormats();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int CheckToolAvailability(string toolName);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int ListBurningDevices(out IntPtr devices, out int deviceCount);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern void FreeDeviceList(IntPtr devices, int deviceCount);

    /* FreeStringMemory() is intentionally NOT bound here: every const char*
     * this library returns points into a DLL-owned buffer, so there is no
     * legitimate call site for it from C#. Binding it only invites the
     * heap-corruption crash described above. (ListBurningDevices' array is
     * freed by FreeDeviceList, which free()s inside the DLL - that one is
     * correct and must stay.) */

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern void SetDefaultDevice(string device);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern void SetDefaultDiscType(string discType);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern void SetDefaultSpeed(int speed);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int GetDefaultDevice(StringBuilder devicePath, int bufferSize);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern IntPtr GetLastErrorMessage();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern void ClearLastError();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int VerifyDisc(string devicePath, string isoPath);

    /* ---- handle-based async job API (BURN_API_VERSION 2) ---- */

    public const int BURN_SUCCESS = 0;
    public const int BURN_ERROR_TIMEOUT = -11;
    public const int BURN_ERROR_INVALID_HANDLE = -13;
    public const int BURN_ERROR_CANCELLED = -10;

    public const int BURN_JOB_PENDING = 0;
    public const int BURN_JOB_RUNNING = 1;
    public const int BURN_JOB_COMPLETED = 2;
    public const int BURN_JOB_FAILED = 3;
    public const int BURN_JOB_CANCELLED = 4;

    public const int BURN_JOB_MODE_IMAGE = 0;
    public const int BURN_JOB_MODE_DIRECTORY = 1;
    public const int BURN_JOB_MODE_CREATE_IMAGE = 2;
    public const int BURN_JOB_MODE_SELFTEST = 3;

    /* BurnJobHandle is `unsigned long long` - use ulong, not IntPtr
     * (IntPtr would be 4 bytes on x86 and truncate). */
    public const ulong BURN_INVALID_JOB_HANDLE = 0UL;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void BurnProgressCallback(int percent, IntPtr statusUtf8, IntPtr userData);

    /* Default Pack=8 reproduces the C layout byte-for-byte on x64:
     * size 0, mode 4, five char* at 8/16/24/32/40, speed 48, verify 52,
     * callback 56, userData 64, sizeof 72. Do NOT set Pack=1 here. */
    [StructLayout(LayoutKind.Sequential)]
    public struct BurnJobOptions
    {
        public int size;
        public int mode;
        public IntPtr sourcePath;      /* UTF-8 bytes or IntPtr.Zero */
        public IntPtr outputPath;
        public IntPtr devicePath;      /* Zero -> configured default */
        public IntPtr discType;        /* Zero -> configured default */
        public IntPtr volumeLabel;     /* Zero -> "DISC" */
        public int speed;              /* <=0 -> configured default; a true multiplier */
        public int verify;             /* reserved, 0 */
        public IntPtr progressCallback; /* via Marshal.GetFunctionPointerForDelegate */
        public IntPtr userData;         /* via GCHandle.ToIntPtr */
    }

    /* size 0, state 4, percent 8, errorCode 12, cancelRequested 16,
     * phase 20, message 84, errorMessage 340, elapsedMs 856, sizeof 864.
     * The 4-byte pad before elapsedMs comes from its 8-byte alignment;
     * Pack=1 would be WRONG (852/860). */
    [StructLayout(LayoutKind.Sequential)]
    public struct BurnJobStatus
    {
        public int size;
        public int state;
        public int percent;
        public int errorCode;
        public int cancelRequested;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)]
        public byte[] phase;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)]
        public byte[] message;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 512)]
        public byte[] errorMessage;
        public ulong elapsedMs;
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern ulong Burn_Start(ref BurnJobOptions options);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int Burn_GetStatus(ulong job, ref BurnJobStatus status);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int Burn_Cancel(ulong job);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int Burn_Wait(ulong job, int timeoutMs);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int Burn_Release(ulong job);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern IntPtr Burn_GetJobErrorMessage(ulong job);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int CancelCurrentBurn();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int Burn_GetActiveJobCount();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int Burn_GetApiVersion();

    /* ---- DeviceInfo (512+256+64+4+4 = 840, no padding surprises) ---- */

    [StructLayout(LayoutKind.Sequential)]
    public struct DeviceInfo
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 512)] public byte[] devicePath;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] deviceName;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)]  public byte[] mediaType;
        public int isWritable;
        public int isEmpty;

        public string DevicePath { get { return DecodeUtf8(devicePath); } }
        public string DeviceName { get { return DecodeUtf8(deviceName); } }
        public string MediaType  { get { return DecodeUtf8(mediaType); } }

        static string DecodeUtf8(byte[] raw)
        {
            if (raw == null) return string.Empty;
            int n = 0;
            while (n < raw.Length && raw[n] != 0) n++;
            return Encoding.UTF8.GetString(raw, 0, n);
        }
    }

    /* ---- helpers ---- */

    /* Copy a NUL-terminated UTF-8 string out of a DLL-owned buffer.
     * NEVER frees: the DLL owns these. Copy immediately - a thread_local
     * buffer is overwritten by the next call on the same thread. */
    static string PtrToNativeUtf8(IntPtr ptr)
    {
        if (ptr == IntPtr.Zero) return string.Empty;
        int len = 0;
        while (Marshal.ReadByte(ptr, len) != 0) len++;
        byte[] buf = new byte[len];
        Marshal.Copy(ptr, buf, 0, len);
        return Encoding.UTF8.GetString(buf, 0, len);
    }

    static string FixedUtf8(byte[] raw)
    {
        if (raw == null) return string.Empty;
        int n = 0;
        while (n < raw.Length && raw[n] != 0) n++;
        return Encoding.UTF8.GetString(raw, 0, n);
    }

    /* Scoped holder for the UTF-8 buffers referenced by BurnJobOptions.
     * Burn_Start() copies every string, so they may be freed right after
     * it returns. */
    sealed class Utf8Strings : IDisposable
    {
        readonly List<IntPtr> allocated = new List<IntPtr>();
        public IntPtr Alloc(string s)
        {
            if (string.IsNullOrEmpty(s)) return IntPtr.Zero;
            int byteCount = Encoding.UTF8.GetByteCount(s);
            IntPtr p = Marshal.AllocHGlobal(byteCount + 1);
            byte[] tmp = new byte[byteCount + 1];
            Encoding.UTF8.GetBytes(s, 0, s.Length, tmp, 0);
            tmp[byteCount] = 0;
            Marshal.Copy(tmp, 0, p, byteCount + 1);
            allocated.Add(p);
            return p;
        }
        public void Dispose()
        {
            for (int i = allocated.Count - 1; i >= 0; --i)
                Marshal.FreeHGlobal(allocated[i]);
            allocated.Clear();
        }
    }

    sealed class JobState
    {
        public int LastPercent = -1;
    }

    /* Static roots: the DLL stores the raw function pointer and calls it
     * later from its worker thread. If the delegate were a local, the GC
     * could collect it while the job runs. */
    static readonly BurnProgressCallback ProgressThunk = OnNativeProgress;

    static void OnNativeProgress(int percent, IntPtr statusUtf8, IntPtr userData)
    {
        /* Invoked on the DLL's worker thread. Copy the buffer immediately,
         * touch no UI, and never let an exception escape into native code. */
        try
        {
            string msg = PtrToNativeUtf8(statusUtf8);
            if (userData != IntPtr.Zero)
            {
                GCHandle h = GCHandle.FromIntPtr(userData);
                JobState js = h.IsAllocated ? h.Target as JobState : null;
                if (js != null) { js.LastPercent = percent; return; }
            }
            Console.WriteLine("  [callback] " + percent + "%  " + msg);
        }
        catch { }
    }

    static string StateName(int state)
    {
        switch (state)
        {
            case BURN_JOB_PENDING: return "PENDING";
            case BURN_JOB_RUNNING: return "RUNNING";
            case BURN_JOB_COMPLETED: return "COMPLETED";
            case BURN_JOB_FAILED: return "FAILED";
            case BURN_JOB_CANCELLED: return "CANCELLED";
            default: return "?" + state;
        }
    }

    /* Start -> poll -> (cancel) -> wait -> release. Replace SELFTEST with
     * IMAGE/DIRECTORY for a real burn; everything else is identical. */
    static int RunJobDemo(bool cancelHalfway)
    {
        if (Burn_GetApiVersion() < 2)
        {
            Console.WriteLine("Job API not available (version " + Burn_GetApiVersion() + "), skipping.");
            return 0;
        }

        /* Prove this file's struct marshalling matches the native ABI
         * before trusting anything built on it. x64 numbers from the C
         * header; an x86 build would legitimately differ in the pointer
         * fields, hence the pointer-size expectation. */
        int expectOptions = 2 * 4 + 5 * IntPtr.Size + 2 * 4 + 2 * IntPtr.Size;
        int expectStatus = 864;   /* fixed: 20 + 64 + 256 + 512 + pad + 8 */
        int gotOptions = Marshal.SizeOf(typeof(BurnJobOptions));
        int gotStatus = Marshal.SizeOf(typeof(BurnJobStatus));
        if (gotOptions != expectOptions || gotStatus != expectStatus)
        {
            Console.WriteLine("ABI mismatch: BurnJobOptions=" + gotOptions +
                " (expected " + expectOptions + "), BurnJobStatus=" + gotStatus +
                " (expected " + expectStatus + "). Job demo skipped.");
            return 1;
        }
        Console.WriteLine("ABI self-check: options=" + gotOptions +
            " status=" + gotStatus + " (OK)");

        Utf8Strings s = new Utf8Strings();
        GCHandle state = GCHandle.Alloc(new JobState());
        try
        {
            BurnJobOptions o = new BurnJobOptions();
            o.size = Marshal.SizeOf(typeof(BurnJobOptions));   /* must be 72 on x64 */
            o.mode = BURN_JOB_MODE_SELFTEST;                  /* no drive needed */
            o.sourcePath = IntPtr.Zero;
            o.outputPath = IntPtr.Zero;
            o.devicePath = IntPtr.Zero;                       /* NULL -> configured default */
            o.discType = IntPtr.Zero;
            o.volumeLabel = s.Alloc("MYDISC");
            o.speed = 0;                                      /* <=0 -> configured default */
            o.verify = 0;
            o.progressCallback = Marshal.GetFunctionPointerForDelegate(ProgressThunk);
            o.userData = GCHandle.ToIntPtr(state);

            ulong job = Burn_Start(ref o);                    /* copies all strings */
            if (job == BURN_INVALID_JOB_HANDLE)
            {
                Console.WriteLine("Burn_Start failed: " + PtrToNativeUtf8(GetLastErrorMessage()));
                return 1;
            }
            Console.WriteLine("Started job " + job);

            BurnJobStatus st = new BurnJobStatus();
            for (; ; )
            {
                st = new BurnJobStatus();
                st.size = Marshal.SizeOf(typeof(BurnJobStatus));  /* 864, every call */
                int r = Burn_GetStatus(job, ref st);
                if (r != BURN_SUCCESS) { Console.WriteLine("Burn_GetStatus -> " + r); break; }

                Console.WriteLine("  [status]   state=" + StateName(st.state) + " " +
                    st.percent + "%  phase=" + FixedUtf8(st.phase) + "  " + FixedUtf8(st.message));

                if (st.state != BURN_JOB_PENDING && st.state != BURN_JOB_RUNNING) break;
                if (cancelHalfway && st.percent >= 20 && st.cancelRequested == 0)
                {
                    Console.WriteLine("  Cancelling... (Burn_Cancel is thread-safe)");
                    Burn_Cancel(job);
                }
                Thread.Sleep(100);
            }

            /* -1 = block until terminal, 0 = poll only, N = bounded. */
            if (Burn_Wait(job, 5000) == BURN_ERROR_TIMEOUT)
                Console.WriteLine("Job did not finish within 5 seconds");

            st = new BurnJobStatus();
            st.size = Marshal.SizeOf(typeof(BurnJobStatus));
            Burn_GetStatus(job, ref st);
            Console.WriteLine("Final: state=" + StateName(st.state) +
                " errorCode=" + st.errorCode + " elapsed=" + st.elapsedMs + "ms" +
                " error=" + FixedUtf8(st.errorMessage));

            int active = Burn_GetActiveJobCount();
            Console.WriteLine("Active jobs now: " + active);
            Console.WriteLine("Release: " + Burn_Release(job));
            Console.WriteLine("Second release (must be " + BURN_ERROR_INVALID_HANDLE + "): " +
                Burn_Release(job));

            /* BurnJobStatus is a pointer-free snapshot: `st` stays valid
             * after Release. Burn_GetJobErrorMessage() after Release does
             * NOT - the handle is gone. */
            return (st.state == BURN_JOB_COMPLETED || st.state == BURN_JOB_CANCELLED) ? 0 : 1;
        }
        finally
        {
            s.Dispose();
            if (state.IsAllocated) state.Free();
        }
    }

    static void Main(string[] args)
    {
        Console.WriteLine("=== Burning Utility C# Example ===");

        Console.WriteLine("Platform: " + PtrToNativeUtf8(GetPlatformInfo()));
        Console.WriteLine("Supported formats: " + PtrToNativeUtf8(GetSupportedFormats()));

        Console.WriteLine("\nTool availability:");
        Console.WriteLine("  growisofs: " + (CheckToolAvailability("growisofs") == 1 ? "Available" : "Not Available"));
        Console.WriteLine("  mkisofs: " + (CheckToolAvailability("mkisofs") == 1 ? "Available" : "Not Available"));

        Console.WriteLine("\n=== Available Devices ===");
        IntPtr devicesPtr;
        int deviceCount;
        if (ListBurningDevices(out devicesPtr, out deviceCount) == BURN_SUCCESS)
        {
            if (deviceCount == 0)
                Console.WriteLine("No devices found");   /* honest: no fabricated entry */
            else
            {
                int structSize = Marshal.SizeOf(typeof(DeviceInfo));   /* 840 */
                for (int i = 0; i < deviceCount; ++i)
                {
                    IntPtr current = new IntPtr(devicesPtr.ToInt64() + (long)i * structSize);
                    DeviceInfo device = (DeviceInfo)Marshal.PtrToStructure(current, typeof(DeviceInfo));
                    Console.WriteLine("Device " + i + ":");
                    Console.WriteLine("  Path: " + device.DevicePath);
                    Console.WriteLine("  Name: " + device.DeviceName);
                    Console.WriteLine("  Media Type: " + device.MediaType);
                    Console.WriteLine("  Writable: " + (device.isWritable == 1 ? "Yes" : "No"));
                    Console.WriteLine("  Empty: " + (device.isEmpty == 1 ? "Yes" : "No"));
                    Console.WriteLine();
                }
            }
            FreeDeviceList(devicesPtr, deviceCount);     /* the ONE correct free path */
        }
        else
        {
            Console.WriteLine("Failed to list devices: " + PtrToNativeUtf8(GetLastErrorMessage()));
        }

        Console.WriteLine("=== Configuration (process-wide; visible on every thread) ===");
        SetDefaultDevice("D:");
        SetDefaultDiscType("DVD-R");
        SetDefaultSpeed(4);
        StringBuilder defaultDevBuffer = new StringBuilder(512);
        if (GetDefaultDevice(defaultDevBuffer, 512) == BURN_SUCCESS)
            Console.WriteLine("Default device: " + defaultDevBuffer.ToString());

        Console.WriteLine("\n=== VerifyDisc honesty ===");
        ClearLastError();
        int vrc = VerifyDisc("D:", "definitely-missing.iso");
        Console.WriteLine("VerifyDisc(missing file) -> " + vrc +
            " (" + PtrToNativeUtf8(GetLastErrorMessage()) + ")");

        Console.WriteLine("\n=== Job API (SELFTEST, no drive needed) ===");
        Environment.Exit(RunJobDemo(args.Length > 0 && args[0] == "cancel"));
    }
}
