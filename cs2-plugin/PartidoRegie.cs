using System.Text;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.UserMessages;
using CounterStrikeSharp.API.Modules.Utils;

namespace Cs2LiveViewerHelper;

// Dos cosas del partido que MatchZy no hace como las quiere Regie.gg, sin
// tocar MatchZy (queda intacto, ver docs/plan-gestor-regie-cs16.md):
//
// 1. Inicio a cargo de la producción (css_regie_hold). Con el freno
//    prendido, el .ready / .r / !ready de los jugadores se anota pero no
//    le llega a MatchZy: MatchZy lee el .ready del evento de chat del
//    juego (EventPlayerChat) y el !ready como css_ready, y frenar el `say`
//    o el comando antes de que el juego lo procese corta los dos. El
//    partido arranca cuando el backend manda css_start de MatchZy (el
//    botón «Arrancar partido» de la consola), que viene de la consola del
//    servidor y no pasa por acá. El freno dura toda la serie (vale para cada mapa) y se
//    apaga con css_regie_hold 0 o al descargar el plugin.
//
// 2. El marcador de la serie en el chat (css_regie_serie_*). MatchZy lo
//    dice al terminar cada mapa, pero escrito en su código, en inglés, y
//    en una serie retomada (cada mapa que falta se le carga como un
//    Mejor de 1) no lo dice nunca y avisa «has won the match» aunque la
//    serie siga. Con css_regie_serie_on (lo manda el backend al cargar un
//    partido) esos mensajes de MatchZy se tapan y el backend, que lleva
//    la serie entera, manda los suyos con css_regie_serie_chat. Sin el
//    on (nadie cargó el partido desde Regie.gg) MatchZy dice lo suyo como
//    siempre.
public sealed class PartidoRegie : IDisposable
{
    public const string Ok = "regie-partido ok";
    // CUserMessageTextMsg: lo que manda Server.PrintToChatAll (dest 3 = chat).
    private const int TextMsgId = 124;
    private const int DestChat = 3;

    // Lo que MatchZy dice de la serie y del final, escrito en su código
    // (Utility.cs HandleMatchEnd, MatchManagement.cs EndSeries).
    private static readonly string[] FrasesDeMatchZy =
    {
        "is winning the series",
        "The series is tied at",
        "has won the match",
        "have tied the match",
    };

    private static readonly string[] Listos = { ".ready", ".r", "!ready", "!r" };

    private readonly BasePlugin _plugin;
    private readonly Action<string> _log;
    private bool _freno;
    private bool _serieRegie;
    private readonly Dictionary<ulong, string> _pidieron = new();

    public PartidoRegie(BasePlugin plugin, Action<string> log)
    {
        _plugin = plugin;
        _log = log;
    }

    public void Start()
    {
        _plugin.AddCommand("css_regie_hold", "Inicio a cargo de la producción: css_regie_hold <0|1>", OnHold);
        _plugin.AddCommand("css_regie_holdstatus", "Quiénes pidieron listo con el freno prendido (steamid64|nombre por línea)", OnHoldStatus);
        _plugin.AddCommand("css_regie_serie_on", "Regie.gg dice el marcador de la serie en el chat (tapa el de MatchZy)", (c, cmd) => Serie(c, cmd, true));
        _plugin.AddCommand("css_regie_serie_off", "MatchZy vuelve a decir el marcador de la serie", (c, cmd) => Serie(c, cmd, false));
        _plugin.AddCommand("css_regie_serie_chat", "Mensaje de la serie de Regie.gg en el chat: css_regie_serie_chat <base64url>", OnSerieChat);
        _plugin.AddCommandListener("say", Frenar, HookMode.Pre);
        _plugin.AddCommandListener("say_team", Frenar, HookMode.Pre);
        _plugin.AddCommandListener("css_ready", FrenarComando, HookMode.Pre);
        _plugin.HookUserMessage(TextMsgId, Tapar, HookMode.Pre);
        _plugin.RegisterListener<Listeners.OnClientDisconnect>(slot =>
        {
            var p = Utilities.GetPlayerFromSlot(slot);
            if (p != null && p.IsValid) _pidieron.Remove(p.SteamID);
        });
    }

    public void Dispose()
    {
        _freno = false;
        _serieRegie = false;
        _pidieron.Clear();
    }

    // --- inicio a cargo de la producción ----------------------------------

