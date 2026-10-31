using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;

namespace Cs2LiveViewerHelper;

// El recuadro HTML del centro de la pantalla (PrintToCenterHtml, y los
// CenterHtmlMenu) no se queda quieto en CS2:
//
// - Dura ~1 s aunque se le pida más (el parámetro de duración se ignora),
//   así que hay que reenviarlo en cada tick para que no desaparezca.
// - Aun reenviado, el juego le repite la animación de entrada una vez por
//   segundo, con el reloj de la ronda: pestañea.
//
// Lo segundo se arregla con el truco de Poggu (CS2FlashingHtmlHudFix):
// CCSGameRules.m_bGameRestart en true mientras no haya un reinicio
// programado. Y hay que AVISARLE AL MOTOR que lo mande a los clientes
// (SetStateChanged sobre cs_gamerules): sin eso el valor queda en el
// servidor, el cliente nunca lo recibe y sigue pestañeando. Medido el
// 20/10/2026 en un servidor real (CS2 1.41.9.1, CounterStrikeSharp 1.0.377):
// sin el aviso pestañeaba una vez por segundo, con el aviso quedó fijo.
//
// Para tocar las reglas del juego lo menos posible:
// - Solo mientras algo del plugin esté mostrando cosas en el centro (el
//   panel del veto, la publicidad): cada uno lo pide con su clave.
// - Solo si NO hay un reinicio programado (RestartRoundTime ya pasó). Con
//   uno pendiente (fin de ronda, mp_restartgame de MatchZy al ir en vivo,
//   cambio de lados) lo único que se hace es bajar a false lo que pusimos
//   nosotros, como el arreglo original; el resto lo maneja el juego.
// - Al dejar de necesitarlo, vuelve a false (su valor sin reinicio
//   pendiente), también avisado a los clientes.
internal static class CentroSinPestaneo
{
    private static readonly HashSet<string> Pedidos = new();
    private static CCSGameRulesProxy? _proxy;
    private static bool _loPusimos;
    private static bool _iniciado;

    /// <summary>Se llama una vez, en el Load del plugin.</summary>
    public static void Iniciar(BasePlugin plugin)
    {
        if (_iniciado) return;
        _iniciado = true;
        plugin.RegisterListener<Listeners.OnTick>(Tick);
        plugin.RegisterListener<Listeners.OnMapStart>(_ =>
        {
            // Las entidades del mapa anterior ya no existen.
            _proxy = null;
            _loPusimos = false;
        });
    }

    /// <summary>Al descargar el plugin: devuelve las reglas como estaban.</summary>
    public static void Detener()
    {
        Pedidos.Clear();
        Soltar();
        _iniciado = false;
    }

    /// <summary>Un módulo pide (o deja de pedir) el centro quieto.</summary>
    public static void Pedir(string quien, bool activo)
    {
        if (activo) Pedidos.Add(quien);
        else Pedidos.Remove(quien);
    }

    /// <summary>
    /// ¿Ese módulo está usando el centro ahora? La publicidad pregunta por
    /// "veto": el panel del veto le gana (el veto lo pide mientras haya panel).
    /// </summary>
    public static bool LoPide(string quien) => Pedidos.Contains(quien);

    private static void Tick()
    {
        try
        {
            if (Pedidos.Count == 0)
            {
                if (_loPusimos) Soltar();
                return;
            }
            var reglas = Reglas();
            if (reglas == null) return;
            if (reglas.RestartRoundTime >= Server.CurrentTime)
            {
                // Reinicio pendiente (fin de ronda, mp_restartgame): si lo
                // habíamos puesto en true, vuelve a false durante la espera,
                // igual que el arreglo original de Poggu. Lo que ponga el
                // juego mismo no se toca.
                if (_loPusimos && reglas.GameRestart)
                {
                    reglas.GameRestart = false;
                    Avisar();
                }
                _loPusimos = false;
                return;
            }
            if (reglas.GameRestart) return;
            reglas.GameRestart = true;
            Avisar();
            _loPusimos = true;
        }
        catch
        {
            // Nunca tirar el tick del servidor por esto: a lo sumo pestañea.
        }
    }

    private static void Soltar()
    {
        try
        {
            var reglas = Reglas();
            if (reglas != null && _loPusimos && reglas.RestartRoundTime < Server.CurrentTime && reglas.GameRestart)
            {
                reglas.GameRestart = false;
                Avisar();
            }
        }
        catch
        {
        }
        _loPusimos = false;
    }

    private static CCSGameRules? Reglas()
    {
        if (_proxy == null || !_proxy.IsValid)
        {
            _proxy = Utilities.FindAllEntitiesByDesignerName<CCSGameRulesProxy>("cs_gamerules").FirstOrDefault();
        }
        return _proxy?.GameRules;
    }

    private static void Avisar()
    {
        if (_proxy != null && _proxy.IsValid) Utilities.SetStateChanged(_proxy, "CCSGameRulesProxy", "m_pGameRules");
    }
}
