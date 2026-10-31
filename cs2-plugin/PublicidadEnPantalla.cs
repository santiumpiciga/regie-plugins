using System.Globalization;
using System.Net;
using System.Text.Json;
using System.Text.RegularExpressions;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Menu;
using CounterStrikeSharp.API.Modules.Utils;

namespace Cs2LiveViewerHelper;

// La publicidad de sponsors dentro del juego (1.5.0): una placa fija en el
// centro de la pantalla, con la imagen del sponsor y una o dos líneas de
// texto. Reemplaza a sv_server_graphic1, la placa nativa del espectador,
// que CS2 no muestra (probado el 20/10/2026: ni con
// cl_server_graphic1_enable 1, ni como espectador, ni con la imagen en el
// cliente). El backend (backend/src/publicidad.ts) decide qué sponsor va
// y lo manda por RCON; acá solo se muestra:
//
//   css_regie_ads_set <id> <parte> <total> <base64url>
//     Qué se muestra, en JSON con claves cortas (ver comandosPlacaCs2 en
//     backend/src/publicidadPlaca.ts):
//       {"a":1,"i":"http://…/uploads/hud/x.png","w":360,"h":60,
//        "l":[["Sponsor oficial","#ffd34d","m"],["sponsor.com","#cccccc","s"]]}
//     a = 1: se muestra según las reglas de abajo; 0: queda cargada solo
//     para la prueba. x (1.11.0, opcional): el SteamID64 del camarógrafo de
//     la transmisión, que no la ve (fuera de la prueba): la opción «Mostrar
//     la placa también en la transmisión» apagada en Producción. i = la imagen (vacía = solo texto), que la baja el
//     juego de cada jugador por HTTP: tiene que ser una dirección que él
//     alcance. w y h su tamaño. l = las líneas: texto, color (#rrggbb, nada
//     más) y tamaño de letra (fontSize-<x> del juego). Partido en pedazos,
//     como el panel del veto, si no entra en una línea de consola.
//   css_regie_ads_test <segundos>
//     La muestra a todos los conectados ese tiempo (hasta 60 s), sin
//     mirar las reglas de cuándo: para probarla desde Producción.
//   css_regie_ads_off
//     La saca (y corta una prueba en curso).
//   css_regie_ads_chat <colorPrefijo> <colorTexto> <base64url> (1.6.0)
//     La frase del sponsor en el chat de todos, como los mensajes de
//     MatchZy: « [Prefijo] frase», el prefijo de un color y la frase de
//     otro. Los colores van por nombre, de una lista cerrada (ColoresChat,
//     la misma que COLORES_CHAT en backend/src/publicidadChat.ts); el
//     texto, en JSON {"p":"prefijo","t":"frase"} y base64url, como el
//     chat del veto: así llegan las tildes y nada puede cortar el comando.
//     El backend lo usa desde esta versión; con una anterior, `say`.
//
// Los cuatro contestan «regie-ads ok <versión>» (la prueba, además,
// «jugadores=<n>»): así el backend sabe que este plugin la sabe mostrar.
//
// Cuándo se ve (fuera de la prueba):
//   - a los muertos y a los espectadores, siempre;
//   - a todos en el calentamiento, el freezetime, las pausas y los
//     tiempos muertos, el entretiempo y el final del mapa;
//   - NUNCA a un jugador vivo en plena ronda.
// Y el centro de la pantalla se comparte:
//   - el panel del veto gana: mientras haya veto no se manda publicidad;
//   - al que tiene un menú del centro abierto (el del capitán, o el de
//     otro plugin) no se le manda nada: se lo taparía.
//
// Se reenvía en cada tick (el recuadro dura ~1 s aunque se le pida más) y
// pide CentroSinPestaneo solo mientras se le está mostrando a alguien: si
// en plena ronda están todos vivos, las reglas del juego no se tocan.
//
// El HTML se arma una vez por placa nueva, no en cada tick. Si el backend
// deja de refrescarla (se cayó, se apagó la PC), se va sola a los VenceMs:
// una publicidad vieja no queda colgada para siempre.
//
// Solo desde la consola del servidor (RCON, caller == null): un jugador
// que los escriba en su consola no puede poner su propia publicidad.
public sealed class PublicidadEnPantalla : IDisposable
{
    public const string Ok = "regie-ads ok";
    private const string Clave = "publicidad";
    private const int MaxPartes = 4;
    private const long VenceMs = 10 * 60 * 1000;
    private const float PruebaMaxSeg = 60f;
    private const int MaxLineas = 3;
    private const int MaxTexto = 64;
    private const int MaxUrl = 300;
    // El backend ya los corta (32 y 120): esto es por si llega otra cosa.
    private const int MaxPrefijoChat = 32;
    private const int MaxTextoChat = 160;
    // Igual que el panel del veto: lo que dura cada envío no importa (el
    // juego lo ignora), se reenvía en cada tick.
    private const int DuracionSeg = 1;

