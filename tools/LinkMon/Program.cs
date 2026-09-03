// LinkMon - telemetry viewer and validator for the OptiLink module.
//
// Two modes. --dashboard renders the module's live state as an operator would
// see it on a line card. --self-test drives a scripted fault scenario and
// checks a set of invariants at every step.
//
// The invariants are the reason this exists. A dashboard that faithfully
// renders inconsistent telemetry is worse than no dashboard, because it lends
// authority to a wrong number. Checking that a lane reported as FAULT really
// does have its transmitter off, and that the active-lane mask really does
// agree with the per-lane states, catches firmware bugs that a purely visual
// tool would render prettily and pass over.

using System.Diagnostics;
using System.Globalization;

namespace LinkMon;

/// <summary>One reading of everything the module publishes.</summary>
internal sealed record Telemetry(
    string ModuleState,
    int ActiveMask,
    bool Up,
    int ModuleFaults,
    double TemperatureC,
    double SupplyV,
    IReadOnlyList<LaneTelemetry> Lanes,
    bool AnyAlarm);

internal sealed record LaneTelemetry(
    int Index,
    string State,
    bool Enabled,
    bool TxOn,
    double BiasMa,
    double TxPowerMw,
    double RxPowerMw,
    int Faults,
    string LastFault);

/// <summary>A rule that must hold for any reading, and its result.</summary>
internal sealed record Finding(string Rule, bool Passed, string Detail);

/// <summary>Speaks the firmware console protocol over a pipe.</summary>
internal sealed class ModuleClient : IDisposable
{
    private readonly Process _process;

    public string Version { get; }

    public ModuleClient(string binary)
    {
        // Resolved up front: Process.Start does not interpret a relative path
        // the same way File.Exists does, so a path that passes the check can
        // still fail to launch.
        binary = Path.GetFullPath(binary);
        if (!File.Exists(binary))
        {
            throw new FileNotFoundException($"{binary} not built. Run `make host` first.");
        }

        _process = new Process
        {
            StartInfo = new ProcessStartInfo(binary)
            {
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                UseShellExecute = false,
            },
        };
        _process.Start();

        var banner = ReadUntilSentinel();
        Version = banner.Count > 0 ? Field(banner[0], "version") ?? "?" : "?";
    }

    /// <summary>Reads response lines up to the ok/err sentinel the firmware
    /// terminates every reply with.</summary>
    private List<string> ReadUntilSentinel()
    {
        var lines = new List<string>();
        while (true)
        {
            var line = _process.StandardOutput.ReadLine()
                ?? throw new InvalidOperationException("firmware exited mid-command");
            if (line == "ok")
            {
                return lines;
            }
            if (line.StartsWith("err ", StringComparison.Ordinal))
            {
                throw new InvalidOperationException(line[4..]);
            }
            lines.Add(line);
        }
    }

    public List<string> Command(string line)
    {
        _process.StandardInput.WriteLine(line);
        _process.StandardInput.Flush();
        return ReadUntilSentinel();
    }

    public void Tick(int milliseconds) => Command($"tick {milliseconds}");

    public void Inject(string signal, params int[] args) =>
        Command($"inject {signal} {string.Join(' ', args)}");

    private static string? Field(string line, string key)
    {
        foreach (var token in line.Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            var split = token.IndexOf('=');
            if (split > 0 && token[..split] == key)
            {
                return token[(split + 1)..];
            }
        }
        return null;
    }

    private static string Require(string line, string key) =>
        Field(line, key) ?? throw new InvalidOperationException($"missing {key} in: {line}");

