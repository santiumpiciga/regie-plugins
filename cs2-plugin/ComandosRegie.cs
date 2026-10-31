using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;

namespace Cs2LiveViewerHelper;

// Quién puede usar los comandos de consola del plugin (css_director_*,
// css_liststeamids, css_lv_events_status).
//
// CounterStrikeSharp deja que cualquier jugador los escriba en su propia
// consola: sin este control, en pleno torneo un jugador podía cambiar el
// camarógrafo o mover la cámara de la transmisión con css_director_spec.
// Los usa el backend por RCON (consola del servidor, caller == null), y
// además el camarógrafo autorizado (el SteamID64 de Producción), que
// puede manejar su propia cámara a mano. Cambiar QUIÉN es el camarógrafo
// queda solo para el servidor: si no, el autorizado podría pasarle el
// control a cualquiera.
internal static class ComandosRegie
{
    /// <summary>SteamID64 del camarógrafo autorizado (0 si no hay). Lo fija el plugin al cargar.</summary>
    public static Func<ulong>? Camarografo;

    public static bool Puede(CCSPlayerController? caller, CommandInfo cmd, bool soloServidor = false)
    {
        if (caller == null) return true;
        if (!soloServidor && caller.IsValid && !caller.IsBot)
        {
            var autorizado = Camarografo?.Invoke() ?? 0;
            if (autorizado != 0 && caller.SteamID == autorizado) return true;
        }
        cmd.ReplyToCommand(soloServidor
            ? "Este comando solo se puede usar desde la consola del servidor."
            : "Este comando solo lo puede usar el camarógrafo autorizado de Regie.gg.");
        return false;
    }
}