    private static readonly Regex ColorHex = new("^#[0-9a-fA-F]{6}$", RegexOptions.CultureInvariant);
    // Las clases de tamaño del HTML del centro de CS2: cualquier otra cosa
    // no llega al HTML (no se arma una clase con lo que viene de afuera).
    private static readonly HashSet<string> Tamanios = new(StringComparer.Ordinal) { "xs", "s", "sm", "m", "l", "xl" };

    // Los colores del chat que se pueden elegir para la frase del sponsor,
    // por nombre: lo que llega por RCON se busca acá, nunca se usa como
    // código. Fuera quedaron los alias de ChatColors (White, LightYellow,
    // Blue, Magenta, Orange: el mismo código que uno de estos), los que en
    // el chat no se distinguen de otro de la lista (DarkRed y LightRed del
    // rojo de Red, Silver del gris de Grey, Olive del verde de Lime,
    // LightPurple del violeta de Purple; LightPurple además es el código
    // del color de equipo de Source) y DarkBlue, que se lee mal sobre el
    // fondo oscuro del chat. Ver COLORES_CHAT en backend/src/publicidadChat.ts.
    private static readonly Dictionary<string, char> ColoresChat = new(StringComparer.Ordinal)
    {
        ["default"] = ChatColors.Default,
        ["green"] = ChatColors.Green,
        ["lime"] = ChatColors.Lime,
        ["yellow"] = ChatColors.Yellow,
        ["gold"] = ChatColors.Gold,
        ["red"] = ChatColors.Red,
        ["purple"] = ChatColors.Purple,
        ["lightblue"] = ChatColors.LightBlue,
        ["grey"] = ChatColors.Grey,
    };

    private readonly BasePlugin _plugin;
    private readonly string _version;
    private readonly Action<string> _log;

    private string _html = "";
    private bool _activa;
    // El camarógrafo de la transmisión, que no la ve (0: nadie).
    private ulong _excluir;
    // Los tiempos van con el reloj de la PC y no con el del juego: el del
    // juego vuelve a cero con cada mapa.
    private long _vence;
    private long _pruebaHasta;

    private int _idEnArmado = -1;
    private string?[] _partes = Array.Empty<string?>();

    private CCSGameRulesProxy? _proxy;
    private CCSGameRules? _reglas;
    private long _proximaBusqueda;
    private long _ultimoError;

    public PublicidadEnPantalla(BasePlugin plugin, string version, Action<string> log)
    {
        _plugin = plugin;
        _version = version;
        _log = log;
    }

    public void Start()
    {
        _plugin.AddCommand("css_regie_ads_set", "Publicidad de Regie.gg en el centro de la pantalla: css_regie_ads_set <id> <parte> <total> <base64url>", OnSet);
        _plugin.AddCommand("css_regie_ads_test", "Muestra la publicidad de Regie.gg a todos un rato: css_regie_ads_test <segundos>", OnTest);
        _plugin.AddCommand("css_regie_ads_off", "Saca la publicidad de Regie.gg del centro de la pantalla", OnOff);
        _plugin.AddCommand("css_regie_ads_chat", "Frase de un sponsor de Regie.gg en el chat: css_regie_ads_chat <colorPrefijo> <colorTexto> <base64url>", OnChat);
        _plugin.RegisterListener<Listeners.OnTick>(Refrescar);
        // Las reglas son una entidad del mapa: con el mapa nuevo hay otras.
        _plugin.RegisterListener<Listeners.OnMapStart>(_ =>
        {
            _proxy = null;
            _reglas = null;
            _proximaBusqueda = 0;
        });
    }

    public void Dispose()
    {
        _plugin.RemoveListener<Listeners.OnTick>(Refrescar);
        _html = "";
        CentroSinPestaneo.Pedir(Clave, false);
    }

    private string Respuesta(string extra = "") => $"{Ok} {_version}{extra}";

    // ---------------------------------------------------------------
    // Comandos
    // ---------------------------------------------------------------