    private static int Number(string text) =>
        text.StartsWith("0x", StringComparison.Ordinal)
            ? int.Parse(text[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
            : int.Parse(text, CultureInfo.InvariantCulture);

    public Telemetry Read()
    {
        var link = Command("link");
        var monitors = Command("mon");
        var flags = Command("flags");

        var module = link[0];
        var moduleMonitors = monitors[0];

        var lanes = new List<LaneTelemetry>();
        for (var i = 0; i < link.Count - 1; i++)
        {
            var laneLink = link[i + 1];
            var laneMon = monitors[i + 1];
            lanes.Add(new LaneTelemetry(
                Index: i,
                State: Require(laneLink, "state"),
                Enabled: Require(laneLink, "enabled") == "1",
                TxOn: Require(laneLink, "tx_on") == "1",
                // Raw CMIS units into engineering units: bias is 2 uA per
                // count, optical power 0.1 uW per count.
                BiasMa: Number(Require(laneMon, "bias")) * 2.0 / 1000.0,
                TxPowerMw: Number(Require(laneMon, "txpower")) / 10000.0,
                RxPowerMw: Number(Require(laneMon, "rxpower")) / 10000.0,
                Faults: Number(Require(laneLink, "faults")),
                LastFault: Require(laneLink, "last_fault")));
        }

        var anyAlarm = flags.Any(l => Field(l, "any_alarm") == "1");

        return new Telemetry(
            ModuleState: Require(module, "module"),
            ActiveMask: Number(Require(module, "active_mask")),
            Up: Require(module, "up") == "1",
            ModuleFaults: Number(Require(module, "faults")),
            TemperatureC: Number(Require(moduleMonitors, "temp")) / 256.0,
            SupplyV: Number(Require(moduleMonitors, "vcc")) / 10000.0,
            Lanes: lanes,
            AnyAlarm: anyAlarm);
    }

    public void Dispose()
    {
        if (!_process.HasExited)
        {
            _process.StandardInput.Close();
            if (!_process.WaitForExit(5000))
            {
                _process.Kill();
            }
        }
        _process.Dispose();
    }
}

/// <summary>The rules any reading must satisfy.</summary>
internal static class Validator
{
    private static readonly string[] ValidModuleStates =
        ["RESET", "LOW_PWR", "PWR_UP", "READY", "FAULT"];

    private static readonly string[] ValidLaneStates =
        ["DEACTIVATED", "INIT", "TX_TURN_ON", "ACTIVATED", "FAULT"];

    public static IReadOnlyList<Finding> Check(Telemetry t)
    {
        var findings = new List<Finding>();

        void Rule(string name, bool passed, string detail) =>
            findings.Add(new Finding(name, passed, detail));

        Rule("module state is a defined state",
            ValidModuleStates.Contains(t.ModuleState),
            t.ModuleState);

        // Monitor values only mean anything once the DDM task has run. In
        // RESET the module has not sampled yet and every reading is zero,
        // which is absence of data rather than a bad measurement.
        var hasSampled = t.ModuleState != "RESET";

        // The bound is the sensor's representable range, inclusive, not the
        // module's comfortable operating band. A reading of 125 C is a real
        // and correctly reported measurement; whether it is acceptable is
        // the threshold logic's business, and the alarm rule below covers it.
        Rule("temperature is a representable sensor reading",
            !hasSampled || t.TemperatureC is >= -40 and <= 125,
            hasSampled ? $"{t.TemperatureC:F1} C" : "not sampled yet");

        Rule("supply is a representable sensor reading",
            !hasSampled || t.SupplyV is >= 0 and <= 3.6,
            hasSampled ? $"{t.SupplyV:F3} V" : "not sampled yet");

        Rule("a ready module is running on a healthy supply",
            t.ModuleState != "READY" || t.SupplyV is > 3.1 and < 3.5,
            $"{t.SupplyV:F3} V in {t.ModuleState}");

        // The eye-safety invariant. A lane the firmware has declared faulted
        // must have had its laser squelched; anything else is a lit
        // transmitter on a link nobody is watching.
        var litFaultedLanes = t.Lanes.Where(l => l.State == "FAULT" && l.TxOn).ToList();
        Rule("no faulted lane still has its transmitter on",
            litFaultedLanes.Count == 0,
            litFaultedLanes.Count == 0
                ? "all faulted lanes dark"
                : $"lanes {string.Join(',', litFaultedLanes.Select(l => l.Index))} lit while faulted");

        var darkActiveLanes = t.Lanes.Where(l => l.State == "ACTIVATED" && !l.TxOn).ToList();
        Rule("every activated lane has its transmitter on",
            darkActiveLanes.Count == 0,
            darkActiveLanes.Count == 0
                ? "all active lanes lit"
                : $"lanes {string.Join(',', darkActiveLanes.Select(l => l.Index))} dark while active");

        // The mask is what a host reads out of the lower page without walking
        // page 11h, so the two views have to agree exactly.
        var derivedMask = t.Lanes
            .Where(l => l.State == "ACTIVATED")
            .Aggregate(0, (mask, l) => mask | (1 << l.Index));
        Rule("active lane mask agrees with the per-lane states",
            derivedMask == t.ActiveMask,
            $"reported 0x{t.ActiveMask:X2}, derived 0x{derivedMask:X2}");

        Rule("link up flag agrees with the lane states",
            t.Up == (t.ModuleState == "READY" && derivedMask != 0),
            $"up={t.Up}, module={t.ModuleState}, mask=0x{derivedMask:X2}");

        Rule("a module carrying an alarm is not advertised as ready",
            !(t.AnyAlarm && t.ModuleState == "READY"),
            $"alarm={t.AnyAlarm}, module={t.ModuleState}");

        foreach (var lane in t.Lanes)
        {
            Rule($"lane {lane.Index} state is a defined state",
                ValidLaneStates.Contains(lane.State),
                lane.State);

            if (lane.State == "ACTIVATED")
            {
                Rule($"lane {lane.Index} carries optical power while active",
                    lane.RxPowerMw > 0.0 && lane.TxPowerMw > 0.0,
                    $"tx {lane.TxPowerMw:F3} mW, rx {lane.RxPowerMw:F3} mW");

                Rule($"lane {lane.Index} bias is plausible for a live laser",
                    lane.BiasMa is > 1.0 and < 120.0,
                    $"{lane.BiasMa:F1} mA");
            }
        }

        return findings;
    }
}

internal static class Program
{
    private static string DefaultBinary()
    {
        var name = OperatingSystem.IsWindows() ? "optilink.exe" : "optilink";
        var root = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", ".."));
        return Path.Combine(root, "build", "host", name);
    }

    private static int Main(string[] args)
    {
        var binary = DefaultBinary();
        var mode = "dashboard";

        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--self-test":
                    mode = "self-test";
                    break;
                case "--dashboard":
                    mode = "dashboard";
                    break;
                case "--binary" when i + 1 < args.Length:
                    binary = args[++i];
                    break;
                case "--help":
                    Console.WriteLine("linkmon [--dashboard|--self-test] [--binary <path>]");
                    return 0;
            }
        }

        try
        {
            using var module = new ModuleClient(binary);
            return mode == "self-test" ? SelfTest(module) : Dashboard(module);
        }
        catch (Exception ex) when (ex is FileNotFoundException or InvalidOperationException)
        {
            Console.Error.WriteLine($"linkmon: {ex.Message}");
            return 2;
        }
    }

