using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Modules.Commands;

namespace Cs2LiveViewerHelper;

// Por qué existe: "status"/"users" por RCON en CS2 no traen el SteamID
// real de los jugadores conectados (confirmado a mano contra un
// servidor real -- el header de la tabla de "status" ni siquiera tiene
// esa columna, a diferencia de CS:GO). CounterStrikeSharp sí tiene
// acceso a esa identidad real vía su propia API del juego
// (CCSPlayerController.SteamID), así que este plugin la expone como un
// comando de consola que el panel puede consultar por RCON como
// cualquier otro.
public class Cs2LiveViewerHelperPlugin : BasePlugin, IPluginConfig<DirectorConfig>
{
    // Config del feed del director (ver DirectorFeed.cs). CounterStrikeSharp
    // la deserializa desde configs/plugins/RegieGG/RegieGG.json
    // y llama a OnConfigParsed antes de Load.
    public DirectorConfig Config { get; set; } = new();
    private DirectorFeed? _director;
    private MatchEvents? _matchEvents;
    // La publicidad no depende del feed: anda aunque el observador
    // automático esté apagado (director_enabled false).
    private PublicidadEnPantalla? _publicidad;
    // Inicio a cargo de la producción y la serie en el chat (PartidoRegie.cs).
    private PartidoRegie? _partido;

    public void OnConfigParsed(DirectorConfig config)
    {
        Config = config;
    }

    public override void Load(bool hotReload)
    {
        // El centro de la pantalla sin pestañeo (veto, publicidad): ver CentroSinPestaneo.cs.
        CentroSinPestaneo.Iniciar(this);
        // El idioma de Regie.gg para lo que dice el plugin (Textos.cs).
        Textos.Registrar(this);
        // Las dos piezas son independientes y opcionales: si una falla al
        // arrancar (puerto ocupado, carpeta sin permisos), la otra y
        // css_liststeamids tienen que seguir andando.
        if (Config.DirectorEnabled)
        {
            try
            {
                _director = new DirectorFeed(this, Config);
                ComandosRegie.Camarografo = () => _director?.CamarografoSteamId() ?? 0;
                _director.Start(hotReload);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[cs2-live-viewer director] no se pudo iniciar el feed: {ex.Message}");
                _director?.Dispose();
                _director = null;
            }
        }
        if (Config.MatchEventsEnabled)
        {
            try
            {
                _matchEvents = new MatchEvents(this, Config);
                _matchEvents.Start();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[cs2-live-viewer eventos] no se pudieron iniciar los eventos de partido: {ex.Message}");
                _matchEvents?.Dispose();
                _matchEvents = null;
            }
        }
        try
        {
            _publicidad = new PublicidadEnPantalla(this, ModuleVersion, msg => Console.WriteLine($"[cs2-live-viewer publicidad] {msg}"));
            _publicidad.Start();
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[cs2-live-viewer publicidad] no se pudo iniciar la publicidad en pantalla: {ex.Message}");
            _publicidad?.Dispose();
            _publicidad = null;
        }
        try
        {
            _partido = new PartidoRegie(this, msg => Console.WriteLine($"[cs2-live-viewer partido] {msg}"));
            _partido.Start();
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[cs2-live-viewer partido] no se pudo iniciar el inicio a cargo de la producción: {ex.Message}");
            _partido?.Dispose();
            _partido = null;
        }
    }

    public override void Unload(bool hotReload)
    {
        _publicidad?.Dispose();
        _publicidad = null;
        _partido?.Dispose();
        _partido = null;
        CentroSinPestaneo.Detener();
        _director?.Dispose();
        _director = null;
        _matchEvents?.Dispose();
        _matchEvents = null;
    }

