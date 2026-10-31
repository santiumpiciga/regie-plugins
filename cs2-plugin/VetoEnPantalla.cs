using System.Text;
using System.Text.Json;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Menu;
using CounterStrikeSharp.API.Modules.Timers;
using CounterStrikeSharp.API.Modules.Utils;

namespace Cs2LiveViewerHelper;

// El veto de mapas por chat, en pantalla (1.4.0). El backend
// (backend/src/vetoServidor/) decide todo y manda por RCON QUÉ mostrar;
// acá solo se muestra:
//
//   css_regie_veto_chat <tipo> <base64url>
//     Un mensaje del veto en el chat de todos, con el prefijo [Veto] y el
//     color de su tipo (ban rojo, pick verde, lado celeste, turno dorado,
//     aviso gris, error naranja, resultado verde claro, info blanco). El
//     texto viaja en base64url (UTF-8): así llegan las tildes, y nada de
//     lo que escribió una persona (el nombre de un equipo con comillas o
//     un ';') puede cortar el comando ni encadenar otro en la consola.
//
//   css_regie_veto_panel <id> <parte> <total> <base64url> | off
//     El estado del veto (JSON con claves cortas, ver comandosPanelCs2 en
//     vetoServidor/comandos.ts), partido en pedazos si no entra en una
//     línea de consola. Se muestra fijo en el centro de la pantalla de
//     todos, re-enviado cada segundo mientras dure el veto; "off" lo saca.
//     Además, al capitán de turno se le abre un menú con los mapas que
//     quedan (o los lados): lo que elige se reenvía al backend por el feed
//     como si lo hubiera escrito en el chat (".vetar mirage"), así el
//     backend no tiene lógica aparte para el menú.
//
// Los dos contestan «regie-veto ok»: si el backend no lee eso (plugin
// viejo, comando que falló), escribe el mensaje con `say` como antes.
//
// El panel del centro y el menú se pisan (los dos son el mismo HTML del
// centro de la pantalla): el que tiene un menú del centro abierto (el
// nuestro o el de otro plugin) no recibe el panel. Así el capitán de turno ve el menú
// y el resto el panel.
//
// Solo desde la consola del servidor (RCON): un jugador que los escriba
// en su consola no puede hacerse pasar por el veto.
public sealed class VetoEnPantalla : IDisposable
{
    public const string Ok = "regie-veto ok";
    // Mientras dure, el panel se reenvía en CADA tick: el recuadro del
    // centro dura ~1 s aunque se le pida más, y reenviado cada 1 s se veía
    // y desaparecía. El pestañeo de una vez por segundo lo saca
    // CentroSinPestaneo, que se pide mientras haya panel.
    private const int DuracionSeg = 1;
    private const int MaxPartes = 16;
    private const int MaxHtml = 900;

    private readonly BasePlugin _plugin;
    private readonly Action<CCSPlayerController, string> _reenviar;
    private readonly Action<string> _log;

    private Panel? _panel;
    // El HTML se arma una vez por panel nuevo, no en cada refresco.
    private string _html = "";

    private int _idEnArmado = -1;
    private string?[] _partes = Array.Empty<string?>();

    private CenterHtmlMenu? _menu;
    private ulong _menuPara;

    private sealed class Panel
    {
        public string[] Equipos = { "", "" };
        public int MejorDe;
        public List<string> Quedan = new();
        public List<Hecho> Hechos = new();
        public Turno? Turno;
        // 1.7.0: la tarjeta del veto en imagen (backend/src/vetoServidor/tarjeta.ts).
        // Si viene, el recuadro muestra la imagen en vez del texto.
        public string Tarjeta = "";
        // 1.9.0: una línea al pie (la cuenta para pasar al primer mapa al
        // terminar el veto), debajo de la tarjeta o del texto.
        public string Pie = "";
    }

    private sealed class Hecho
    {
        public char Tipo; // 'b' ban, 'p' pick, 'l' lado
        public int Equipo;
        public string Mapa = "";
        public string Lado = "";
    }

    private sealed class Turno
    {
        public int Equipo;
        public string Capitan = "";
        public ulong SteamId;
        public string Accion = ""; // vetar | elegir | lado
        public string Que = "";
    }

    public VetoEnPantalla(BasePlugin plugin, Action<CCSPlayerController, string> reenviar, Action<string> log)
    {
        _plugin = plugin;
        _reenviar = reenviar;
        _log = log;
    }

