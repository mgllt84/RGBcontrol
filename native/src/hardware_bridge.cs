using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using LibreHardwareMonitor.Hardware;

internal static class HardwareBridge
{
    private sealed class SensorRecord
    {
        public ISensor Sensor;
        public string Type;
        public string Name;
        public string Id;
        public string Hardware;
        public string HardwareId;
        public string HardwareType;
        public double? Value;
    }

    private static Computer OpenComputer()
    {
        Computer computer = new Computer();
        computer.IsCpuEnabled = true;
        computer.IsMotherboardEnabled = true;
        computer.IsGpuEnabled = true;
        computer.IsControllerEnabled = true;
        computer.IsPsuEnabled = true;
        computer.IsPowerMonitorEnabled = true;
        computer.Open();
        return computer;
    }

    private static void Collect(IHardware hardware, List<SensorRecord> records)
    {
        try { hardware.Update(); } catch { }
        foreach (ISensor sensor in hardware.Sensors)
        {
            double? value = sensor.Value.HasValue ? (double?)sensor.Value.Value : null;
            records.Add(new SensorRecord {
                Sensor = sensor,
                Type = sensor.SensorType.ToString(),
                Name = sensor.Name ?? "",
                Id = sensor.Identifier.ToString(),
                Hardware = hardware.Name ?? "",
                HardwareId = hardware.Identifier.ToString(),
                HardwareType = hardware.HardwareType.ToString(),
                Value = value
            });
        }
        foreach (IHardware child in hardware.SubHardware) Collect(child, records);
    }

    private static List<SensorRecord> Collect(Computer computer)
    {
        List<SensorRecord> records = new List<SensorRecord>();
        foreach (IHardware hardware in computer.Hardware) Collect(hardware, records);
        return records;
    }

    private static string Encode(string value)
    {
        return Convert.ToBase64String(Encoding.UTF8.GetBytes(value ?? ""));
    }

    private static string Decode(string value)
    {
        return Encoding.UTF8.GetString(Convert.FromBase64String(value));
    }

    private static int Channel(string identifier)
    {
        string[] parts = (identifier ?? "").Split('/');
        int channel;
        return parts.Length > 0 && Int32.TryParse(parts[parts.Length - 1], out channel) ? channel : -1;
    }

    private static int Minimum(string name, string hardware)
    {
        string text = ((name ?? "") + " " + (hardware ?? "")).ToLowerInvariant();
        return text.Contains("pump") || text.Contains("pompe") || text.Contains("water") || text.Contains("aio") ? 50 : 30;
    }

    private static string Number(double? value)
    {
        return value.HasValue ? value.Value.ToString("0.##", CultureInfo.InvariantCulture) : "-1";
    }

    private static int Scan(Computer computer)
    {
            List<SensorRecord> records = Collect(computer);
            SensorRecord cpu = records.Where(r => r.Type == "Temperature" && r.HardwareType == "Cpu" && r.Value.HasValue && r.Value.Value > 0)
                .OrderBy(r => (r.Name.IndexOf("Tctl", StringComparison.OrdinalIgnoreCase) >= 0 || r.Name.IndexOf("Package", StringComparison.OrdinalIgnoreCase) >= 0) ? 0 : 1).FirstOrDefault();
            SensorRecord gpu = records.Where(r => r.Type == "Temperature" && r.HardwareType.IndexOf("Gpu", StringComparison.OrdinalIgnoreCase) >= 0 && r.Value.HasValue && r.Value.Value > 0)
                .OrderBy(r => r.Name.IndexOf("GPU Core", StringComparison.OrdinalIgnoreCase) >= 0 ? 0 : 1).FirstOrDefault();
            Console.WriteLine("TEMP\tCPU\t" + Number(cpu == null ? null : cpu.Value));
            Console.WriteLine("TEMP\tGPU\t" + Number(gpu == null ? null : gpu.Value));

            List<SensorRecord> fans = records.Where(r => r.Type == "Fan").ToList();
            List<SensorRecord> controls = records.Where(r => r.Type == "Control" && r.Sensor.Control != null).ToList();
            HashSet<string> paired = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (SensorRecord control in controls)
            {
                int channel = Channel(control.Id);
                SensorRecord fan = fans.FirstOrDefault(f => String.Equals(f.HardwareId, control.HardwareId, StringComparison.OrdinalIgnoreCase) && Channel(f.Id) == channel);
                if (fan == null)
                {
                    string channelText = channel >= 0 ? channel.ToString(CultureInfo.InvariantCulture) : "";
                    fan = fans.FirstOrDefault(f => String.Equals(f.HardwareId, control.HardwareId, StringComparison.OrdinalIgnoreCase) && (channelText.Length == 0 || f.Name.Contains(channelText)));
                }
                if (fan != null) paired.Add(fan.Id);
                string name = !String.IsNullOrWhiteSpace(control.Name) && !control.Name.StartsWith("Control", StringComparison.OrdinalIgnoreCase)
                    ? control.Name : (fan != null ? fan.Name : "Ventilateur " + (channel + 1).ToString(CultureInfo.InvariantCulture));
                Console.WriteLine(String.Join("\t", new string[] {
                    "FAN", Encode(control.Id), Encode(name), Encode(control.Hardware),
                    Number(fan == null ? null : fan.Value), Number(control.Value),
                    Minimum(name, control.Hardware).ToString(CultureInfo.InvariantCulture), "1"
                }));
            }
            foreach (SensorRecord fan in fans)
            {
                if (paired.Contains(fan.Id)) continue;
                Console.WriteLine(String.Join("\t", new string[] {
                    "FAN", Encode(fan.Id), Encode(fan.Name), Encode(fan.Hardware),
                    Number(fan.Value), "-1", Minimum(fan.Name, fan.Hardware).ToString(CultureInfo.InvariantCulture), "0"
                }));
            }
        return 0;
    }

