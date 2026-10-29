using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Principal;
using System.Threading;
using LibreHardwareMonitor.Hardware;

// Started by ZX7Pro.exe. Reads CPU/GPU temperatures via LibreHardwareMonitorLib
// (which needs the PawnIO driver and administrator rights) and writes them to a
// small text file next to this executable.
internal static class SensorHost
{
    private static string OutPath()
    {
        try
        {
            string dir = AppDomain.CurrentDomain.BaseDirectory;
            string p = Path.Combine(dir, "zx7sensors.txt");
            File.WriteAllText(p, "cpu=-1.0;gpu=-1.0;t=0");
            File.Delete(p);
            return p;
        }
        catch { }

        try
        {
            string dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "ZX7Pro");
            Directory.CreateDirectory(dir);
            return Path.Combine(dir, "zx7sensors.txt");
        }
        catch { }

        return Path.Combine(Path.GetTempPath(), "zx7sensors.txt");
    }

    private static bool IsAdmin()
    {
        try
        {
            using (WindowsIdentity id = WindowsIdentity.GetCurrent())
                return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
        }
        catch { return false; }
    }

    private static void Main()
    {
        string outPath = OutPath();

        // PawnIO 只允许管理员访问。宿主应当由最高权限计划任务启动；
        // 如果不是管理员，说明没走任务，直接退出（不弹 UAC），让主程序提示安装服务。
        if (!IsAdmin())
        {
            try { File.WriteAllText(outPath, "cpu=-1.0;gpu=-1.0;t=0;adm=0"); } catch { }
            return;
        }

        Computer computer;
        try
        {
            computer = new Computer
            {
                IsCpuEnabled = true,
                IsGpuEnabled = true,
                IsMemoryEnabled = false,
                IsMotherboardEnabled = false,
                IsStorageEnabled = false,
                IsNetworkEnabled = false,
                IsControllerEnabled = false
            };
            computer.Open();
        }
        catch (Exception ex)
        {
            try { File.WriteAllText(outPath, "err=" + ex.GetType().Name + ";adm=1"); } catch { }
            return;
        }

        while (true)
        {
            double cpu = -1;
            var gpuTemps = new System.Collections.Generic.List<double>();
            var gpuNames = new System.Collections.Generic.List<string>();
            var gpuLoad = new System.Collections.Generic.List<double>();
            var gpuClock = new System.Collections.Generic.List<double>();

            try
            {
                foreach (IHardware hw in computer.Hardware)
                {
                    hw.Update();
                    foreach (IHardware sub in hw.SubHardware) sub.Update();

                    bool isCpu = hw.HardwareType == HardwareType.Cpu;
                    bool isGpu = hw.HardwareType == HardwareType.GpuNvidia ||
                                 hw.HardwareType == HardwareType.GpuAmd ||
                                 hw.HardwareType == HardwareType.GpuIntel;
                    if (!isCpu && !isGpu) continue;

                    double best = -1, load = -1, clock = -1;
                    foreach (ISensor s in hw.Sensors)
                    {
                        if (s.Value == null) continue;
                        if (!isCpu && isGpu)
                        {
                            string ln = (s.Name ?? "").ToLowerInvariant();
                            if (s.SensorType == SensorType.Load && ln.Contains("gpu core"))
                                load = s.Value.Value;
                            else if (s.SensorType == SensorType.Clock && ln.Contains("gpu core"))
                                clock = s.Value.Value;
                        }
                        if (s.SensorType != SensorType.Temperature) continue;
                        double v = s.Value.Value;
                        if (v <= 0 || v > 130) continue;

                        if (isCpu)
                        {
                            if (v > cpu) cpu = v;
                        }
                        else
                        {
                            string n = (s.Name ?? "").ToLowerInvariant();
                            // 优先 "GPU Core"，其次任意含 gpu 的温度，最后取该卡最高温度
                            if (n.Contains("gpu core") || n.Contains("gpu"))
                            {
                                if (v > best) best = v;
                            }
                        }
                    }

                    if (isGpu)
                    {
                        if (best < 0)
                        {
                            foreach (ISensor s in hw.Sensors)
                                if (s.SensorType == SensorType.Temperature && s.Value != null &&
                                    s.Value.Value > best && s.Value.Value < 130)
                                    best = s.Value.Value;
                        }
                        gpuTemps.Add(best);
                        gpuNames.Add(hw.Name ?? ("GPU " + gpuTemps.Count));
                        gpuLoad.Add(load);
                        gpuClock.Add(clock);
                    }
                }
            }
            catch { }

            try
            {
                var sb = new System.Text.StringBuilder();
                sb.Append("cpu=").Append(cpu.ToString("F1"));
                for (int i = 0; i < gpuTemps.Count; i++)
                {
                    sb.Append(";gpu").Append(i).Append('=').Append(gpuTemps[i].ToString("F1"));
                    sb.Append(";use").Append(i).Append('=').Append(gpuLoad[i].ToString("F0"));
                    sb.Append(";clk").Append(i).Append('=').Append(gpuClock[i].ToString("F0"));
                }
                sb.Append(";names=");
                for (int i = 0; i < gpuNames.Count; i++)
                {
                    if (i > 0) sb.Append('|');
                    sb.Append(gpuNames[i].Replace('|', '/').Replace(';', ','));
                }
                sb.Append(";t=").Append(DateTimeOffset.UtcNow.ToUnixTimeSeconds());
                sb.Append(";adm=1");
                File.WriteAllText(outPath, sb.ToString());
            }
            catch { }

            Thread.Sleep(500);
        }
    }
}