    public void Start()
    {
        _plugin.AddCommand("css_regie_veto_chat", "Mensaje del veto por chat de Regie.gg: css_regie_veto_chat <tipo> <base64url>", OnChat);
        _plugin.AddCommand("css_regie_veto_panel", "Panel del veto por chat de Regie.gg: css_regie_veto_panel <id> <parte> <total> <base64url> | off", OnPanel);
        // El que entra a mitad del veto: el estado completo, y el menú si es el capitán de turno.
        _plugin.RegisterEventHandler<EventPlayerConnectFull>((e, _) => { AlEntrar(e.Userid); return HookResult.Continue; }, HookMode.Post);
        _plugin.RegisterListener<Listeners.OnTick>(Refrescar);
    }

    public void Dispose()
    {
        _plugin.RemoveListener<Listeners.OnTick>(Refrescar);
        CerrarMenu();
        _panel = null;
        CentroSinPestaneo.Pedir("veto", false);
    }

    // ---------------------------------------------------------------
    // Chat
    // ---------------------------------------------------------------

    private static char ColorDe(string tipo) => tipo switch
    {
        "ban" => ChatColors.Red,
        "pick" => ChatColors.Green,
        "lado" => ChatColors.LightBlue,
        "turno" => ChatColors.Gold,
        "aviso" => ChatColors.Grey,
        "error" => ChatColors.Orange,
        "resultado" => ChatColors.Lime,
        "info" => ChatColors.Default,
        _ => '\0',
    };

    // El espacio del principio no es decorativo: sin él, CS2 ignora el
    // color del primer caracter del mensaje.
    private static string Linea(char color, string texto) => $" {ChatColors.Purple}[Veto]{ChatColors.Default} {color}{texto}";

    private void OnChat(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (caller != null) return;
        if (cmd.ArgCount < 3) { cmd.ReplyToCommand("uso: css_regie_veto_chat <tipo> <base64url>"); return; }
        var color = ColorDe(cmd.GetArg(1).ToLowerInvariant());
        var texto = Decodificar(cmd.GetArg(2));
        if (color == '\0' || texto == null) { cmd.ReplyToCommand("regie-veto: tipo o texto invalido"); return; }
        texto = Limpio(texto, 200);
        if (texto.Length == 0) { cmd.ReplyToCommand("regie-veto: texto vacio"); return; }
        Server.PrintToChatAll(Linea(color, texto));
        cmd.ReplyToCommand(Ok);
    }

    // ---------------------------------------------------------------
    // Panel
    // ---------------------------------------------------------------

    private void OnPanel(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (caller != null) return;
        if (cmd.ArgCount >= 2 && cmd.GetArg(1).Equals("off", StringComparison.OrdinalIgnoreCase))
        {
            Sacar();
            cmd.ReplyToCommand(Ok);
            return;
        }
        if (cmd.ArgCount < 5
            || !int.TryParse(cmd.GetArg(1), out var id)
            || !int.TryParse(cmd.GetArg(2), out var parte)
            || !int.TryParse(cmd.GetArg(3), out var total)
            || total < 1 || total > MaxPartes || parte < 1 || parte > total)
        {
            cmd.ReplyToCommand("uso: css_regie_veto_panel <id> <parte> <total> <base64url> | off");
            return;
        }
        // Un id nuevo descarta los pedazos de un panel que quedó a medias.
        if (id != _idEnArmado || _partes.Length != total)
        {
            _idEnArmado = id;
            _partes = new string?[total];
        }
        _partes[parte - 1] = cmd.GetArg(4);
        if (_partes.Any(p => p == null)) { cmd.ReplyToCommand(Ok); return; }

        var json = Decodificar(string.Concat(_partes));
        _idEnArmado = -1;
        _partes = Array.Empty<string?>();
        var panel = json != null ? Leer(json) : null;
        if (panel == null) { cmd.ReplyToCommand("regie-veto: panel invalido"); return; }
        _panel = panel;
        CentroSinPestaneo.Pedir("veto", true);
        // El ancho y el alto se piden igual: el recuadro estira la imagen a su
        // ancho fijo (medido el 20/10/2026), así que se muestra entera.
        _html = panel.Tarjeta.Length > 0 ? $"<img src='{panel.Tarjeta}' width='560' height='250'/>" + PieHtml(panel, true) : ArmarHtml(panel);
        ActualizarMenu();
        Refrescar();
        cmd.ReplyToCommand(Ok);
    }

