using System.Net;
using System.Runtime.InteropServices;
using System.Net.Sockets;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading.Channels;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Cvars;
using CounterStrikeSharp.API.Modules.Events;
using CounterStrikeSharp.API.Modules.Memory;
using CounterStrikeSharp.API.Modules.Utils;

namespace Cs2LiveViewerHelper;

// Config del plugin. CounterStrikeSharp la lee/crea sola en
// game/csgo/addons/counterstrikesharp/configs/plugins/Cs2LiveViewerHelper/Cs2LiveViewerHelper.json
public class DirectorConfig : BasePluginConfig
{
    [JsonPropertyName("director_enabled")] public bool DirectorEnabled { get; set; } = true;
    // 127.0.0.1 por defecto: el backend corre en la misma PC. Para un
    // backend en otra máquina poner "0.0.0.0" y filtrar por firewall.
    [JsonPropertyName("director_bind")] public string DirectorBind { get; set; } = "127.0.0.1";
    [JsonPropertyName("director_port")] public int DirectorPort { get; set; } = 27100;
    // Varias instancias de la MISMA instalación leen este mismo archivo:
    // con un solo puerto, la segunda no podía abrir el feed (y con él se
    // perdían el chat del veto, el veto en pantalla y la cámara). Con esto
    // cada una suma a director_port lo que su puerto de juego pasa del
    // 27015: 27015 -> 27100, 27016 -> 27101. Ver PuertoDelFeed.
    [JsonPropertyName("director_port_por_instancia")] public bool DirectorPortPorInstancia { get; set; } = true;
    // Snapshots por segundo. 64 ticks / 16 Hz = un snapshot cada 4 ticks.
    [JsonPropertyName("director_hz")] public int DirectorHz { get; set; } = 16;
    // SteamID64 del espectador que hace de camarógrafo. Vacío = se fija
    // en vivo con css_director_cameraman.
    [JsonPropertyName("director_cameraman_steamid")] public string DirectorCameramanSteamId { get; set; } = "";

    // Eventos de partido al backend (ver MatchEvents.cs). Por default se
    // mandan a la misma URL y con el mismo header que MatchZy tiene para
    // el match cargado (leídos de su backup); match_events_url la pisa.
    [JsonPropertyName("match_events_enabled")] public bool MatchEventsEnabled { get; set; } = true;
    [JsonPropertyName("match_events_url")] public string MatchEventsUrl { get; set; } = "";
}

