using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;

namespace Cs2LiveViewerHelper;

// Lo que el plugin les dice a los jugadores por su cuenta (el panel del
// veto, el inicio a cargo de la producción), en el idioma de Regie.gg. El
// idioma lo manda el backend con css_regie_lang (backend/src/idioma.ts) al
// cambiarlo, al cargar un partido y al empezar un veto; hasta entonces,
// castellano. Los mensajes que arma el backend (el veto por chat, la serie)
// ya llegan traducidos.
internal static class Textos
{
    public const string Ok = "regie-lang ok";

    private static string _idioma = "es";

    private static readonly Dictionary<string, (string es, string en)> Tabla = new()
    {
        ["mejorDe"] = ("mejor de", "best of"),
        ["quedan"] = ("Quedan", "Left"),
        ["turno"] = ("Turno", "Turn"),
        ["turnoPaso"] = ("Ese turno ya pasó.", "That turn is over."),
        ["arrancaProduccion"] = (
            "El partido arranca cuando lo indique la producción. Pongan {g}.ready{d} igual: queda anotado.",
            "The match starts when the production says so. Type {g}.ready{d} anyway: it is noted."),
        ["estaListo"] = (
            "{g}{0}{d} está listo. El partido arranca cuando lo indique la producción.",
            "{g}{0}{d} is ready. The match starts when the production says so."),
        ["yaAnotado"] = (
            "Ya estás anotado como listo. El partido arranca cuando lo indique la producción.",
            "You are already marked as ready. The match starts when the production says so."),
    };

    /// <summary>El texto de la clave en el idioma actual; {0} se reemplaza por el argumento.</summary>
    public static string T(string clave, string? arg = null)
    {
        if (!Tabla.TryGetValue(clave, out var t)) return clave;
        var texto = _idioma == "en" ? t.en : t.es;
        return arg == null ? texto : texto.Replace("{0}", arg);
    }

    public static void Registrar(BasePlugin plugin)
    {
        plugin.AddCommand("css_regie_lang", "Idioma de Regie.gg para los mensajes del plugin: css_regie_lang <es|en>", (caller, cmd) =>
        {
            if (!ComandosRegie.Puede(caller, cmd, soloServidor: true)) return;
            var pedido = cmd.GetArg(1).Trim().ToLowerInvariant();
            if (pedido is "es" or "en") _idioma = pedido;
            cmd.ReplyToCommand($"{Ok} {_idioma}");
        });
    }
}