    private static int Change(Computer computer, string encodedId, bool automatic, float requested)
    {
        string id = Decode(encodedId);
            List<SensorRecord> records = Collect(computer);
            SensorRecord record = records.FirstOrDefault(r => String.Equals(r.Id, id, StringComparison.OrdinalIgnoreCase) && r.Sensor.Control != null);
            if (record == null)
            {
                Console.Error.WriteLine("Canal de ventilation introuvable.");
                return 3;
            }
            if (automatic)
            {
                record.Sensor.Control.SetDefault();
            }
            else
            {
                float minimum = Minimum(record.Name, record.Hardware);
                float value = Math.Max(minimum, Math.Min(100.0f, requested));
                record.Sensor.Control.SetSoftware(value);
            }
        return 0;
    }

    private static int Scan()
    {
        Computer computer = OpenComputer();
        try { return Scan(computer); }
        finally { computer.Close(); }
    }

    private static int Change(string encodedId, bool automatic, float requested)
    {
        Computer computer = OpenComputer();
        try { return Change(computer, encodedId, automatic, requested); }
        finally { computer.Close(); }
    }

    private static int Server()
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        Computer computer = OpenComputer();
        HashSet<string> modified = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            string command;
            while ((command = Console.ReadLine()) != null)
            {
                string[] parts = command.Trim().Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length == 0) { Console.WriteLine("RESULT\t2"); Console.Out.Flush(); continue; }
                if (String.Equals(parts[0], "quit", StringComparison.OrdinalIgnoreCase))
                {
                    Console.WriteLine("RESULT\t0");
                    Console.Out.Flush();
                    break;
                }
                int result = 2;
                try
                {
                    if (String.Equals(parts[0], "scan", StringComparison.OrdinalIgnoreCase)) result = Scan(computer);
                    else if (parts.Length >= 2 && String.Equals(parts[0], "auto", StringComparison.OrdinalIgnoreCase))
                    {
                        result = Change(computer, parts[1], true, 0);
                        if (result == 0) modified.Remove(parts[1]);
                    }
                    else if (parts.Length >= 3 && String.Equals(parts[0], "set", StringComparison.OrdinalIgnoreCase))
                    {
                        float value;
                        if (Single.TryParse(parts[2], NumberStyles.Float, CultureInfo.InvariantCulture, out value))
                        {
                            result = Change(computer, parts[1], false, value);
                            if (result == 0) modified.Add(parts[1]);
                        }
                    }
                }
                catch (Exception error)
                {
                    Console.Error.WriteLine(error.Message);
                    result = 1;
                }
                Console.WriteLine("RESULT\t" + result.ToString(CultureInfo.InvariantCulture));
                Console.Out.Flush();
            }
        }
        finally
        {
            foreach (string id in modified)
            {
                try { Change(computer, id, true, 0); } catch { }
            }
            computer.Close();
        }
        return 0;
    }

    public static int Main(string[] args)
    {
        try
        {
            if (args.Length >= 1 && String.Equals(args[0], "server", StringComparison.OrdinalIgnoreCase)) return Server();
            if (args.Length == 0 || String.Equals(args[0], "scan", StringComparison.OrdinalIgnoreCase)) return Scan();
            if (args.Length >= 2 && String.Equals(args[0], "auto", StringComparison.OrdinalIgnoreCase)) return Change(args[1], true, 0);
            if (args.Length >= 3 && String.Equals(args[0], "set", StringComparison.OrdinalIgnoreCase))
            {
                float value;
                if (!Single.TryParse(args[2], NumberStyles.Float, CultureInfo.InvariantCulture, out value)) return 2;
                return Change(args[1], false, value);
            }
            Console.Error.WriteLine("Usage: scan | set <id-base64> <pourcentage> | auto <id-base64>");
            return 2;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error.ToString());
            return 1;
        }
    }
}