    private void OnHold(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!ComandosRegie.Puede(caller, cmd, soloServidor: true)) return;
        var antes = _freno;
        _freno = cmd.GetArg(1) == "1";
        if (!_freno) _pidieron.Clear();
        if (_freno && !antes)
            Server.PrintToChatAll(Linea(Textos.T("arrancaProduccion")));
        cmd.ReplyToCommand($"{Ok} hold={(_freno ? 1 : 0)}");
    }

    private void OnHoldStatus(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!ComandosRegie.Puede(caller, cmd, soloServidor: true)) return;
        cmd.ReplyToCommand($"{Ok} hold={(_freno ? 1 : 0)} listos={_pidieron.Count}");
        foreach (var (sid, nombre) in _pidieron) cmd.ReplyToCommand($"{sid}|{nombre}");
    }

    private HookResult Frenar(CCSPlayerController? p, CommandInfo info)
    {
        if (!_freno || p == null || !p.IsValid || p.IsBot) return HookResult.Continue;
        var texto = info.ArgString.Trim().Trim('"').Trim().ToLowerInvariant();
        if (!Listos.Contains(texto)) return HookResult.Continue;
        Anotar(p);
        return HookResult.Stop;
    }

    private HookResult FrenarComando(CCSPlayerController? p, CommandInfo info)
    {
        if (!_freno || p == null || !p.IsValid || p.IsBot) return HookResult.Continue;
        Anotar(p);
        return HookResult.Stop;
    }

    private void Anotar(CCSPlayerController p)
    {
        var nuevo = !_pidieron.ContainsKey(p.SteamID);
        _pidieron[p.SteamID] = p.PlayerName;
        if (nuevo) Server.PrintToChatAll(Linea(Textos.T("estaListo", Limpio(p.PlayerName))));
        else p.PrintToChat(Linea(Textos.T("yaAnotado")));
    }

    // --- la serie en el chat ------------------------------------------------

    private void Serie(CCSPlayerController? caller, CommandInfo cmd, bool on)
    {
        if (!ComandosRegie.Puede(caller, cmd, soloServidor: true)) return;
        _serieRegie = on;
        cmd.ReplyToCommand($"{Ok} serie={(on ? 1 : 0)}");
    }

    private void OnSerieChat(CCSPlayerController? caller, CommandInfo cmd)
    {
        if (!ComandosRegie.Puede(caller, cmd, soloServidor: true)) return;
        string texto;
        try
        {
            texto = Encoding.UTF8.GetString(DesdeBase64Url(cmd.GetArg(1))).Trim();
        }
        catch
        {
            cmd.ReplyToCommand("css_regie_serie_chat: el texto no es base64url");
            return;
        }
        if (texto.Length == 0 || texto.Length > 200)
        {
            cmd.ReplyToCommand("css_regie_serie_chat: texto vacío o demasiado largo");
            return;
        }
        // Quien manda la serie es Regie.gg: desde acá se tapa la de MatchZy
        // aunque el on se haya perdido (plugin recargado a mitad de serie).
        _serieRegie = true;
        Server.PrintToChatAll(Linea(Limpio(texto, conMarcas: true)));
        cmd.ReplyToCommand(Ok);
    }

    private HookResult Tapar(UserMessage um)
    {
        if (!_serieRegie) return HookResult.Continue;
        try
        {
            if (um.ReadInt("dest") != DestChat) return HookResult.Continue;
            var texto = um.ReadString("param", 0);
            if (!string.IsNullOrEmpty(texto) && FrasesDeMatchZy.Any(f => texto.Contains(f, StringComparison.Ordinal)))
                return HookResult.Stop;
        }
        catch (Exception ex)
        {
            _log($"no se pudo leer un mensaje del chat: {ex.Message}");
        }
        return HookResult.Continue;
    }

    // --- formato ---------------------------------------------------------------

    // « [Regie.gg] texto», como el prefijo que Regie.gg le pone a MatchZy
    // ([{Green}Regie.gg{Default}]). En el texto, {g} y {d} pasan a verde y
    // al color normal: los arma el backend o este plugin, nunca un jugador
    // (Limpio los saca de lo que viene de afuera).
    private static string Linea(string texto) =>
        $" {ChatColors.Default}[{ChatColors.Green}Regie.gg{ChatColors.Default}] "
        + texto.Replace("{g}", ChatColors.Green.ToString()).Replace("{d}", ChatColors.Default.ToString());

    // Sin caracteres de control (los colores del chat de CS2 son bytes
    // de control) y, salvo que se pidan, sin las marcas {g}/{d}.
    private static string Limpio(string s, bool conMarcas = false)
    {
        var sb = new StringBuilder(s.Length);
        foreach (var ch in s) if (!char.IsControl(ch)) sb.Append(ch);
        var t = sb.ToString();
        return conMarcas ? t : t.Replace("{g}", "").Replace("{d}", "");
    }

    private static byte[] DesdeBase64Url(string s)
    {
        var b = s.Replace('-', '+').Replace('_', '/');
        switch (b.Length % 4)
        {
            case 2: b += "=="; break;
            case 3: b += "="; break;
        }
        return Convert.FromBase64String(b);
    }
}
