using System;
using System.Runtime.InteropServices;
using System.Text;

class BurningUtilityExample
{
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr GetPlatformInfo();
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr GetSupportedFormats();
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern int CheckToolAvailability(string toolName);
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern int ListBurningDevices(out IntPtr devices, out int deviceCount);
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void FreeDeviceList(IntPtr devices, int deviceCount);
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void FreeStringMemory(IntPtr ptr);
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void SetDefaultDevice(string device);
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void SetDefaultDiscType(string discType);
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void SetDefaultSpeed(int speed);
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern int GetDefaultDevice(StringBuilder devicePath, int bufferSize);
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr GetLastErrorMessage();
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void ClearLastError();
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern int CreateISOImage(string sourcePath, string isoPath, string volumeLabel);
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern int CreateISOToDisc(string isoPath, string devicePath, string discType, int speed);
    
    [DllImport("burning_dll.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern int BurnDirectoryToDisc(string sourceDir, string devicePath, string discType, int speed);
    
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct DeviceInfo
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 512)]
        public string devicePath;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string deviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string mediaType;
        public int isWritable;
        public int isEmpty;
    }
    
    static string PtrToString(IntPtr ptr)
    {
        if (ptr == IntPtr.Zero) return "";
        string result = Marshal.PtrToStringAnsi(ptr);
        FreeStringMemory(ptr);
        return result;
    }
    
    static void Main()
    {
        Console.WriteLine("=== Burning Utility C# Example ===");
        Console.WriteLine();
        
        try
        {
            Console.WriteLine("Platform: " + PtrToString(GetPlatformInfo()));
            Console.WriteLine("Supported formats: " + PtrToString(GetSupportedFormats()));
            
            Console.WriteLine("\nTool availability:");
            Console.WriteLine("  growisofs: " + (CheckToolAvailability("growisofs") == 1 ? "Available" : "Not Available"));
            Console.WriteLine("  mkisofs: " + (CheckToolAvailability("mkisofs") == 1 ? "Available" : "Not Available"));
            Console.WriteLine("  genisoimage: " + (CheckToolAvailability("genisoimage") == 1 ? "Available" : "Not Available"));
            Console.WriteLine("  wodim: " + (CheckToolAvailability("wodim") == 1 ? "Available" : "Not Available"));
            
            Console.WriteLine("\n=== Available Devices ===");
            IntPtr devicesPtr;
            int deviceCount;
            if (ListBurningDevices(out devicesPtr, out deviceCount) == 0)
            {
                if (deviceCount == 0)
                {
                    Console.WriteLine("No devices found");
                }
                else
                {
                    int structSize = Marshal.SizeOf(typeof(DeviceInfo));
                    for (int i = 0; i < deviceCount; ++i)
                    {
                        IntPtr currentPtr = new IntPtr(devicesPtr.ToInt64() + i * structSize);
                        DeviceInfo device = (DeviceInfo)Marshal.PtrToStructure(currentPtr, typeof(DeviceInfo));
                        
                        Console.WriteLine("Device " + i + ":");
                        Console.WriteLine("  Path: " + device.devicePath);
                        Console.WriteLine("  Name: " + device.deviceName);
                        Console.WriteLine("  Media Type: " + device.mediaType);
                        Console.WriteLine("  Writable: " + (device.isWritable == 1 ? "Yes" : "No"));
                        Console.WriteLine("  Empty: " + (device.isEmpty == 1 ? "Yes" : "No"));
                        Console.WriteLine();
                    }
                }
                FreeDeviceList(devicesPtr, deviceCount);
            }
            else
            {
                Console.WriteLine("Failed to list devices: " + PtrToString(GetLastErrorMessage()));
            }
            
            Console.WriteLine("=== Configuration ===");
            SetDefaultDevice("D:");
            SetDefaultDiscType("DVD-R");
            SetDefaultSpeed(4);
            
            StringBuilder defaultDevBuffer = new StringBuilder(512);
            if (GetDefaultDevice(defaultDevBuffer, 512) == 0)
            {
                Console.WriteLine("Default device: " + defaultDevBuffer.ToString());
            }
            
            Console.WriteLine("\n=== Example Usage ===");
            Console.WriteLine("// CreateISOImage(\"./data\", \"./output.iso\", \"MYDISC\");");
            Console.WriteLine("// CreateISOToDisc(\"./output.iso\", \"D:\", \"DVD-R\", 4);");
            Console.WriteLine("// BurnDirectoryToDisc(\"./data\", \"D:\", \"DVD-R\", 4);");
            
            Console.WriteLine("\n=== Example completed ===");
        }
        catch (Exception ex)
        {
            Console.WriteLine("Error: " + ex.Message);
        }
    }
}
