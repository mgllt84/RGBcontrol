using System.Globalization;
using HIDMaestro;

namespace RGBCcontrol.GamepadBridge;

internal static class Program
{
    private const int Square = 0;
    private const int Cross = 1;
    private const int Circle = 2;
    private const int Triangle = 3;
    private const int L1 = 4;
    private const int R1 = 5;
    private const int Create = 8;
    private const int Options = 9;
    private const int L3 = 10;
    private const int R3 = 11;
    private const int Ps = 12;

    private static HMButton MapButtons(ushort source)
    {
        HMButton result = 0;
        if ((source & (1 << Cross)) != 0) result |= HMButton.A;
        if ((source & (1 << Circle)) != 0) result |= HMButton.B;
        if ((source & (1 << Square)) != 0) result |= HMButton.X;
        if ((source & (1 << Triangle)) != 0) result |= HMButton.Y;
        if ((source & (1 << L1)) != 0) result |= HMButton.LeftBumper;
        if ((source & (1 << R1)) != 0) result |= HMButton.RightBumper;
        if ((source & (1 << Create)) != 0) result |= HMButton.Back;
        if ((source & (1 << Options)) != 0) result |= HMButton.Start;
        if ((source & (1 << L3)) != 0) result |= HMButton.LeftStick;
        if ((source & (1 << R3)) != 0) result |= HMButton.RightStick;
        if ((source & (1 << Ps)) != 0) result |= HMButton.Guide;
        return result;
    }

    private static HMHat MapHat(int dpad) => dpad switch
    {
        0 => HMHat.North,
        1 => HMHat.NorthEast,
        2 => HMHat.East,
        3 => HMHat.SouthEast,
        4 => HMHat.South,
        5 => HMHat.SouthWest,
        6 => HMHat.West,
        7 => HMHat.NorthWest,
        _ => HMHat.None
    };

    private static bool TryParseState(string line, out ushort buttons, out int dpad,
                                      out byte lx, out byte ly, out byte rx, out byte ry,
                                      out byte lt, out byte rt)
    {
        buttons = 0;
        dpad = 8;
        lx = ly = rx = ry = 128;
        lt = rt = 0;
        string[] parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return parts.Length == 9 && parts[0].Equals("STATE", StringComparison.OrdinalIgnoreCase) &&
               ushort.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out buttons) &&
               int.TryParse(parts[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out dpad) &&
               byte.TryParse(parts[3], NumberStyles.Integer, CultureInfo.InvariantCulture, out lx) &&
               byte.TryParse(parts[4], NumberStyles.Integer, CultureInfo.InvariantCulture, out ly) &&
               byte.TryParse(parts[5], NumberStyles.Integer, CultureInfo.InvariantCulture, out rx) &&
               byte.TryParse(parts[6], NumberStyles.Integer, CultureInfo.InvariantCulture, out ry) &&
               byte.TryParse(parts[7], NumberStyles.Integer, CultureInfo.InvariantCulture, out lt) &&
               byte.TryParse(parts[8], NumberStyles.Integer, CultureInfo.InvariantCulture, out rt) &&
               dpad is >= 0 and <= 8;
    }

    private static int SelfTest()
    {
        ushort source = (ushort)((1 << Cross) | (1 << Square) | (1 << R1) | (1 << Options));
        HMButton mapped = MapButtons(source);
        bool buttonsOk = mapped.HasFlag(HMButton.A) && mapped.HasFlag(HMButton.X) &&
                         mapped.HasFlag(HMButton.RightBumper) && mapped.HasFlag(HMButton.Start) &&
                         !mapped.HasFlag(HMButton.B);
        bool parseOk = TryParseState("STATE 3 7 0 255 128 64 10 240", out ushort parsedButtons,
                                     out int dpad, out byte lx, out byte ly, out byte rx, out byte ry,
                                     out byte lt, out byte rt) && parsedButtons == 3 && dpad == 7 &&
                       lx == 0 && ly == 255 && rx == 128 && ry == 64 && lt == 10 && rt == 240;
        bool hatOk = MapHat(0) == HMHat.North && MapHat(7) == HMHat.NorthWest && MapHat(8) == HMHat.None;
        Console.WriteLine(buttonsOk && parseOk && hatOk ? "SELFTEST OK" : "SELFTEST FAILED");
        return buttonsOk && parseOk && hatOk ? 0 : 21;
    }

    private static int Main(string[] args)
    {
        if (args.Length == 1 && args[0].Equals("--self-test", StringComparison.OrdinalIgnoreCase))
            return SelfTest();

        try
        {
            using var context = new HMContext();
            context.LoadDefaultProfiles();
            context.InstallDriver();
            var profile = context.GetProfile("xbox-360-wired")
                          ?? throw new InvalidOperationException("Xbox 360 profile unavailable.");
            using var controller = context.CreateController(profile);
            Console.WriteLine("READY");
            Console.Out.Flush();

            string? line;
            while ((line = Console.ReadLine()) is not null)
            {
                if (line.Equals("STOP", StringComparison.OrdinalIgnoreCase)) break;
                if (!TryParseState(line, out ushort buttons, out int dpad,
                                   out byte lx, out byte ly, out byte rx, out byte ry,
                                   out byte lt, out byte rt)) continue;
                controller.SubmitState(new HMGamepadState
                {
                    Axes = HMGamepadStateHelpers.StandardAxes(profile,
                        leftStickX: lx / 255.0f,
                        leftStickY: ly / 255.0f,
                        rightStickX: rx / 255.0f,
                        rightStickY: ry / 255.0f,
                        leftTrigger: lt / 255.0f,
                        rightTrigger: rt / 255.0f),
                    Buttons = MapButtons(buttons),
                    Hat = MapHat(dpad)
                });
            }
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine("ERROR " + exception.Message.Replace('\r', ' ').Replace('\n', ' '));
            return 31;
        }
    }
}