// Por qué existe: para anticipar la acción (ver docs/investigacion-
// observador-automatico.md) hace falta el estado del juego a mayor
// frecuencia y con más detalle del que da GSI (10 Hz, sin velocidad,
// sin tick, kills inferidos). El servidor lo tiene todo tick a tick;
// este feed lo saca por TCP como NDJSON (una línea JSON por mensaje,
// esquema en docs/plan-observador-automatico.md) y además expone los
// comandos para mover la cámara del camarógrafo desde el backend.
public sealed class DirectorFeed : IDisposable
{
    private readonly BasePlugin _plugin;
    private readonly DirectorConfig _config;
    private readonly NdjsonServer _server;
    private readonly Channel<object> _outbox = Channel.CreateUnbounded<object>(new UnboundedChannelOptions { SingleReader = true });
    private readonly CancellationTokenSource _cts = new();
    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        NumberHandling = JsonNumberHandling.AllowNamedFloatingPointLiterals,
    };

    private int _ticksPerSnapshot;
    private string _mapName = "";
    private CCSGameRules? _gameRules;
    // Cámara forzada: mientras _lock esté activo se re-aplica cada tick,
    // porque no está confirmado que CS2 respete m_hObserverTarget escrito
    // desde el servidor más allá del siguiente think del observer.
    private int _cameramanSlot = -1;
    private int _lockTargetSlot = -1;
    private long _snapshotsSent;
    private long _eventsSent;
    private string _lastSpecResult = "(todavia no se uso css_director_spec)";
    // El veto en pantalla (1.4.0): chat con formato, panel y menú del capitán.
    private VetoEnPantalla? _veto;

    // Las granadas del inventario: de estas se manda cuántas lleva (1.13.0).
    private static readonly HashSet<string> GranadasDeInventario = new()
    {
        "weapon_flashbang", "weapon_hegrenade", "weapon_smokegrenade", "weapon_molotov", "weapon_incgrenade", "weapon_decoy",
    };

    private static readonly string[] ProjectileNames =
    {
        "smokegrenade_projectile", "flashbang_projectile", "hegrenade_projectile",
        "molotov_projectile", "decoy_projectile",
    };
    private static readonly Dictionary<string, string> ProjectileType = new()
    {
        ["smokegrenade_projectile"] = "smoke", ["flashbang_projectile"] = "flash",
        ["hegrenade_projectile"] = "he", ["molotov_projectile"] = "molotov", ["decoy_projectile"] = "decoy",
    };

    /** Desde cuándo se ve cada granada o fuego (por índice de entidad), para su tiempo de vida (1.16.0). */
    private readonly Dictionary<uint, float> _granadaDesde = new();
    /**
     * Los fuegos de molotov prendidos (1.16.0), por índice de entidad: se
     * anotan con EventInfernoStartburn y se borran al apagarse. Buscarlos
     * por nombre de entidad no los encontraba (probado en vivo).
     */
    private readonly HashSet<int> _fuegos = new();
    /** Cuándo el fuego le hizo daño a cada jugador por última vez (1.16.0), por slot. */
    private readonly Dictionary<int, float> _quemadoEn = new();
    private const string Fuego = "inferno";
    /**
     * Cómo terminó cada ronda del mapa (1.15.0): el motivo de cada
     * EventRoundEnd, en orden. Lo lleva el plugin porque el juego no lo da
     * en un campo confiable (m_iMatchStats_RoundResults no trae el motivo).
     */
    private readonly List<int> _resultados = new();

    public DirectorFeed(BasePlugin plugin, DirectorConfig config)
    {
        _plugin = plugin;
        _config = config;
        _ticksPerSnapshot = Math.Max(1, 64 / Math.Clamp(config.DirectorHz, 1, 64));
        _hostPort = PuertoDeJuego();
        _feedPort = PuertoDelFeed(config, _hostPort);
        _server = new NdjsonServer(config.DirectorBind, _feedPort, HelloLine, Log);
        if (ulong.TryParse(config.DirectorCameramanSteamId, out var sid) && sid != 0)
        {
            _pendingCameramanSteamId = sid;
        }
    }

    private ulong _pendingCameramanSteamId;
    private readonly int _hostPort;
    private readonly int _feedPort;

    /** El puerto de juego de esta instancia (-port), o 0 si no se pudo leer. */
    public static int PuertoDeJuego()
    {
        try
        {
            return ConVar.Find("hostport")?.GetPrimitiveValue<int>() ?? 0;
        }
        catch
        {
            return 0;
        }
    }

    /**
     * El puerto del feed de esta instancia: director_port más lo que el
     * puerto de juego pasa del 27015 (hasta 50; fuera de eso, o con
     * director_port_por_instancia en false, director_port tal cual). La
     * misma cuenta hace el backend (servidoresCs2/puertos.ts) para saber a
     * qué puerto conectarse con cada servidor del panel.
     */
    public static int PuertoDelFeed(DirectorConfig config, int hostPort)
    {
        if (!config.DirectorPortPorInstancia) return config.DirectorPort;
        var offset = hostPort - 27015;
        return offset >= 0 && offset <= 50 ? config.DirectorPort + offset : config.DirectorPort;
    }

    public void Start(bool hotReload)
    {
        if (hotReload)
        {
            _mapName = Server.MapName;
            _gameRules = null;
        }
        _server.Start();
        _ = Task.Run(SerializerLoop);

        _plugin.RegisterListener<Listeners.OnMapStart>(OnMapStart);
        _plugin.RegisterListener<Listeners.OnTick>(OnTick);

        _plugin.RegisterEventHandler<EventRoundStart>((e, _) => Emit(e, "round_start", null), HookMode.Post);
        _plugin.RegisterEventHandler<EventRoundFreezeEnd>((e, _) => Emit(e, "round_freeze_end", null), HookMode.Post);
        _plugin.RegisterEventHandler<EventRoundEnd>((e, _) => Emit(e, "round_end", d => { d["winner"] = e.Winner; d["reason"] = e.Reason; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventRoundEnd>((e, _) =>
        {
            // Las del calentamiento no cuentan: el recorte por rondas jugadas
            // (BuildSnapshot) las saca igual.
            if (GameRules()?.WarmupPeriod != true) _resultados.Add(e.Reason);
            return HookResult.Continue;
        }, HookMode.Post);
        _plugin.RegisterEventHandler<EventRoundStart>((e, _) =>
        {
            // La primera ronda (después del calentamiento o de un
            // mp_restartgame): lo de antes era otro partido.
            if (GameRules()?.TotalRoundsPlayed == 0) _resultados.Clear();
            return HookResult.Continue;
        }, HookMode.Post);
        _plugin.RegisterEventHandler<EventPlayerHurt>((e, _) => Emit(e, "player_hurt", d =>
        {
            d["attacker"] = SlotOf(e.Attacker); d["victim"] = SlotOf(e.Userid);
            d["dmg"] = e.DmgHealth; d["dmgArmor"] = e.DmgArmor; d["hp"] = e.Health; d["weapon"] = e.Weapon; d["hitgroup"] = e.Hitgroup;
        }), HookMode.Post);
        _plugin.RegisterEventHandler<EventPlayerDeath>((e, _) => Emit(e, "player_death", d =>
        {
            d["attacker"] = SlotOf(e.Attacker); d["victim"] = SlotOf(e.Userid); d["assister"] = SlotOf(e.Assister);
            d["weapon"] = e.Weapon; d["headshot"] = e.Headshot; d["penetrated"] = e.Penetrated;
            d["thrusmoke"] = e.Thrusmoke; d["attackerblind"] = e.Attackerblind; d["noscope"] = e.Noscope; d["distance"] = e.Distance;
        }), HookMode.Post);
        _plugin.RegisterEventHandler<EventWeaponFire>((e, _) => Emit(e, "weapon_fire", d =>
        {
            d["slot"] = SlotOf(e.Userid); d["weapon"] = e.Weapon; d["silenced"] = e.Silenced;
            // El análisis del duelo (1.17.1): de dónde sale la bala, la mira y
            // la velocidad. En un método aparte y con try: si al CounterStrikeSharp
            // del servidor le falta una propiedad, el disparo sale igual (en la
            // 1.17.0 AimPunchAngle, que ya no existe, se llevaba puesto el evento entero).
            try { MiraDelDisparo(e.Userid, d); } catch (Exception ex) { d["fireError"] = $"{ex.GetType().Name}: {ex.Message}"; }
        }), HookMode.Post);
        // Dónde pegó cada bala (1.17.1): la línea de los ojos al impacto es la
        // dirección real de la bala, con el retroceso y la dispersión.
        _plugin.RegisterEventHandler<EventBulletImpact>((e, _) => Emit(e, "bullet_impact", d => { d["slot"] = SlotOf(e.Userid); d["pos"] = new[] { e.X, e.Y, e.Z }; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventPlayerSound>((e, _) => Emit(e, "player_sound", d => { d["slot"] = SlotOf(e.Userid); d["radius"] = e.Radius; d["duration"] = e.Duration; d["step"] = e.Step; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventPlayerFootstep>((e, _) => Emit(e, "player_footstep", d => { d["slot"] = SlotOf(e.Userid); }), HookMode.Post);
        _plugin.RegisterEventHandler<EventPlayerBlind>((e, _) => Emit(e, "player_blind", d => { d["slot"] = SlotOf(e.Userid); d["attacker"] = SlotOf(e.Attacker); d["duration"] = e.BlindDuration; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventGrenadeThrown>((e, _) => Emit(e, "grenade_thrown", d => { d["slot"] = SlotOf(e.Userid); d["weapon"] = e.Weapon; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventBombBeginplant>((e, _) => Emit(e, "bomb_beginplant", d => { d["slot"] = SlotOf(e.Userid); d["site"] = e.Site; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventBombAbortplant>((e, _) => Emit(e, "bomb_abortplant", d => { d["slot"] = SlotOf(e.Userid); d["site"] = e.Site; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventBombPlanted>((e, _) => Emit(e, "bomb_planted", d => { d["slot"] = SlotOf(e.Userid); d["site"] = e.Site; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventBombBegindefuse>((e, _) => Emit(e, "bomb_begindefuse", d => { d["slot"] = SlotOf(e.Userid); d["haskit"] = e.Haskit; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventBombAbortdefuse>((e, _) => Emit(e, "bomb_abortdefuse", d => { d["slot"] = SlotOf(e.Userid); }), HookMode.Post);
        _plugin.RegisterEventHandler<EventBombDefused>((e, _) => Emit(e, "bomb_defused", d => { d["slot"] = SlotOf(e.Userid); d["site"] = e.Site; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventBombExploded>((e, _) => Emit(e, "bomb_exploded", d => { d["site"] = e.Site; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventBombPickup>((e, _) => Emit(e, "bomb_pickup", d => { d["slot"] = SlotOf(e.Userid); }), HookMode.Post);
        _plugin.RegisterEventHandler<EventBombDropped>((e, _) => Emit(e, "bomb_dropped", d => { d["slot"] = SlotOf(e.Userid); }), HookMode.Post);
        _plugin.RegisterEventHandler<EventSmokegrenadeDetonate>((e, _) => Emit(e, "smokegrenade_detonate", d => { d["slot"] = SlotOf(e.Userid); d["pos"] = new[] { e.X, e.Y, e.Z }; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventFlashbangDetonate>((e, _) => Emit(e, "flashbang_detonate", d => { d["slot"] = SlotOf(e.Userid); d["pos"] = new[] { e.X, e.Y, e.Z }; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventHegrenadeDetonate>((e, _) => Emit(e, "hegrenade_detonate", d => { d["slot"] = SlotOf(e.Userid); d["pos"] = new[] { e.X, e.Y, e.Z }; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventDecoyDetonate>((e, _) => Emit(e, "decoy_detonate", d => { d["slot"] = SlotOf(e.Userid); d["pos"] = new[] { e.X, e.Y, e.Z }; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventInfernoStartburn>((e, _) => Emit(e, "inferno_startburn", d => { d["pos"] = new[] { e.X, e.Y, e.Z }; }), HookMode.Post);
        _plugin.RegisterEventHandler<EventInfernoStartburn>((e, _) => { _fuegos.Add(e.Entityid); return HookResult.Continue; }, HookMode.Post);
        _plugin.RegisterEventHandler<EventInfernoExpire>((e, _) => { _fuegos.Remove(e.Entityid); return HookResult.Continue; }, HookMode.Post);
        _plugin.RegisterEventHandler<EventInfernoExtinguish>((e, _) => { _fuegos.Remove(e.Entityid); return HookResult.Continue; }, HookMode.Post);
        _plugin.RegisterEventHandler<EventPlayerHurt>((e, _) =>
        {
            // El daño del fuego llega con el arma "inferno" (1.16.0).
            var victima = e.Userid;
            if (victima != null && victima.IsValid && (e.Weapon == "inferno" || e.Weapon == "molotov" || e.Weapon == "incgrenade"))
                _quemadoEn[victima.Slot] = Server.CurrentTime;
            return HookResult.Continue;
        }, HookMode.Post);
        // El chat, pero SOLO los comandos del veto de mapas por chat
        // (backend/src/vetoServidor/): el capitán escribe ".vetar mirage" y
        // el backend lo aplica. El resto del chat no se manda: no le sirve
        // al observador y no tiene por qué salir del servidor.
        _plugin.RegisterEventHandler<EventPlayerChat>((e, _) => { ChatDelVeto(e); return HookResult.Continue; }, HookMode.Post);

        // Vive acá porque lo que el capitán elige en el menú sale por este
        // mismo feed, igual que lo que escribe en el chat.
        try
        {
            _veto = new VetoEnPantalla(_plugin, ReenviarComandoDelVeto, Log);
            _veto.Start();
        }
        catch (Exception ex)
        {
            Log($"no se pudo iniciar el veto en pantalla: {ex.Message}");
            _veto?.Dispose();
            _veto = null;
        }

        _plugin.AddCommand("css_director_status", "Estado del feed del director y de la cámara forzada.", OnStatus);
        _plugin.AddCommand("css_director_cameraman", "Fija el camarógrafo: css_director_cameraman <steamid64|slot|none>", OnCameraman);
        _plugin.AddCommand("css_director_spec", "Pone al camarógrafo en primera persona sobre un jugador: css_director_spec <slot|steamid64|none>", OnSpec);
        _plugin.AddCommand("css_director_lock", "Re-aplicar la cámara forzada cada tick: css_director_lock <0|1>", OnLock);
        _plugin.AddCommand("css_regie_hud", "Acción del HUD de la transmisión: css_regie_hud <acción> [show|hide]", OnHud);
        _plugin.AddCommand("css_director_specmode", "Metodo para forzar la camara (1..5, ver DirectorFeed.cs) y offsets resueltos: css_director_specmode [n]", OnSpecMode);

        Log($"feed escuchando en {_config.DirectorBind}:{_feedPort} (servidor :{_hostPort}) a {64 / _ticksPerSnapshot} Hz");
    }

    private bool _lockEnabled = false;
    // Metodo para forzar la camara, seleccionable en caliente con
    // css_director_specmode para poder bisectar sin recompilar:
    //   1 = solo m_hObserverTarget
    //   2 = target + m_iObserverMode (IN_EYE)
    //   3 = 2 + objetivo/modo DESEADOS en el controller (CCSPlayerController.
    //       m_hDesiredObserverTarget / m_DesiredObserverMode) + SetStateChanged
    //   4 = 2 + SetStateChanged(m_pObserverServices) en el pawn
    //   5 = 3 + 4   [default: el unico que llega al cliente, verificado 2026-09-21]
    // Sospecha del primer crash (2026-09-21): escribir un miembro cuyo
    // offset no existe -> offset 0 -> pisa el puntero de la vtable.
    private int _specMode = 5;

    // ---------------------------------------------------------------
    // Snapshot por tick
    // ---------------------------------------------------------------

    private void OnMapStart(string mapName)
    {
        _mapName = mapName;
        _gameRules = null;
        _lockTargetSlot = -1;
        _granadaDesde.Clear();
        _fuegos.Clear();
        _quemadoEn.Clear();
        _resultados.Clear();
    }

    private void OnTick()
    {
        var tick = Server.TickCount;
        if (_lockEnabled && _lockTargetSlot >= 0 && _cameramanSlot >= 0)
        {
            ApplySpectate(_cameramanSlot, _lockTargetSlot, quiet: true);
        }
        if (tick % _ticksPerSnapshot != 0) return;
        if (_pendingCameramanSteamId != 0) ResolvePendingCameraman();

        try
        {
            var snap = BuildSnapshot(tick);
            if (snap != null && _outbox.Writer.TryWrite(snap)) _snapshotsSent++;
        }
        catch (Exception ex)
        {
            // Nunca tirar el tick del servidor por un fallo del feed.
            if (_snapshotsSent % 64 == 0) Log($"error armando snapshot: {ex.Message}");
        }
    }

    private CCSGameRules? GameRules()
    {
        if (_gameRules != null) return _gameRules;
        var proxy = Utilities.FindAllEntitiesByDesignerName<CCSGameRulesProxy>("cs_gamerules").FirstOrDefault();
        _gameRules = proxy?.GameRules;
        return _gameRules;
    }

    private SnapMsg? BuildSnapshot(int tick)
    {
        var rules = GameRules();
        var now = Server.CurrentTime;
        var players = new List<SnapPlayer>(10);
        var pawnIndexToSlot = new Dictionary<uint, int>(10);
        int carrierSlot = -1;

        foreach (var c in Utilities.GetPlayers())
        {
            if (c == null || !c.IsValid || c.IsHLTV) continue;
            var team = c.TeamNum;
            if (team != 2 && team != 3) continue;
            var pawn = c.PlayerPawn.Value;
            var alive = c.PawnIsAlive && pawn != null && pawn.IsValid && pawn.LifeState == (byte)LifeState_t.LIFE_ALIVE;
            var p = new SnapPlayer
            {
                Slot = c.Slot,
                Sid = c.SteamID.ToString(),
                Name = c.PlayerName,
                Team = team,
                Alive = alive,
                Bot = c.IsBot,
                // Lo que antes solo traía el GSI de un observador (1.8.0).
                Money = c.InGameMoneyServices?.Account ?? 0,
                Mvps = c.MVPs,
                Score = c.Score,
            };
            var stats = c.ActionTrackingServices?.MatchStats;
            if (stats != null)
            {
                p.Kills = stats.Kills;
                p.Deaths = stats.Deaths;
                p.Assists = stats.Assists;
                p.Damage = stats.Damage;
            }
            if (pawn != null && pawn.IsValid)
            {
                pawnIndexToSlot[pawn.Index] = c.Slot;
                var pos = pawn.AbsOrigin;
                var vel = pawn.AbsVelocity;
                var ang = pawn.EyeAngles;
                if (pos != null) p.Pos = new[] { pos.X, pos.Y, pos.Z };
                if (vel != null) p.Vel = new[] { vel.X, vel.Y, vel.Z };
                if (ang != null) p.Ang = new[] { ang.X, ang.Y };
                p.Hp = pawn.Health;
                p.Armor = pawn.ArmorValue;
                p.Flash = pawn.FlashDuration;
                p.Scoped = pawn.IsScoped;
                p.Walking = pawn.IsWalking;
                p.Defusing = pawn.IsDefusing;
                p.InBombZone = pawn.InBombZone;
                p.Ducking = (pawn.Flags & 2) != 0; // FL_DUCKING
                p.Place = pawn.LastPlaceName;
                // Quemándose (1.16.0): hace cuánto le hizo daño el fuego de
                // una molotov; el backend lo pasa a la escala del GSI.
                if (_quemadoEn.TryGetValue(c.Slot, out var quemadoEn) && now - quemadoEn >= 0 && now - quemadoEn < 2) p.Burn = now - quemadoEn;
                if (pawn.ItemServices != null)
                {
                    var items = new CCSPlayer_ItemServices(pawn.ItemServices.Handle);
                    p.Helmet = items.HasHelmet;
                    p.DefuseKit = items.HasDefuser;
                }
                var ws = pawn.WeaponServices;
                if (ws != null)
                {
                    var activa = ws.ActiveWeapon.Value;
                    p.Weapon = activa?.DesignerName;
                    // La munición del arma en mano (1.13.0), como el GSI: el
                    // cargador, lo que entra en uno y la reserva.
                    if (activa != null && activa.IsValid && activa.VData != null && activa.VData.MaxClip1 > 0)
                    {
                        p.Clip = activa.Clip1;
                        p.ClipMax = activa.VData.MaxClip1;
                        p.Reserve = activa.ReserveAmmo[0];
                    }
                    // Todo el inventario (1.8.0): el HUD muestra la principal,
                    // la secundaria y las granadas. Antes cortaba al encontrar
                    // la C4.
                    var inventario = new List<string>(6);
                    foreach (var wh in ws.MyWeapons)
                    {
                        var w = wh.Value;
                        if (w == null || !w.IsValid) continue;
                        inventario.Add(w.DesignerName);
                        if (w.DesignerName == "weapon_c4") { p.HasBomb = true; carrierSlot = c.Slot; }
                        // Cuántas granadas iguales lleva (1.13.0): dos flashes
                        // son una sola arma con munición 2.
                        if (GranadasDeInventario.Contains(w.DesignerName) && w.VData != null)
                        {
                            var tipo = w.VData.PrimaryAmmoType;
                            var cantidad = tipo < ws.Ammo.Length ? ws.Ammo[tipo] : 1;
                            if (cantidad > 1) (p.Nades ??= new Dictionary<string, int>())[w.DesignerName] = cantidad;
                        }
                    }
                    p.Weapons = inventario;
                }
            }
            players.Add(p);
        }

        var nades = new List<SnapNade>();
        var vistas = new HashSet<uint>();
        // Segundos desde que se ve (1.16.0), como el lifetime del GSI.
        float VidaDe(uint indice)
        {
            vistas.Add(indice);
            if (!_granadaDesde.TryGetValue(indice, out var desde)) _granadaDesde[indice] = desde = now;
            return now - desde;
        }
        float TicksASegundos(int desdeTick) => desdeTick > 0 ? Math.Max(0, (Server.TickCount - desdeTick) / 64f) : 0;
        foreach (var designer in ProjectileNames)
        {
            foreach (var g in Utilities.FindAllEntitiesByDesignerName<CBaseCSGrenadeProjectile>(designer))
            {
                if (g == null || !g.IsValid) continue;
                var pos = g.AbsOrigin; var vel = g.AbsVelocity;
                var thrower = g.Thrower.Value;
                int throwerSlot = thrower != null && pawnIndexToSlot.TryGetValue(thrower.Index, out var s) ? s : -1;
                var nade = new SnapNade
                {
                    Id = (int)g.Index,
                    Type = ProjectileType[designer],
                    Thrower = throwerSlot,
                    Pos = pos != null ? new[] { pos.X, pos.Y, pos.Z } : null,
                    Vel = vel != null ? new[] { vel.X, vel.Y, vel.Z } : null,
                    Det = g.DetonateTime > 0 ? g.DetonateTime - now : (float?)null,
                    Life = VidaDe(g.Index),
                };
                // Un humo ya desplegado (1.16.0): hace cuánto, como el
                // effecttime del GSI. Con eso el radar dibuja la nube.
                if (designer == "smokegrenade_projectile")
                {
                    var humo = new CSmokeGrenadeProjectile(g.Handle);
                    if (humo.DidSmokeEffect) nade.Eff = TicksASegundos(humo.SmokeEffectTickBegin);
                }
                nades.Add(nade);
            }
        }
        // El fuego de las molotov e incendiarias (1.16.0): cada llama que
        // sigue prendida, como los flames del GSI.
        foreach (var indice in _fuegos.ToList())
        {
            var f = Utilities.GetEntityFromIndex<CInferno>(indice);
            if (f == null || !f.IsValid) { _fuegos.Remove(indice); continue; }
            // Un fuego que no se pudo leer no tira el snapshot entero.
            try
            {
                var duenio = f.OwnerEntity.Value;
                int throwerSlot = duenio != null && pawnIndexToSlot.TryGetValue(duenio.Index, out var s) ? s : -1;
                var pos = f.AbsOrigin;
                nades.Add(new SnapNade
                {
                    Id = (int)f.Index,
                    Type = Fuego,
                    Thrower = throwerSlot,
                    Pos = pos != null ? new[] { pos.X, pos.Y, pos.Z } : null,
                    Life = VidaDe(f.Index),
                    Eff = TicksASegundos(f.FireEffectTickBegin),
                    Flames = LlamasDe(f),
                });
            }
            catch (Exception ex)
            {
                if (!_avisoFuego) { _avisoFuego = true; Log($"no se pudo leer un fuego: {ex.Message}"); }
            }
        }
        foreach (var vieja in _granadaDesde.Keys.Where(k => !vistas.Contains(k)).ToList()) _granadaDesde.Remove(vieja);

        SnapBomb? bomb = null;
        var planted = Utilities.FindAllEntitiesByDesignerName<CPlantedC4>("planted_c4").FirstOrDefault(b => b != null && b.IsValid);
        if (planted != null)
        {
            var pos = planted.AbsOrigin;
            string state = planted.BombDefused ? "defused" : !planted.BombTicking ? "exploded" : planted.BeingDefused ? "defusing" : "planted";
            bomb = new SnapBomb
            {
                State = state,
                Site = planted.BombSite,
                Pos = pos != null ? new[] { pos.X, pos.Y, pos.Z } : null,
                TimeLeft = planted.BombTicking ? planted.C4Blow - now : (float?)null,
                DefuseLeft = planted.BeingDefused ? planted.DefuseCountDown - now : (float?)null,
            };
        }
        else if (carrierSlot >= 0)
        {
            bomb = new SnapBomb { State = "carried", Carrier = carrierSlot };
        }
        else
        {
            var dropped = Utilities.FindAllEntitiesByDesignerName<CC4>("weapon_c4").FirstOrDefault(b => b != null && b.IsValid && !b.OwnerEntity.IsValid);
            if (dropped != null)
            {
                var pos = dropped.AbsOrigin;
                bomb = new SnapBomb { State = "dropped", Pos = pos != null ? new[] { pos.X, pos.Y, pos.Z } : null };
            }
        }

        string phase = "live";
        float? roundTimeLeft = null;
        if (rules != null)
        {
            if (rules.WarmupPeriod) phase = "warmup";
            else if (rules.MatchWaitingForResume || rules.CTTimeOutActive || rules.TerroristTimeOutActive || rules.TechnicalTimeOut) phase = "paused";
            else if (rules.FreezePeriod) phase = "freezetime";
            else if (rules.RoundWinStatus != 0) phase = "over";
            else if (rules.BombPlanted) phase = "bomb";
            roundTimeLeft = rules.RoundStartTime + rules.RoundTime - now;
        }

        // Nombre y rondas ganadas de cada lado (1.8.0): el marcador sale del
        // servidor, con o sin un partido de MatchZy cargado.
        var teams = new List<SnapTeam>(2);
        foreach (var t in Utilities.FindAllEntitiesByDesignerName<CCSTeam>("cs_team_manager"))
        {
            if (t == null || !t.IsValid || (t.TeamNum != 2 && t.TeamNum != 3)) continue;
            // Los timeouts que le quedan a cada equipo (1.13.0), de las reglas del juego.
            int? timeouts = rules == null ? null : t.TeamNum == 3 ? rules.CTTimeOuts : rules.TerroristTimeOuts;
            // Las derrotas seguidas (1.15.0), las mismas que el GSI manda como
            // consecutive_round_losses: con ellas sale el bonus por derrota.
            int? derrotas = rules == null ? null : t.TeamNum == 3 ? rules.NumConsecutiveCTLoses : rules.NumConsecutiveTerroristLoses;
            teams.Add(new SnapTeam { Team = t.TeamNum, Name = t.ClanTeamname, Score = t.Score, Timeouts = timeouts, Losses = derrotas });
        }

        // Cómo terminó cada ronda del mapa (1.15.0): el motivo de
        // EventRoundEnd.Reason (1 = explotó la bomba, 7 = desactivada, 8/9 =
        // eliminación, 12 = tiempo). Así el historial del HUD vuelve entero
        // aunque el backend se haya reiniciado a mitad del mapa. Si quedaron
        // más que las rondas jugadas (más una: el fin de ronda llega antes de
        // que el juego la cuente), sobran las más viejas.
        List<int>? resultados = null;
        if (rules != null)
        {
            var sobran = _resultados.Count - (Math.Max(0, rules.TotalRoundsPlayed) + 1);
            if (sobran > 0) _resultados.RemoveRange(0, sobran);
            resultados = new List<int>(_resultados);
        }

        return new SnapMsg
        {
            Tick = tick,
            Teams = teams,
            Time = now,
            Phase = phase,
            Round = rules?.TotalRoundsPlayed,
            RoundTimeLeft = roundTimeLeft,
            Bomb = bomb,
            Players = players,
            Nades = nades,
            Results = resultados,
            Cam = _cameramanSlot >= 0 ? new SnapCam { Slot = _cameramanSlot, Target = CurrentObserverTargetSlot(pawnIndexToSlot), Lock = _lockTargetSlot >= 0 ? _lockTargetSlot : null } : null,
        };
    }

    private bool _avisoFuego;
    private static int _offLlamas = -1, _offPrendidas = -1;

    /**
     * Las llamas prendidas de un fuego (1.16.0). m_firePositions es un
     * arreglo de vectores que la API no deja leer como Span (probado en vivo:
     * tiraba el snapshot entero): se lee directo de la memoria de la entidad,
     * 12 bytes por llama (x, y, z).
     */
    private static List<float[]> LlamasDe(CInferno f)
    {
        if (_offLlamas < 0) _offLlamas = Schema.GetSchemaOffset("CInferno", "m_firePositions");
        if (_offPrendidas < 0) _offPrendidas = Schema.GetSchemaOffset("CInferno", "m_bFireIsBurning");
        var cuantas = Math.Clamp(f.FireCount, 0, 64);
        var llamas = new List<float[]>(cuantas);
        for (var i = 0; i < cuantas; i++)
        {
            if (Marshal.ReadByte(f.Handle + _offPrendidas + i) == 0) continue;
            var p = f.Handle + _offLlamas + i * 12;
            llamas.Add(new[]
            {
                BitConverter.Int32BitsToSingle(Marshal.ReadInt32(p)),
                BitConverter.Int32BitsToSingle(Marshal.ReadInt32(p + 4)),
                BitConverter.Int32BitsToSingle(Marshal.ReadInt32(p + 8)),
            });
        }
        return llamas;
    }

    private int? CurrentObserverTargetSlot(Dictionary<uint, int> pawnIndexToSlot)
    {
        var cam = Utilities.GetPlayerFromSlot(_cameramanSlot);
        var services = cam?.ObserverPawn.Value?.ObserverServices;
        var target = services?.ObserverTarget.Value;
        if (target == null) return null;
        return pawnIndexToSlot.TryGetValue(target.Index, out var s) ? s : null;
    }

    // ---------------------------------------------------------------
    // Eventos
    // ---------------------------------------------------------------

    // Los mismos que reconoce backend/src/vetoServidor/comandos.ts
    // (PREFIJOS_VETO). Ninguno empieza con un prefijo de chat de MatchZy
    // (.ban, .pick, .in, .ts, .map...): si no, los dos plugins
    // reaccionarían al mismo mensaje.
    private static readonly string[] ComandosDelVeto = { ".vetar", ".elegir", ".lado", ".veto", ".vban", ".vpick", ".vside" };

    private void ChatDelVeto(EventPlayerChat e)
    {
        try
        {
            var texto = (e.Text ?? "").Trim();
            if (!ComandosDelVeto.Any(c => texto.StartsWith(c, StringComparison.OrdinalIgnoreCase))) return;
            var p = Utilities.GetPlayerFromUserid(e.Userid);
            if (p == null || !p.IsValid || p.IsBot || p.IsHLTV) return;
            ReenviarComandoDelVeto(p, texto);
        }
        catch (Exception ex)
        {
            Log($"error con un mensaje del chat: {ex.Message}");
        }
    }

    // Lo que escribió (o eligió en el menú del veto) un jugador, al
    // backend. El SteamID64 es la identidad del plantel en CS2: con esto
    // el backend sabe si es el capitán.
    private void ReenviarComandoDelVeto(CCSPlayerController p, string texto)
    {
        var d = new Dictionary<string, object?>(5)
        {
            ["t"] = "chat", ["sid"] = p.SteamID.ToString(), ["name"] = p.PlayerName, ["team"] = p.TeamNum, ["text"] = texto,
        };
        _outbox.Writer.TryWrite(d);
    }

    private HookResult Emit<T>(T e, string name, Action<Dictionary<string, object?>>? fill) where T : GameEvent
    {
        try
        {
            // Los campos del evento se copian ACÁ: el objeto GameEvent se
            // invalida al salir del handler, no se puede serializar después.
            var d = new Dictionary<string, object?>(8) { ["t"] = "ev", ["tick"] = Server.TickCount, ["name"] = name };
            fill?.Invoke(d);
            if (_outbox.Writer.TryWrite(d)) _eventsSent++;
        }
        catch (Exception ex)
        {
            Log($"error en evento {name}: {ex.Message}");
        }
        return HookResult.Continue;
    }

    private static int? SlotOf(CCSPlayerController? c) => c != null && c.IsValid ? c.Slot : null;

    /** Lo del análisis del duelo en un weapon_fire: los ojos, la mira y la velocidad del que dispara. */
    private static void MiraDelDisparo(CCSPlayerController? c, Dictionary<string, object?> d)
    {
        var pawn = c?.PlayerPawn.Value;
        if (pawn == null || !pawn.IsValid) return;
        var pos = pawn.AbsOrigin;
        var vel = pawn.AbsVelocity;
        var ang = pawn.EyeAngles;
        if (pos != null) d["eye"] = new[] { pos.X, pos.Y, pos.Z + pawn.ViewOffset.Z };
        if (ang != null) d["ang"] = new[] { ang.X, ang.Y };
        if (vel != null) d["vel"] = MathF.Sqrt(vel.X * vel.X + vel.Y * vel.Y);
    }

    // ---------------------------------------------------------------
    // Cámara forzada (experimental, ver plan paso 1)
    // ---------------------------------------------------------------

    private void ResolvePendingCameraman()
    {
        var c = Utilities.GetPlayers().FirstOrDefault(p => p != null && p.IsValid && p.SteamID == _pendingCameramanSteamId);
        if (c == null) return;
        _cameramanSlot = c.Slot;
        _pendingCameramanSteamId = 0;
        Log($"camarografo (de config): {c.PlayerName} slot {c.Slot}");
    }

    private static CCSPlayerController? FindController(string arg)
    {
        if (ulong.TryParse(arg, out var n))
        {
            if (n >= 76561197960265728UL) // SteamID64
                return Utilities.GetPlayers().FirstOrDefault(p => p != null && p.IsValid && p.SteamID == n);
            var bySlot = Utilities.GetPlayerFromSlot((int)n);
            return bySlot != null && bySlot.IsValid ? bySlot : null;
        }
        return Utilities.GetPlayers().FirstOrDefault(p => p != null && p.IsValid && string.Equals(p.PlayerName, arg, StringComparison.OrdinalIgnoreCase));
    }

    // El backend reconoce "esta jugando" en la respuesta (director/actuator.ts).
    private static string Jugando(CCSPlayerController c) =>
        $"camarografo {c.PlayerName} esta jugando ({(c.TeamNum == 2 ? "T" : "CT")}): tiene que estar en espectadores";

    /// <summary>
    /// SteamID64 del camarógrafo autorizado: el fijado con css_director_cameraman
    /// o, si todavía no se resolvió, el de la config. 0 si no hay ninguno.
    /// </summary>
    public ulong CamarografoSteamId()
    {
        if (_cameramanSlot >= 0)
        {
            var c = Utilities.GetPlayerFromSlot(_cameramanSlot);
            if (c != null && c.IsValid) return c.SteamID;
        }
        if (_pendingCameramanSteamId != 0) return _pendingCameramanSteamId;
        return ulong.TryParse(_config.DirectorCameramanSteamId, out var sid) ? sid : 0;
    }

    private string ApplySpectate(int cameramanSlot, int targetSlot, bool quiet)
    {
        string Fail(string why) { if (!quiet) { _lastSpecResult = why; Log(why); } return why; }
        var cam = Utilities.GetPlayerFromSlot(cameramanSlot);
        if (cam == null || !cam.IsValid) return Fail($"camarografo slot {cameramanSlot} no valido");
        // En CS2 todo controller tiene observer pawn, aun vivo y jugando: el
        // chequeo de abajo no frena nada. Si el camarografo esta en T o CT,
        // forzarle la camara le pone la vista de espectador en medio de la
        // ronda; su pawn sigue vivo, asi que la ronda no termina por
        // eliminacion (paso el 20/10/2026 con un 1v1 de prueba en el que el
        // camarografo configurado era uno de los jugadores).
        if (cam.TeamNum == 2 || cam.TeamNum == 3) return Fail(Jugando(cam));
        var obsPawn = cam.ObserverPawn.Value;
        if (obsPawn == null || !obsPawn.IsValid) return Fail($"{cam.PlayerName} no tiene observer pawn (esta vivo o no es espectador?)");
        var services = obsPawn.ObserverServices;
        if (services == null) return Fail("observer pawn sin ObserverServices");
        var target = Utilities.GetPlayerFromSlot(targetSlot);
        var targetPawn = target?.PlayerPawn.Value;
        if (target == null || !target.IsValid || targetPawn == null || !targetPawn.IsValid) return Fail($"objetivo slot {targetSlot} no valido");
        if (!target.PawnIsAlive) return Fail($"objetivo {target.PlayerName} esta muerto");

        var raw = targetPawn.EntityHandle.Raw;
        if (services.ObserverTarget.Raw == raw && services.ObserverMode == (byte)ObserverMode_t.OBS_MODE_IN_EYE)
        {
            return "ok (ya estaba)";
        }
        try
        {
            // Nunca escribir en un offset que no se pudo resolver (0 = vtable).
            int offTarget = Schema.GetSchemaOffset("CPlayer_ObserverServices", "m_hObserverTarget");
            int offMode = Schema.GetSchemaOffset("CPlayer_ObserverServices", "m_iObserverMode");
            if (offTarget <= 0) return Fail("offset de m_hObserverTarget no resuelto");
            services.ObserverTarget.Raw = raw;
            if (_specMode >= 2)
            {
                if (offMode <= 0) return Fail("offset de m_iObserverMode no resuelto");
                Schema.SetSchemaValue(services.Handle, "CPlayer_ObserverServices", "m_iObserverMode", (byte)ObserverMode_t.OBS_MODE_IN_EYE);
            }
            if (_specMode == 3 || _specMode == 5)
            {
                int offDesiredT = Schema.GetSchemaOffset("CCSPlayerController", "m_hDesiredObserverTarget");
                int offDesiredM = Schema.GetSchemaOffset("CCSPlayerController", "m_DesiredObserverMode");
                if (offDesiredT <= 0 || offDesiredM <= 0) return Fail($"offsets deseados no resueltos (target={offDesiredT}, mode={offDesiredM})");
                Schema.SetSchemaValue(cam.Handle, "CCSPlayerController", "m_hDesiredObserverTarget", raw);
                Schema.SetSchemaValue(cam.Handle, "CCSPlayerController", "m_DesiredObserverMode", (int)ObserverMode_t.OBS_MODE_IN_EYE);
                Utilities.SetStateChanged(cam, "CCSPlayerController", "m_hDesiredObserverTarget");
                Utilities.SetStateChanged(cam, "CCSPlayerController", "m_DesiredObserverMode");
            }
            if (_specMode == 5)
            {
                Utilities.SetStateChanged(obsPawn, "CBasePlayerPawn", "m_pObserverServices");
            }
            if (_specMode == 4)
            {
                Utilities.SetStateChanged(obsPawn, "CBasePlayerPawn", "m_pObserverServices");
            }
        }
        catch (Exception ex)
        {
            return Fail($"error escribiendo el target: {ex.Message}");
        }
        var ok = $"ok: {cam.PlayerName} -> {target.PlayerName} (slot {targetSlot})";
        if (!quiet) { _lastSpecResult = ok; }
        return ok;
    }

    private void OnStatus(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!ComandosRegie.Puede(caller, cmd)) return;
        var cam = _cameramanSlot >= 0 ? Utilities.GetPlayerFromSlot(_cameramanSlot) : null;
        cmd.ReplyToCommand($"director feed: {(_config.DirectorEnabled ? "on" : "off")} {_config.DirectorBind}:{_config.DirectorPort} {64 / _ticksPerSnapshot}Hz clientes={_server.ClientCount} snaps={_snapshotsSent} eventos={_eventsSent} tick={Server.TickCount} mapa={_mapName}");
        cmd.ReplyToCommand($"camarografo: {(cam != null && cam.IsValid ? $"{cam.PlayerName} (slot {cam.Slot})" : "(ninguno)")} lock={(_lockEnabled ? "on" : "off")} lockTarget={(_lockTargetSlot >= 0 ? _lockTargetSlot.ToString() : "-")}");
        cmd.ReplyToCommand($"ultimo spec: {_lastSpecResult}");
    }

    // Teclas dentro del juego (1.14.0): el camarógrafo autorizado (o la
    // consola del servidor) dispara una acción del HUD de la transmisión
    // con un bind de su CS2. Va al backend por el feed, que la aplica como
    // un botón de Control de transmisión (servidoresCs2/feeds.ts).
    private void OnHud(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!ComandosRegie.Puede(caller, cmd)) return;
        if (cmd.ArgCount < 2) { cmd.ReplyToCommand("uso: css_regie_hud <acción> [show|hide]"); return; }
        var accion = cmd.GetArg(1).Trim();
        if (accion.Length is < 2 or > 30 || !accion.All(char.IsLetter)) { cmd.ReplyToCommand("acción inválida"); return; }
        var data = cmd.ArgCount >= 3 ? cmd.GetArg(2).Trim().ToLowerInvariant() : null;
        if (data != null && data != "show" && data != "hide" && data != "none") data = null;
        var d = new Dictionary<string, object?>(4) { ["t"] = "hud", ["accion"] = accion, ["data"] = data };
        if (caller != null && caller.IsValid) d["sid"] = caller.SteamID.ToString();
        _outbox.Writer.TryWrite(d);
        cmd.ReplyToCommand($"hud: {accion}{(data != null ? " " + data : "")}");
    }

    private void OnCameraman(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!ComandosRegie.Puede(caller, cmd, soloServidor: true)) return;
        if (cmd.ArgCount < 2) { cmd.ReplyToCommand("uso: css_director_cameraman <steamid64|slot|nombre|none>"); return; }
        var arg = cmd.GetArg(1);
        if (arg.Equals("none", StringComparison.OrdinalIgnoreCase)) { _cameramanSlot = -1; _lockTargetSlot = -1; cmd.ReplyToCommand("camarografo: ninguno"); return; }
        var c = FindController(arg);
        if (c == null) { cmd.ReplyToCommand($"no encontre un jugador para '{arg}'"); return; }
        _cameramanSlot = c.Slot;
        _lockTargetSlot = -1;
        // Se acepta igual (puede pasar a espectadores despues), pero se avisa:
        // mientras juegue, la camara no se mueve.
        var aviso = c.TeamNum == 2 || c.TeamNum == 3 ? $". Ojo: {Jugando(c)}" : "";
        cmd.ReplyToCommand($"camarografo: {c.PlayerName} (slot {c.Slot}, steamid {c.SteamID}){aviso}");
    }

    private void OnSpec(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!ComandosRegie.Puede(caller, cmd)) return;
        if (cmd.ArgCount < 2) { cmd.ReplyToCommand("uso: css_director_spec <slot|steamid64|nombre|none>"); return; }
        if (_cameramanSlot < 0) { cmd.ReplyToCommand("primero fija el camarografo con css_director_cameraman"); return; }
        var arg = cmd.GetArg(1);
        if (arg.Equals("none", StringComparison.OrdinalIgnoreCase)) { _lockTargetSlot = -1; cmd.ReplyToCommand("camara liberada"); return; }
        var target = FindController(arg);
        if (target == null) { cmd.ReplyToCommand($"no encontre un jugador para '{arg}'"); return; }
        var result = ApplySpectate(_cameramanSlot, target.Slot, quiet: false);
        if (result.StartsWith("ok")) _lockTargetSlot = target.Slot;
        cmd.ReplyToCommand(result);
    }

    private void OnSpecMode(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!ComandosRegie.Puede(caller, cmd)) return;
        if (cmd.ArgCount >= 2 && int.TryParse(cmd.GetArg(1), out var m) && m >= 1 && m <= 5) _specMode = m;
        int offTarget = -1, offMode = -1, offDesiredT = -1, offDesiredM = -1;
        try { offTarget = Schema.GetSchemaOffset("CPlayer_ObserverServices", "m_hObserverTarget"); } catch { }
        try { offMode = Schema.GetSchemaOffset("CPlayer_ObserverServices", "m_iObserverMode"); } catch { }
        try { offDesiredT = Schema.GetSchemaOffset("CCSPlayerController", "m_hDesiredObserverTarget"); } catch { }
        try { offDesiredM = Schema.GetSchemaOffset("CCSPlayerController", "m_DesiredObserverMode"); } catch { }
        cmd.ReplyToCommand($"specmode={_specMode} offsets: pawn.m_hObserverTarget={offTarget} pawn.m_iObserverMode={offMode} controller.m_hDesiredObserverTarget={offDesiredT} controller.m_DesiredObserverMode={offDesiredM}");
    }

    private void OnLock(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!ComandosRegie.Puede(caller, cmd)) return;
        if (cmd.ArgCount >= 2) _lockEnabled = cmd.GetArg(1) != "0";
        cmd.ReplyToCommand($"lock por tick: {(_lockEnabled ? "on" : "off")}");
    }

    // ---------------------------------------------------------------
    // Serialización y salida (fuera del hilo del juego)
    // ---------------------------------------------------------------

    private byte[] HelloLine()
    {
        var hello = new Dictionary<string, object?>
        {
            ["t"] = "hello", ["v"] = 1, ["map"] = _mapName, ["tickrate"] = 64, ["hz"] = 64 / _ticksPerSnapshot,
            ["plugin"] = _plugin.ModuleVersion,
            // De qué servidor es este feed (1.8.0): con dos instancias el
            // backend no tiene que adivinarlo por el puerto.
            ["hostport"] = _hostPort, ["feedport"] = _feedPort,
        };
        return JsonSerializer.SerializeToUtf8Bytes(hello, JsonOpts).Concat(new byte[] { (byte)'\n' }).ToArray();
    }

    private async Task SerializerLoop()
    {
        try
        {
            await foreach (var msg in _outbox.Reader.ReadAllAsync(_cts.Token))
            {
                if (_server.ClientCount == 0) continue;
                var bytes = JsonSerializer.SerializeToUtf8Bytes(msg, msg.GetType(), JsonOpts);
                var line = new byte[bytes.Length + 1];
                Buffer.BlockCopy(bytes, 0, line, 0, bytes.Length);
                line[^1] = (byte)'\n';
                _server.Broadcast(line);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex) { Log($"serializer termino: {ex.Message}"); }
    }

    private void Log(string msg) => Console.WriteLine($"[cs2-live-viewer director] {msg}");

    public void Dispose()
    {
        _veto?.Dispose();
        _veto = null;
        _cts.Cancel();
        _outbox.Writer.TryComplete();
        _server.Dispose();
    }

    // ---------------------------------------------------------------
    // Mensajes (nombres cortos en el JSON para no inflar 16 líneas/s)
    // ---------------------------------------------------------------

    public sealed class SnapMsg
    {
        [JsonPropertyName("t")] public string T { get; } = "snap";
        [JsonPropertyName("tick")] public int Tick { get; set; }
        [JsonPropertyName("time")] public float Time { get; set; }
        [JsonPropertyName("phase")] public string Phase { get; set; } = "live";
        [JsonPropertyName("round")] public int? Round { get; set; }
        [JsonPropertyName("roundTimeLeft")] public float? RoundTimeLeft { get; set; }
        [JsonPropertyName("bomb")] public SnapBomb? Bomb { get; set; }
        [JsonPropertyName("players")] public List<SnapPlayer> Players { get; set; } = new();
        [JsonPropertyName("teams")] public List<SnapTeam> Teams { get; set; } = new();
        [JsonPropertyName("nades")] public List<SnapNade> Nades { get; set; } = new();
        /** Motivo del fin de cada ronda jugada del mapa, en orden (1.15.0). */
        [JsonPropertyName("results")] public List<int>? Results { get; set; }
        [JsonPropertyName("cam")] public SnapCam? Cam { get; set; }
    }

    public sealed class SnapTeam
    {
        /** 2 = T, 3 = CT. */
        [JsonPropertyName("team")] public int Team { get; set; }
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("score")] public int Score { get; set; }
        [JsonPropertyName("timeouts")] public int? Timeouts { get; set; }
        [JsonPropertyName("losses")] public int? Losses { get; set; }
    }

    public sealed class SnapPlayer
    {
        [JsonPropertyName("slot")] public int Slot { get; set; }
        [JsonPropertyName("sid")] public string Sid { get; set; } = "";
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("team")] public int Team { get; set; }
        [JsonPropertyName("alive")] public bool Alive { get; set; }
        [JsonPropertyName("bot")] public bool Bot { get; set; }
        [JsonPropertyName("pos")] public float[]? Pos { get; set; }
        [JsonPropertyName("vel")] public float[]? Vel { get; set; }
        [JsonPropertyName("ang")] public float[]? Ang { get; set; }
        [JsonPropertyName("hp")] public int Hp { get; set; }
        [JsonPropertyName("armor")] public int Armor { get; set; }
        [JsonPropertyName("flash")] public float Flash { get; set; }
        [JsonPropertyName("scoped")] public bool Scoped { get; set; }
        [JsonPropertyName("walking")] public bool Walking { get; set; }
        [JsonPropertyName("ducking")] public bool Ducking { get; set; }
        [JsonPropertyName("defusing")] public bool Defusing { get; set; }
        [JsonPropertyName("inBombZone")] public bool InBombZone { get; set; }
        [JsonPropertyName("hasBomb")] public bool HasBomb { get; set; }
        [JsonPropertyName("weapon")] public string? Weapon { get; set; }
        [JsonPropertyName("place")] public string? Place { get; set; }
        [JsonPropertyName("money")] public int Money { get; set; }
        [JsonPropertyName("k")] public int Kills { get; set; }
        [JsonPropertyName("d")] public int Deaths { get; set; }
        [JsonPropertyName("a")] public int Assists { get; set; }
        [JsonPropertyName("dmg")] public int Damage { get; set; }
        [JsonPropertyName("mvps")] public int Mvps { get; set; }
        [JsonPropertyName("score")] public int Score { get; set; }
        [JsonPropertyName("helmet")] public bool Helmet { get; set; }
        [JsonPropertyName("kit")] public bool DefuseKit { get; set; }
        [JsonPropertyName("weapons")] public List<string>? Weapons { get; set; }
        [JsonPropertyName("clip")] public int? Clip { get; set; }
        [JsonPropertyName("clipMax")] public int? ClipMax { get; set; }
        [JsonPropertyName("reserve")] public int? Reserve { get; set; }
        /** Solo las granadas que lleva repetidas: nombre -> cantidad. */
        [JsonPropertyName("nades")] public Dictionary<string, int>? Nades { get; set; }
        /** Segundos desde el último daño del fuego de una molotov, si fue hace menos de 2 (1.16.0). */
        [JsonPropertyName("burn")] public float? Burn { get; set; }
    }

    public sealed class SnapNade
    {
        [JsonPropertyName("id")] public int Id { get; set; }
        [JsonPropertyName("type")] public string Type { get; set; } = "";
        [JsonPropertyName("thrower")] public int Thrower { get; set; }
        [JsonPropertyName("pos")] public float[]? Pos { get; set; }
        [JsonPropertyName("vel")] public float[]? Vel { get; set; }
        [JsonPropertyName("det")] public float? Det { get; set; }
        /** Segundos desde que se la ve (1.16.0). */
        [JsonPropertyName("life")] public float? Life { get; set; }
        /** Segundos desde que se desplegó el humo o se prendió el fuego (1.16.0). */
        [JsonPropertyName("eff")] public float? Eff { get; set; }
        /** Las llamas prendidas de un fuego (1.16.0). */
        [JsonPropertyName("flames")] public List<float[]>? Flames { get; set; }
    }

    public sealed class SnapBomb
    {
        [JsonPropertyName("state")] public string State { get; set; } = "";
        [JsonPropertyName("carrier")] public int? Carrier { get; set; }
        [JsonPropertyName("site")] public int? Site { get; set; }
        [JsonPropertyName("pos")] public float[]? Pos { get; set; }
        [JsonPropertyName("timeLeft")] public float? TimeLeft { get; set; }
        [JsonPropertyName("defuseLeft")] public float? DefuseLeft { get; set; }
    }

    public sealed class SnapCam
    {
        [JsonPropertyName("slot")] public int Slot { get; set; }
        [JsonPropertyName("target")] public int? Target { get; set; }
        [JsonPropertyName("lock")] public int? Lock { get; set; }
    }
}

// Servidor TCP mínimo: acepta clientes, les manda la línea "hello" y
// después todo lo que se emita. Cada cliente tiene su cola acotada: si
// no lee a tiempo se descartan las líneas más viejas, nunca se bloquea
// al que escribe (el hilo del serializador) ni, por transitiva, al
// hilo del juego.
internal sealed class NdjsonServer : IDisposable
{
    private readonly TcpListener _listener;
    private readonly Func<byte[]> _hello;
    private readonly Action<string> _log;
    private readonly CancellationTokenSource _cts = new();
    private readonly List<Client> _clients = new();
    private readonly object _lock = new();

    private sealed class Client
    {
        public required TcpClient Tcp;
        public required Channel<byte[]> Queue;
    }

    public NdjsonServer(string bind, int port, Func<byte[]> hello, Action<string> log)
    {
        var ip = IPAddress.TryParse(bind, out var parsed) ? parsed : IPAddress.Loopback;
        _listener = new TcpListener(ip, port);
        _hello = hello;
        _log = log;
    }

    public int ClientCount { get { lock (_lock) return _clients.Count; } }

    public void Start()
    {
        _listener.Start();
        _ = Task.Run(AcceptLoop);
    }

    private async Task AcceptLoop()
    {
        while (!_cts.IsCancellationRequested)
        {
            TcpClient tcp;
            try { tcp = await _listener.AcceptTcpClientAsync(_cts.Token); }
            catch (OperationCanceledException) { break; }
            catch (Exception ex) { _log($"accept: {ex.Message}"); await Task.Delay(500); continue; }
            tcp.NoDelay = true;
            var client = new Client
            {
                Tcp = tcp,
                Queue = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(256) { FullMode = BoundedChannelFullMode.DropOldest, SingleReader = true }),
            };
            lock (_lock) _clients.Add(client);
            _log($"cliente conectado desde {tcp.Client.RemoteEndPoint} (total {ClientCount})");
            client.Queue.Writer.TryWrite(_hello());
            _ = Task.Run(() => WriteLoop(client));
        }
    }

    private async Task WriteLoop(Client client)
    {
        try
        {
            using var stream = client.Tcp.GetStream();
            await foreach (var line in client.Queue.Reader.ReadAllAsync(_cts.Token))
            {
                await stream.WriteAsync(line, _cts.Token);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex) { _log($"cliente desconectado: {ex.Message}"); }
        finally
        {
            lock (_lock) _clients.Remove(client);
            try { client.Tcp.Close(); } catch { }
        }
    }

    public void Broadcast(byte[] line)
    {
        Client[] snapshot;
        lock (_lock) snapshot = _clients.ToArray();
        foreach (var c in snapshot) c.Queue.Writer.TryWrite(line);
    }

    public void Dispose()
    {
        _cts.Cancel();
        try { _listener.Stop(); } catch { }
        Client[] snapshot;
        lock (_lock) { snapshot = _clients.ToArray(); _clients.Clear(); }
        foreach (var c in snapshot) { try { c.Tcp.Close(); } catch { } }
    }
}