    private static int Dashboard(ModuleClient module)
    {
        module.Tick(300);
        Render(module.Read(), module.Version);
        return 0;
    }

    private static void Render(Telemetry t, string version)
    {
        var health = t.Up ? "UP" : "DOWN";
        Console.WriteLine($"┌─ OptiLink {version} ── module {t.ModuleState,-8} link {health,-5} "
                          + $"mask 0x{t.ActiveMask:X2} ─┐");
        Console.WriteLine($"│ temperature {t.TemperatureC,7:F1} C     supply {t.SupplyV,6:F3} V"
                          + $"     faults {t.ModuleFaults,3}      │");
        Console.WriteLine("├───────────────────────────────────────────────────────────────┤");
        Console.WriteLine("│ lane  state        tx    bias      tx pwr    rx pwr   fault    │");
        foreach (var lane in t.Lanes)
        {
            Console.WriteLine($"│  {lane.Index}    {lane.State,-12} {(lane.TxOn ? "on " : "off")}"
                              + $"  {lane.BiasMa,6:F1}mA  {lane.TxPowerMw,6:F3}mW  {lane.RxPowerMw,6:F3}mW"
                              + $"  {lane.LastFault,-9}│");
        }
        Console.WriteLine("└───────────────────────────────────────────────────────────────┘");
    }

    /// <summary>Drives a fault scenario, checking every invariant at each
    /// step. Any violation fails the run.</summary>
    private static int SelfTest(ModuleClient module)
    {
        var failures = 0;
        var checks = 0;

        void Step(string description, Action stimulus)
        {
            stimulus();
            var telemetry = module.Read();
            var findings = Validator.Check(telemetry);
            checks += findings.Count;

            var broken = findings.Where(f => !f.Passed).ToList();
            if (broken.Count == 0)
            {
                Console.WriteLine($"  pass  {description}  ({findings.Count} invariants)");
                return;
            }
            failures += broken.Count;
            Console.WriteLine($"  FAIL  {description}");
            foreach (var finding in broken)
            {
                Console.WriteLine($"          {finding.Rule}: {finding.Detail}");
            }
        }

        Console.WriteLine($"LinkMon self-test against firmware {module.Version}");

        Step("cold start", () => { });
        Step("link brought up", () => module.Tick(300));
        Step("lane 1 loses signal", () =>
        {
            module.Inject("los", 1, 1);
            module.Tick(60);
        });
        Step("lane 1 signal restored", () =>
        {
            module.Inject("los", 1, 0);
            module.Tick(300);
        });
        Step("lane 2 laser faults", () =>
        {
            module.Inject("txfault", 2, 1);
            module.Tick(60);
        });
        Step("module over temperature", () =>
        {
            module.Inject("temp", 4095);
            module.Tick(300);
        });
        Step("temperature recovered and alarm acknowledged", () =>
        {
            module.Inject("temp", 1985);
            module.Inject("txfault", 2, 0);
            module.Command("i2cr 8 1");
            module.Tick(600);
        });

        Console.WriteLine();
        Console.WriteLine($"{checks} invariants checked, {failures} violations");
        if (failures == 0)
        {
            Render(module.Read(), module.Version);
        }
        return failures == 0 ? 0 : 1;
    }
}