    private void OnSet(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (caller != null) return;
        if (cmd.ArgCount < 5
            || !int.TryParse(cmd.GetArg(1), NumberStyles.Integer, CultureInfo.InvariantCulture, out var id)
            || !int.TryParse(cmd.GetArg(2), NumberStyles.Integer, CultureInfo.InvariantCulture, out var parte)
            || !int.TryParse(cmd.GetArg(3), NumberStyles.Integer, CultureInfo.InvariantCulture, out var total)
            || total < 1 || total > MaxPartes || parte < 1 || parte > total)
        {
            cmd.ReplyToCommand("uso: css_regie_ads_set <id> <parte> <total> <base64url>");
            return;
        }
        // Un id nuevo descarta los pedazos de una placa que quedó a medias.
        if (id != _idEnArmado || _partes.Length != total)
        {
            _idEnArmado = id;
            _partes = new string?[total];
        }
        _partes[parte - 1] = cmd.GetArg(4);
        if (_partes.Any(p => p == null))
        {
            cmd.ReplyToCommand(Respuesta());
            return;
        }

        var json = VetoEnPantalla.Decodificar(string.Concat(_partes));
        _idEnArmado = -1;
        _partes = Array.Empty<string?>();
        var placa = json != null ? Leer(json) : (null, "base64 invalido", false);
        if (placa.html == null)
        {
            // La que estaba sigue: mejor la vieja que ninguna por un error.
            cmd.ReplyToCommand($"regie-ads: placa invalida ({placa.error})");
            return;
        }
        _html = placa.html;
        _activa = placa.activa;
        _excluir = Excluido(json!);
        _vence = Environment.TickCount64 + VenceMs;
        cmd.ReplyToCommand(Respuesta());
    }

    private void OnTest(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (caller != null) return;
        // InvariantCulture: con la cultura de la PC (es-AR usa coma) "2.5"
        // no se entendería.
        if (cmd.ArgCount < 2 || !float.TryParse(cmd.GetArg(1), NumberStyles.Float, CultureInfo.InvariantCulture, out var segundos) || segundos <= 0)
        {
            cmd.ReplyToCommand("uso: css_regie_ads_test <segundos>");
            return;
        }
        if (_html.Length == 0)
        {
            cmd.ReplyToCommand("regie-ads: no hay publicidad cargada");
            return;
        }
        _pruebaHasta = Environment.TickCount64 + (long)(Math.Min(segundos, PruebaMaxSeg) * 1000);
        cmd.ReplyToCommand(Respuesta($" jugadores={Humanos().Count()}"));
    }

    private void OnOff(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (caller != null) return;
        _html = "";
        _activa = false;
        _excluir = 0;
        _pruebaHasta = 0;
        cmd.ReplyToCommand(Respuesta());
    }

    private void OnChat(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (caller != null) return;
        if (cmd.ArgCount < 4)
        {
            cmd.ReplyToCommand("uso: css_regie_ads_chat <colorPrefijo> <colorTexto> <base64url>");
            return;
        }
        if (!ColoresChat.TryGetValue(cmd.GetArg(1), out var colorPrefijo) || !ColoresChat.TryGetValue(cmd.GetArg(2), out var colorTexto))
        {
            cmd.ReplyToCommand("regie-ads: color invalido");
            return;
        }
        var json = VetoEnPantalla.Decodificar(cmd.GetArg(3));
        var frase = json != null ? LeerFrase(json) : null;
        if (frase == null)
        {
            cmd.ReplyToCommand("regie-ads: frase invalida");
            return;
        }
        var (prefijo, texto, corchetesBlancos) = frase.Value;
        if (texto.Length == 0)
        {
            cmd.ReplyToCommand("regie-ads: frase vacia");
            return;
        }
        Server.PrintToChatAll(LineaChat(colorPrefijo, prefijo, colorTexto, texto, corchetesBlancos));
        cmd.ReplyToCommand(Respuesta());
    }

    // ---------------------------------------------------------------
    // En cada tick
    // ---------------------------------------------------------------

    private static IEnumerable<CCSPlayerController> Humanos() =>
        Utilities.GetPlayers().Where(p => p != null && p.IsValid && !p.IsBot && !p.IsHLTV && p.Connected == PlayerConnectedState.PlayerConnected);

    /// <summary>Vivo y en un equipo: en plena ronda, a ese no se le tapa la pantalla.</summary>
    private static bool Jugando(CCSPlayerController p) => (p.TeamNum == 2 || p.TeamNum == 3) && p.PawnIsAlive;