    public override string ModuleName => "cs2-live-viewer helper";
    // 1.3.0: reenvía por el feed los comandos del veto por chat. El backend
    // la exige para ese veto (PLUGIN_CS2_CON_CHAT en vetoServidor/canales.ts).
    // 1.3.1: el observador automatico no mueve la camara de un camarografo
    // que esta jugando en T o CT (DirectorFeed.ApplySpectate).
    // 1.4.0: el veto por chat en pantalla (VetoEnPantalla.cs): mensajes con
    // tildes y colores por tipo (css_regie_veto_chat), el panel fijo en el
    // centro y el menú del capitán de turno (css_regie_veto_panel), el
    // resumen al que entra a mitad del veto. Los comandos de consola
    // (css_director_*, css_liststeamids, css_lv_events_status) solo los
    // usan el servidor y el camarógrafo autorizado (ComandosRegie.cs).
    // El backend lo usa desde esta
    // versión (PLUGIN_CS2_VETO_VISUAL); con una anterior, todo por `say`.
    // 1.5.0: la publicidad de sponsors en el centro de la pantalla
    // (PublicidadEnPantalla.cs: css_regie_ads_set, css_regie_ads_test y
    // css_regie_ads_off, solo desde la consola del servidor), en lugar de
    // sv_server_graphic1, que CS2 no muestra. Comparte el centro con el
    // panel del veto (que gana) y el arreglo del pestañeo
    // (CentroSinPestaneo). El backend la usa desde esta versión
    // (PLUGIN_CS2_PUBLICIDAD en backend/src/publicidadPlaca.ts); con una
    // anterior, la publicidad del juego queda en la frase del chat.
    // 1.6.0: la frase del sponsor en el chat como los mensajes de MatchZy
    // (css_regie_ads_chat en PublicidadEnPantalla.cs, solo desde la consola
    // del servidor): « [Prefijo] frase», cada parte con su color de una
    // lista cerrada de ChatColors, y con tildes. El backend la usa desde
    // esta versión (PLUGIN_CS2_CHAT en backend/src/publicidadChat.ts); con
    // una anterior, o si el comando falla, sale por `say` como antes.
    // 1.8.0: varias instancias de la misma instalación. El feed escucha en
    // director_port + (puerto de juego - 27015), así cada servidor tiene el
    // suyo (antes el segundo no podía abrirlo y perdía feed, veto y
    // cámara), y el hello dice de qué servidor es. El snapshot trae además
    // los equipos con su puntaje y, de cada jugador, plata, K/D/A, daño,
    // casco, kit y el inventario: el marcador y el HUD salen del servidor,
    // sin un observador con GSI. Los reintentos de eventos van a una
    // carpeta por instancia y el backup de MatchZy se elige por los
    // equipos de este servidor. El backend usa todo esto desde esta
    // versión (PLUGIN_CS2_MULTI en backend/src/servidoresCs2/puertos.ts).
    // 1.8.1: el contexto del partido (mapa, ronda) se olvida al cambiar de
    // mapa y cuando ningún respaldo de MatchZy es de lo que se juega; antes
    // se arrastraba el anterior (el «Mapa 4» fantasma de la Final).
    // 1.9.0: el partido como lo quiere Regie.gg (PartidoRegie.cs): inicio a
    // cargo de la producción (css_regie_hold frena el .ready de los
    // jugadores; arranca con el css_start de MatchZy), el marcador de la serie en el
    // chat con el de MatchZy tapado (css_regie_serie_on/chat), y el pie del
    // recuadro del veto con la cuenta para pasar al primer mapa. El backend
    // lo usa desde esta versión (PLUGIN_CS2_PARTIDO en
    // backend/src/partidoRegie.ts).
    // 1.10.0: lo que el plugin les dice a los jugadores (panel del veto,
    // inicio a cargo de la producción) en el idioma de Regie.gg, que manda el
    // backend con css_regie_lang (Textos.cs, backend/src/idioma.ts).
    // 1.11.0: la placa de publicidad del centro no se le muestra al
    // camarógrafo de la transmisión (PublicidadEnPantalla.cs, "x").
    // 1.12.0: cada evento de partido que manda el plugin lleva el puerto de
    // juego de su servidor (server_port, MatchEvents.cs). Con varios
    // servidores de CS2 es lo que dice dónde se juega un pug (.ready sin
    // config), que MatchZy no muestra en get5_status (backend/src/
    // routes/webhook.ts, servidorDelPuerto).
    public override string ModuleVersion => "1.17.1";
    public override string ModuleAuthor => "cs2-live-viewer";
    public override string ModuleDescription =>
        "Expone por RCON el SteamID64 real de los jugadores conectados, un feed del estado del juego a 16 Hz para el observador automático, comandos para mover la cámara del camarógrafo, y los eventos del partido (con stats exactas) al backend.";

    [ConsoleCommand("css_liststeamids", "Lista los jugadores humanos conectados con su SteamID64 real, uno por línea (steamid64|nombre).")]
    public void OnListSteamIds(CCSPlayerController? caller, CommandInfo command)
    {
        if (!ComandosRegie.Puede(caller, command)) return;
        var players = Utilities.GetPlayers();
        var humans = players.Where(p => p != null && p.IsValid && !p.IsBot && !p.IsHLTV).ToList();

        if (humans.Count == 0)
        {
            command.ReplyToCommand("(nadie conectado)");
            return;
        }

        // Formato deliberadamente simple (un separador que no puede
        // aparecer en un SteamID64 ni en el nombre de forma ambigua
        // para el parser) en vez de JSON -- no hay forma fácil de
        // asegurar que una respuesta de RCON de varios KB llegue en un
        // solo paquete sin el mismo truco de "echo <token>" que ya usa
        // el resto del panel, y una línea por jugador es más fácil de
        // seguir leyendo aunque se corte.
        foreach (var player in humans)
        {
            command.ReplyToCommand($"{player.SteamID}|{player.PlayerName}");
        }
    }
}