    private void Sacar()
    {
        _panel = null;
        CentroSinPestaneo.Pedir("veto", false);
        _html = "";
        CerrarMenu();
    }

    /// <summary>
    /// La dirección de la tarjeta, si es una http(s) que no puede romper el
    /// HTML del recuadro (sin comillas, sin &lt; &gt;, sin espacios). null si no.
    /// </summary>
    private static string? UrlDeTarjeta(string? url)
    {
        if (string.IsNullOrEmpty(url) || url.Length > 300) return null;
        if (!url.StartsWith("http://", StringComparison.OrdinalIgnoreCase) && !url.StartsWith("https://", StringComparison.OrdinalIgnoreCase)) return null;
        return url.Any(c => c == (char)39 || c == (char)34 || c == '<' || c == '>' || char.IsWhiteSpace(c) || char.IsControl(c)) ? null : url;
    }

    private static Panel? Leer(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var r = doc.RootElement;
            var p = new Panel();
            if (r.TryGetProperty("e", out var e) && e.ValueKind == JsonValueKind.Array && e.GetArrayLength() == 2)
            {
                p.Equipos = new[] { Limpio(e[0].GetString(), 24), Limpio(e[1].GetString(), 24) };
            }
            if (r.TryGetProperty("bo", out var bo) && bo.ValueKind == JsonValueKind.Number) p.MejorDe = bo.GetInt32();
            if (r.TryGetProperty("i", out var i) && i.ValueKind == JsonValueKind.String && UrlDeTarjeta(i.GetString()) is { } url) p.Tarjeta = url;
            if (r.TryGetProperty("f", out var f) && f.ValueKind == JsonValueKind.String) p.Pie = Limpio(f.GetString(), 60);
            if (r.TryGetProperty("q", out var q) && q.ValueKind == JsonValueKind.Array)
            {
                foreach (var m in q.EnumerateArray().Take(32)) p.Quedan.Add(Limpio(m.GetString(), 24));
            }
            if (r.TryGetProperty("h", out var h) && h.ValueKind == JsonValueKind.Array)
            {
                foreach (var x in h.EnumerateArray().Take(32))
                {
                    if (x.ValueKind != JsonValueKind.Array || x.GetArrayLength() < 3) continue;
                    var tipo = x[0].GetString() ?? "";
                    if (tipo.Length != 1 || "bpl".IndexOf(tipo[0]) < 0) continue;
                    p.Hechos.Add(new Hecho
                    {
                        Tipo = tipo[0],
                        Equipo = x[1].ValueKind == JsonValueKind.Number && x[1].GetInt32() == 1 ? 1 : 0,
                        Mapa = Limpio(x[2].GetString(), 24),
                        Lado = x.GetArrayLength() > 3 ? Limpio(x[3].GetString(), 8) : "",
                    });
                }
            }
            if (r.TryGetProperty("t", out var t) && t.ValueKind == JsonValueKind.Object)
            {
                var accion = t.TryGetProperty("a", out var a) ? a.GetString() ?? "" : "";
                if (accion is "vetar" or "elegir" or "lado")
                {
                    p.Turno = new Turno
                    {
                        Equipo = t.TryGetProperty("e", out var te) && te.ValueKind == JsonValueKind.Number && te.GetInt32() == 1 ? 1 : 0,
                        Capitan = t.TryGetProperty("c", out var c) ? Limpio(c.GetString(), 24) : "",
                        SteamId = t.TryGetProperty("s", out var s) && ulong.TryParse(s.GetString(), out var sid) ? sid : 0,
                        Accion = accion,
                        Que = t.TryGetProperty("x", out var x) ? Limpio(x.GetString(), 40) : "",
                    };
                }
            }
            return p;
        }
        catch (Exception)
        {
            return null;
        }
    }

    // "Ban Alfa: mirage, anubis · Beta: nuke": agrupado por equipo, para
    // que entre en el centro de la pantalla. Con el lado, si ya se eligió:
    // "Pick Alfa: inferno (CT Beta)".
    private static List<(char tipo, string texto)> Agrupados(Panel p)
    {
        var lados = new Dictionary<string, string>();
        foreach (var h in p.Hechos.Where(h => h.Tipo == 'l')) lados[h.Mapa] = $"{h.Lado} {p.Equipos[h.Equipo]}";
        var lineas = new List<(char, string)>();
        foreach (var tipo in new[] { 'b', 'p' })
        {
            var grupos = new List<string>();
            for (var eq = 0; eq < 2; eq++)
            {
                var mapas = p.Hechos.Where(h => h.Tipo == tipo && h.Equipo == eq)
                    .Select(h => lados.TryGetValue(h.Mapa, out var l) ? $"{h.Mapa} ({l})" : h.Mapa).ToList();
                if (mapas.Count > 0) grupos.Add($"{p.Equipos[eq]}: {string.Join(", ", mapas)}");
            }
            if (grupos.Count > 0) lineas.Add((tipo, string.Join(" · ", grupos)));
        }
        return lineas;
    }

    private static string Quedan(Panel p) =>
        string.Join("  ", p.Quedan.Select((m, i) => $"{i + 1}) {m}"));

    private static string TextoTurno(Panel p) =>
        p.Turno == null ? "" : $"{Textos.T("turno")}: {p.Equipos[p.Turno.Equipo]} ({p.Turno.Capitan}) · {p.Turno.Que}";

    private static string ArmarHtml(Panel p)
    {
        string Html(string s) => System.Net.WebUtility.HtmlEncode(s);
        var titulo = $"<font class='fontSize-m' color='#c084fc'>VETO</font> <font class='fontSize-m'>{Html(p.Equipos[0])} vs {Html(p.Equipos[1])} · {Textos.T("mejorDe")} {p.MejorDe}</font>";
        var hechos = Agrupados(p).Select(l =>
            $"<font class='fontSize-s' color='{(l.tipo == 'b' ? "#ff6b6b" : "#6bff8f")}'>{(l.tipo == 'b' ? "Ban" : "Pick")}</font> <font class='fontSize-s'>{Html(l.texto)}</font>").ToList();
        var quedan = p.Quedan.Count > 0 ? $"<font class='fontSize-s' color='#bbbbbb'>{Textos.T("quedan")}: {Html(Quedan(p))}</font>" : "";
        var turno = p.Turno != null ? $"<font class='fontSize-m' color='#ffd166'>{Html(TextoTurno(p))}</font>" : "";
        var pie = PieHtml(p, false);
        string Unir() => string.Join("<br>", new[] { titulo }.Concat(hechos).Append(quedan).Append(turno).Append(pie).Where(s => s.Length > 0));
        var html = Unir();
        // El centro de la pantalla no muestra un HTML muy largo: se pierden
        // los hechos (están en el chat) antes que lo que queda y el turno.
        while (html.Length > MaxHtml && hechos.Count > 0)
        {
            hechos.RemoveAt(0);
            html = Unir();
        }
        return html;
    }

    private static string PieHtml(Panel p, bool conSalto) =>
        p.Pie.Length == 0 ? "" : $"{(conSalto ? "<br>" : "")}<font class='fontSize-m' color='#ffd166'>{System.Net.WebUtility.HtmlEncode(p.Pie)}</font>";

    private void Refrescar()
    {
        if (_panel == null || _html.Length == 0) return;
        try
        {
            foreach (var p in Humanos())
            {
                // Con un menú del centro abierto (el del capitán, o el de
                // otro plugin) el panel lo taparía: ese jugador ve el menú.
                // Uno de chat no choca con el panel.
                if (MenuManager.GetActiveMenu(p) is CenterHtmlMenuInstance) continue;
                p.PrintToCenterHtml(_html, DuracionSeg);
            }
        }
        catch (Exception ex)
        {
            _log($"veto: error mostrando el panel: {ex.Message}");
        }
    }

    // ---------------------------------------------------------------
    // Menú del capitán
    // ---------------------------------------------------------------

    private static IEnumerable<CCSPlayerController> Humanos() =>
        Utilities.GetPlayers().Where(p => p != null && p.IsValid && !p.IsBot && !p.IsHLTV && p.Connected == PlayerConnectedState.PlayerConnected);

    private static CCSPlayerController? PorSteamId(ulong sid) =>
        sid == 0 ? null : Humanos().FirstOrDefault(p => p.SteamID == sid);

    private void CerrarMenu()
    {
        if (_menu == null) return;
        try
        {
            var p = PorSteamId(_menuPara);
            if (p != null && (MenuManager.GetActiveMenu(p) as BaseMenuInstance)?.Menu == _menu) MenuManager.CloseActiveMenu(p);
        }
        catch (Exception ex)
        {
            _log($"veto: error cerrando el menu: {ex.Message}");
        }
        _menu = null;
        _menuPara = 0;
    }

    // Con cada panel (un paso nuevo, o el recordatorio del backend): el
    // menú al capitán de turno, y a nadie más.
    private void ActualizarMenu()
    {
        CerrarMenu();
        var turno = _panel?.Turno;
        if (turno == null) return;
        var capitan = PorSteamId(turno.SteamId);
        if (capitan == null) return; // no está conectado: se le abre al entrar
        AbrirMenu(capitan, turno);
    }

    private void AbrirMenu(CCSPlayerController capitan, Turno turno)
    {
        try
        {
            var menu = new CenterHtmlMenu(System.Net.WebUtility.HtmlEncode($"Veto: {turno.Que}"), _plugin)
            {
                PostSelectAction = PostSelectAction.Close,
                ExitButton = true,
            };
            if (turno.Accion == "lado")
            {
                menu.AddMenuOption("CT", (p, _) => Elegir(p, ".lado ct"));
                menu.AddMenuOption("T", (p, _) => Elegir(p, ".lado t"));
            }
            else
            {
                foreach (var mapa in _panel!.Quedan)
                {
                    var m = mapa;
                    menu.AddMenuOption(System.Net.WebUtility.HtmlEncode(m), (p, _) => Elegir(p, $".{turno.Accion} {m}"));
                }
            }
            MenuManager.OpenCenterHtmlMenu(_plugin, capitan, menu);
            _menu = menu;
            _menuPara = capitan.SteamID;
        }
        catch (Exception ex)
        {
            _log($"veto: error abriendo el menu: {ex.Message}");
        }
    }

    private void Elegir(CCSPlayerController p, string comando)
    {
        // Un menú viejo (el turno pasó por la web, o el panel cambió): no
        // se manda nada, el backend igual lo rechazaría.
        if (_panel?.Turno == null || _panel.Turno.SteamId != p.SteamID)
        {
            p.PrintToChat(Linea(ChatColors.Grey, Textos.T("turnoPaso")));
            return;
        }
        _menu = null;
        _menuPara = 0;
        _reenviar(p, comando);
    }

    // ---------------------------------------------------------------
    // El que entra a mitad del veto
    // ---------------------------------------------------------------

    private void AlEntrar(CCSPlayerController? jugador)
    {
        if (jugador == null || !jugador.IsValid || jugador.IsBot || jugador.IsHLTV) return;
        var sid = jugador.SteamID;
        // Recién conectado el chat todavía no se ve: unos segundos después.
        _plugin.AddTimer(3.0f, () =>
        {
            var p = PorSteamId(sid);
            var panel = _panel;
            if (p == null || panel == null) return;
            p.PrintToChat(Linea(ChatColors.Default, $"{panel.Equipos[0]} vs {panel.Equipos[1]} · {Textos.T("mejorDe")} {panel.MejorDe}"));
            foreach (var (tipo, texto) in Agrupados(panel))
            {
                p.PrintToChat(Linea(tipo == 'b' ? ChatColors.Red : ChatColors.Green, $"{(tipo == 'b' ? "Bans" : "Picks")}: {texto}"));
            }
            if (panel.Quedan.Count > 0) p.PrintToChat(Linea(ChatColors.Default, $"{Textos.T("quedan")}: {Quedan(panel)}"));
            if (panel.Turno != null)
            {
                p.PrintToChat(Linea(ChatColors.Gold, TextoTurno(panel)));
                if (panel.Turno.SteamId == sid && _menu == null) AbrirMenu(p, panel.Turno);
            }
        });
    }

    // ---------------------------------------------------------------
    // Texto (lo usa también PublicidadEnPantalla, por eso internal)
    // ---------------------------------------------------------------

    internal static string? Decodificar(string b64url)
    {
        try
        {
            var s = b64url.Trim().Replace('-', '+').Replace('_', '/');
            s = s.PadRight(s.Length + (4 - s.Length % 4) % 4, '=');
            return Encoding.UTF8.GetString(Convert.FromBase64String(s));
        }
        catch (FormatException)
        {
            return null;
        }
    }

    // Sin caracteres de control: en el chat de CS2 los colores SON
    // caracteres de control, y un nombre con uno adentro cambiaría el
    // color del resto del mensaje.
    internal static string Limpio(string? s, int max)
    {
        if (string.IsNullOrEmpty(s)) return "";
        var sb = new StringBuilder(Math.Min(s.Length, max));
        foreach (var ch in s)
        {
            if (char.IsControl(ch)) continue;
            sb.Append(ch);
        }
        var t = sb.ToString().Trim();
        return t.Length > max ? t[..(max - 1)] + "…" : t;
    }
}