    private void Refrescar()
    {
        var mostrando = false;
        try
        {
            var aTodos = ATodos();
            var enPrueba = Environment.TickCount64 < _pruebaHasta;
            if (aTodos != null && !CentroSinPestaneo.LoPide("veto"))
            {
                foreach (var p in Humanos())
                {
                    if (MenuManager.GetActiveMenu(p) is CenterHtmlMenuInstance) continue;
                    // El camarógrafo de la transmisión, si así se eligió (la prueba la ven todos).
                    if (_excluir != 0 && !enPrueba && p.SteamID == _excluir) continue;
                    if (aTodos == false && Jugando(p)) continue;
                    p.PrintToCenterHtml(_html, DuracionSeg);
                    mostrando = true;
                }
            }
        }
        catch (Exception ex)
        {
            // Nunca tirar el tick del servidor por esto, y no llenar la
            // consola con el mismo error 64 veces por segundo.
            var ahora = Environment.TickCount64;
            if (ahora - _ultimoError > 10_000)
            {
                _ultimoError = ahora;
                _log($"publicidad: error mostrando la placa: {ex.Message}");
            }
        }
        CentroSinPestaneo.Pedir(Clave, mostrando);
    }

    /// <summary>null: no se muestra; true: a todos (la prueba, o un momento sin juego); false: solo a muertos y espectadores.</summary>
    private bool? ATodos()
    {
        if (_html.Length == 0) return null;
        var ahora = Environment.TickCount64;
        if (ahora < _pruebaHasta) return true;
        if (!_activa || ahora > _vence) return null;
        return SinJuego(Reglas());
    }

    // Los momentos sin juego: ahí la ven todos, también los vivos. Sin las
    // reglas (entre mapas) no se sabe: solo muertos y espectadores.
    private static bool SinJuego(CCSGameRules? r)
    {
        if (r == null) return false;
        return r.WarmupPeriod
            || r.FreezePeriod
            || r.MatchWaitingForResume
            || r.TerroristTimeOutActive
            || r.CTTimeOutActive
            || r.TechnicalTimeOut
            // GamePhase del juego: 4 = entretiempo, 5 = terminó el mapa.
            || r.GamePhase == 4
            || r.GamePhase == 5;
    }

    private CCSGameRules? Reglas()
    {
        if (_proxy != null && _proxy.IsValid) return _reglas;
        _proxy = null;
        _reglas = null;
        // Sin reglas se busca de vuelta una vez por segundo, no en cada
        // tick: recorrer las entidades no es gratis.
        var ahora = Environment.TickCount64;
        if (ahora < _proximaBusqueda) return null;
        _proximaBusqueda = ahora + 1000;
        _proxy = Utilities.FindAllEntitiesByDesignerName<CCSGameRulesProxy>("cs_gamerules").FirstOrDefault();
        _reglas = _proxy?.GameRules;
        return _reglas;
    }

    // ---------------------------------------------------------------
    // La placa
    // ---------------------------------------------------------------

