using System.Diagnostics;
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
    private const int PreferredXInputSlot = 0;
    private const int NeutralTimeoutMs = 120;

    private readonly record struct SourceState(ushort Buttons, int Dpad,
                                               byte LeftX, byte LeftY, byte RightX, byte RightY,
                                               byte LeftTrigger, byte RightTrigger)
    {
        public static SourceState Neutral => new(0, 8, 128, 128, 128, 128, 0, 0);
    }

    private readonly record struct FilteredState(HMButton Buttons, HMHat Hat,
                                                 float LeftX, float LeftY, float RightX, float RightY,
                                                 float LeftTrigger, float RightTrigger);

    private sealed class InputFilter
    {
        private bool _initialized;
        private long _lastTimestamp;
        private float _leftX = 0.5f, _leftY = 0.5f, _rightX = 0.5f, _rightY = 0.5f;
        private float _leftTrigger, _rightTrigger;

        public void Reset()
        {
            _initialized = false;
            _lastTimestamp = 0;
            _leftX = _leftY = _rightX = _rightY = 0.5f;
            _leftTrigger = _rightTrigger = 0.0f;
        }

        public FilteredState Apply(SourceState source, long timestamp)
        {
            (float leftX, float leftY) = RadialStick(source.LeftX, source.LeftY);
            (float rightX, float rightY) = RadialStick(source.RightX, source.RightY);
            float leftTrigger = Trigger(source.LeftTrigger);
            float rightTrigger = Trigger(source.RightTrigger);

            float elapsedSeconds = _lastTimestamp == 0
                ? 1.0f / 250.0f
                : Math.Clamp((float)Stopwatch.GetElapsedTime(_lastTimestamp, timestamp).TotalSeconds,
                             0.0005f, 0.050f);
            _lastTimestamp = timestamp;
            if (!_initialized)
            {
                _leftX = leftX;
                _leftY = leftY;
                _rightX = rightX;
                _rightY = rightY;
                _leftTrigger = leftTrigger;
                _rightTrigger = rightTrigger;
                _initialized = true;
            }
            else
            {
                _leftX = Smooth(_leftX, leftX, elapsedSeconds);
                _leftY = Smooth(_leftY, leftY, elapsedSeconds);
                _rightX = Smooth(_rightX, rightX, elapsedSeconds);
                _rightY = Smooth(_rightY, rightY, elapsedSeconds);
                _leftTrigger = Smooth(_leftTrigger, leftTrigger, elapsedSeconds);
                _rightTrigger = Smooth(_rightTrigger, rightTrigger, elapsedSeconds);
            }

            return new FilteredState(MapButtons(source.Buttons), MapHat(source.Dpad),
                                     _leftX, _leftY, _rightX, _rightY,
                                     _leftTrigger, _rightTrigger);
        }

        private static (float X, float Y) RadialStick(byte rawX, byte rawY)
        {
            const float deadZone = 0.035f;
            float x = (rawX - 127.5f) / 127.5f;
            float y = (rawY - 127.5f) / 127.5f;
            float magnitude = MathF.Sqrt(x * x + y * y);
            if (magnitude <= deadZone) return (0.5f, 0.5f);
            float scaledMagnitude = Math.Clamp((magnitude - deadZone) / (1.0f - deadZone), 0.0f, 1.0f);
            float scale = scaledMagnitude / Math.Max(magnitude, 0.0001f);
            return (Math.Clamp((x * scale + 1.0f) * 0.5f, 0.0f, 1.0f),
                    Math.Clamp((y * scale + 1.0f) * 0.5f, 0.0f, 1.0f));
        }

        private static float Trigger(byte raw)
        {
            const float deadZone = 0.018f;
            float value = raw / 255.0f;
            return value <= deadZone ? 0.0f : (value - deadZone) / (1.0f - deadZone);
        }

        private static float Smooth(float current, float target, float elapsedSeconds)
        {
            float distance = MathF.Abs(target - current);
            if (distance >= 0.055f) return target;
            float alpha = 1.0f - MathF.Exp(-elapsedSeconds / 0.0045f);
            alpha = Math.Clamp(alpha, 0.38f, 1.0f);
            return current + (target - current) * alpha;
        }
    }

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

    private static bool TryReadInteger(ReadOnlySpan<char> input, ref int offset, out int value)
    {
        value = 0;
        while (offset < input.Length && input[offset] == ' ') ++offset;
        int begin = offset;
        while (offset < input.Length && input[offset] is >= '0' and <= '9') ++offset;
        return begin != offset && int.TryParse(input[begin..offset], NumberStyles.None,
                                               CultureInfo.InvariantCulture, out value);
    }

    private static bool TryParseState(string line, out SourceState state)
    {
        state = SourceState.Neutral;
        ReadOnlySpan<char> input = line.AsSpan().Trim();
        if (!input.StartsWith("STATE ", StringComparison.OrdinalIgnoreCase)) return false;
        int offset = 5;
        Span<int> values = stackalloc int[8];
        for (int index = 0; index < values.Length; ++index)
            if (!TryReadInteger(input, ref offset, out values[index])) return false;
        while (offset < input.Length && input[offset] == ' ') ++offset;
        if (offset != input.Length || values[0] is < 0 or > ushort.MaxValue || values[1] is < 0 or > 8)
            return false;
        for (int index = 2; index < values.Length; ++index)
            if (values[index] is < byte.MinValue or > byte.MaxValue) return false;
        state = new SourceState((ushort)values[0], values[1],
                                (byte)values[2], (byte)values[3], (byte)values[4], (byte)values[5],
                                (byte)values[6], (byte)values[7]);
        return true;
    }

    private static int SelfTest()
    {
        ushort source = (ushort)((1 << Cross) | (1 << Square) | (1 << R1) | (1 << Options));
        HMButton mapped = MapButtons(source);
        bool buttonsOk = mapped.HasFlag(HMButton.A) && mapped.HasFlag(HMButton.X) &&
                         mapped.HasFlag(HMButton.RightBumper) && mapped.HasFlag(HMButton.Start) &&
                         !mapped.HasFlag(HMButton.B);
        bool parseOk = TryParseState("STATE 3 7 0 255 128 64 10 240", out SourceState parsed) &&
                       parsed.Buttons == 3 && parsed.Dpad == 7 && parsed.LeftX == 0 && parsed.LeftY == 255 &&
                       parsed.RightX == 128 && parsed.RightY == 64 && parsed.LeftTrigger == 10 &&
                       parsed.RightTrigger == 240;
        bool rejectOk = !TryParseState("STATE 3 9 0 255 128 64 10 240", out _) &&
                        !TryParseState("STATE 3 7 0 256 128 64 10 240", out _);
        var filter = new InputFilter();
        FilteredState centered = filter.Apply(new SourceState(0, 8, 130, 126, 128, 128, 2, 0),
                                              Stopwatch.GetTimestamp());
        bool filterOk = MathF.Abs(centered.LeftX - 0.5f) < 0.001f &&
                        MathF.Abs(centered.LeftY - 0.5f) < 0.001f && centered.LeftTrigger == 0.0f;
        bool hatOk = MapHat(0) == HMHat.North && MapHat(7) == HMHat.NorthWest && MapHat(8) == HMHat.None;
        bool ok = buttonsOk && parseOk && rejectOk && filterOk && hatOk && PreferredXInputSlot == 0;
        Console.WriteLine(ok ? "SELFTEST OK" : "SELFTEST FAILED");
        return ok ? 0 : 21;
    }

    private static int Main(string[] args)
    {
        if (args.Length == 1 && args[0].Equals("--self-test", StringComparison.OrdinalIgnoreCase))
            return SelfTest();

        try
        {
            try { Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.High; } catch { }
            try { Thread.CurrentThread.Priority = ThreadPriority.Highest; } catch { }

            using var context = new HMContext();
            context.LoadDefaultProfiles();
            context.InstallDriver();
            var profile = context.GetProfile("xbox-360-wired")
                          ?? throw new InvalidOperationException("Xbox 360 profile unavailable.");

            HMController controller;
            try { controller = context.CreateControllerAt(PreferredXInputSlot, profile); }
            catch { controller = context.CreateController(profile); }
            using (controller)
            using (var stateReady = new AutoResetEvent(false))
            {
                object gate = new();
                SourceState latest = SourceState.Neutral;
                long publishedVersion = 0;
                bool stopping = false;
                var reader = new Thread(() =>
                {
                    try { Thread.CurrentThread.Priority = ThreadPriority.Highest; } catch { }
                    string? line;
                    while ((line = Console.ReadLine()) is not null)
                    {
                        if (line.Equals("STOP", StringComparison.OrdinalIgnoreCase))
                        {
                            lock (gate) stopping = true;
                            stateReady.Set();
                            return;
                        }
                        if (!TryParseState(line, out SourceState parsed)) continue;
                        lock (gate)
                        {
                            latest = parsed;
                            ++publishedVersion;
                        }
                        stateReady.Set();
                    }
                    lock (gate) stopping = true;
                    stateReady.Set();
                })
                {
                    IsBackground = true,
                    Name = "RGBCcontrol DualSense input",
                    Priority = ThreadPriority.Highest
                };
                reader.Start();

                var axes = HMGamepadStateHelpers.StandardAxes(profile, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.0f);
                HMGamepadState output = new() { Axes = axes, Buttons = 0, Hat = HMHat.None };
                var inputFilter = new InputFilter();
                long consumedVersion = 0;
                long lastInputTimestamp = Stopwatch.GetTimestamp();
                bool neutralSubmitted = true;

                void Submit(FilteredState state)
                {
                    axes[HMAxis.X] = state.LeftX;
                    axes[HMAxis.Y] = state.LeftY;
                    axes[HMAxis.Rx] = state.RightX;
                    axes[HMAxis.Ry] = state.RightY;
                    axes[HMAxis.Vx] = state.LeftTrigger;
                    axes[HMAxis.Vy] = state.RightTrigger;
                    output.Buttons = state.Buttons;
                    output.Hat = state.Hat;
                    controller.SubmitState(output);
                }

                Console.WriteLine("READY SLOT=1 FILTER=ADAPTIVE TIMEOUT=120");
                Console.Out.Flush();
                while (true)
                {
                    stateReady.WaitOne(4);
                    SourceState sourceState;
                    long version;
                    bool shouldStop;
                    lock (gate)
                    {
                        sourceState = latest;
                        version = publishedVersion;
                        shouldStop = stopping;
                    }
                    if (shouldStop) break;

                    long now = Stopwatch.GetTimestamp();
                    if (version != consumedVersion)
                    {
                        Submit(inputFilter.Apply(sourceState, now));
                        consumedVersion = version;
                        lastInputTimestamp = now;
                        neutralSubmitted = false;
                    }
                    else if (!neutralSubmitted &&
                             Stopwatch.GetElapsedTime(lastInputTimestamp, now).TotalMilliseconds >= NeutralTimeoutMs)
                    {
                        inputFilter.Reset();
                        Submit(new FilteredState(0, HMHat.None, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.0f));
                        neutralSubmitted = true;
                    }
                }

                inputFilter.Reset();
                Submit(new FilteredState(0, HMHat.None, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.0f));
            }
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine("ERROR " + exception.Message.Replace('\r', ' ').Replace('\n', ' '));
            Console.Error.Flush();
            return 31;
        }
    }
}
