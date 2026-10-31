using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading.Channels;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Cvars;

namespace Cs2LiveViewerHelper;

// El plugin como fuente de eventos del partido (docs/analisis-y-propuestas.md,
// 4.2): manda al backend los mismos eventos que MatchZy manda por su
// webhook (series_start, going_live, round_end con stats por jugador,
// map_result, series_end) más player_death exacto, con "source":"plugin".
//
// Por qué: el webhook de MatchZy tiene tres fallas que no se arreglan
// desde afuera -- descarta matchzy_remote_log_url al cargar el match
// (se pierde series_start), y round_end trae KAST y otras stats en cero
// (issue #405). Acá las stats se acumulan de los eventos del juego
// (EventPlayerHurt/Death/RoundMvp), que sí son exactos. MatchZy sigue
// arbitrando (ready, knife, veto, backups); esto solo observa.
//
// De dónde sale el contexto del partido (matchid, número de mapa, qué
// equipo es team1/team2 y de qué lado está, marcador de la serie,
// cantidad de mapas, URL y header del webhook): del backup más nuevo
// que MatchZy escribe al arrancar cada ronda en
// game/csgo/MatchZyDataBackup/matchzy_<matchid>_<mapa>_round<NN>.json.
// Leerlo evita depender de una API de MatchZy que no existe y de tener
// que configurar un token propio: se usa el mismo webhook y el mismo
// header que MatchZy ya tiene para ese match. Verificado contra los
// archivos reales del servidor de esta PC (campos y formato).
//
// Nada de esto corre en el hilo del juego más allá de acumular
// contadores y encolar: la serialización, el HTTP y la cola en disco
// van en tareas aparte. Si el backend no responde, el evento se guarda
// en <plugin>/outbox/ y se reintenta cada 10 s en orden.
public sealed class MatchEvents : IDisposable
{
    private readonly BasePlugin _plugin;
    private readonly DirectorConfig _config;
    private readonly string _outboxDir;
    private readonly Channel<Outgoing> _outbox = Channel.CreateUnbounded<Outgoing>(new UnboundedChannelOptions { SingleReader = true });
    private readonly CancellationTokenSource _cts = new();
    private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(5) };
    private static readonly JsonSerializerOptions JsonOpts = new() { DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull };
    private const float TradeWindowSeconds = 5f;
    private const int MaxDamagePerVictimPerRound = 100;

    private sealed record Outgoing(string Url, string HeaderKey, string HeaderValue, string Body);

    private sealed class BackupContext
    {
        public long MatchId;
        public int MapNumber;
        public int Round; // rondas jugadas ANTES de la que arranca con este backup
        public string MapName = "";
        public string Team1Name = "";
        public string Team2Name = "";
        public string Team1Side = ""; // "CT" o "TERRORIST", tal cual el backup
        public string Team2Side = "";
        public int Team1Series;
        public int Team2Series;
        public int NumMaps = 1;
        public string RemoteLogUrl = "";
        public string HeaderKey = "";
        public string HeaderValue = "";
    }

    private sealed class PlayerStats
    {
        public string Name = "";
        public string Team = ""; // "team1"/"team2", lo último visto
        public int Kills, Deaths, Assists, Damage, HeadshotKills, Mvp, Kast, RoundsPlayed;
        public bool RoundKill, RoundAssist, RoundTraded, RoundDead, InRound;
        public readonly Dictionary<ulong, int> RoundDamageTo = new();

        public void ResetRound()
        {
            RoundKill = RoundAssist = RoundTraded = RoundDead = InRound = false;
            RoundDamageTo.Clear();
        }
    }

    private BackupContext? _ctx;
    private string _ctxFile = "";
    private DateTime _ctxMtime;
    private string? _backupDir;
    private long _announcedSeries = -1;
    private (long matchid, int map) _announcedLive = (-1, -1);
    private (long matchid, int map) _finishedMap = (-1, -1);
    private (long matchid, int map) _statsKey = (-1, -1);
    private readonly Dictionary<ulong, PlayerStats> _stats = new();
    private int _scoreCt1, _scoreT1, _scoreCt2, _scoreT2; // rondas ganadas por cada team en cada lado, desde going_live
    private string _startingSide1 = "";
    private string _startingSide2 = "";
    private float _roundStartTime;
    private readonly List<(ulong victim, ulong killer, float time)> _roundDeaths = new();
    private long _sent, _queued, _dropped;
    private int _seq;
    private string _lastError = "";
    private bool _warnedNoUrl;
    private int _hostPort; // el puerto de juego de esta instancia (0 = todavía no se pudo leer)

    public MatchEvents(BasePlugin plugin, DirectorConfig config)
    {
        _plugin = plugin;
        _config = config;
        // Una carpeta por instancia (1.8.0): dos servidores de la misma
        // instalación comparten ModuleDirectory, y el RetryLoop de cada uno
        // vaciaba (y mandaba) los pendientes del otro.
        var hostPort = DirectorFeed.PuertoDeJuego();
        _hostPort = hostPort;
        _outboxDir = hostPort > 0 ? Path.Combine(plugin.ModuleDirectory, "outbox", hostPort.ToString()) : Path.Combine(plugin.ModuleDirectory, "outbox");
    }

    public void Start()
    {
        Directory.CreateDirectory(_outboxDir);
        _ = Task.Run(SenderLoop);
        _ = Task.Run(RetryLoop);

        _plugin.RegisterEventHandler<EventRoundStart>((e, _) => { OnRoundStart(); return HookResult.Continue; }, HookMode.Post);
        _plugin.RegisterEventHandler<EventRoundFreezeEnd>((e, _) => { OnFreezeEnd(); return HookResult.Continue; }, HookMode.Post);
        _plugin.RegisterEventHandler<EventPlayerHurt>((e, _) => { OnPlayerHurt(e); return HookResult.Continue; }, HookMode.Post);
        _plugin.RegisterEventHandler<EventPlayerDeath>((e, _) => { OnPlayerDeath(e); return HookResult.Continue; }, HookMode.Post);
        _plugin.RegisterEventHandler<EventRoundMvp>((e, _) => { OnRoundMvp(e); return HookResult.Continue; }, HookMode.Post);
        _plugin.RegisterEventHandler<EventRoundEnd>((e, _) => { OnRoundEnd(e); return HookResult.Continue; }, HookMode.Post);
        _plugin.RegisterEventHandler<EventCsWinPanelMatch>((e, _) => { OnMatchEnd(); return HookResult.Continue; }, HookMode.Post);
        // Mapa nuevo: el contexto del anterior ya no vale (ver OlvidarContexto).
        _plugin.RegisterListener<Listeners.OnMapStart>(_ => OlvidarContexto());

        _plugin.AddCommand("css_lv_events_status", "Estado de los eventos de partido que manda este plugin al backend.", OnStatus);
        Log($"eventos de partido activos (outbox en {_outboxDir})");
    }

    // ---------------------------------------------------------------
    // Contexto: el backup más nuevo de MatchZy
    // ---------------------------------------------------------------

    private string? BackupDir()
    {
        if (_backupDir != null && Directory.Exists(_backupDir)) return _backupDir;
        // Server.GameDirectory apunta a .../game/csgo en CounterStrikeSharp;
        // se prueban las dos formas por si cambia entre versiones.
        foreach (var candidate in new[]
                 {
                     Path.Combine(Server.GameDirectory, "MatchZyDataBackup"),
                     Path.Combine(Server.GameDirectory, "csgo", "MatchZyDataBackup"),
                 })
        {
            if (Directory.Exists(candidate)) return _backupDir = candidate;
        }
        return null;
    }

    private BackupContext? RefreshContext()
    {
        var dir = BackupDir();
        if (dir == null) return _ctx = null;
        // MatchZyDataBackup es UNO para todas las instancias de la misma
        // instalación: el backup más nuevo puede ser del partido del OTRO
        // servidor. Se toma el más nuevo que sea de ESTE: el mismo mapa y
        // los equipos que este servidor tiene puestos (mp_teamname_1/2, que
        // pone MatchZy al cargar). Si no se puede saber (sin nombres), el
        // más nuevo del mismo mapa, como antes.
        var candidatos = new DirectoryInfo(dir)
            .EnumerateFiles("matchzy_*_round*.json")
            .OrderByDescending(f => f.LastWriteTimeUtc)
            .Take(8)
            .ToList();
        if (candidatos.Count == 0) return _ctx = null;
        var equipos = new[] { ConVar.Find("mp_teamname_1")?.StringValue ?? "", ConVar.Find("mp_teamname_2")?.StringValue ?? "" }
            .Where(n => n.Length > 0)
            .Select(n => n.ToLowerInvariant())
            .ToHashSet();
        var mapa = Server.MapName ?? "";
        FileInfo? elegido = null;
        BackupContext? elegidoCtx = null;
        foreach (var f in candidatos)
        {
            BackupContext parsed;
            try
            {
                parsed = f.FullName == _ctxFile && f.LastWriteTimeUtc == _ctxMtime && _ctx != null ? _ctx : ParseBackup(File.ReadAllText(f.FullName));
            }
            catch (Exception ex)
            {
                // MatchZy puede estar escribiéndolo justo ahora: se prueba el siguiente.
                _lastError = $"backup {f.Name}: {ex.Message}";
                continue;
            }
            if (!EsDeLoQueSeJuega(parsed, mapa, equipos)) continue;
            elegido = f;
            elegidoCtx = parsed;
            break;
        }
        if (elegido == null || elegidoCtx == null)
        {
            // Ningún respaldo es de lo que se juega ahora. Antes se seguía
            // con el contexto anterior aunque fuera de otro mapa: al retomar
            // la Final del «Torneo loco» (20/10/2026, un Mejor de 1 con el
            // mismo matchid) el plugin mandó eventos con el mapa del Inferno
            // y el backend los guardó como un «Mapa 4» que nunca existió.
            // Sin un respaldo de ESTE mapa y estos equipos, no hay contexto:
            // MatchZy escribe uno al terminar la primera ronda.
            if (_ctx != null && !EsDeLoQueSeJuega(_ctx, mapa, equipos)) OlvidarContexto();
            return _ctx;
        }
        _ctx = elegidoCtx;
        _ctxFile = elegido.FullName;
        _ctxMtime = elegido.LastWriteTimeUtc;
        return _ctx;
    }

    /// <summary>El respaldo es del mapa que está puesto y de los equipos que tiene este servidor (si se saben).</summary>
    private static bool EsDeLoQueSeJuega(BackupContext ctx, string mapa, HashSet<string> equipos)
    {
        if (mapa.Length > 0 && ctx.MapName.Length > 0 && !string.Equals(ctx.MapName, mapa, StringComparison.OrdinalIgnoreCase)) return false;
        if (equipos.Count > 0 && !(equipos.Contains(ctx.Team1Name.ToLowerInvariant()) && equipos.Contains(ctx.Team2Name.ToLowerInvariant()))) return false;
        return true;
    }

    // Al cambiar de mapa (y cuando el contexto dejó de ser de lo que se
    // juega) se vuelve a buscar de cero. Los marcadores y stats por mapa
    // los reinicia EnsureMatchState cuando aparezca el contexto nuevo.
    private void OlvidarContexto()
    {
        _ctx = null;
        _ctxFile = "";
        _ctxMtime = default;
    }

    private static string Str(JsonElement el, string name) =>
        el.TryGetProperty(name, out var v) ? (v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : v.ToString()) : "";

    private static int Int(JsonElement el, string name)
    {
        if (!el.TryGetProperty(name, out var v)) return 0;
        if (v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var n)) return n;
        return int.TryParse(v.ToString(), out var p) ? p : 0;
    }

    private static BackupContext ParseBackup(string json)
    {
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        var ctx = new BackupContext
        {
            MatchId = long.TryParse(Str(root, "matchid"), out var id) ? id : 0,
            MapNumber = Int(root, "mapnumber"),
            Round = Int(root, "round"),
            MapName = Str(root, "map_name"),
            Team1Name = Str(root, "team1_name"),
            Team2Name = Str(root, "team2_name"),
            Team1Side = Str(root, "team1_side"),
            Team2Side = Str(root, "team2_side"),
            Team1Series = Int(root, "team1_series_score"),
            Team2Series = Int(root, "team2_series_score"),
        };
        // match_config viene como STRING con JSON adentro (así lo guarda MatchZy).
        var cfgRaw = Str(root, "match_config");
        if (!string.IsNullOrWhiteSpace(cfgRaw))
        {
            using var cfg = JsonDocument.Parse(cfgRaw);
            var c = cfg.RootElement;
            ctx.NumMaps = Math.Max(1, Int(c, "NumMaps"));
            ctx.RemoteLogUrl = Str(c, "RemoteLogURL");
            ctx.HeaderKey = Str(c, "RemoteLogHeaderKey");
            ctx.HeaderValue = Str(c, "RemoteLogHeaderValue");
        }
        return ctx;
    }

    // Un backup viejo (de un match anterior) no vale como contexto: los
    // nombres tienen que coincidir con los mp_teamname_* que MatchZy pone
    // al cargar el match, y el mapa no puede ser uno que ya se dio por
    // terminado (la ronda de cuchillo del mapa siguiente pasa antes de
    // que exista su primer backup).
    private BackupContext? ActiveContext()
    {
        var ctx = RefreshContext();
        if (ctx == null || ctx.MatchId <= 0) return null;
        if (_finishedMap.matchid == ctx.MatchId && ctx.MapNumber <= _finishedMap.map) return null;
        var n1 = ConVar.Find("mp_teamname_1")?.StringValue?.Trim() ?? "";
        var n2 = ConVar.Find("mp_teamname_2")?.StringValue?.Trim() ?? "";
        if (n1.Length == 0 && n2.Length == 0) return null; // sin match cargado
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { n1, n2 };
        if (!names.Contains(ctx.Team1Name.Trim()) || !names.Contains(ctx.Team2Name.Trim())) return null;
        return ctx;
    }

    private static bool InWarmup()
    {
        var rules = Utilities.FindAllEntitiesByDesignerName<CCSGameRulesProxy>("cs_gamerules").FirstOrDefault()?.GameRules;
        return rules?.WarmupPeriod ?? false;
    }

    // ---------------------------------------------------------------
    // Eventos del juego (hilo del juego: solo contadores y encolar)
    // ---------------------------------------------------------------

    private void OnRoundStart()
    {
        _roundStartTime = Server.CurrentTime;
        _roundDeaths.Clear();
        foreach (var s in _stats.Values) s.ResetRound();
        foreach (var c in Utilities.GetPlayers())
        {
            if (c == null || !c.IsValid || c.IsBot || c.IsHLTV) continue;
            if (c.TeamNum != 2 && c.TeamNum != 3) continue;
            StatsOf(c).InRound = true;
        }
    }

    private void OnFreezeEnd()
    {
        try
        {
            var ctx = ActiveContext();
            if (ctx == null || InWarmup()) return;
            EnsureMatchState(ctx);
        }
        catch (Exception ex)
        {
            _lastError = $"freeze_end: {ex.Message}";
        }
    }

    // series_start una vez por matchid, going_live una vez por mapa; el
    // acumulado de stats y los marcadores por lado se reinician por mapa.
    private void EnsureMatchState(BackupContext ctx)
    {
        if (_announcedSeries != ctx.MatchId)
        {
            _announcedSeries = ctx.MatchId;
            Enqueue(ctx, "series_start", new Dictionary<string, object?>
            {
                ["num_maps"] = ctx.NumMaps,
                ["team1"] = new Dictionary<string, object?> { ["id"] = "team1", ["name"] = ctx.Team1Name },
                ["team2"] = new Dictionary<string, object?> { ["id"] = "team2", ["name"] = ctx.Team2Name },
            });
        }
        var key = (ctx.MatchId, ctx.MapNumber);
        if (_statsKey != key)
        {
            _statsKey = key;
            _stats.Clear();
            _scoreCt1 = _scoreT1 = _scoreCt2 = _scoreT2 = 0;
            _startingSide1 = ctx.Team1Side;
            _startingSide2 = ctx.Team2Side;
            OnRoundStart();
        }
        if (_announcedLive != key)
        {
            _announcedLive = key;
            Enqueue(ctx, "going_live", new Dictionary<string, object?> { ["map_number"] = ctx.MapNumber });
        }
    }

    private PlayerStats StatsOf(CCSPlayerController c)
    {
        if (!_stats.TryGetValue(c.SteamID, out var s))
        {
            s = new PlayerStats();
            _stats[c.SteamID] = s;
        }
        s.Name = c.PlayerName;
        if (_ctx != null && (c.TeamNum == 2 || c.TeamNum == 3)) s.Team = TeamLabel(_ctx, c.TeamNum);
        return s;
    }

    private static bool IsHuman(CCSPlayerController? c) => c != null && c.IsValid && !c.IsBot && !c.IsHLTV;

    private void OnPlayerHurt(EventPlayerHurt e)
    {
        try
        {
            if (_ctx == null) return;
            var victim = e.Userid;
            var attacker = e.Attacker;
            if (!IsHuman(victim) || !IsHuman(attacker) || victim!.Slot == attacker!.Slot) return;
            if (victim.TeamNum == attacker.TeamNum) return;
            var a = StatsOf(attacker);
            var already = a.RoundDamageTo.GetValueOrDefault(victim.SteamID);
            var add = Math.Min(Math.Max(0, e.DmgHealth), MaxDamagePerVictimPerRound - already);
            if (add <= 0) return;
            a.Damage += add;
            a.RoundDamageTo[victim.SteamID] = already + add;
        }
        catch (Exception ex)
        {
            _lastError = $"player_hurt: {ex.Message}";
        }
    }

    private void OnPlayerDeath(EventPlayerDeath e)
    {
        try
        {
            if (_ctx == null) return;
            var victim = e.Userid;
            if (!IsHuman(victim)) return;
            var v = StatsOf(victim!);
            v.Deaths++;
            v.RoundDead = true;

            var attacker = e.Attacker;
            var killerSid = 0UL;
            if (IsHuman(attacker) && attacker!.Slot != victim!.Slot && attacker.TeamNum != victim.TeamNum)
            {
                var a = StatsOf(attacker);
                a.Kills++;
                a.RoundKill = true;
                if (e.Headshot) a.HeadshotKills++;
                killerSid = attacker.SteamID;
                // Trade: el que acaba de morir había matado a un compañero del
                // atacante hace menos de TradeWindowSeconds -> ese compañero
                // cuenta como "traded" para su KAST.
                var now = Server.CurrentTime;
                foreach (var d in _roundDeaths)
                {
                    if (d.killer == victim.SteamID && now - d.time <= TradeWindowSeconds && _stats.TryGetValue(d.victim, out var traded) && traded.Team == a.Team)
                    {
                        traded.RoundTraded = true;
                    }
                }
            }
            var assister = e.Assister;
            if (IsHuman(assister) && assister!.TeamNum != victim!.TeamNum)
            {
                var s = StatsOf(assister);
                s.Assists++;
                s.RoundAssist = true;
            }
            _roundDeaths.Add((victim!.SteamID, killerSid, Server.CurrentTime));

            var ctx = _ctx;
            Enqueue(ctx, "player_death", new Dictionary<string, object?>
            {
                ["map_number"] = ctx.MapNumber,
                ["round_number"] = ctx.Round + 1,
                ["round_time"] = Math.Max(0, (int)(Server.CurrentTime - _roundStartTime)),
                ["victim"] = PlayerRef(victim),
                ["attacker"] = IsHuman(attacker) ? PlayerRef(attacker!) : null,
                ["assister"] = IsHuman(assister) ? PlayerRef(assister!) : null,
                ["weapon"] = e.Weapon,
                ["headshot"] = e.Headshot,
            });
        }
        catch (Exception ex)
        {
            _lastError = $"player_death: {ex.Message}";
        }
    }

    private Dictionary<string, object?> PlayerRef(CCSPlayerController c) => new()
    {
        ["steamid"] = c.SteamID.ToString(),
        ["name"] = c.PlayerName,
        ["team"] = _ctx != null && (c.TeamNum == 2 || c.TeamNum == 3) ? TeamLabel(_ctx, c.TeamNum) : null,
    };

    private void OnRoundMvp(EventRoundMvp e)
    {
        try
        {
            if (_ctx == null || !IsHuman(e.Userid)) return;
            StatsOf(e.Userid!).Mvp++;
        }
        catch (Exception ex)
        {
            _lastError = $"round_mvp: {ex.Message}";
        }
    }

    private void OnRoundEnd(EventRoundEnd e)
    {
        try
        {
            var ctx = ActiveContext();
            if (ctx == null || InWarmup()) return;
            EnsureMatchState(ctx);

            var winnerTeam = e.Winner == 2 || e.Winner == 3 ? TeamLabel(ctx, e.Winner) : (string?)null;
            var winnerSide = e.Winner == 3 ? "ct" : e.Winner == 2 ? "t" : "spec";
            if (winnerTeam == "team1") { if (e.Winner == 3) _scoreCt1++; else _scoreT1++; }
            else if (winnerTeam == "team2") { if (e.Winner == 3) _scoreCt2++; else _scoreT2++; }

            foreach (var s in _stats.Values)
            {
                if (!s.InRound) continue;
                s.RoundsPlayed++;
                if (s.RoundKill || s.RoundAssist || !s.RoundDead || s.RoundTraded) s.Kast++;
            }

            var (score1, score2) = TeamScores(ctx);
            var payload = new Dictionary<string, object?>
            {
                ["map_number"] = ctx.MapNumber,
                // El backup se escribe al arrancar la ronda con las rondas ya
                // jugadas: la que termina ahora es la siguiente. Si el
                // marcador dice otra cosa (backup viejo), manda el marcador.
                ["round_number"] = Math.Max(ctx.Round + 1, score1 + score2),
                ["round_time"] = Math.Max(0, (int)(Server.CurrentTime - _roundStartTime)),
                ["reason"] = e.Reason,
                ["winner"] = new Dictionary<string, object?> { ["side"] = winnerSide, ["team"] = winnerTeam },
                ["team1"] = TeamBlock(ctx, true, score1, ctx.Team1Series),
                ["team2"] = TeamBlock(ctx, false, score2, ctx.Team2Series),
            };
            Enqueue(ctx, "round_end", payload);
        }
        catch (Exception ex)
        {
            _lastError = $"round_end: {ex.Message}";
        }
    }

    private void OnMatchEnd()
    {
        try
        {
            var ctx = ActiveContext();
            if (ctx == null) return;
            var (score1, score2) = TeamScores(ctx);
            var winnerTeam = score1 == score2 ? (string?)null : score1 > score2 ? "team1" : "team2";
            var series1 = ctx.Team1Series + (winnerTeam == "team1" ? 1 : 0);
            var series2 = ctx.Team2Series + (winnerTeam == "team2" ? 1 : 0);
            var winnerSide = winnerTeam == null ? "spec" : SideCode(winnerTeam == "team1" ? ctx.Team1Side : ctx.Team2Side);

            Enqueue(ctx, "map_result", new Dictionary<string, object?>
            {
                ["map_number"] = ctx.MapNumber,
                ["winner"] = new Dictionary<string, object?> { ["side"] = winnerSide, ["team"] = winnerTeam },
                ["team1"] = TeamBlock(ctx, true, score1, series1),
                ["team2"] = TeamBlock(ctx, false, score2, series2),
            });
            _finishedMap = (ctx.MatchId, ctx.MapNumber);

            var toClinch = ctx.NumMaps / 2 + 1;
            var mapsPlayed = ctx.MapNumber + 1;
            if (series1 >= toClinch || series2 >= toClinch || mapsPlayed >= ctx.NumMaps)
            {
                Enqueue(ctx, "series_end", new Dictionary<string, object?>
                {
                    ["team1_series_score"] = series1,
                    ["team2_series_score"] = series2,
                    ["winner"] = new Dictionary<string, object?>
                    {
                        ["side"] = winnerSide,
                        ["team"] = series1 == series2 ? winnerTeam : series1 > series2 ? "team1" : "team2",
                    },
                    ["time_until_restore"] = 0,
                });
            }
        }
        catch (Exception ex)
        {
            _lastError = $"match_end: {ex.Message}";
        }
    }

    // ---------------------------------------------------------------
    // Armado de los bloques (mismo JSON que MatchZy, ver backend/src/types.ts)
    // ---------------------------------------------------------------

    private static string SideCode(string backupSide) =>
        backupSide.StartsWith("CT", StringComparison.OrdinalIgnoreCase) ? "ct" :
        backupSide.StartsWith("T", StringComparison.OrdinalIgnoreCase) ? "t" : "spec";

    // Qué team (team1/team2 del match) está en un TeamNum del juego (2=T, 3=CT).
    private static string TeamLabel(BackupContext ctx, int teamNum)
    {
        var team1IsCt = SideCode(ctx.Team1Side) == "ct";
        return teamNum == 3 ? (team1IsCt ? "team1" : "team2") : (team1IsCt ? "team2" : "team1");
    }

    // Marcador del mapa desde las entidades de equipo del juego (siguen a
    // los lados al cambiar de mitad, igual que el propio marcador).
    private static (int team1, int team2) TeamScores(BackupContext ctx)
    {
        int ct = 0, t = 0;
        foreach (var team in Utilities.FindAllEntitiesByDesignerName<CCSTeam>("cs_team_manager"))
        {
            if (team == null || !team.IsValid) continue;
            if (team.TeamNum == 3) ct = team.Score;
            else if (team.TeamNum == 2) t = team.Score;
        }
        return SideCode(ctx.Team1Side) == "ct" ? (ct, t) : (t, ct);
    }

    private Dictionary<string, object?> TeamBlock(BackupContext ctx, bool first, int score, int seriesScore)
    {
        var label = first ? "team1" : "team2";
        var players = new List<Dictionary<string, object?>>();
        foreach (var (sid, s) in _stats)
        {
            if (s.Team != label) continue;
            players.Add(new Dictionary<string, object?>
            {
                ["steamid"] = sid.ToString(),
                ["name"] = s.Name,
                ["stats"] = new Dictionary<string, object?>
                {
                    ["kills"] = s.Kills,
                    ["deaths"] = s.Deaths,
                    ["assists"] = s.Assists,
                    ["damage"] = s.Damage,
                    ["headshot_kills"] = s.HeadshotKills,
                    ["mvp"] = s.Mvp,
                    ["kast"] = s.Kast,
                    ["rounds_played"] = s.RoundsPlayed,
                },
            });
        }
        return new Dictionary<string, object?>
        {
            ["id"] = label,
            ["name"] = first ? ctx.Team1Name : ctx.Team2Name,
            ["series_score"] = seriesScore,
            ["score"] = score,
            ["score_ct"] = first ? _scoreCt1 : _scoreCt2,
            ["score_t"] = first ? _scoreT1 : _scoreT2,
            ["side"] = SideCode(first ? ctx.Team1Side : ctx.Team2Side),
            ["starting_side"] = SideCode(first ? _startingSide1 : _startingSide2),
            ["players"] = players,
        };
    }

    // ---------------------------------------------------------------
    // Salida: HTTP con cola en disco (fuera del hilo del juego)
    // ---------------------------------------------------------------

    private void Enqueue(BackupContext ctx, string eventName, Dictionary<string, object?> payload)
    {
        payload["event"] = eventName;
        payload["matchid"] = ctx.MatchId;
        payload["source"] = "plugin";
        // De qué servidor es (1.12.0): el puerto de juego de esta instancia.
        // MatchZy no dice nada del servidor en su webhook y no muestra un
        // pug en get5_status; con esto el backend sabe dónde se juega cada
        // matchid, también el de un pug. Sin puerto (0), no va.
        if (_hostPort <= 0) _hostPort = DirectorFeed.PuertoDeJuego();
        if (_hostPort > 0) payload["server_port"] = _hostPort;
        var url = !string.IsNullOrWhiteSpace(_config.MatchEventsUrl) ? _config.MatchEventsUrl.Trim() : ctx.RemoteLogUrl.Trim();
        if (url.Length == 0)
        {
            if (!_warnedNoUrl)
            {
                _warnedNoUrl = true;
                Log("sin URL para los eventos: ni match_events_url en la config ni matchzy_remote_log_url en el match cargado");
            }
            return;
        }
        var body = JsonSerializer.Serialize(payload, JsonOpts);
        _outbox.Writer.TryWrite(new Outgoing(url, ctx.HeaderKey, ctx.HeaderValue, body));
    }

    private async Task SenderLoop()
    {
        try
        {
            await foreach (var msg in _outbox.Reader.ReadAllAsync(_cts.Token))
            {
                var result = await Send(msg);
                if (result == SendResult.Ok) _sent++;
                else if (result == SendResult.Retry) QueueToDisk(msg);
                else _dropped++;
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex) { Log($"sender terminó: {ex.Message}"); }
    }

    private enum SendResult { Ok, Retry, Drop }

    private async Task<SendResult> Send(Outgoing msg)
    {
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Post, msg.Url) { Content = new StringContent(msg.Body, Encoding.UTF8, "application/json") };
            if (!string.IsNullOrEmpty(msg.HeaderKey) && !string.IsNullOrEmpty(msg.HeaderValue)) req.Headers.TryAddWithoutValidation(msg.HeaderKey, msg.HeaderValue);
            using var res = await Http.SendAsync(req, _cts.Token);
            if (res.IsSuccessStatusCode) return SendResult.Ok;
            var code = (int)res.StatusCode;
            _lastError = $"HTTP {code} de {msg.Url}";
            // 4xx (salvo tiempo agotado / rate limit) no va a mejorar reintentando.
            return code >= 400 && code < 500 && code != 408 && code != 429 ? SendResult.Drop : SendResult.Retry;
        }
        catch (OperationCanceledException) when (_cts.IsCancellationRequested) { return SendResult.Retry; }
        catch (Exception ex)
        {
            _lastError = $"{msg.Url}: {ex.Message}";
            return SendResult.Retry;
        }
    }

    private void QueueToDisk(Outgoing msg)
    {
        try
        {
            var name = $"{DateTime.UtcNow:yyyyMMddHHmmssfff}-{Interlocked.Increment(ref _seq):D4}.json";
            File.WriteAllText(Path.Combine(_outboxDir, name), JsonSerializer.Serialize(msg, JsonOpts));
            _queued++;
        }
        catch (Exception ex)
        {
            Log($"no se pudo guardar en la cola: {ex.Message}");
        }
    }

    private async Task RetryLoop()
    {
        try
        {
            while (!_cts.IsCancellationRequested)
            {
                await Task.Delay(TimeSpan.FromSeconds(10), _cts.Token);
                string[] files;
                try { files = Directory.GetFiles(_outboxDir, "*.json").OrderBy(f => f, StringComparer.Ordinal).ToArray(); }
                catch { continue; }
                foreach (var file in files)
                {
                    Outgoing? msg = null;
                    try
                    {
                        if (File.GetLastWriteTimeUtc(file) < DateTime.UtcNow.AddDays(-7)) { File.Delete(file); continue; }
                        msg = JsonSerializer.Deserialize<Outgoing>(File.ReadAllText(file), JsonOpts);
                    }
                    catch { }
                    if (msg == null) { try { File.Delete(file); } catch { } continue; }
                    var result = await Send(msg);
                    if (result == SendResult.Retry) break; // se mantiene el orden: el próximo intento arranca por este
                    try { File.Delete(file); } catch { }
                    if (result == SendResult.Ok) _sent++; else _dropped++;
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex) { Log($"retry terminó: {ex.Message}"); }
    }

    private void OnStatus(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!ComandosRegie.Puede(caller, cmd)) return;
        var ctx = _ctx;
        int pending = 0;
        try { pending = Directory.GetFiles(_outboxDir, "*.json").Length; } catch { }
        cmd.ReplyToCommand($"match events: enviados={_sent} encolados={_queued} descartados={_dropped} pendientes_en_disco={pending} jugadores_con_stats={_stats.Count}");
        cmd.ReplyToCommand(ctx == null
            ? "contexto: (sin backup de MatchZy leído todavía)"
            : $"contexto: matchid={ctx.MatchId} mapa={ctx.MapNumber} ({ctx.MapName}) ronda={ctx.Round} {ctx.Team1Name}({ctx.Team1Side}) vs {ctx.Team2Name}({ctx.Team2Side}) serie={ctx.Team1Series}-{ctx.Team2Series} num_maps={ctx.NumMaps} url={(ctx.RemoteLogUrl.Length > 0 ? ctx.RemoteLogUrl : "(vacía)")} header={(ctx.HeaderKey.Length > 0 ? ctx.HeaderKey : "(sin header)")}");
        cmd.ReplyToCommand($"ultimo error: {(_lastError.Length > 0 ? _lastError : "(ninguno)")}");
    }

    private static void Log(string msg) => Console.WriteLine($"[cs2-live-viewer eventos] {msg}");

    public void Dispose()
    {
        _cts.Cancel();
        _outbox.Writer.TryComplete();
    }
}