    /// <summary>El SteamID64 de la clave "x" (el camarógrafo que no la ve), o 0.</summary>
    private static ulong Excluido(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var r = doc.RootElement;
            if (r.ValueKind != JsonValueKind.Object || !r.TryGetProperty("x", out var x) || x.ValueKind != JsonValueKind.String) return 0;
            return ulong.TryParse(x.GetString(), NumberStyles.None, CultureInfo.InvariantCulture, out var sid) && sid > 76561190000000000UL ? sid : 0;
        }
        catch (JsonException)
        {
            return 0;
        }
    }

    private static (string? html, string error, bool activa) Leer(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var r = doc.RootElement;
            if (r.ValueKind != JsonValueKind.Object) return (null, "no es un objeto", false);
            var activa = Entero(r, "a") == 1;

            var partes = new List<string>();
            var imagen = r.TryGetProperty("i", out var i) && i.ValueKind == JsonValueKind.String ? i.GetString() ?? "" : "";
            if (imagen.Length > 0)
            {
                if (!UrlSegura(imagen)) return (null, "url invalida", false);
                var w = Entero(r, "w");
                var h = Entero(r, "h");
                if (w < 16 || w > 1000 || h < 1 || h > 1000) return (null, "tamano invalido", false);
                // La URL ya pasó por UrlSegura: sin comillas ni < >, no
                // puede cerrar el atributo ni abrir otra etiqueta.
                partes.Add($"<img src='{imagen}' width='{w}' height='{h}'/>");
            }

            if (r.TryGetProperty("l", out var l) && l.ValueKind == JsonValueKind.Array)
            {
                foreach (var x in l.EnumerateArray().Take(MaxLineas))
                {
                    if (x.ValueKind != JsonValueKind.Array || x.GetArrayLength() < 2) continue;
                    var texto = VetoEnPantalla.Limpio(x[0].ValueKind == JsonValueKind.String ? x[0].GetString() : "", MaxTexto);
                    if (texto.Length == 0) continue;
                    var color = x[1].ValueKind == JsonValueKind.String ? x[1].GetString() ?? "" : "";
                    // Un color que no es #rrggbb no se acepta: va tal cual
                    // adentro de un atributo del HTML.
                    if (!ColorHex.IsMatch(color)) return (null, "color invalido", false);
                    var tamanio = x.GetArrayLength() > 2 && x[2].ValueKind == JsonValueKind.String ? x[2].GetString() ?? "" : "";
                    if (!Tamanios.Contains(tamanio)) tamanio = "m";
                    // El texto lo escribió una persona: va escapado.
                    partes.Add($"<font class='fontSize-{tamanio}' color='{color}'>{WebUtility.HtmlEncode(texto)}</font>");
                }
            }

            if (partes.Count == 0) return (null, "nada para mostrar", false);
            return (string.Join("<br>", partes), "", activa);
        }
        catch (JsonException)
        {
            return (null, "json invalido", false);
        }
    }

    // ---------------------------------------------------------------
    // La frase del chat
    // ---------------------------------------------------------------

    private static (string prefijo, string texto, bool corchetesBlancos)? LeerFrase(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var r = doc.RootElement;
            if (r.ValueKind != JsonValueKind.Object) return null;
            var prefijo = r.TryGetProperty("p", out var p) && p.ValueKind == JsonValueKind.String ? p.GetString() : "";
            var texto = r.TryGetProperty("t", out var t) && t.ValueKind == JsonValueKind.String ? t.GetString() : "";
            // "b": 1 = corchetes en blanco, como MatchZy; si falta, del color del prefijo.
            var corchetesBlancos = r.TryGetProperty("b", out var b) && b.ValueKind == JsonValueKind.Number && b.TryGetInt32(out var nb) && nb == 1;
            // Sin caracteres de control (Limpio): en el chat de CS2 los
            // colores SON caracteres de control, y uno escondido en la
            // frase cambiaría el color del resto. Así los únicos colores
            // del mensaje son los dos de la lista.
            return (VetoEnPantalla.Limpio(prefijo, MaxPrefijoChat), VetoEnPantalla.Limpio(texto, MaxTextoChat), corchetesBlancos);
        }
        catch (JsonException)
        {
            return null;
        }
    }

    // Como MatchZy: « [Prefijo] frase». El espacio del principio no es
    // decorativo: sin él, CS2 ignora el color del primer caracter (igual
    // que en el chat del veto). Sin prefijo, solo la frase. Los corchetes
    // van del color del prefijo, o en blanco como los de MatchZy.
    private static string LineaChat(char colorPrefijo, string prefijo, char colorTexto, string texto, bool corchetesBlancos)
    {
        if (prefijo.Length == 0) return $" {colorTexto}{texto}";
        return corchetesBlancos
            ? $" {ChatColors.Default}[{colorPrefijo}{prefijo}{ChatColors.Default}] {colorTexto}{texto}"
            : $" {colorPrefijo}[{prefijo}] {colorTexto}{texto}";
    }

    private static int Entero(JsonElement r, string clave) =>
        r.TryGetProperty(clave, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var n) ? n : 0;

    // http(s) y solo caracteres que no pueden romper el atributo del HTML:
    // sin comillas, sin < >, sin espacios ni caracteres de control. La
    // misma regla que urlSegura en backend/src/publicidadPlaca.ts.
    private static bool UrlSegura(string u)
    {
        if (u.Length > MaxUrl) return false;
        foreach (var c in u)
        {
            if (c <= ' ' || c >= 127 || c == '\'' || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`') return false;
        }
        return Uri.TryCreate(u, UriKind.Absolute, out var uri) && (uri.Scheme == Uri.UriSchemeHttp || uri.Scheme == Uri.UriSchemeHttps);
    }
}
