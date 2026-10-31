// Gestor de partido Regie.gg (docs/plan-gestor-regie-cs16.md): lo que hace
// MatchZy en CS2, con nombres propios.
//   Etapa 1: cargar el partido, equipos fijos y whitelist, lados del veto,
//   calentamiento con .ready, en vivo, entretiempo, fin de mapa, paso al mapa
//   siguiente y fin de serie, con los eventos y las estadisticas que espera
//   el backend.
//   Etapa 5: el veto de mapas de MatchZy en el servidor: si el match config
//   trae mas mapas que la serie (y no dice skip_veto), es el pool y se veta
//   con .ban / .pick / .ct / .t, con veto_first, veto_mode y side_type como
//   en MatchZy, y los eventos map_vetoed / map_picked / side_picked.
//   Etapa 4: respaldos de ronda como los de MatchZy: al empezar cada ronda
//   se guarda data/regiegg/<puerto>/respaldos/regie_<matchid>_<mapa>_round<NN>.json
//   (marcador, lados, estadisticas y, por jugador, plata, armas, chaleco y
//   kit); regie_restore <N>, .stop (regie_stop_command_available),
//   regie_listbackups y regie_loadbackup, que retoma el partido despues de
//   que se cayo el servidor. Y una serie retomada mapa a mapa
//   ("_regie_serie" en el match config) dice el marcador de la serie entera.
//   Etapa 3: pausas como MatchZy (.pause / .tech con maximo y duracion,
//   .tac con el cupo de pausas tacticas, .unpause de los dos equipos,
//   regie_forcepause / regie_forceunpause del admin, la automatica cuando se
//   desconecta un jugador, regie_pause_on_disconnect) y el reporte de daño al
//   terminar cada ronda. La pausa congela el freezetime (no el servidor
//   entero, que en GoldSrc frena tambien los relojes de AMX Mod X).
//   Etapa 2: ronda de cuchillo y .stay / .switch, prorroga, el inicio a cargo
//   de la produccion (regie_hold, como css_regie_hold en CS2), cargar el
//   partido desde un archivo (regie_loadmatch, sin backend), el partido
//   informal (.ready sin partido cargado, como MatchZy) y, al reconectar a
//   mitad de mapa, los kills, las muertes y la plata que se tenian. Los
//   avisos del juego que MatchZy reemplaza ("Game Commencing"...) se tapan.
//   Sin partido y con regie_pug_enabled, calentamiento libre (warmup.cfg,
//   compra gratis) sin tocar mp_timelimit. Al terminar un partido vuelven
//   las cvars de antes de cargarlo (la foto "_regie_cvars" de partido.json).
//
// Como habla con el backend:
//   - RCON: regie_loadmatch_url "<url>", regie_loadmatch "<archivo>",
//     regie_status, regie_forceready, regie_restart, regie_endmatch,
//     regie_hold, regie_holdstatus (los mismos que MatchZy y que el plugin
//     de CS2, con otro nombre; ver COMANDOS_GESTOR en
//     backend/src/serverControl.ts y backend/src/partidoRegie.ts).
//   - Por la conexion del feed (la misma del HUD): pide el match config
//     ({"t":"cfg_req","url":...}, contesta "cfg <json>" o "cfgerr ...") y
//     manda los eventos con el cuerpo del webhook de MatchZy
//     ({"t":"gestor","seq":N,...}, contesta "gestorack <seq> <status>").
//     Ver backend/src/games/cs16/gestor.ts.
//
// AMX Mod X vuelve a cargar los plugins en cada cambio de mapa: el estado
// del partido se guarda en data/regiegg/<puerto>/partido.json y se retoma
// en plugin_cfg. Lo mismo la cola de eventos sin confirmar (cola.txt). Va
// por el puerto del juego (1.19.0): dos servidores lanzados desde la misma
// carpeta no se pisan el partido, la cola ni los respaldos.

#include <reapi>
#include <json>

#define GZ_MAX_MAPAS   7
#define GZ_MAX_POOL    16
#define GZ_NOMBRE      64
#define GZ_MAPA        32
#define GZ_LADO        12
#define GZ_SID         32
#define GZ_EVENTO      8192
#define GZ_COLA_MAX    64
#define GZ_REENVIO     5.0
#define GZ_TRADE       5.0
#define GZ_TOPE_DANIO  100
#define GZ_ASISTENCIA  40

// --------------------------------------------------------------------
// Idioma: el de Regie.gg (backend/src/idioma.ts), que manda el backend con
// regie_lang al cambiarlo, al cargar un partido y al empezar un veto. Se
// guarda en data/regiegg/idioma.txt para el proximo arranque. Cada mensaje
// esta en castellano y en ingles en GZ_TEXTOS; TX(n) es el del idioma de
// ahora. No se usa el sistema de idiomas de AMX Mod X: el amxx.cfg repone su
// amx_language en cada mapa, y asi los colores del chat (^4, ^3, ^1) quedan
// escritos igual que siempre.
// --------------------------------------------------------------------

new gz_idioma = 0 // 0 castellano, 1 ingles
#define TX(%1) GZ_TEXTOS[%1][gz_idioma]

// Largo fijo: con largos distintos, el compilador de AMX Mod X ubica mal las
// frases de una tabla de tres dimensiones (salian cortadas o de otra fila).
// Sin comentarios adentro: la fila n queda n + 1 lineas abajo de la que
// abre la tabla. Las filas nuevas van al final: 131 el recordatorio del
// informal (el de MatchZy), 132-144 el cartel fijo de arriba (hudmessage:
// sin tildes ni eñes, que el HUD del 1.6 no las dibuja), 145-150 .help,
// 151-157 la bienvenida, 158-162 los avisos de cada ronda en vivo, 167 el
// aviso del marcador en cero al terminar, 168 el espectador del partido al
// elegir equipo, 169 el cartel del informal con regie_minimum_ready_required
// y 170-192 la votacion de mapa (183-185 van al menu y al cartel: sin tildes)
// y 193-202 la pausa automatica por desconexion (199-201, el cartel: sin tildes).
new const GZ_TEXTOS[203][2][160] = {
	{ "Ya estás en tu equipo: ^4%s^1.", "You are already on your team: ^4%s^1." },
	{ "No estás en ningún equipo de esta partida: te quedas como espectador.", "You are not on any team in this match: you stay as a spectator." },
	{ "Te hemos movido a tu equipo: ^4%s^1.", "You have been moved to your team: ^4%s^1." },
	{ "Partida: ^4%s^1 vs ^4%s^1. Juegas con ^4%s^1.", "Match: ^4%s^1 vs ^4%s^1. You play for ^4%s^1." },
	{ "Cambiando a ^4%s^1.", "Changing to ^4%s^1." },
	{ "Se va a retomar el mapa %d (^4%s^1) desde la ronda ^4%d^1. Escribe ^4.ready^1 cuando estés listo.", "Map %d (^4%s^1) will resume from round ^4%d^1. Type ^4.ready^1 when you are ready." },
	{ "Calentamiento del mapa %d: ^4%s^1. Escribe ^4.ready^1 cuando estés listo.", "Warmup for map %d: ^4%s^1. Type ^4.ready^1 when you are ready." },
	{ "La partida empieza cuando lo indique la organización.", "The match starts when the organizers say so." },
	{ "No estás en ningún equipo de esta partida.", "You are not on any team in this match." },
	{ "^4%s^1 está preparado. La partida empieza cuando lo indique la organización.", "^4%s^1 is ready. The match starts when the organizers say so." },
	{ "Ya estás marcado como preparado. La partida empieza cuando lo indique la organización.", "You are already marked as ready. The match starts when the organizers say so." },
	{ "Has sido marcado como no preparado.", "You have been marked as not ready." },
	{ "Has sido marcado como preparado.", "You have been marked as ready." },
	{ "Únete a un equipo para jugar.", "Join a team to play." },
	{ "¡Todos preparados! Empieza la partida informal.", "Everyone is ready! The casual match is starting." },
	{ "Esperando a que se unan los jugadores: ^4%s^1 %d/%d, ^4%s^1 %d/%d.", "Waiting for players to join: ^4%s^1 %d/%d, ^4%s^1 %d/%d." },
	{ "Por favor, escribe ^4.ready^1 para estar preparado. Jugadores no preparados: %s", "Please type ^4.ready^1 to ready up! Unready players: %s" },
	{ "No se ha encontrado la copia de seguridad de la ronda %d: el mapa empieza de cero.", "No backup was found for round %d: the map starts from scratch." },
	{ "^4¡CUCHILLO!", "^4KNIFE!" },
	{ "^4%s^1 ganó la ronda de cuchillos. Esperando a que escriban ^4.stay^1 (mantener) o ^4.switch^1 (cambiar) el bando.", "^4%s^1 won the knife round. Waiting for them to type ^4.stay^1 or ^4.switch^1." },
	{ "Solo puede elegir bando el equipo que ganó la ronda de cuchillos: ^4%s^1.", "Only the team that won the knife round can pick the side: ^4%s^1." },
	{ "^4%s^1 ha decidido mantener bando.", "^4%s^1 has decided to stay." },
	{ "^4%s^1 ha decidido cambiar bando.", "^4%s^1 has decided to switch." },
	{ "Cambio de bando.", "Switching sides." },
	{ "La partida se ha pausado por la restauración de ronda. Ambos equipos deben escribir ^4.unpause^1 para reanudar la partida.", "The match has been paused because of the round restore. Both teams need to type ^4.unpause^1 to unpause the match." },
	{ "^4¡EN VIVO!", "^4LIVE!" },
	{ "La petición de ^4.stop^1 de ^4%s^1 se ha cancelado porque ha terminado la ronda.", "The ^4.stop^1 request from ^4%s^1 was canceled because the round ended." },
	{ "Empate ^4%d-%d^1: ¡prórroga %d!", "Tied ^4%d-%d^1: overtime %d!" },
	{ "^4%s^1 ha ganado el mapa ^4%d-%d^1.", "^4%s^1 has won the map ^4%d-%d^1." },
	{ "El mapa ha terminado en empate ^4%d-%d^1.", "The map ended in a ^4%d-%d^1 draw." },
	{ "Serie ^4%d-%d^1. El próximo mapa es ^4%s^1.", "Series ^4%d-%d^1. The next map is ^4%s^1." },
	{ "Un admin forzó la finalización de la partida.", "An admin force-ended the match." },
	{ "La serie está empatada ^4%d-%d^1. El próximo mapa lo carga Regie.gg.", "The series is tied ^4%d-%d^1. Regie.gg loads the next map." },
	{ "^4%s^1 va ganando la serie ^4%d-%d^1. El próximo mapa lo carga Regie.gg.", "^4%s^1 is leading the series ^4%d-%d^1. Regie.gg loads the next map." },
	{ "^4%s^1 y ^4%s^1 han empatado la serie ^4%d-%d^1.", "^4%s^1 and ^4%s^1 have tied the series ^4%d-%d^1." },
	{ "^4%s^1 ha ganado la serie ^4%d-%d^1.", "^4%s^1 has won the series ^4%d-%d^1." },
	{ "Selección de mapas (Mejor de %d). Escribe ^4.ready^1 cuando estés listo para empezar el veto.", "Map selection (Best of %d). Type ^4.ready^1 when you are ready to start the veto." },
	{ "La selección de mapas empieza en %d", "Map selection starts in %d" },
	{ "Capitán de ^4%s^1: ^4%s^1", "Captain of ^4%s^1: ^4%s^1" },
	{ "Nuevo capitán de ^4%s^1: ^4%s^1", "New captain of ^4%s^1: ^4%s^1" },
	{ "^4%s^1 tiene que elegir bando para jugar ^4%s^1.", "^4%s^1 must pick a side to play ^4%s^1." },
	{ "Usa ^4.ct^1 o ^4.t^1 para elegir bando.", "Use ^4.ct^1 or ^4.t^1 to pick a side." },
	{ "^4%s^1 tiene que ^4elegir^1 un mapa para jugar como mapa %d.", "^4%s^1 must ^4pick^1 a map to play as map %d." },
	{ "^4%s^1 tiene que ^3vetar^1 un mapa.", "^4%s^1 must ^3ban^1 a map." },
	{ "Mapas restantes: %s", "Remaining maps: %s" },
	{ "^4%s^1 no es un mapa válido o coincide con más de uno de los que quedan.", "^4%s^1 is not a valid map or matches more than one of the remaining maps." },
	{ "^4%s^1 ha elegido ^4%s^1 como mapa %d.", "^4%s^1 picked ^4%s^1 as map %d." },
	{ "^4%s^1 ha vetado ^3%s^1.", "^4%s^1 banned ^3%s^1." },
	{ "^4%s^1 ha elegido empezar como ^4%s^1 en ^4%s^1.", "^4%s^1 chose to start as ^4%s^1 on ^4%s^1." },
	{ "Un equipo se ha quedado sin capitán: la selección de mapas queda en pausa.", "A team has no captain left: the map selection is paused." },
	{ "Escribe ^4.ready^1 cuando estés listo para seguir con la selección de mapas.", "Type ^4.ready^1 when you are ready to continue the map selection." },
	{ "Ya están elegidos los mapas:", "The maps are set:" },
	{ "Mapa %d: ^4%s^1.", "Map %d: ^4%s^1." },
	{ "Se restaura el inicio de la ronda ^4%d^1 (^4%s^1 %d - %d ^4%s^1).", "Restoring the start of round ^4%d^1 (^4%s^1 %d - %d ^4%s^1)." },
	{ "Ambos equipos han pedido volver al inicio de la ronda.", "Both teams have asked to go back to the start of the round." },
	{ "No hay copia de seguridad de esta ronda.", "There is no backup for this round." },
	{ "^4%s^1 quiere restaurar la partida al principio de la ronda actual. ^4%s^1, por favor, escribe ^4.stop^1 para confirmar.", "^4%s^1 wants to restore the game to the beginning of the current round. ^4%s^1, please type ^4.stop^1 to confirm." },
	{ "¡^4Admin^1 empezó la partida!", "^4Admin^1 has started the game!" },
	{ "La partida empieza cuando lo indique la organización. Escribe ^4.ready^1 igualmente: queda anotado.", "The match starts when the organizers say so. Type ^4.ready^1 anyway: it is noted." },
	{ "No puedes ejecutar este comando una vez la partida ha finalizado.", "You cannot use this command after the game has ended." },
	{ "Las pausas técnicas no están activadas.", "Technical pauses are not enabled." },
	{ "¡^4%s^1 no tiene más pausas técnicas!", "^4%s^1 has no more tech pauses!" },
	{ "^4%s^1 ha pedido una pausa técnica (%d/%d). Ambos equipos deben escribir ^4.unpause^1 para reanudar la partida.", "^4%s^1 called for a technical pause (%d/%d). Both teams need to type ^4.unpause^1 to unpause the match." },
	{ "^4%s^1 ha pausado la partida. Ambos equipos deben escribir ^4.unpause^1 para reanudar la partida.", "^4%s^1 has paused the match. Both teams need to type ^4.unpause^1 to unpause the match." },
	{ "¡No te quedan pausas tácticas!", "You do not have any tactical timeouts left!" },
	{ "^4%s^1 ha pedido una pausa táctica (%d/%d).", "^4%s^1 called for a tactical timeout (%d/%d)." },
	{ "La pausa empezará al inicio de la próxima ronda.", "The pause will start at the beginning of the next round." },
	{ "Ha terminado la pausa táctica de ^4%s^1.", "The tactical timeout of ^4%s^1 is over." },
	{ "Se ha alcanzado la duración máxima de la pausa técnica. Ahora cualquiera puede usar ^4.unpause^1.", "The maximum technical pause length has been reached. Anyone may ^4.unpause^1 now." },
	{ "La partida fue pausada por un admin, por lo que solo puede ser reanudada por un admin.", "The match was paused by an admin, so only an admin can unpause it." },
	{ "^4%s^1 ha cancelado su petición de pausa.", "^4%s^1 canceled their pause request." },
	{ "^4%s^1 ha cancelado su pausa táctica.", "^4%s^1 canceled their tactical timeout." },
	{ "^4%s^1 ha reanudado la partida.", "^4%s^1 unpaused the match." },
	{ "Ambos equipos han pedido terminar la pausa, ¡reanudando la partida!", "Both teams have unpaused, resuming the match!" },
	{ "^4%s^1 quiere reanudar la partida. ^4%s^1, por favor, escribe ^4.unpause^1 para confirmar.", "^4%s^1 wants to unpause the match. ^4%s^1, please type ^4.unpause^1 to confirm." },
	{ "¡^4Admin^1 ha pausado la partida!", "^4Admin^1 has paused the match!" },
	{ "^4Admin^1 retiró la pausa, ¡reanudando la partida!", "^4Admin^1 has unpaused, resuming the match!" },
	{ "Tiempo muerto de %s: %d:%02d", "Tactical timeout for %s: %d:%02d" },
	{ "Partida pausada por un admin", "Match paused by an admin" },
	{ "Ronda restaurada. Esperando ^".unpause^" de los dos equipos.", "Round restored. Waiting for ^".unpause^" from both teams." },
	{ "Pausa de %s%s. Esperando a que se reanude la partida.", "Technical pause for %s%s. Awaiting unpause." },
	{ "Pausa de %s%s. Cualquiera puede reanudar en %d:%02d", "Technical pause for %s%s. Anyone can unpause in %d:%02d" },
	{ "[Regie.gg] Una partida ya ha sido creada con el id: %d, ¡no se puede cargar una nueva partida!", "[Regie.gg] A match is already setup with id: %d, cannot load a new match!" },
	{ "[Regie.gg] Uso: regie_loadmatch_url ^"<url>^"", "[Regie.gg] Usage: regie_loadmatch_url ^"<url>^"" },
	{ "[Regie.gg] Sin conexión con Regie.gg: no se puede pedir la partida.", "[Regie.gg] No connection to Regie.gg: the match cannot be requested." },
	{ "[Regie.gg] Pidiendo la partida a Regie.gg.", "[Regie.gg] Requesting the match from Regie.gg." },
	{ "[Regie.gg] Uso: regie_loadmatch ^"<archivo>^"", "[Regie.gg] Usage: regie_loadmatch ^"<file>^"" },
	{ "[Regie.gg] ¡Error al cargar la partida! La ruta debe ser relativa a cstrike/.", "[Regie.gg] Error loading the match! The path must be relative to cstrike/." },
	{ "[Regie.gg] ¡Error al cargar la partida! El fichero %s no existe.", "[Regie.gg] Error loading the match! The file %s does not exist." },
	{ "[Regie.gg] ¡Error al cargar la partida! %s no es JSON.", "[Regie.gg] Error loading the match! %s is not JSON." },
	{ "[Regie.gg] ¡Error al cargar la partida! La configuración de la partida no es JSON.", "[Regie.gg] Error loading the match! The match config is not JSON." },
	{ "[Regie.gg] ¡Error al cargar la partida! Faltan datos en la configuración de la partida.", "[Regie.gg] Error loading the match! The match config is missing required data." },
	{ "[Regie.gg] Partida %d cargada: %s vs %s (Mejor de %d).", "[Regie.gg] Match %d loaded: %s vs %s (Best of %d)." },
	{ "[Regie.gg] El servidor se ha reiniciado: retoma la partida desde una copia de seguridad de ronda (regie_listbackups / regie_loadbackup).", "[Regie.gg] The server has restarted: resume the match from a round backup (regie_listbackups / regie_loadbackup)." },
	{ "[Regie.gg] No hay una partida en vivo.", "[Regie.gg] There is no live match." },
	{ "[Regie.gg] Uso: regie_restore <ronda>", "[Regie.gg] Usage: regie_restore <round>" },
	{ "[Regie.gg] No existe la copia de seguridad de la ronda %d de este mapa (not found).", "[Regie.gg] There is no backup of round %d of this map (not found)." },
	{ "[Regie.gg] Ronda %d restaurada.", "[Regie.gg] Round %d restored." },
	{ "[Regie.gg] No hay copias de seguridad de esa partida (no backup files).", "[Regie.gg] There are no backups of that match (no backup files)." },
	{ "[Regie.gg] Uso: regie_loadbackup ^"regie_<matchid>_<mapa>_round<NN>.json^" (invalid).", "[Regie.gg] Usage: regie_loadbackup ^"regie_<matchid>_<map>_round<NN>.json^" (invalid)." },
	{ "[Regie.gg] El archivo de copia de seguridad %s no existe (not found).", "[Regie.gg] Backup file %s does not exist (not found)." },
	{ "[Regie.gg] La copia de seguridad %s no es JSON (invalid).", "[Regie.gg] Backup %s is not JSON (invalid)." },
	{ "[Regie.gg] La copia de seguridad %s no contiene una partida que se pueda cargar (invalid).", "[Regie.gg] Backup %s does not contain a match that can be loaded (invalid)." },
	{ "[Regie.gg] Copia de seguridad cargada correctamente: %s vs %s, mapa %d (%s), ronda %d.", "[Regie.gg] Backup file loaded successfully: %s vs %s, map %d (%s), round %d." },
	{ "[Regie.gg] Selección de mapas empezada.", "[Regie.gg] Map selection started." },
	{ "[Regie.gg] No hay un calentamiento en curso.", "[Regie.gg] There is no warmup in progress." },
	{ "[Regie.gg] Partida empezada.", "[Regie.gg] Match started." },
	{ "[Regie.gg] No hay una partida cargada.", "[Regie.gg] There is no match loaded." },
	{ "[Regie.gg] Vuelta al calentamiento.", "[Regie.gg] Back to warmup." },
	{ "[Regie.gg] ¡La partida ya se encuentra en pausa!", "[Regie.gg] The match is already paused!" },
	{ "[Regie.gg] Partida pausada.", "[Regie.gg] Match paused." },
	{ "[Regie.gg] La partida no está en pausa.", "[Regie.gg] The match is not paused." },
	{ "[Regie.gg] Partida reanudada.", "[Regie.gg] Match resumed." },
	{ "[Regie.gg] Partida terminada.", "[Regie.gg] Match ended." },
	{ "^4[Regie.gg]^1 %s (%s): %s, %s.", "^4[Regie.gg]^1 %s (%s): %s, %s." },
	{ "Usa ^4.pick <mapa>^1 para elegir un mapa.", "Use ^4.pick <map>^1 to pick a map." },
	{ "Usa ^4.ban <mapa>^1 para vetar un mapa.", "Use ^4.ban <map>^1 to ban a map." },
	{ "Ahora toca vetar: usa ^4.ban <mapa>^1.", "It is time to ban: use ^4.ban <map>^1." },
	{ "Ahora toca elegir: usa ^4.pick <mapa>^1.", "It is time to pick: use ^4.pick <map>^1." },
	{ "No puedes ejecutar este comando durante una pausa táctica.", "You cannot use this command during a tactical pause." },
	{ "¡La partida ya se encuentra en pausa!", "The match is already paused!" },
	{ "[Regie.gg] ¡Error al cargar la partida! %s", "[Regie.gg] Error loading the match! %s" },
	{ "No estás en la lista de jugadores de esta partida.", "You are not on this match's player list." },
	{ "%d de vida", "%d HP" },
	{ "muerto", "dead" },
	{ "le has hecho ^4%d^1 en %d golpe", "dealt ^4%d^1 in %d hit" },
	{ "le has hecho ^4%d^1 en %d golpes", "dealt ^4%d^1 in %d hits" },
	{ "le has hecho ^4%d^1", "dealt ^4%d^1" },
	{ "te ha hecho ^4%d^1 en %d golpe", "took ^4%d^1 in %d hit" },
	{ "te ha hecho ^4%d^1 en %d golpes", "took ^4%d^1 in %d hits" },
	{ "te ha hecho ^4%d^1", "took ^4%d^1" },
	{ "Jugadores no preparados: %s. Por favor, escribe ^4.ready^1 para estar preparado.", "Unready players: %s. Please type ^4.ready^1 to ready up!" },
	{ "CALENTAMIENTO", "WARMUP" },
	{ "Escribe .ready para estar preparado", "Type .ready to ready up" },
	{ "Preparados: %d/%d", "Ready: %d/%d" },
	{ "Empieza cuando lo indique el organizador", "Starts when the organizers say so" },
	{ "Con todos preparados empieza una partida informal", "When everyone is ready, a casual match starts" },
	{ "Escribe .ready para empezar el veto de mapas", "Type .ready to start the map veto" },
	{ "RONDA DE CUCHILLOS", "KNIFE ROUND" },
	{ "El equipo que gane elige bando", "The winning team picks the side" },
	{ "Esperando .stay o .switch de %s", "Waiting for .stay or .switch from %s" },
	{ "SELECCION DE MAPAS", "MAP VETO" },
	{ "Turno de %s: .ban <mapa>", "%s to ban: .ban <map>" },
	{ "Turno de %s: .pick <mapa>", "%s to pick: .pick <map>" },
	{ "Turno de %s: .ct o .t", "%s to pick a side: .ct or .t" },
	{ "Comandos disponibles:", "Available commands:" },
	{ "Preparado/No preparado: ^4.ready^1, ^4.unready^1", "Ready/Unready: ^4.ready^1, ^4.unready^1" },
	{ "Elección de bando: ^4.stay^1, ^4.switch^1, ^4.ct^1, ^4.t^1", "Side selection: ^4.stay^1, ^4.switch^1, ^4.ct^1, ^4.t^1" },
	{ "En vivo: %s", "Live: %s" },
	{ "Selección de mapas: ^4.ban <mapa>^1, ^4.pick <mapa>^1, ^4.ct^1, ^4.t^1", "Map veto: ^4.ban <map>^1, ^4.pick <map>^1, ^4.ct^1, ^4.t^1" },
	{ "Ahora no hay comandos de partida.", "There are no match commands right now." },
	{ "Bienvenido a ^4%s^1. Escribe ^4.ready^1 para jugar una partida informal. ^4.help^1: comandos.", "Welcome to ^4%s^1. Type ^4.ready^1 to play a casual match. ^4.help^1: commands." },
	{ "Bienvenido a ^4%s^1. ^4.help^1: comandos.", "Welcome to ^4%s^1. ^4.help^1: commands." },
	{ "Partida: ^4%s^1 vs ^4%s^1 en ^4%s^1.", "Match: ^4%s^1 vs ^4%s^1 on ^4%s^1." },
	{ "Partida: ^4%s^1 vs ^4%s^1, mapa %d de %d (^4%s^1). Serie ^4%d-%d^1.", "Match: ^4%s^1 vs ^4%s^1, map %d of %d (^4%s^1). Series ^4%d-%d^1." },
	{ "Partida: ^4%s^1 vs ^4%s^1 (Mejor de %d): selección de mapas.", "Match: ^4%s^1 vs ^4%s^1 (Best of %d): map selection." },
	{ "Juegas con ^4%s^1. ^4.help^1: comandos.", "You play for ^4%s^1. ^4.help^1: commands." },
	{ "No juegas en esta partida: eres espectador. ^4.help^1: comandos.", "You are not playing in this match: you are a spectator. ^4.help^1: commands." },
	{ "Ronda %d · ^4%s^1 %d - %d ^4%s^1", "Round %d · ^4%s^1 %d - %d ^4%s^1" },
	{ "Última ronda de la primera mitad.", "Last round of the first half." },
	{ "Última ronda de la primera mitad de la prórroga.", "Last round of the first half of overtime." },
	{ "Punto de mapa para ^4%s^1.", "Map point for ^4%s^1." },
	{ "Punto de partido para ^4%s^1.", "Match point for ^4%s^1." },
	{ "ULTIMA RONDA DE LA PRIMERA MITAD", "LAST ROUND OF THE FIRST HALF" },
	{ "ULTIMA RONDA DE LA MITAD DE LA PRORROGA", "LAST ROUND OF THE OVERTIME HALF" },
	{ "PUNTO DE MAPA^n%s", "MAP POINT^n%s" },
	{ "PUNTO DE PARTIDO^n%s", "MATCH POINT^n%s" },
	{ "La partida ha terminado: el marcador vuelve a cero para el calentamiento.", "The match is over: the score goes back to zero for the warmup." },
	{ "Eres espectador de esta partida.", "You are a spectator in this match." },
	{ "Con %d preparados empieza una partida informal", "With %d ready, a casual match starts" },
	{ "Votación de mapa: elige el próximo mapa en el menú (%d s).", "Map vote: pick the next map in the menu (%d s)." },
	{ "^4%s^1 ha votado por ^4%s^1.", "^4%s^1 voted for ^4%s^1." },
	{ "Ha ganado ^4%s^1 (votos: %d).", "^4%s^1 has won (votes: %d)." },
	{ "Empate a %d votos: se ha elegido ^4%s^1 al azar.", "Tied at %d votes: ^4%s^1 was picked at random." },
	{ "Nadie ha votado: el próximo mapa es ^4%s^1.", "Nobody voted: the next map is ^4%s^1." },
	{ "Se sigue jugando en ^4%s^1.", "Staying on ^4%s^1." },
	{ "^4%s^1 quiere votar el mapa (%d/%d). Escribe ^4.rtv^1 si tú también quieres.", "^4%s^1 wants a map vote (%d/%d). Type ^4.rtv^1 if you want one too." },
	{ "Ya has pedido la votación de mapa (%d/%d).", "You have already asked for a map vote (%d/%d)." },
	{ "No se puede votar el mapa con una partida en curso.", "The map cannot be voted during a match." },
	{ "Ya hay una votación de mapa en curso.", "A map vote is already in progress." },
	{ "La votación de mapa no está activada.", "Map voting is not enabled." },
	{ "No hay otros mapas en el mapcycle para votar.", "There are no other maps in the mapcycle to vote for." },
	{ "Votación de mapa: ^4.rtv^1, ^4.votemap^1", "Map vote: ^4.rtv^1, ^4.votemap^1" },
	{ "VOTACION DE MAPA", "MAP VOTE" },
	{ "Seguir en este mapa", "Stay on this map" },
	{ "Elige en el menu: quedan %d s", "Pick in the menu: %d s left" },
	{ "[Regie.gg] Votación de mapa abierta.", "[Regie.gg] Map vote opened." },
	{ "[Regie.gg] Hay una partida cargada: no se puede votar el mapa.", "[Regie.gg] A match is loaded: the map cannot be voted." },
	{ "[Regie.gg] Ya hay una votación de mapa en curso.", "[Regie.gg] A map vote is already in progress." },
	{ "[Regie.gg] No hay otros mapas en el mapcycle para votar.", "[Regie.gg] There are no other maps in the mapcycle to vote for." },
	{ "[Regie.gg] No hay jugadores conectados para votar.", "[Regie.gg] There are no players connected to vote." },
	{ "Se ha cancelado la votación de mapa: ha empezado una partida.", "The map vote was canceled: a match has started." },
	{ "^4%s^1 ha votado por seguir en este mapa.", "^4%s^1 voted to stay on this map." },
	{ "^4%s^1 se ha desconectado: pausa automática de hasta %d s si no vuelve.", "^4%s^1 disconnected: automatic pause of up to %d s if they do not come back." },
	{ "^4%s^1 se ha desconectado: pausa automática hasta que vuelva.", "^4%s^1 disconnected: automatic pause until they come back." },
	{ "^4%s^1 ha vuelto: se cancela la pausa automática.", "^4%s^1 is back: the automatic pause is canceled." },
	{ "^4%s^1 ha vuelto. Ambos equipos deben escribir ^4.unpause^1 para reanudar la partida.", "^4%s^1 is back. Both teams need to type ^4.unpause^1 to unpause the match." },
	{ "^4%s^1 ha vuelto, ¡reanudando la partida!", "^4%s^1 is back, resuming the match!" },
	{ "^4%s^1 no ha vuelto a tiempo, ¡reanudando la partida!", "^4%s^1 did not come back in time, resuming the match!" },
	{ "Pausa por la desconexion de %s. Se reanuda sola en %d:%02d", "Paused: %s disconnected. Resuming in %d:%02d" },
	{ "Pausa por la desconexion de %s. Esperando a que vuelva.", "Paused: %s disconnected. Waiting for them to come back." },
	{ "Pausa por desconexion. Esperando a que se reanude la partida.", "Disconnect pause. Awaiting unpause." },
	{ "^4%s^1 ha vuelto.", "^4%s^1 is back." }
}

#define GZ_TASK_COLA   7501
#define GZ_TASK_AVISO  7502
#define GZ_TASK_MAPA   7504
#define GZ_TASK_FIN    7505
#define GZ_TASK_PAUSA  7506
#define GZ_TASK_RESPALDO 7507
#define GZ_TASK_REPONER  7508
#define GZ_TASK_VETO     7509
#define GZ_TASK_VETOBOT  7510
#define GZ_TASK_CARTEL   7511
// El reporte de daño de fin de ronda, un instante despues (ver gz_fin_ronda).
#define GZ_TASK_REPORTE_RONDA 7512
// Las cvars de la foto que se devuelven un rato despues de terminar (ver
// gz_aplicar_foto).
#define GZ_TASK_PENDIENTES 7513
#define GZ_DEMORA_PENDIENTES 120.0
// Al terminar un partido, el calentamiento libre arranca sin reiniciar (no se
// corta el post partido) y GZ_DEMORA_MARCADOR segundos despues el marcador
// vuelve a cero con un sv_restartround (ver gz_task_marcador_a_cero). Lo mismo
// que tarda el cambio al mapa siguiente de una serie.
#define GZ_TASK_MARCADOR 7514
#define GZ_DEMORA_MARCADOR 15.0
// Votacion de mapa (sin partido cargado): abrirla al terminar un pug (un rato
// despues, para que se lea el resultado), cerrarla y el changelevel al
// ganador. Ver gz_votacion_fin_pug.
#define GZ_TASK_VOTO      7515
#define GZ_TASK_VOTO_FIN  7516
#define GZ_TASK_VOTO_MAPA 7517
#define GZ_DEMORA_VOTO    6.0
#define GZ_DEMORA_CAMBIO  5.0
// Por si el reinicio del cuchillo o de la salida en vivo nunca llega: el fin
// de ronda vuelve a valer (ver gz_fin_ronda_pre).
#define GZ_TASK_REINICIO  7518
// Mapas por votacion (mas "seguir en este mapa": entra en un menu sin paginas)
// y los que se leen del mapcycle.
#define GZ_VOTO_MAX  8
#define GZ_CICLO_MAX 64
// El cartel fijo de arriba se vuelve a mandar cada GZ_CARTEL_CADA segundos y
// dura un poco mas: queda fijo sin parpadear y, si se deja de mandar, se va solo.
#define GZ_CARTEL_CADA   1.0
// Cuantas lineas contesta regie_listbackups (las mas nuevas).
#define GZ_MAX_LISTA   40
#define GZ_TASK_EQUIPO 7600
// La bienvenida (3 s despues de entrar), una por jugador: 7650 + id.
#define GZ_TASK_SALUDO 7650
// El reporte de daño al morir (un instante despues), uno por jugador: 7700 + id.
#define GZ_TASK_REPORTE 7700
// Echar al que no es del partido (medio segundo despues), uno por jugador: 7750 + id.
#define GZ_TASK_ECHAR 7750
// La pausa automatica del que se desconecto (un instante despues: en un
// cambio de mapa AMX Mod X tambien llama a client_disconnected, y sus tareas
// no llegan a correr), una por jugador: 7800 + id.
#define GZ_TASK_DESCONEXION 7800
// Un comando del chat, un instante despues (ver gz_cmd_say), uno por jugador: 7850 + id.
#define GZ_TASK_SAY 7850

// La foto de las cvars de antes del partido (como
// matchzy_reset_cvars_on_series_end): estas, mas las "cvars" del match
// config. Va en partido.json ("_regie_cvars") y se devuelve al terminar.
new const GZ_CVARS_FOTO[][] = {
	"mp_round_infinite", "mp_forcerespawn", "mp_freezetime", "mp_roundtime",
	"mp_startmoney", "mp_buytime", "mp_buy_anywhere", "mp_give_player_c4",
	"mp_c4timer", "mp_maxrounds", "mp_winlimit", "mp_timelimit",
	"mp_friendlyfire", "mp_autoteambalance", "mp_limitteams", "mp_autokick",
	"mp_tkpunish", "sv_alltalk", "regie_hostname_format", "regie_whitelist_enabled_default"
}
// Las que terminan el mapa si se devuelven en el acto (el juego cuenta el
// tiempo y las rondas desde que se salio en vivo): van GZ_DEMORA_PENDIENTES
// segundos despues, para no cortar el post partido. Mientras tanto quedan en
// el localinfo "regie_<cvar>", que sobrevive a un cambio de mapa.
new const GZ_CVARS_DESPUES[][] = { "mp_timelimit", "mp_maxrounds", "mp_winlimit" }

// Lo que contesta regie_hold / regie_holdstatus, igual que el plugin de CS2
// (backend/src/partidoRegie.ts, parsearListos).
#define GZ_OK_PARTIDO  "regie-partido ok"

enum {
	GS_NONE = 0,
	GS_WARMUP,
	GS_KNIFE,
	GS_WAIT_KNIFE,
	GS_GOING_LIVE,
	GS_LIVE,
	GS_POST,
	// Etapa 5: antes del veto (esperando .ready) y durante el veto.
	GS_PREVETO,
	GS_VETO
}
new const GZ_ESTADOS[][] = { "none", "warmup", "knife", "waiting_for_knife_decision", "going_live", "live", "post_game", "pre_veto", "veto" }

// Estadisticas por jugador (mismo criterio que cs2-plugin/MatchEvents.cs).
enum {
	ST_KILLS = 0,
	ST_DEATHS,
	ST_ASSISTS,
	ST_DAMAGE,
	ST_HS,
	ST_MVP,
	ST_KAST,
	ST_ROUNDS,
	ST_TEAM,
	// 1.20.0: plantas y desactivadas (bomb_plants / bomb_defuses de MatchZy).
	// Van despues de ST_TEAM: los respaldos guardan el arreglo por posicion,
	// y uno de antes las deja en 0.
	ST_PLANTS,
	ST_DEFUSES,
	ST_TOTAL
}

new gz_estado = GS_NONE
new gz_matchid
new gz_nombre[3][GZ_NOMBRE]
new gz_numMaps
new gz_cantMapas
new gz_mapas[GZ_MAX_MAPAS][GZ_MAPA]
new gz_lados[GZ_MAX_MAPAS][GZ_LADO]
new gz_mapa
new gz_serie[3]
new gz_porEquipo = 5
// Con un partido cargado desde Regie.gg (no el informal), como MatchZy y
// Get5: se echa a quien no sea de un equipo ni espectador del partido.
new bool:gz_echarAjenos
new Trie:gz_plantel
// Los espectadores del partido ("spectators" del match config), por
// SteamID64: el camarografo de la transmision y los que agregue Regie.gg.
new Trie:gz_espect
new CsTeams:gz_ladoT1 = CS_TEAM_CT
new CsTeams:gz_ladoInicialT1 = CS_TEAM_CT
new gz_puntos[3]
new gz_ctT[3][2]
new gz_ronda
new bool:gz_mitadPendiente
new bool:gz_esperandoVivo
// Entre el sv_restartround del cuchillo o de la salida en vivo y el reinicio:
// la ronda que corre es la del calentamiento y no puede terminar.
new bool:gz_reinicioPendiente
new Float:gz_inicioRonda
new bool:gz_listo[33]
new gz_hostnameOriginal[128]
// Cuchillo: ya se jugo en este mapa (no se repite al salir en vivo) y quien
// lo gano (1 o 2), que elige lado con .stay / .switch.
new bool:gz_cuchilloHecho
new gz_ganadorCuchillo
// Inicio a cargo de la produccion: el .ready queda anotado y el partido
// arranca con regie_forceready. Dura toda la serie (va en partido.json).
new bool:gz_freno
// Prorroga: la plata de arranque y el inventario de fabrica van al empezar
// la ronda que sigue (gz_reinicio_post).
new bool:gz_plataProrroga
// Pausas (etapa 3). La pausa se pide en cualquier momento de la ronda y
// toma efecto en el freezetime: mientras dura, el freezetime no termina.
enum {
	PZ_NINGUNA = 0,
	PZ_TECNICA,
	PZ_TACTICA,
	PZ_ADMIN,
	// Despues de restaurar una ronda: la levantan los dos equipos (o el admin).
	PZ_RESTAURO,
	// Automatica, porque se desconecto un jugador de un equipo (1.17.0, ver
	// gz_desconectado): se levanta sola si no vuelve a tiempo.
	PZ_DESCONEXION
}
new gz_pausa = PZ_NINGUNA
new gz_pausaEquipo
// get_systime() de cuando tomo efecto (0: pedida, todavia no empezo).
new gz_pausaDesde
new bool:gz_pausaLibre
new bool:gz_quiereSeguir[3]
new gz_tecnicasUsadas[3]
new gz_tacticasUsadas[3]
// Los que se desconectaron de un equipo y todavia no volvieron (pausa
// automatica): SteamID64 (la clave de gz_steam64), nombre y equipo.
#define GZ_AUSENTES_MAX 10
new gz_ausSid[GZ_AUSENTES_MAX][GZ_SID]
new gz_ausNombre[GZ_AUSENTES_MAX][32]
new gz_ausEquipo[GZ_AUSENTES_MAX]
new gz_cantAus
// Lo del que se acaba de ir, para gz_task_desconexion (un instante despues).
new gz_descSid[33][GZ_SID]
new gz_descNombre[33][32]
new gz_descEquipo[33]
// Reporte de daño: golpes de cada uno a cada otro en la ronda.
new gz_golpes[33][33]
// Respaldos (etapa 4): restaurar despues del reinicio de ronda, la ronda a
// restaurar al salir en vivo (regie_loadbackup, -1 ninguna) y el .stop.
new bool:gz_restaurando
new gz_rondaPendiente = -1
new bool:gz_stop[3]
// Serie retomada mapa a mapa: lo que ya se jugo antes de esta carga y de
// cuantos mapas es la serie entera (0: la de esta carga).
new gz_seriePrevia[3]
new gz_numMapsSerie
// Veto (etapa 5), como MapVeto.cs de MatchZy.
new gz_pool[GZ_MAX_POOL][GZ_MAPA]
new gz_cantPool
new bool:gz_quedan[GZ_MAX_POOL]
new gz_orden[GZ_MAX_POOL][12]
new gz_cantOrden
new gz_tipoLados[16] = "standard"
new bool:gz_vetoPendiente
// Los mapas de la serie salieron de un veto (van en partido.json).
new bool:gz_vetoHecho
new gz_vetoCuenta
new gz_capitan[3]
// El equipo (1/2) del ultimo pick: el lado de ese mapa lo elige el otro.
new gz_ultimoEnElegir
new bool:gz_ladoPendiente
// Partido informal (.ready sin partido cargado): uno de un mapa, sin backend
// que lo haya pedido. Al terminar no queda nada.
new bool:gz_informal
// Calentamiento libre: sin partido y con regie_pug_enabled, warmup.cfg
// (rondas sin fin, reaparicion, plata, compra) hasta que se arme uno.
new bool:gz_libre
// El marcador a cero despues de un partido (gz_task_marcador_a_cero): el
// sv_restartround tambien pone en cero el reloj de mp_timelimit y las rondas
// jugadas, y se devuelven en gz_reinicio_post. Asi el mapa termina a la
// misma hora que sin el reinicio (cuando vuelve mp_timelimit, ver
// GZ_CVARS_DESPUES) y el servidor sigue con su lista de mapas.
new bool:gz_marcadorACero
new Float:gz_inicioJuego
new Float:gz_limiteTiempo
new gz_rondasJugadas
// La plata de los que se fueron a mitad de mapa, para devolversela si vuelven.
new Trie:gz_plata
// Votacion de mapa (sin partido cargado): al terminar un pug
// (regie_pug_mapvote), con .rtv de mas de la mitad o con regie_mapvote.
// VT_SIGUE y VT_CAMBIA quedan hasta el proximo pug o el proximo mapa: el panel
// los lee en regie_status ("mapvote") para no cambiar el mapa el tambien.
enum {
	VT_NADA = 0,
	VT_VOTANDO,
	VT_SIGUE,
	VT_CAMBIA
}
new const GZ_VOTO_ESTADOS[][] = { "none", "voting", "stay", "changing" }
new gz_voto = VT_NADA
new gz_votoMenu = -1
new gz_votoMapas[GZ_VOTO_MAX][GZ_MAPA]
new gz_votoCant
// El que sigue al actual en el mapcycle: el de siempre si nadie vota.
new gz_votoSiguiente[GZ_MAPA]
new gz_votoGanador[GZ_MAPA]
// Votos por opcion (la ultima, gz_votoCant, es seguir en este mapa) y lo que
// voto cada uno (-1: nada todavia).
new gz_votos[GZ_VOTO_MAX + 1]
new gz_votoDe[33]
new Float:gz_votoHasta
// Cuando se abre el menu (al terminar un pug, GZ_DEMORA_VOTO despues; 0 si ya
// esta abierto) y cuando sale el changelevel al ganador: para regie_status.
new Float:gz_votoAbreEn
new Float:gz_votoCambioEn
new bool:gz_rtv[33]
// Globales: dos listas de 64 mapas no entran comodas en la pila.
new gz_ciclo[GZ_CICLO_MAX][GZ_MAPA]
new gz_otros[GZ_CICLO_MAX][GZ_MAPA]

new Trie:gz_stats
new Trie:gz_nombres
new bool:gz_rJugo[33]
new bool:gz_rKill[33]
new bool:gz_rAsist[33]
new bool:gz_rMuerto[33]
new bool:gz_rTrade[33]
new gz_rKills[33]
new gz_rDanio[33]
new gz_danioTope[33][33]
new gz_danioReal[33][33]
// El reporte de daño (gz_reporte_danio) lleva su propia cuenta: los stats
// del partido solo cuentan en vivo y el reporte sale siempre (calentamiento,
// cuchillo, partida suelta). Solo daño entre rivales, por ronda (o por vida
// en el calentamiento, que no tiene fin de ronda). gz_repVisto: ya lo vio
// al morir, no se le repite al terminar la ronda.
new gz_repDanio[33][33]
new gz_repGolpes[33][33]
new bool:gz_repVisto[33]
new Float:gz_vidaAntes[33]
new gz_plantador
new gz_desactivador
// Ultimas muertes de la ronda, para los "traded" del KAST.
new gz_muerteAsesino[32]
new gz_muerteVictima[32]
new Float:gz_muerteCuando[32]
new gz_muertes

new Array:gz_cola
new Array:gz_colaSeq
// Cuando se mando cada evento por ultima vez: se reenvia recien a los
// GZ_REENVIO segundos (si no, la cola lo repetia antes de leer el ack).
new Array:gz_colaHora
new gz_seq
new gz_buf[GZ_EVENTO]

new gz_pcvarHostname
new gz_pcvarMaxRounds
new gz_pcvarAviso
new gz_pcvarBotsLibres
new gz_pcvarMinListos
new gz_pcvarCuchillo
new gz_pcvarInformal
new gz_pcvarProrroga
new gz_pcvarProrrogaRondas
new gz_pcvarProrrogaPlata
new gz_pcvarTecnica
new gz_pcvarTecnicaDuracion
new gz_pcvarTecnicaMax
new gz_pcvarPausaEsTactica
new gz_pcvarTacticaMax
new gz_pcvarTacticaDuracion
new gz_pcvarReporteDanio
new gz_pcvarStop
new gz_pcvarCartel
new gz_pcvarBienvenida
new gz_pcvarAvisosRonda
new gz_pcvarVotoPug
new gz_pcvarVotoTiempo
new gz_pcvarVotoMapas
new gz_pcvarRtv
new gz_pcvarPausaDesconexion
new gz_pcvarPausaDesconexionTiempo
new gz_pcvarPausaDesconexionVuelta

// El cartel fijo de arriba: un canal de HUD propio (sync object), para no
// pisarse con el veto por chat (g_vetoHudSync) ni con el de las pausas.
new gz_cartelSync
// Los anuncios grandes del centro (punto de mapa o de partido, ultima ronda
// de la mitad): un canal propio, para no pisarse con el cartel fijo ni con
// el de las pausas.
new gz_anuncioSync
new bool:gz_cartelPuesto
// La ultima ronda con avisos (marcador, punto de mapa...): uno por ronda.
new gz_rondaAvisada = -1

// --------------------------------------------------------------------
// Ciclo de vida (lo llama RegieGG.sma)
// --------------------------------------------------------------------

stock gestor_init() {
	gz_pcvarHostname = create_cvar("regie_hostname_format", "Regie.gg | {TEAM1} vs {TEAM2}", FCVAR_NONE, "Nombre del servidor durante el partido ({TEAM1}, {TEAM2})")
	gz_pcvarMaxRounds = create_cvar("regie_maxrounds", "30", FCVAR_NONE, "Rondas del mapa (MR15 = 30)")
	gz_pcvarAviso = create_cvar("regie_chat_messages_timer_delay", "13", FCVAR_NONE, "Cada cuantos segundos se recuerda quien falta .ready")
	gz_pcvarBotsLibres = create_cvar("regie_bots_libres", "0", FCVAR_NONE, "1: los bots juegan en cualquier equipo (para probar)")
	gz_pcvarMinListos = create_cvar("regie_minimum_ready_required", "0", FCVAR_NONE, "Jugadores listos para arrancar (0: todos los del plantel)")
	// Queda para que el backend y regie-gestor.cfg la puedan seguir poniendo.
	create_cvar("regie_whitelist_enabled_default", "0", FCVAR_NONE, "Sin efecto desde 1.12.0: con partido cargado (no informal) solo entran los equipos y los espectadores del partido")
	gz_pcvarCuchillo = create_cvar("regie_knife_enabled_default", "1", FCVAR_NONE, "1: el partido informal arranca con cuchillo")
	gz_pcvarInformal = create_cvar("regie_pug_enabled", "1", FCVAR_NONE, "1: sin partido cargado, .ready de todos arranca un partido informal")
	gz_pcvarProrroga = create_cvar("regie_overtime_enabled", "1", FCVAR_NONE, "1: empate al final del mapa va a prorroga")
	gz_pcvarProrrogaRondas = create_cvar("regie_overtime_maxrounds", "6", FCVAR_NONE, "Rondas de cada prorroga (MR3 = 6)")
	gz_pcvarProrrogaPlata = create_cvar("regie_overtime_startmoney", "10000", FCVAR_NONE, "Plata de arranque en la prorroga")
	gz_pcvarTecnica = create_cvar("regie_enable_tech_pause", "1", FCVAR_NONE, "1: .tech (y .pause) piden una pausa tecnica")
	gz_pcvarTecnicaDuracion = create_cvar("regie_tech_pause_duration", "300", FCVAR_NONE, "Segundos de pausa tecnica antes de que cualquiera pueda reanudar (-1: sin limite)")
	gz_pcvarTecnicaMax = create_cvar("regie_max_tech_pauses_allowed", "2", FCVAR_NONE, "Pausas tecnicas por equipo y por mapa (0: sin limite)")
	gz_pcvarPausaEsTactica = create_cvar("regie_use_pause_command_for_tactical_pause", "0", FCVAR_NONE, "1: .pause pide una pausa tactica en vez de una tecnica")
	gz_pcvarTacticaMax = create_cvar("regie_team_timeout_max", "3", FCVAR_NONE, "Pausas tacticas por equipo y por mapa")
	gz_pcvarTacticaDuracion = create_cvar("regie_team_timeout_time", "30", FCVAR_NONE, "Segundos de cada pausa tactica")
	gz_pcvarReporteDanio = create_cvar("regie_enable_damage_report", "1", FCVAR_NONE, "1: al terminar la ronda, el daño hecho y recibido con cada rival")
	gz_pcvarStop = create_cvar("regie_stop_command_available", "0", FCVAR_NONE, "1: .stop de los dos equipos vuelve al inicio de la ronda")
	gz_pcvarCartel = create_cvar("regie_warmup_hud", "1", FCVAR_NONE, "1: cartel fijo arriba con la fase (calentamiento, cuchillo, veto) y que escribir")
	gz_pcvarBienvenida = create_cvar("regie_welcome_enabled", "1", FCVAR_NONE, "1: bienvenida en el chat al entrar al servidor")
	gz_pcvarAvisosRonda = create_cvar("regie_round_messages", "1", FCVAR_NONE, "1: marcador al empezar cada ronda, ultima de la mitad y punto de mapa o de partida")
	gz_pcvarVotoPug = create_cvar("regie_pug_mapvote", "1", FCVAR_NONE, "1: al terminar un pug, votacion del mapa siguiente con los del mapcycle")
	gz_pcvarVotoTiempo = create_cvar("regie_mapvote_time", "20", FCVAR_NONE, "Segundos que dura la votacion de mapa")
	gz_pcvarVotoMapas = create_cvar("regie_mapvote_maps", "5", FCVAR_NONE, "Mapas en la votacion (1-8, mas seguir en este mapa)")
	gz_pcvarRtv = create_cvar("regie_rtv_enabled", "1", FCVAR_NONE, "1: sin partido, .rtv de mas de la mitad de los jugadores abre una votacion de mapa")
	gz_pcvarPausaDesconexion = create_cvar("regie_pause_on_disconnect", "1", FCVAR_NONE, "1: si se desconecta un jugador de un equipo, pausa automatica al empezar la ronda que sigue")
	gz_pcvarPausaDesconexionTiempo = create_cvar("regie_disconnect_pause_time", "120", FCVAR_NONE, "Segundos de la pausa automatica: si no vuelve, se reanuda sola (0: hasta que vuelva)")
	gz_pcvarPausaDesconexionVuelta = create_cvar("regie_disconnect_unpause_on_return", "0", FCVAR_NONE, "1: al volver el que se desconecto, la pausa automatica se levanta sola (0: .unpause de los dos equipos)")

	register_srvcmd("regie_loadmatch_url", "gz_cmd_cargar")
	register_srvcmd("regie_loadmatch", "gz_cmd_cargar_archivo")
	register_srvcmd("regie_status", "gz_cmd_estado")
	register_srvcmd("regie_forceready", "gz_cmd_forceready")
	register_srvcmd("regie_restart", "gz_cmd_restart")
	register_srvcmd("regie_endmatch", "gz_cmd_endmatch")
	register_srvcmd("regie_lang", "gz_cmd_lang")
	register_srvcmd("regie_restore", "gz_cmd_restore")
	register_srvcmd("regie_listbackups", "gz_cmd_listbackups")
	register_srvcmd("regie_loadbackup", "gz_cmd_loadbackup")
	register_srvcmd("regie_forcepause", "gz_cmd_forcepause")
	register_srvcmd("regie_forceunpause", "gz_cmd_forceunpause")
	register_srvcmd("regie_hold", "gz_cmd_hold")
	register_srvcmd("regie_holdstatus", "gz_cmd_holdstatus")
	register_srvcmd("regie_mapvote", "gz_cmd_mapvote")

	register_clcmd("say", "gz_cmd_say")
	register_clcmd("say_team", "gz_cmd_say")

	RegisterHookChain(RG_HandleMenu_ChooseTeam, "gz_elegir_equipo", false)
	RegisterHookChain(RG_RoundEnd, "gz_fin_ronda_pre", false)
	RegisterHookChain(RG_RoundEnd, "gz_fin_ronda", true)
	RegisterHookChain(RG_CSGameRules_RestartRound, "gz_reinicio_pre", false)
	RegisterHookChain(RG_CSGameRules_RestartRound, "gz_reinicio_post", true)
	RegisterHookChain(RG_CSGameRules_OnRoundFreezeEnd, "gz_fin_freeze_pre", false)
	RegisterHookChain(RG_CSGameRules_OnRoundFreezeEnd, "gz_fin_freeze", true)
	RegisterHookChain(RG_CBasePlayer_TakeDamage, "gz_danio_pre", false)
	RegisterHookChain(RG_CBasePlayer_TakeDamage, "gz_danio_post", true)
	RegisterHookChain(RG_CBasePlayer_Killed, "gz_muerte", true)
	// Cuchillo: solo cuchillo (ni compra, ni lo de arranque, ni lo del piso).
	RegisterHookChain(RG_CBasePlayer_HasRestrictItem, "gz_restringido", false)
	RegisterHookChain(RG_CBasePlayer_OnSpawnEquip, "gz_equipar", true)
	// Calentamiento: comprar no cuesta.
	RegisterHookChain(RG_CBasePlayer_AddAccount, "gz_plata_pre", false)
	// Los avisos del juego que MatchZy reemplaza.
	register_message(get_user_msgid("TextMsg"), "gz_textmsg")
	register_logevent("gz_log_plant", 3, "2=Planted_The_Bomb")
	register_logevent("gz_log_defuse", 3, "2=Defused_The_Bomb")

	gz_plantel = TrieCreate()
	gz_espect = TrieCreate()
	gz_stats = TrieCreate()
	gz_nombres = TrieCreate()
	gz_plata = TrieCreate()
	gz_cola = ArrayCreate(GZ_EVENTO)
	gz_colaSeq = ArrayCreate(1)
	gz_colaHora = ArrayCreate(1)
	// Crece siempre, tambien entre mapas (AMX Mod X recarga el plugin en
	// cada cambio y una cola guardada podria tener los numeros de antes).
	gz_seq = get_systime()
	set_task(GZ_REENVIO, "gz_task_cola", GZ_TASK_COLA, _, _, "b")
	gz_cartelSync = CreateHudSyncObj()
	gz_anuncioSync = CreateHudSyncObj()
	set_task(GZ_CARTEL_CADA, "gz_task_cartel", GZ_TASK_CARTEL, _, _, "b")
}

stock gestor_cfg() {
	gz_leer_idioma()
	gz_crear_cfgs()
	gz_migrar_datos()
	gz_leer_cola()
	// Un proceso nuevo arranca con las cvars de fabrica (y server.cfg): la
	// foto del calentamiento libre del proceso anterior ya no sirve.
	new proceso[4]
	get_localinfo("regie_proceso", proceso, charsmax(proceso))
	if (!proceso[0]) gz_borrar_archivo("calentamiento.json")
	gz_retomar()
	if (gz_estado == GS_NONE) {
		// Sin partido no queda nada del ultimo: lo que faltaba devolver (se
		// cambio de mapa antes de los GZ_DEMORA_PENDIENTES segundos) y la foto
		// de un partido.json que no se pudo retomar.
		gz_task_pendientes()
		gz_soltar_partido()
		// Con el informal apagado, servidor normal: si venia del calentamiento
		// libre, vuelve lo de antes de warmup.cfg. Con el informal, el
		// calentamiento libre arranca en gestor_configs_ejecutadas.
		if (get_pcvar_num(gz_pcvarInformal) != 1) gz_dejar_calentamiento_libre()
		// El recordatorio sirve para el informal (si alguien ya puso .ready);
		// con partido lo arranca el calentamiento.
		gz_poner_aviso()
	}
}

// OnConfigsExecuted (RegieGG.sma): ya corrieron server.cfg (con su hostname)
// y los cfg de AMX Mod X, que en cada mapa corren despues de plugin_cfg.
// Con partido, vuelve el nombre del partido y, en su calentamiento, warmup.cfg
// (por si server.cfg piso algo). Sin partido, el calentamiento libre: asi su
// foto tiene los valores de server.cfg y server.cfg no lo deshace.
stock gestor_configs_ejecutadas() {
	if (gz_estado == GS_NONE) {
		gz_calentamiento_libre(false)
		return
	}
	// RegieGG.sma acaba de volver a leer regie-gestor.cfg: las cvars del match
	// config le siguen ganando, como al cargarlo.
	gz_reaplicar_cvars_partido()
	gz_poner_hostname()
	if (gz_estado == GS_WARMUP || gz_estado == GS_PREVETO) gz_exec_calentamiento(true, true)
}

stock gz_poner_aviso() {
	remove_task(GZ_TASK_AVISO)
	set_task(float(max(5, get_pcvar_num(gz_pcvarAviso))), "gz_task_aviso", GZ_TASK_AVISO, _, _, "b")
}

stock gestor_end() {
	gz_guardar_cola()
}

// --------------------------------------------------------------------
// Archivos (data/regiegg y cfg de las fases)
// --------------------------------------------------------------------

// data/regiegg/: lo comun a todos los servidores de esta carpeta (idioma.txt)
// y donde dejaba todo un plugin anterior al 1.19.0.
stock gz_dir_base(path[], len) {
	get_localinfo("amxx_datadir", path, len)
	if (!path[0]) copy(path, len, "addons/amxmodx/data")
	add(path, len, "/regiegg")
	if (!dir_exists(path)) mkdir(path)
}

// data/regiegg/<puerto>/: lo de ESTE servidor (partido, cola, calentamiento
// y respaldos).
stock gz_dir_datos(path[], len) {
	gz_dir_base(path, len)
	new puerto[12]
	formatex(puerto, charsmax(puerto), "/%d", regie_puerto())
	add(path, len, puerto)
	if (!dir_exists(path)) mkdir(path)
}

stock gz_archivo(const nombre[], path[], len) {
	gz_dir_datos(path, len)
	add(path, len, "/")
	add(path, len, nombre)
}

stock gz_archivo_comun(const nombre[], path[], len) {
	gz_dir_base(path, len)
	add(path, len, "/")
	add(path, len, nombre)
}

// Lo que un plugin anterior al 1.19.0 dejo en data/regiegg/ y hay que
// retomar: un partido en curso y los eventos sin confirmar. Lo toma el
// primer servidor que arranca con este plugin (con uno solo, el de siempre),
// si todavia no tiene uno propio; se mueve, asi otro servidor no lo vuelve a
// tomar. calentamiento.json no: es de un proceso que ya no esta.
new const GZ_ARCHIVOS_VIEJOS[][] = { "partido.json", "cola.txt" }

stock gz_migrar_datos() {
	new viejo[256], nuevo[256]
	for (new i = 0; i < sizeof(GZ_ARCHIVOS_VIEJOS); i++) {
		gz_archivo_comun(GZ_ARCHIVOS_VIEJOS[i], viejo, charsmax(viejo))
		if (!file_exists(viejo)) continue
		gz_archivo(GZ_ARCHIVOS_VIEJOS[i], nuevo, charsmax(nuevo))
		if (file_exists(nuevo)) continue
		if (rename_file(viejo, nuevo, 1)) log_amx("Regie.gg: %s paso a %s", viejo, nuevo)
		else log_amx("Regie.gg: no se pudo mover %s a %s", viejo, nuevo)
	}
}

// cfg/regiegg/warmup.cfg y live.cfg, como cfg/MatchZy/ en CS2: se crean con
// valores de fabrica si no estan, y el panel los puede reescribir.
stock gz_crear_cfgs() {
	if (!dir_exists("cfg")) mkdir("cfg")
	if (!dir_exists("cfg/regiegg")) mkdir("cfg/regiegg")
	if (!file_exists("cfg/regiegg/warmup.cfg")) {
		write_file("cfg/regiegg/warmup.cfg", "// Regie.gg: calentamiento (se ejecuta al cargar el partido y en cada mapa)")
		write_file("cfg/regiegg/warmup.cfg", "mp_round_infinite 1")
		write_file("cfg/regiegg/warmup.cfg", "mp_forcerespawn 1")
		write_file("cfg/regiegg/warmup.cfg", "mp_freezetime 0")
		write_file("cfg/regiegg/warmup.cfg", "mp_startmoney 16000")
		write_file("cfg/regiegg/warmup.cfg", "mp_buytime 99")
		write_file("cfg/regiegg/warmup.cfg", "mp_buy_anywhere 1")
		write_file("cfg/regiegg/warmup.cfg", "mp_give_player_c4 0")
	}
	if (!file_exists("cfg/regiegg/knife.cfg")) {
		write_file("cfg/regiegg/knife.cfg", "// Regie.gg: ronda de cuchillo (solo cuchillo y chaleco con casco)")
		write_file("cfg/regiegg/knife.cfg", "mp_round_infinite 0")
		write_file("cfg/regiegg/knife.cfg", "mp_forcerespawn 0")
		write_file("cfg/regiegg/knife.cfg", "mp_freezetime 5")
		write_file("cfg/regiegg/knife.cfg", "mp_roundtime 1.92")
		write_file("cfg/regiegg/knife.cfg", "mp_buy_anywhere 0")
		write_file("cfg/regiegg/knife.cfg", "mp_give_player_c4 0")
		write_file("cfg/regiegg/knife.cfg", "mp_friendlyfire 0")
	}
	if (!file_exists("cfg/regiegg/live.cfg")) {
		write_file("cfg/regiegg/live.cfg", "// Regie.gg: en vivo (se ejecuta al salir en vivo)")
		write_file("cfg/regiegg/live.cfg", "mp_round_infinite 0")
		write_file("cfg/regiegg/live.cfg", "mp_forcerespawn 0")
		write_file("cfg/regiegg/live.cfg", "mp_freezetime 15")
		write_file("cfg/regiegg/live.cfg", "mp_roundtime 1.75")
		write_file("cfg/regiegg/live.cfg", "mp_startmoney 800")
		write_file("cfg/regiegg/live.cfg", "mp_buytime 0.25")
		write_file("cfg/regiegg/live.cfg", "mp_buy_anywhere 0")
		write_file("cfg/regiegg/live.cfg", "mp_give_player_c4 1")
		write_file("cfg/regiegg/live.cfg", "mp_c4timer 35")
		write_file("cfg/regiegg/live.cfg", "mp_maxrounds 0")
		write_file("cfg/regiegg/live.cfg", "mp_winlimit 0")
		write_file("cfg/regiegg/live.cfg", "mp_timelimit 0")
		write_file("cfg/regiegg/live.cfg", "mp_friendlyfire 1")
		write_file("cfg/regiegg/live.cfg", "mp_autoteambalance 0")
		write_file("cfg/regiegg/live.cfg", "mp_limitteams 0")
		write_file("cfg/regiegg/live.cfg", "mp_autokick 0")
		write_file("cfg/regiegg/live.cfg", "mp_tkpunish 0")
		write_file("cfg/regiegg/live.cfg", "sv_alltalk 0")
	}
}

stock gz_borrar_archivo(const nombre[]) {
	new path[256]
	gz_archivo(nombre, path, charsmax(path))
	if (file_exists(path)) delete_file(path)
}

// warmup.cfg. Con partido, ademas, sin limite de tiempo ni de rondas: que el
// mapa no se termine en el calentamiento (no se reescribe el .cfg, que el
// panel puede tener cambiado). Los dos van al buffer en orden: el exec mete
// el archivo adelante de lo que sigue, asi que el mp_timelimit 0 corre
// despues. ya: ejecutar el buffer en el acto (no adentro de un hook del juego).
stock gz_exec_calentamiento(bool:partido, bool:ya) {
	server_cmd("exec cfg/regiegg/warmup.cfg")
	if (partido) server_cmd("mp_timelimit 0;mp_maxrounds 0")
	if (ya) server_exec()
}

// En que calentamiento la compra es gratis y se aparece con toda la plata:
// el libre, el del partido, el de antes del veto y el veto (sigue el
// warmup.cfg del preveto) y mientras el del cuchillo elige lado.
stock bool:gz_en_calentamiento() {
	switch (gz_estado) {
		case GS_NONE: return gz_libre
		case GS_WARMUP, GS_PREVETO, GS_VETO, GS_WAIT_KNIFE: return true
	}
	return false
}

stock gz_plata_maxima() {
	static pcvar = -1
	if (pcvar == -1) pcvar = get_cvar_pointer("mp_maxmoney")
	new plata = pcvar ? get_pcvar_num(pcvar) : 0
	return plata > 0 ? plata : 16000
}

// Calentamiento libre (sin partido, con regie_pug_enabled): warmup.cfg sin
// tocar mp_timelimit, asi el servidor sigue su lista de mapas. La primera
// vez, la foto de lo que warmup.cfg cambia (calentamiento.json), para
// devolverlo si despues se apaga el informal o para la foto de un partido.
// plata: entra a mitad de mapa (al terminar un partido) y los que estan vivos
// no van a reaparecer: toda la plata ya.
stock gz_calentamiento_libre(bool:plata) {
	if (gz_estado != GS_NONE || get_pcvar_num(gz_pcvarInformal) != 1) return
	new path[256]
	gz_archivo("calentamiento.json", path, charsmax(path))
	if (!file_exists(path)) {
		new JSON:foto = json_init_object()
		gz_foto_de_cfg(foto, "cfg/regiegg/warmup.cfg")
		json_serial_to_file(foto, path)
		json_free(foto)
	}
	gz_libre = true
	gz_exec_calentamiento(false, false)
	if (!plata) return
	new jugadores[32], n
	get_players(jugadores, n, "a")
	for (new i = 0; i < n; i++) {
		new CsTeams:lado = CsTeams:get_member(jugadores[i], m_iTeam)
		if (lado == CS_TEAM_CT || lado == CS_TEAM_T) rg_add_account(jugadores[i], gz_plata_maxima(), AS_SET, false)
	}
}

// Con el informal apagado: vuelve lo de antes del calentamiento libre.
stock gz_dejar_calentamiento_libre() {
	gz_libre = false
	new path[256]
	gz_archivo("calentamiento.json", path, charsmax(path))
	if (!file_exists(path)) return
	new JSON:foto = json_parse(path, true)
	if (foto != Invalid_JSON) {
		gz_aplicar_foto(foto, false)
		json_free(foto)
	}
	delete_file(path)
}

// --------------------------------------------------------------------
// Foto de las cvars de antes del partido
// --------------------------------------------------------------------

stock gz_foto_agregar(JSON:foto, const nombre[]) {
	if (!gz_nombre_seguro(nombre) || !get_cvar_pointer(nombre)) return
	new valor[128]
	get_cvar_string(nombre, valor, charsmax(valor))
	json_object_set_string(foto, nombre, valor)
}

// Las cvars que pone un .cfg (la primera palabra de cada linea).
stock gz_foto_de_cfg(JSON:foto, const archivo[]) {
	new f = fopen(archivo, "rt")
	if (!f) return
	new linea[192], nombre[64]
	while (!feof(f)) {
		fgets(f, linea, charsmax(linea))
		trim(linea)
		if (!linea[0] || equal(linea, "//", 2)) continue
		parse(linea, nombre, charsmax(nombre))
		strtolower(nombre)
		if (!json_object_has_value(foto, nombre)) gz_foto_agregar(foto, nombre)
	}
	fclose(f)
}

// Antes de que el match config cambie nada: la foto, en el mismo cfg (va a
// partido.json con gz_guardar y sobrevive a los cambios de mapa de la serie).
// Un cfg que ya la trae (un respaldo de regie_loadbackup) la conserva. Lo que
// cambio el calentamiento libre y lo que todavia no se devolvio de un partido
// anterior entran con su valor de antes.
stock gz_sacar_foto(JSON:cfg) {
	if (json_object_has_value(cfg, "_regie_cvars")) return
	new JSON:foto = json_init_object()
	for (new i = 0; i < sizeof(GZ_CVARS_FOTO); i++) gz_foto_agregar(foto, GZ_CVARS_FOTO[i])
	new JSON:cvars = json_object_get_value(cfg, "cvars")
	if (cvars != Invalid_JSON) {
		new n = json_object_get_count(cvars)
		for (new i = 0; i < n; i++) {
			new nombre[64]
			json_object_get_name(cvars, i, nombre, charsmax(nombre))
			if (equal(nombre, "regie_", 6) || equal(nombre, "mp_", 3) || equal(nombre, "sv_", 3)) {
				if (!json_object_has_value(foto, nombre)) gz_foto_agregar(foto, nombre)
			}
		}
		json_free(cvars)
	}
	new path[256]
	gz_archivo("calentamiento.json", path, charsmax(path))
	if (file_exists(path)) {
		new JSON:previa = json_parse(path, true)
		if (previa != Invalid_JSON) {
			new n = json_object_get_count(previa)
			for (new i = 0; i < n; i++) {
				new nombre[64], valor[128]
				json_object_get_name(previa, i, nombre, charsmax(nombre))
				json_object_get_string(previa, nombre, valor, charsmax(valor))
				json_object_set_string(foto, nombre, valor)
			}
			json_free(previa)
		}
	}
	for (new i = 0; i < sizeof(GZ_CVARS_DESPUES); i++) {
		new clave[48], valor[64]
		formatex(clave, charsmax(clave), "regie_%s", GZ_CVARS_DESPUES[i])
		get_localinfo(clave, valor, charsmax(valor))
		if (valor[0]) json_object_set_string(foto, GZ_CVARS_DESPUES[i], valor)
	}
	gz_poner(cfg, "_regie_cvars", foto)
}

// Ya cargado un partido: lo pendiente de devolver del anterior queda en su foto.
stock gz_cancelar_pendientes() {
	remove_task(GZ_TASK_PENDIENTES)
	remove_task(GZ_TASK_MARCADOR)
	for (new i = 0; i < sizeof(GZ_CVARS_DESPUES); i++) {
		new clave[48]
		formatex(clave, charsmax(clave), "regie_%s", GZ_CVARS_DESPUES[i])
		set_localinfo(clave, "")
	}
}

stock bool:gz_cvar_despues(const nombre[]) {
	for (new i = 0; i < sizeof(GZ_CVARS_DESPUES); i++) if (equal(nombre, GZ_CVARS_DESPUES[i])) return true
	return false
}

// Devuelve las cvars de una foto. demorar: las de GZ_CVARS_DESPUES, un rato
// despues (al terminar el partido; al empezar un mapa van en el acto).
stock gz_aplicar_foto(JSON:foto, bool:demorar) {
	new bool:hay = false
	new n = json_object_get_count(foto)
	for (new i = 0; i < n; i++) {
		new nombre[64], valor[128]
		json_object_get_name(foto, i, nombre, charsmax(nombre))
		json_object_get_string(foto, nombre, valor, charsmax(valor))
		if (!gz_nombre_seguro(nombre) || contain(valor, "^"") != -1 || contain(valor, ";") != -1 || !get_cvar_pointer(nombre)) continue
		if (demorar && gz_cvar_despues(nombre) && valor[0] && contain(valor, "\") == -1) {
			new clave[48]
			formatex(clave, charsmax(clave), "regie_%s", nombre)
			set_localinfo(clave, valor)
			hay = true
			continue
		}
		set_cvar_string(nombre, valor)
	}
	if (hay) {
		remove_task(GZ_TASK_PENDIENTES)
		set_task(GZ_DEMORA_PENDIENTES, "gz_task_pendientes", GZ_TASK_PENDIENTES)
	}
}

// Lo que quedo pendiente de la foto (GZ_CVARS_DESPUES), si sigue sin partido.
public gz_task_pendientes() {
	remove_task(GZ_TASK_PENDIENTES)
	for (new i = 0; i < sizeof(GZ_CVARS_DESPUES); i++) {
		new clave[48], valor[64]
		formatex(clave, charsmax(clave), "regie_%s", GZ_CVARS_DESPUES[i])
		get_localinfo(clave, valor, charsmax(valor))
		if (!valor[0]) continue
		set_localinfo(clave, "")
		if (gz_estado == GS_NONE) set_cvar_string(GZ_CVARS_DESPUES[i], valor)
	}
}

// La foto de partido.json, si hay. demorar: como en gz_aplicar_foto.
stock gz_devolver_foto(bool:demorar) {
	new path[256]
	gz_archivo("partido.json", path, charsmax(path))
	if (!file_exists(path)) return
	new JSON:cfg = json_parse(path, true)
	if (cfg == Invalid_JSON) return
	new JSON:foto = json_object_get_value(cfg, "_regie_cvars")
	if (foto != Invalid_JSON) {
		gz_aplicar_foto(foto, demorar)
		json_free(foto)
	}
	json_free(cfg)
}

// Un mapa que arranca sin partido con un partido.json que no se pudo
// retomar: se devuelve su foto y se borra.
stock gz_soltar_partido() {
	new path[256]
	gz_archivo("partido.json", path, charsmax(path))
	if (!file_exists(path)) return
	gz_devolver_foto(false)
	delete_file(path)
}

// --------------------------------------------------------------------
// Mensajes: solo los de Regie.gg, con su prefijo
// --------------------------------------------------------------------

stock gz_decir(id, const fmt[], any:...) {
	new msg[256]
	vformat(msg, charsmax(msg), fmt, 3)
	client_print_color(id, print_team_default, "^4[Regie.gg]^1 %s", msg)
}

// --------------------------------------------------------------------
// SteamID64 (en string: no entra en una celda de 32 bits)
// --------------------------------------------------------------------

stock gz_sumar(const a[], const b[], out[], len) {
	new tmp[48], k = 0
	new i = strlen(a) - 1, j = strlen(b) - 1, carry = 0
	while (i >= 0 || j >= 0 || carry) {
		new d = carry
		if (i >= 0) d += a[i--] - '0'
		if (j >= 0) d += b[j--] - '0'
		tmp[k++] = '0' + (d % 10)
		carry = d / 10
		if (k >= charsmax(tmp)) break
	}
	new n = 0
	for (new x = k - 1; x >= 0 && n < len; x--) out[n++] = tmp[x]
	out[n] = 0
}

// "STEAM_0:1:17743821" -> "76561197995753371". Los bots: "BOT_<nombre>".
stock gz_steam64(id, out[], len) {
	new authid[40]
	get_user_authid(id, authid, charsmax(authid))
	if (is_user_bot(id) || !equal(authid, "STEAM_", 6)) {
		new nombre[32]
		get_user_name(id, nombre, charsmax(nombre))
		formatex(out, len, "BOT_%s", nombre)
		return
	}
	// authid[6] es el universo; authid[8] es X; desde authid[10], Y.
	new x[2]
	x[0] = authid[8]
	x[1] = 0
	new y[24]
	copy(y, charsmax(y), authid[10])
	new doble[32], cuenta[32]
	gz_sumar(y, y, doble, charsmax(doble))
	gz_sumar(doble, x, cuenta, charsmax(cuenta))
	gz_sumar(cuenta, "76561197960265728", out, len)
}

// --------------------------------------------------------------------
// Equipos: plantel, lado del mapa, whitelist
// --------------------------------------------------------------------

// "cs16:<nombre>": la clave con la que el backend sigue a un jugador sin
// cuenta de Steam (playerKeyFrom en backend/src/games/cs16/identity.ts):
// el nombre sin espacios de mas y en minusculas. Los planteles de 1.6 se
// arman con esa clave cuando el jugador se registro jugando sin Steam.
stock gz_clave_nombre(id, out[], len) {
	new nombre[32], limpio[32]
	get_user_name(id, nombre, charsmax(nombre))
	new n = 0, bool:espacio = false
	for (new i = 0; nombre[i] && n < charsmax(limpio); i++) {
		if (nombre[i] == ' ' || nombre[i] == '^t') {
			espacio = n > 0
			continue
		}
		if (espacio) {
			limpio[n++] = ' '
			espacio = false
			if (n >= charsmax(limpio)) break
		}
		limpio[n++] = nombre[i]
	}
	limpio[n] = 0
	strtolower(limpio)
	formatex(out, len, "cs16:%s", limpio)
}

// 1 o 2 si esta en el plantel de team1/team2, 0 si no. Primero por
// SteamID; si no aparece, por "cs16:<nombre>" (el plantel de un jugador
// que se registro sin Steam, aunque ahora juegue con Steam).
stock gz_equipo_de(id) {
	new sid[GZ_SID], equipo
	gz_steam64(id, sid, charsmax(sid))
	if (TrieGetCell(gz_plantel, sid, equipo)) return equipo
	new clave[48]
	gz_clave_nombre(id, clave, charsmax(clave))
	if (TrieGetCell(gz_plantel, clave, equipo)) return equipo
	return 0
}

stock CsTeams:gz_lado_de_equipo(equipo) {
	if (equipo == 1) return gz_ladoT1
	return gz_ladoT1 == CS_TEAM_CT ? CS_TEAM_T : CS_TEAM_CT
}

// El equipo (1/2) que esta de ese lado ahora.
stock gz_equipo_del_lado(CsTeams:lado) {
	return lado == gz_ladoT1 ? 1 : 2
}

stock bool:gz_bot_libre(id) {
	return is_user_bot(id) && get_pcvar_num(gz_pcvarBotsLibres) == 1
}

// Un bot que cuenta como listo y juega del lado en el que esta: los de
// regie_bots_libres y, en un pug, todos (al armarlo ya contaron como listos;
// sin esto, despues de un restart el pug con bots no volvia a arrancar).
stock bool:gz_bot_listo(id) {
	return gz_bot_libre(id) || (gz_informal && is_user_bot(id))
}

// Espectador del partido: por SteamID64, como en MatchZy. Sin Steam
// (STEAM_ID_LAN, VALVE_...) no hay SteamID y no puede serlo.
stock bool:gz_es_espectador(id) {
	new sid[GZ_SID]
	gz_steam64(id, sid, charsmax(sid))
	return TrieKeyExists(gz_espect, sid)
}

// Con un partido cargado desde Regie.gg (no el informal), quien no es de un
// equipo ni espectador del partido no puede quedarse, como en MatchZy. Los
// bots (de regie_bots_libres o no) y el HLTV no se echan.
stock bool:gz_es_ajeno(id) {
	if (gz_estado == GS_NONE || !gz_echarAjenos) return false
	if (is_user_bot(id) || is_user_hltv(id)) return false
	return !gz_equipo_de(id) && !gz_es_espectador(id)
}

// Un momento despues, para que le llegue el motivo y no se eche en medio de
// la conexion. Se vuelve a mirar: el puesto pudo cambiar de dueño.
stock gz_echar(id) {
	if (!task_exists(GZ_TASK_ECHAR + id)) set_task(0.5, "gz_task_echar", GZ_TASK_ECHAR + id)
}

public gz_task_echar(taskid) {
	new id = taskid - GZ_TASK_ECHAR
	if (!is_user_connected(id) || !gz_es_ajeno(id)) return
	server_cmd("kick #%d ^"%s^"", get_user_userid(id), TX(122))
}

// Al cargar el partido: los que ya estaban adentro, con el mismo criterio.
stock gz_echar_ajenos() {
	new maxPlayers = get_maxplayers()
	for (new id = 1; id <= maxPlayers; id++) {
		if (is_user_connected(id) && gz_es_ajeno(id)) gz_echar(id)
	}
}

// El menu de equipos (y jointeam, que pasa por el mismo HandleMenu_ChooseTeam),
// antes de que el juego cambie a nadie: el bando del otro equipo no se puede
// elegir, y "auto" o el bando ajeno lo mandan al suyo. Si ya esta en su lado,
// se frena ahi mismo. HandleMenu_ChooseTeam devuelve un BOOL de ReGameDLL (un
// int): su valor va con ATYPE_INTEGER. Con ATYPE_BOOL, SetHookChainReturn da
// un error de ejecucion que corta el hook, el juego hacia el cambio que se
// habia pedido y recien la vez siguiente lo devolvia a su lado (hasta 1.16.0).
public gz_elegir_equipo(const id, const MenuChooseTeam:slot) {
	if (gz_estado == GS_NONE || gz_bot_libre(id)) return HC_CONTINUE
	new equipo = gz_equipo_de(id)
	new MenuChooseTeam:quiere = MenuChoose_Spec
	if (equipo) quiere = gz_lado_de_equipo(equipo) == CS_TEAM_T ? MenuChoose_T : MenuChoose_CT
	if (slot == quiere) return HC_CONTINUE
	new CsTeams:actual = CsTeams:get_member(id, m_iTeam)
	new CsTeams:destino = quiere == MenuChoose_T ? CS_TEAM_T : quiere == MenuChoose_CT ? CS_TEAM_CT : CS_TEAM_SPECTATOR
	if (actual == destino) {
		if (equipo) gz_decir(id, TX(0), gz_nombre[equipo])
		else gz_decir(id, gz_es_espectador(id) ? TX(168) : TX(1))
		SetHookChainReturn(ATYPE_INTEGER, 0)
		return HC_SUPERCEDE
	}
	SetHookChainArg(2, ATYPE_INTEGER, quiere)
	if (equipo) gz_decir(id, TX(2), gz_nombre[equipo])
	else gz_decir(id, gz_es_espectador(id) ? TX(168) : TX(1))
	return HC_CONTINUE
}

// Al cargar el partido o empezar el mapa: cada uno a su lado.
stock gz_acomodar_todos() {
	new jugadores[32], n
	get_players(jugadores, n, "h")
	for (new i = 0; i < n; i++) gz_acomodar(jugadores[i])
}

stock gz_acomodar(id) {
	if (!is_user_connected(id) || gz_bot_libre(id)) return
	if (gz_es_ajeno(id)) {
		gz_echar(id)
		return
	}
	new equipo = gz_equipo_de(id)
	new CsTeams:destino = equipo ? gz_lado_de_equipo(equipo) : CS_TEAM_SPECTATOR
	if (CsTeams:get_member(id, m_iTeam) != destino) {
		rg_set_user_team(id, _:destino)
		if (destino != CS_TEAM_SPECTATOR && gz_estado == GS_WARMUP) rg_round_respawn(id)
	}
}

public client_putinserver(id) {
	// Para los eventos del partido del feed (regiegg_partido.inc).
	partido_entra(id)
	gz_listo[id] = false
	gz_rtv[id] = false
	gz_votoDe[id] = -1
	// El puesto se reusa: nada del reporte de daño del que estaba antes.
	remove_task(GZ_TASK_REPORTE + id)
	gz_rep_olvidar(id)
	if (gz_estado == GS_NONE) {
		gz_poner_saludo(id)
		return
	}
	if (gz_es_ajeno(id)) {
		gz_echar(id)
		return
	}
	gz_poner_saludo(id)
	set_task(1.0, "gz_task_bienvenida", GZ_TASK_EQUIPO + id)
	gz_volvio(id)
}

public gz_task_bienvenida(taskid) {
	new id = taskid - GZ_TASK_EQUIPO
	if (!is_user_connected(id)) return
	new equipo = gz_equipo_de(id)
	// Con la bienvenida activada, el equipo lo dice ella (gz_task_saludo).
	if (equipo && get_pcvar_num(gz_pcvarBienvenida) != 1) gz_decir(id, TX(3), gz_nombre[1], gz_nombre[2], gz_nombre[equipo])
	gz_reponer(id)
}

stock gz_poner_saludo(id) {
	if (is_user_bot(id) || is_user_hltv(id) || get_pcvar_num(gz_pcvarBienvenida) != 1) return
	remove_task(GZ_TASK_SALUDO + id)
	set_task(3.0, "gz_task_saludo", GZ_TASK_SALUDO + id)
}

// La bienvenida, solo al que entra. Sin partido: el nombre del servidor y
// como se arma el informal (si regie_pug_enabled). Con partido: el cruce,
// el mapa (y de cuantos, con la serie si es al mejor de mas de uno) y su
// equipo.
public gz_task_saludo(taskid) {
	new id = taskid - GZ_TASK_SALUDO
	if (!is_user_connected(id)) return
	if (gz_estado == GS_NONE) {
		new host[64]
		get_cvar_string("hostname", host, charsmax(host))
		gz_decir(id, get_pcvar_num(gz_pcvarInformal) == 1 ? TX(151) : TX(152), host)
		return
	}
	// Una serie retomada mapa a mapa llega como un Mejor de 1: se cuenta la
	// serie entera, como en gz_decir_serie.
	new total = gz_numMapsSerie > 0 ? gz_numMapsSerie : gz_numMaps
	new t1 = gz_seriePrevia[1] + gz_serie[1]
	new t2 = gz_seriePrevia[2] + gz_serie[2]
	if (gz_estado == GS_PREVETO || gz_estado == GS_VETO || !gz_cantMapas) gz_decir(id, TX(155), gz_nombre[1], gz_nombre[2], total)
	else if (total <= 1) gz_decir(id, TX(153), gz_nombre[1], gz_nombre[2], gz_mapas[gz_mapa])
	else gz_decir(id, TX(154), gz_nombre[1], gz_nombre[2], min(total, gz_seriePrevia[1] + gz_seriePrevia[2] + gz_mapa + 1), total, gz_mapas[gz_mapa], t1, t2)
	new equipo = gz_equipo_de(id)
	if (equipo) gz_decir(id, TX(156), gz_nombre[equipo])
	else gz_decir(id, TX(157))
}

// El que vuelve a mitad de mapa: en CS 1.6 entra con 0 kills, 0 muertes y la
// plata de arranque (CS2 se lo guarda). Se le devuelve lo que tenia: los
// kills y las muertes de la tabla salen de las estadisticas del partido, y
// la plata de lo que tenia al irse.
stock gz_reponer(id) {
	if (gz_estado != GS_LIVE || is_user_bot(id)) return
	new sid[GZ_SID], st[ST_TOTAL]
	gz_steam64(id, sid, charsmax(sid))
	if (TrieGetArray(gz_stats, sid, st, ST_TOTAL)) {
		set_entvar(id, var_frags, float(st[ST_KILLS]))
		set_member(id, m_iDeaths, st[ST_DEATHS])
		gz_mandar_puntaje(id)
	}
	new plata
	if (TrieGetCell(gz_plata, sid, plata)) {
		rg_add_account(id, plata, AS_SET, false)
		TrieDeleteKey(gz_plata, sid)
	}
}

// La fila del jugador en la tabla (TAB) de todos.
stock gz_mandar_puntaje(id) {
	static msgScoreInfo
	if (!msgScoreInfo) msgScoreInfo = get_user_msgid("ScoreInfo")
	message_begin(MSG_ALL, msgScoreInfo)
	write_byte(id)
	write_short(floatround(Float:get_entvar(id, var_frags)))
	write_short(get_member(id, m_iDeaths))
	write_short(0)
	write_short(get_member(id, m_iTeam))
	message_end()
}

public client_disconnected(id) {
	partido_sale(id)
	gz_listo[id] = false
	gz_rtv[id] = false
	remove_task(GZ_TASK_EQUIPO + id)
	remove_task(GZ_TASK_SALUDO + id)
	remove_task(GZ_TASK_REPORTE + id)
	gz_rep_olvidar(id)
	if (gz_estado == GS_LIVE && !is_user_bot(id)) {
		new sid[GZ_SID]
		gz_steam64(id, sid, charsmax(sid))
		TrieSetCell(gz_plata, sid, get_member(id, m_iAccount))
	}
	gz_anotar_desconexion(id)
}

// --------------------------------------------------------------------
// Pausa automatica por desconexion (1.17.0)
// --------------------------------------------------------------------
// Con regie_pause_on_disconnect, el que se va de un equipo en juego (pug,
// partido cargado a mano o de torneo; cuchillo o en vivo) pide una pausa
// como .tech: toma efecto en el freezetime de la ronda que sigue y no gasta
// las pausas tecnicas del equipo. Si vuelve, se avisa y la pausa espera el
// .unpause de los dos equipos (o se levanta sola con
// regie_disconnect_unpause_on_return). Si no vuelve en
// regie_disconnect_pause_time segundos de pausa, se levanta sola y se juega
// igual. Los dos equipos con .unpause (o el admin) la levantan antes. Los
// bots, el HLTV y los espectadores no cuentan. Para el feed es una pausa mas
// ("paused", gz_fase_feed).

stock bool:gz_fase_con_desconexion() {
	switch (gz_estado) {
		case GS_KNIFE, GS_WAIT_KNIFE, GS_GOING_LIVE, GS_LIVE: return true
	}
	return false
}

stock gz_anotar_desconexion(id) {
	remove_task(GZ_TASK_DESCONEXION + id)
	if (get_pcvar_num(gz_pcvarPausaDesconexion) != 1 || !gz_fase_con_desconexion()) return
	if (is_user_bot(id) || is_user_hltv(id)) return
	new CsTeams:lado = CsTeams:get_member(id, m_iTeam)
	if (lado != CS_TEAM_CT && lado != CS_TEAM_T) return
	new equipo = gz_equipo_que_juega(id)
	if (equipo != 1 && equipo != 2) return
	gz_steam64(id, gz_descSid[id], charsmax(gz_descSid[]))
	get_user_name(id, gz_descNombre[id], charsmax(gz_descNombre[]))
	gz_descEquipo[id] = equipo
	set_task(0.1, "gz_task_desconexion", GZ_TASK_DESCONEXION + id)
}

stock gz_ausente(const sid[]) {
	for (new i = 0; i < gz_cantAus; i++) if (equal(gz_ausSid[i], sid)) return i
	return -1
}

public gz_task_desconexion(taskid) {
	new id = taskid - GZ_TASK_DESCONEXION
	if (get_pcvar_num(gz_pcvarPausaDesconexion) != 1 || !gz_fase_con_desconexion()) return
	if (gz_ausente(gz_descSid[id]) == -1) {
		if (gz_cantAus >= GZ_AUSENTES_MAX) return
		copy(gz_ausSid[gz_cantAus], charsmax(gz_ausSid[]), gz_descSid[id])
		copy(gz_ausNombre[gz_cantAus], charsmax(gz_ausNombre[]), gz_descNombre[id])
		gz_ausEquipo[gz_cantAus] = gz_descEquipo[id]
		gz_cantAus++
	}
	new tiempo = get_pcvar_num(gz_pcvarPausaDesconexionTiempo)
	if (tiempo > 0) gz_decir(0, TX(193), gz_descNombre[id], tiempo)
	else gz_decir(0, TX(194), gz_descNombre[id])
	// En vivo y sin otra pausa: se pide ya (en el freezetime empieza en el
	// acto). Con otra pausa, o en el cuchillo, la pide gz_fin_freeze_pre al
	// empezar la ronda en vivo que sigue, si todavia no volvio.
	if (gz_estado == GS_LIVE && gz_pausa == PZ_NINGUNA) {
		gz_iniciar_pausa(PZ_DESCONEXION, gz_descEquipo[id])
		if (!get_member_game(m_bFreezePeriod)) gz_decir(0, TX(66))
	}
}

// El que vuelve (client_putinserver): sale de la lista y, si era el ultimo
// que faltaba, la pausa automatica se cancela (todavia no habia empezado),
// se levanta (regie_disconnect_unpause_on_return) o espera el .unpause.
stock gz_volvio(id) {
	if (!gz_cantAus || is_user_bot(id)) return
	new sid[GZ_SID]
	gz_steam64(id, sid, charsmax(sid))
	new i = gz_ausente(sid)
	if (i == -1) return
	new nombre[32]
	copy(nombre, charsmax(nombre), gz_ausNombre[i])
	gz_cantAus--
	for (new j = i; j < gz_cantAus; j++) {
		copy(gz_ausSid[j], charsmax(gz_ausSid[]), gz_ausSid[j + 1])
		copy(gz_ausNombre[j], charsmax(gz_ausNombre[]), gz_ausNombre[j + 1])
		gz_ausEquipo[j] = gz_ausEquipo[j + 1]
	}
	if (gz_pausa != PZ_DESCONEXION || gz_cantAus > 0) {
		gz_decir(0, TX(202), nombre)
		// La pausa queda a nombre de uno que sigue afuera.
		if (gz_pausa == PZ_DESCONEXION) gz_pausaEquipo = gz_ausEquipo[0]
		return
	}
	if (!gz_pausaDesde) {
		gz_decir(0, TX(195), nombre)
		gz_terminar_pausa()
		return
	}
	if (get_pcvar_num(gz_pcvarPausaDesconexionVuelta) == 1) {
		gz_decir(0, TX(197), nombre)
		gz_terminar_pausa()
		return
	}
	gz_quiereSeguir[1] = false
	gz_quiereSeguir[2] = false
	gz_decir(0, TX(196), nombre)
	gz_task_pausa()
}

// --------------------------------------------------------------------
// Cargar el partido (por el feed)
// --------------------------------------------------------------------

public gz_cmd_cargar() {
	new url[512]
	read_argv(1, url, charsmax(url))
	if (gz_estado != GS_NONE) {
		server_print(TX(82), gz_matchid)
		return PLUGIN_HANDLED
	}
	if (!url[0]) {
		server_print(TX(83))
		return PLUGIN_HANDLED
	}
	if (g_socket == -1 || !g_helloSent) {
		server_print(TX(84))
		return PLUGIN_HANDLED
	}
	new JSON:pedido = json_init_object()
	json_object_set_string(pedido, "t", "cfg_req")
	json_object_set_string(pedido, "url", url)
	json_serial_to_string(pedido, gz_buf, charsmax(gz_buf))
	json_free(pedido)
	add(gz_buf, charsmax(gz_buf), "^n")
	socket_send(g_socket, gz_buf, strlen(gz_buf))
	server_print(TX(85))
	return PLUGIN_HANDLED
}

// regie_loadmatch "<archivo>": el mismo match config, desde un archivo del
// servidor (relativo a cstrike/, como matchzy_loadmatch en CS2). Anda sin
// backend: los eventos quedan en la cola hasta que haya conexion.
public gz_cmd_cargar_archivo() {
	new archivo[256]
	read_argv(1, archivo, charsmax(archivo))
	if (gz_estado != GS_NONE) {
		server_print(TX(82), gz_matchid)
		return PLUGIN_HANDLED
	}
	trim(archivo)
	if (!archivo[0]) {
		server_print(TX(86))
		return PLUGIN_HANDLED
	}
	// Solo dentro de cstrike/: nada de rutas absolutas ni de subir carpetas.
	if (contain(archivo, "..") != -1 || contain(archivo, ":") != -1 || archivo[0] == '/' || archivo[0] == 92) {
		server_print(TX(87))
		return PLUGIN_HANDLED
	}
	if (!file_exists(archivo)) {
		server_print(TX(88), archivo)
		return PLUGIN_HANDLED
	}
	new JSON:cfg = json_parse(archivo, true)
	if (cfg == Invalid_JSON) {
		server_print(TX(89), archivo)
		return PLUGIN_HANDLED
	}
	gz_cargar_config(cfg)
	return PLUGIN_HANDLED
}

// "cfg <json>" del backend.
stock gz_al_recibir_config(const json[]) {
	if (gz_estado != GS_NONE) return
	new JSON:cfg = json_parse(json)
	if (cfg == Invalid_JSON) {
		server_print(TX(90))
		return
	}
	gz_cargar_config(cfg)
}

// Carga un match config ya leido (por el feed, de un archivo o el informal)
// y lo libera.
stock gz_cargar_config(JSON:cfg, bool:informal = false) {
	// Antes de que el match config cambie nada (gz_leer_config aplica sus
	// "cvars"): la foto, para devolver todo al terminar.
	gz_sacar_foto(cfg)
	if (!gz_leer_config(cfg)) {
		json_free(cfg)
		server_print(TX(91))
		return
	}
	gz_cancelar_pendientes()
	gz_cancelar_votacion(true)
	gz_libre = false
	gz_mapa = 0
	gz_serie[1] = 0
	gz_serie[2] = 0
	gz_informal = informal
	// El nombre de antes del partido, para devolverlo al terminar: se lee
	// antes de guardar (va en partido.json) y antes de cambiarlo.
	get_cvar_string("hostname", gz_hostnameOriginal, charsmax(gz_hostnameOriginal))
	gz_guardar(cfg)
	json_free(cfg)
	gz_poner_hostname()
	server_print(TX(92), gz_matchid, gz_nombre[1], gz_nombre[2], gz_numMaps)
	gz_echar_ajenos()

	new JSON:ev = json_init_object()
	json_object_set_number(ev, "num_maps", gz_numMaps)
	gz_poner(ev, "team1", gz_equipo_corto(1))
	gz_poner(ev, "team2", gz_equipo_corto(2))
	// El informal no tiene veto y arranca ya: el backend no lo deja "en veto"
	// (el panel mostraba un veto al ponerse todos .ready).
	if (informal) json_object_set_bool(ev, "regie_informal", true)
	gz_enviar(ev, "series_start")

	// El informal arranca ya: todos pusieron .ready para crearlo.
	if (informal) {
		gz_empezar_calentamiento(true)
		gz_salir_en_vivo()
		return
	}
	gz_ir_al_mapa_o_calentar()
	gz_reiniciar_si_queda()
}

// Un partido recien cargado que se calienta en este mapa (sin cambiarlo):
// todos arrancan de nuevo, con la plata del calentamiento.
stock gz_reiniciar_si_queda() {
	if (!task_exists(GZ_TASK_MAPA)) server_cmd("sv_restartround 1")
}

// json_object_set_value copia el valor: el original se libera aca.
stock gz_poner(JSON:o, const clave[], JSON:valor) {
	json_object_set_value(o, clave, valor)
	json_free(valor)
}

stock JSON:gz_equipo_corto(equipo) {
	new JSON:t = json_init_object()
	json_object_set_string(t, "id", equipo == 1 ? "team1" : "team2")
	json_object_set_string(t, "name", gz_nombre[equipo])
	return t
}

stock bool:gz_leer_config(JSON:cfg) {
	gz_matchid = json_object_get_number(cfg, "matchid")
	if (gz_matchid <= 0) return false
	TrieClear(gz_plantel)
	for (new e = 1; e <= 2; e++) {
		new JSON:equipo = json_object_get_value(cfg, e == 1 ? "team1" : "team2")
		if (equipo == Invalid_JSON) return false
		json_object_get_string(equipo, "name", gz_nombre[e], charsmax(gz_nombre[]))
		new JSON:jugadores = json_object_get_value(equipo, "players")
		if (jugadores != Invalid_JSON) {
			new n = json_object_get_count(jugadores)
			for (new i = 0; i < n; i++) {
				// 48: entra "cs16:" + un nombre de 1.6 entero (31).
				new sid[48]
				json_object_get_name(jugadores, i, sid, charsmax(sid))
				TrieSetCell(gz_plantel, sid, e)
			}
			json_free(jugadores)
		}
		json_free(equipo)
	}
	gz_numMaps = max(1, json_object_get_number(cfg, "num_maps"))
	// Despues de un veto, los mapas que salieron (los guarda gz_guardar).
	gz_vetoHecho = json_object_has_value(cfg, "_regie_maplist")
	new JSON:mapas = json_object_get_value(cfg, gz_vetoHecho ? "_regie_maplist" : "maplist")
	if (mapas == Invalid_JSON) return false
	gz_cantPool = min(GZ_MAX_POOL, json_array_get_count(mapas))
	for (new i = 0; i < gz_cantPool; i++) json_array_get_string(mapas, i, gz_pool[i], charsmax(gz_pool[]))
	json_free(mapas)
	if (gz_cantPool == 0) return false
	gz_tipoLados = "standard"
	if (json_object_has_value(cfg, "side_type")) json_object_get_string(cfg, "side_type", gz_tipoLados, charsmax(gz_tipoLados))
	strtolower(gz_tipoLados)
	// Como MatchZy (y Get5): un pool mas grande que la serie se veta, salvo
	// "skip_veto": true; con skip_veto se juegan los primeros num_maps.
	new bool:saltear = gz_vetoHecho || gz_cantPool <= gz_numMaps || (json_object_has_value(cfg, "skip_veto") && json_object_get_bool(cfg, "skip_veto"))
	gz_vetoPendiente = !saltear
	gz_ladoPendiente = false
	gz_capitan[1] = 0
	gz_capitan[2] = 0
	if (saltear) {
		gz_cantMapas = min(GZ_MAX_MAPAS, gz_vetoHecho ? gz_cantPool : min(gz_cantPool, gz_numMaps))
		new JSON:lados = json_object_get_value(cfg, gz_vetoHecho ? "_regie_mapsides" : "map_sides")
		for (new i = 0; i < gz_cantMapas; i++) {
			copy(gz_mapas[i], charsmax(gz_mapas[]), gz_pool[i])
			gz_lados[i][0] = 0
			if (lados != Invalid_JSON && i < json_array_get_count(lados)) json_array_get_string(lados, i, gz_lados[i], charsmax(gz_lados[]))
			if (!gz_lados[i][0]) gz_lado_automatico(gz_lados[i], charsmax(gz_lados[]))
		}
		if (lados != Invalid_JSON) json_free(lados)
	} else {
		gz_cantMapas = 0
		for (new i = 0; i < gz_cantPool; i++) gz_quedan[i] = true
		gz_armar_orden(cfg)
	}
	gz_porEquipo = json_object_has_value(cfg, "players_per_team") ? max(1, json_object_get_number(cfg, "players_per_team")) : 5
	// Serie retomada (tournament/continuarSerie.ts): cada mapa que falta llega
	// como un Mejor de 1, con lo que ya se jugo.
	gz_seriePrevia[1] = 0
	gz_seriePrevia[2] = 0
	gz_numMapsSerie = 0
	new JSON:previa = json_object_get_value(cfg, "_regie_serie")
	if (previa != Invalid_JSON) {
		gz_seriePrevia[1] = json_object_get_number(previa, "team1")
		gz_seriePrevia[2] = json_object_get_number(previa, "team2")
		gz_numMapsSerie = json_object_get_number(previa, "num_maps")
		json_free(previa)
	}
	gz_aplicar_cvars(cfg)
	// Espectadores del partido, como MatchZy: "spectators": {"players":
	// {SteamID64: nombre}}. Van en partido.json con el resto del config.
	TrieClear(gz_espect)
	new JSON:espect = json_object_get_value(cfg, "spectators")
	if (espect != Invalid_JSON) {
		new JSON:quienes = json_object_get_value(espect, "players")
		if (quienes != Invalid_JSON) {
			new n = json_object_get_count(quienes)
			for (new i = 0; i < n; i++) {
				new sid[48]
				json_object_get_name(quienes, i, sid, charsmax(sid))
				TrieSetCell(gz_espect, sid, 1)
			}
			json_free(quienes)
		}
		json_free(espect)
	}
	// Con partido cargado se echa al que no es del partido (la whitelist de
	// antes, ahora siempre). El informal es de los que estaban: no se echa
	// a nadie.
	gz_echarAjenos = !(json_object_has_value(cfg, "_regie_informal") && json_object_get_bool(cfg, "_regie_informal"))
	gz_estado = GS_WARMUP
	return true
}

// Las "cvars" del match config, como MatchZy: las regie_* (nombre del
// servidor, whitelist...) y reglas mp_/sv_. Solo nombres y valores simples:
// nada que pueda colar otro comando en la consola.
stock gz_aplicar_cvars(JSON:cfg) {
	new JSON:cvars = json_object_get_value(cfg, "cvars")
	if (cvars == Invalid_JSON) return
	new n = json_object_get_count(cvars)
	for (new i = 0; i < n; i++) {
		new nombre[64], valor[128]
		json_object_get_name(cvars, i, nombre, charsmax(nombre))
		json_object_get_string(cvars, nombre, valor, charsmax(valor))
		if (!gz_nombre_seguro(nombre) || contain(valor, "^"") != -1 || contain(valor, ";") != -1) continue
		if (equal(nombre, "regie_", 6) || equal(nombre, "mp_", 3) || equal(nombre, "sv_", 3)) {
			if (get_cvar_pointer(nombre)) set_cvar_string(nombre, valor)
		}
	}
	json_free(cvars)
}

// Las "cvars" del match config del partido en curso (partido.json), otra vez.
stock gz_reaplicar_cvars_partido() {
	new path[256]
	gz_archivo("partido.json", path, charsmax(path))
	if (!file_exists(path)) return
	new JSON:cfg = json_parse(path, true)
	if (cfg == Invalid_JSON) return
	gz_aplicar_cvars(cfg)
	json_free(cfg)
}

stock bool:gz_nombre_seguro(const nombre[]) {
	for (new i = 0; nombre[i]; i++) {
		new c = nombre[i]
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false
	}
	return nombre[0] != 0
}

// partido.json: el match config, mas el mapa en juego y la serie.
stock gz_guardar(JSON:cfg) {
	json_object_set_number(cfg, "_regie_mapa", gz_mapa)
	json_object_set_number(cfg, "_regie_serie1", gz_serie[1])
	json_object_set_number(cfg, "_regie_serie2", gz_serie[2])
	json_object_set_string(cfg, "_regie_hostname", gz_hostnameOriginal)
	json_object_set_bool(cfg, "_regie_freno", gz_freno)
	json_object_set_bool(cfg, "_regie_informal", gz_informal)
	json_object_set_number(cfg, "_regie_restaurar", gz_rondaPendiente)
	if (gz_vetoHecho) {
		new JSON:mapas = json_init_array()
		new JSON:lados = json_init_array()
		for (new i = 0; i < gz_cantMapas; i++) {
			json_array_append_string(mapas, gz_mapas[i])
			json_array_append_string(lados, gz_lados[i])
		}
		json_object_set_value(cfg, "_regie_maplist", mapas)
		json_object_set_value(cfg, "_regie_mapsides", lados)
		json_free(mapas)
		json_free(lados)
	}
	new path[256]
	gz_archivo("partido.json", path, charsmax(path))
	json_serial_to_file(cfg, path)
}

stock gz_guardar_estado() {
	new path[256]
	gz_archivo("partido.json", path, charsmax(path))
	new JSON:cfg = json_parse(path, true)
	if (cfg == Invalid_JSON) return
	gz_guardar(cfg)
	json_free(cfg)
}

stock gz_borrar_partido() {
	new path[256]
	gz_archivo("partido.json", path, charsmax(path))
	if (file_exists(path)) delete_file(path)
}

// Al empezar cada mapa: si hay un partido guardado, se sigue. Salvo que el
// servidor recien haya arrancado (se cayo o lo apagaron): ahi, como MatchZy
// en CS2, el partido queda sin cargar y se retoma desde un respaldo de ronda
// (regie_loadbackup, "Reanudar desde la ronda N" del panel). El localinfo
// sobrevive a los cambios de mapa, no a un proceso nuevo.
stock gz_retomar() {
	new path[256]
	gz_archivo("partido.json", path, charsmax(path))
	new proceso[4]
	get_localinfo("regie_proceso", proceso, charsmax(proceso))
	set_localinfo("regie_proceso", "1")
	if (!proceso[0]) {
		if (file_exists(path)) {
			delete_file(path)
			server_print(TX(93))
		}
		return
	}
	if (!file_exists(path)) return
	new JSON:cfg = json_parse(path, true)
	if (cfg == Invalid_JSON) return
	if (gz_leer_config(cfg)) {
		gz_mapa = json_object_get_number(cfg, "_regie_mapa")
		gz_serie[1] = json_object_get_number(cfg, "_regie_serie1")
		gz_serie[2] = json_object_get_number(cfg, "_regie_serie2")
		json_object_get_string(cfg, "_regie_hostname", gz_hostnameOriginal, charsmax(gz_hostnameOriginal))
		gz_freno = json_object_has_value(cfg, "_regie_freno") && json_object_get_bool(cfg, "_regie_freno")
		gz_informal = json_object_has_value(cfg, "_regie_informal") && json_object_get_bool(cfg, "_regie_informal")
		gz_rondaPendiente = json_object_has_value(cfg, "_regie_restaurar") ? json_object_get_number(cfg, "_regie_restaurar") : -1
		gz_poner_hostname()
		gz_ir_al_mapa_o_calentar()
	}
	json_free(cfg)
}

stock gz_poner_hostname() {
	new formato[128]
	get_pcvar_string(gz_pcvarHostname, formato, charsmax(formato))
	if (!formato[0]) return
	replace_all(formato, charsmax(formato), "{TEAM1}", gz_nombre[1])
	replace_all(formato, charsmax(formato), "{TEAM2}", gz_nombre[2])
	set_cvar_string("hostname", formato)
}

stock gz_ir_al_mapa_o_calentar() {
	if (gz_vetoPendiente) {
		gz_empezar_preveto()
		return
	}
	new actual[GZ_MAPA]
	get_mapname(actual, charsmax(actual))
	if (!equali(actual, gz_mapas[gz_mapa])) {
		gz_decir(0, TX(4), gz_mapas[gz_mapa])
		set_task(3.0, "gz_task_cambiar_mapa", GZ_TASK_MAPA)
		return
	}
	gz_empezar_calentamiento()
}

public gz_task_cambiar_mapa() {
	server_cmd("changelevel %s", gz_mapas[gz_mapa])
}

// Lado del team1 en este mapa, segun map_sides. Con "knife", team1 juega el
// cuchillo de CT y el lado de en vivo lo elige el que lo gane.
stock gz_lado_inicial() {
	if (equal(gz_lados[gz_mapa], "team1_t") || equal(gz_lados[gz_mapa], "team2_ct")) gz_ladoInicialT1 = CS_TEAM_T
	else gz_ladoInicialT1 = CS_TEAM_CT
	gz_ladoT1 = gz_ladoInicialT1
}

stock gz_empezar_calentamiento(bool:silencioso = false) {
	gz_estado = GS_WARMUP
	gz_lado_inicial()
	gz_reset_mapa()
	gz_cantAus = 0
	gz_cuchilloHecho = false
	gz_ganadorCuchillo = 0
	for (new i = 1; i <= 32; i++) gz_listo[i] = false
	gz_exec_calentamiento(true, true)
	gz_acomodar_todos()
	gz_poner_aviso()
	if (silencioso) return
	if (gz_rondaPendiente >= 0) gz_decir(0, TX(5), gz_mapa + 1, gz_mapas[gz_mapa], gz_rondaPendiente + 1)
	else gz_decir(0, TX(6), gz_mapa + 1, gz_mapas[gz_mapa])
	if (gz_freno) gz_decir(0, TX(7))
}

stock gz_reset_mapa() {
	gz_puntos[1] = 0
	gz_puntos[2] = 0
	gz_ctT[1][0] = 0
	gz_ctT[1][1] = 0
	gz_ctT[2][0] = 0
	gz_ctT[2][1] = 0
	gz_ronda = 0
	gz_rondaAvisada = -1
	gz_mitadPendiente = false
	gz_esperandoVivo = false
	gz_plataProrroga = false
	TrieClear(gz_stats)
	TrieClear(gz_plata)
	gz_tecnicasUsadas[1] = 0
	gz_tecnicasUsadas[2] = 0
	gz_tacticasUsadas[1] = 0
	gz_tacticasUsadas[2] = 0
	gz_terminar_pausa()
}

// --------------------------------------------------------------------
// Calentamiento: .ready
// --------------------------------------------------------------------

// Los comandos corren un instante despues, no adentro del "say": arrancar el
// cuchillo o salir en vivo hace server_exec(), que pisa los argumentos del
// comando del jugador, y el juego ya no mostraba el mensaje en el chat ni lo
// escribia en el log (el .ready que arrancaba algo no se veia).
public gz_cmd_say(id) {
	new texto[64]
	read_args(texto, charsmax(texto))
	remove_quotes(texto)
	trim(texto)
	if (texto[0] != '.' && texto[0] != '!' && !equali(texto, "rtv")) return PLUGIN_CONTINUE
	set_task(0.1, "gz_task_say", GZ_TASK_SAY + id, texto, sizeof(texto))
	return PLUGIN_CONTINUE
}

public gz_task_say(const datos[], taskid) {
	new id = taskid - GZ_TASK_SAY
	if (!is_user_connected(id)) return
	new texto[64]
	copy(texto, charsmax(texto), datos)
	gz_comando_say(id, texto)
}

stock gz_comando_say(id, texto[]) {
	new bool:listo = equali(texto, ".ready") || equali(texto, "!ready") || equali(texto, ".r")
	new bool:noListo = equali(texto, ".unready") || equali(texto, "!unready") || equali(texto, ".notready") || equali(texto, ".ur")
	if (equali(texto, ".help") || equali(texto, "!help")) {
		gz_ayuda(id)
		return PLUGIN_CONTINUE
	}
	if (equali(texto, ".rtv") || equali(texto, "!rtv") || equali(texto, "rtv") || equali(texto, ".votemap") || equali(texto, "!votemap")) {
		gz_pedir_rtv(id)
		return PLUGIN_CONTINUE
	}
	if (gz_estado == GS_NONE) {
		if (listo || noListo) gz_listo_informal(id, listo)
		return PLUGIN_CONTINUE
	}
	if (listo || noListo) {
		gz_poner_listo(id, listo)
		return PLUGIN_CONTINUE
	}
	if (equali(texto, ".pause") || equali(texto, ".p")) {
		if (get_pcvar_num(gz_pcvarPausaEsTactica) == 1) gz_pedir_pausa(id, PZ_TACTICA)
		else gz_pedir_pausa(id, PZ_TECNICA)
		return PLUGIN_CONTINUE
	}
	if (equali(texto, ".tech")) {
		gz_pedir_pausa(id, PZ_TECNICA)
		return PLUGIN_CONTINUE
	}
	if (equali(texto, ".tac")) {
		gz_pedir_pausa(id, PZ_TACTICA)
		return PLUGIN_CONTINUE
	}
	if (equali(texto, ".unpause") || equali(texto, ".up")) {
		gz_pedir_seguir(id)
		return PLUGIN_CONTINUE
	}
	if (equali(texto, ".stop")) {
		gz_pedir_stop(id)
		return PLUGIN_CONTINUE
	}
	if (gz_estado == GS_VETO) {
		if (equali(texto, ".ban ", 5)) gz_vetar(id, texto[5], false)
		else if (equali(texto, ".pick ", 6)) gz_vetar(id, texto[6], true)
		else if (equali(texto, ".ct")) gz_lado_del_veto(id, CS_TEAM_CT)
		else if (equali(texto, ".t")) gz_lado_del_veto(id, CS_TEAM_T)
		return PLUGIN_CONTINUE
	}
	if (gz_estado == GS_WAIT_KNIFE) {
		if (equali(texto, ".stay")) gz_decidir_lado(id, 0)
		else if (equali(texto, ".switch") || equali(texto, ".swap")) gz_decidir_lado(id, 1)
		else if (equali(texto, ".ct")) gz_decidir_lado(id, 2)
		else if (equali(texto, ".t")) gz_decidir_lado(id, 3)
	}
	return PLUGIN_CONTINUE
}

// .help, como el de MatchZy: solo al que lo pide, los comandos de la fase en
// la que esta el servidor (dos lineas).
stock gz_ayuda(id) {
	gz_decir(id, TX(145))
	switch (gz_estado) {
		case GS_NONE: {
			gz_decir(id, get_pcvar_num(gz_pcvarInformal) == 1 ? TX(146) : TX(150))
			if (get_pcvar_num(gz_pcvarRtv) == 1) gz_decir(id, TX(182))
		}
		case GS_WARMUP, GS_PREVETO: gz_decir(id, TX(146))
		case GS_KNIFE, GS_WAIT_KNIFE: gz_decir(id, TX(147))
		case GS_VETO: gz_decir(id, TX(149))
		case GS_GOING_LIVE, GS_LIVE: {
			// .pause pide una tecnica salvo que sea la tactica: sin pausas
			// tecnicas, solo sirve en ese caso.
			new bool:tecnica = get_pcvar_num(gz_pcvarTecnica) == 1
			new lista[128]
			if (tecnica || get_pcvar_num(gz_pcvarPausaEsTactica) == 1) copy(lista, charsmax(lista), "^4.pause^1, ")
			add(lista, charsmax(lista), "^4.unpause^1, ^4.tac^1")
			if (tecnica) add(lista, charsmax(lista), ", ^4.tech^1")
			if (get_pcvar_num(gz_pcvarStop) == 1) add(lista, charsmax(lista), ", ^4.stop^1")
			gz_decir(id, TX(148), lista)
		}
		default: gz_decir(id, TX(150))
	}
}

stock gz_poner_listo(id, bool:listo) {
	if (gz_estado != GS_WARMUP && gz_estado != GS_PREVETO) return
	if (!gz_equipo_de(id) && !gz_bot_libre(id)) {
		gz_decir(id, TX(8))
		return
	}
	new bool:antes = gz_listo[id]
	gz_listo[id] = listo
	if (gz_freno && gz_estado == GS_WARMUP) {
		// Inicio a cargo de la produccion: queda anotado y nada mas.
		new nombre[32]
		get_user_name(id, nombre, charsmax(nombre))
		if (listo && !antes) gz_decir(0, TX(9), nombre)
		else if (listo) gz_decir(id, TX(10))
		else gz_decir(id, TX(11))
		return
	}
	if (listo) gz_decir(id, TX(12))
	else gz_decir(id, TX(11))
	if (listo) gz_revisar_listos()
}

// Arranca con los dos equipos completos y todos listos (o con
// regie_minimum_ready_required listos, si esta puesto).
stock gz_revisar_listos() {
	new listos[3], presentes[3]
	new jugadores[32], n
	get_players(jugadores, n, "h")
	for (new i = 0; i < n; i++) {
		new id = jugadores[i]
		new equipo = gz_equipo_de(id)
		if (!equipo && gz_bot_listo(id)) equipo = gz_equipo_del_lado(CsTeams:get_member(id, m_iTeam))
		if (equipo != 1 && equipo != 2) continue
		presentes[equipo]++
		if (gz_listo[id] || gz_bot_listo(id)) listos[equipo]++
	}
	new minimo = get_pcvar_num(gz_pcvarMinListos)
	new bool:arrancar = minimo > 0 ? (listos[1] + listos[2] >= minimo) : (listos[1] >= gz_porEquipo && listos[2] >= gz_porEquipo)
	if (!arrancar) return
	if (gz_estado == GS_PREVETO) gz_empezar_veto()
	else if (!gz_freno) gz_salir_en_vivo()
}

// --------------------------------------------------------------------
// Partido informal: .ready sin partido cargado (el "pug" de MatchZy)
// --------------------------------------------------------------------

stock gz_listo_informal(id, bool:listo) {
	if (get_pcvar_num(gz_pcvarInformal) != 1) return
	new CsTeams:lado = CsTeams:get_member(id, m_iTeam)
	if (lado != CS_TEAM_T && lado != CS_TEAM_CT) {
		gz_decir(id, TX(13))
		return
	}
	gz_listo[id] = listo
	if (!listo) {
		gz_decir(id, TX(11))
		return
	}
	gz_decir(id, TX(12))
	gz_revisar_informal()
}

// Como matchzy_minimum_ready_required en un pug de MatchZy:
// regie_minimum_ready_required 0, arranca cuando todos los que estan en CT o T
// pusieron .ready; N > 0, cuando hay N listos, aunque otros no lo hayan puesto.
// Los bots no escriben: los que estan en CT o T cuentan como listos (para
// probar con bots). Siempre hace falta al menos una persona lista.
// Hasta la 1.12.0 ademas tenia que haber alguien de cada lado y nadie sin
// .ready aunque hubiera minimo: un bot que habia quedado en Espectadores (los
// manda ahi un partido cargado) o uno sin .ready trababan el arranque.
stock gz_revisar_informal() {
	new faltan
	if (gz_listos_informal(true, faltan) < 0) return
	gz_crear_informal()
}

// Los listos del informal: los que cuentan (personas con .ready y bots en CT
// o T). arranca: -1 si con eso todavia no alcanza (ver gz_revisar_informal).
// faltan: cuantos hacen falta segun el minimo (0 = todos los de CT y T).
stock gz_listos_informal(bool:arranca, &faltan) {
	new listos, presentes, humanosListos
	new jugadores[32], n
	get_players(jugadores, n, "h")
	for (new i = 0; i < n; i++) {
		new id = jugadores[i]
		new CsTeams:lado = CsTeams:get_member(id, m_iTeam)
		if (lado != CS_TEAM_T && lado != CS_TEAM_CT) continue
		presentes++
		if (is_user_bot(id)) {
			listos++
			continue
		}
		if (!gz_listo[id]) continue
		listos++
		humanosListos++
	}
	new minimo = get_pcvar_num(gz_pcvarMinListos)
	faltan = minimo > 0 ? minimo : presentes
	if (!arranca) return listos
	if (!humanosListos || listos < faltan) return -1
	return listos
}

// Un match config armado con lo que hay: un mapa (el actual), los equipos
// como estan (team1 = los CT) con el nombre del primero de cada uno, como
// MatchZy ("team_<nombre>"), y cuchillo si regie_knife_enabled_default.
stock gz_crear_informal() {
	new JSON:cfg = json_init_object()
	json_object_set_number(cfg, "matchid", get_systime())
	new porEquipo = 1
	for (new e = 1; e <= 2; e++) {
		new CsTeams:lado = e == 1 ? CS_TEAM_CT : CS_TEAM_T
		new JSON:equipo = json_init_object()
		new JSON:plantel = json_init_object()
		new nombreEquipo[GZ_NOMBRE], cuantos
		new jugadores[32], n
		get_players(jugadores, n, "h")
		for (new i = 0; i < n; i++) {
			new id = jugadores[i]
			if (CsTeams:get_member(id, m_iTeam) != lado) continue
			new sid[GZ_SID], nombre[32]
			gz_steam64(id, sid, charsmax(sid))
			get_user_name(id, nombre, charsmax(nombre))
			json_object_set_string(plantel, sid, nombre)
			if (!cuantos++) formatex(nombreEquipo, charsmax(nombreEquipo), "team_%s", nombre)
		}
		porEquipo = max(porEquipo, cuantos)
		json_object_set_string(equipo, "name", nombreEquipo)
		gz_poner(equipo, "players", plantel)
		gz_poner(cfg, e == 1 ? "team1" : "team2", equipo)
	}
	json_object_set_number(cfg, "num_maps", 1)
	new mapa[GZ_MAPA]
	get_mapname(mapa, charsmax(mapa))
	new JSON:mapas = json_init_array()
	json_array_append_string(mapas, mapa)
	gz_poner(cfg, "maplist", mapas)
	new JSON:lados = json_init_array()
	json_array_append_string(lados, get_pcvar_num(gz_pcvarCuchillo) == 1 ? "knife" : "team1_ct")
	gz_poner(cfg, "map_sides", lados)
	json_object_set_number(cfg, "players_per_team", porEquipo)
	json_object_set_bool(cfg, "_regie_informal", true)
	gz_decir(0, TX(14))
	gz_cargar_config(cfg, true)
}

public gz_task_aviso() {
	if (gz_estado == GS_NONE) {
		gz_aviso_informal()
		return
	}
	if (gz_estado == GS_WAIT_KNIFE) {
		gz_aviso_cuchillo()
		return
	}
	if (gz_estado != GS_WARMUP && gz_estado != GS_PREVETO) {
		remove_task(GZ_TASK_AVISO)
		return
	}
	new faltan[192], presentes[3]
	new jugadores[32], n
	get_players(jugadores, n, "ch")
	for (new i = 0; i < n; i++) {
		new id = jugadores[i]
		new equipo = gz_equipo_de(id)
		if (!equipo) continue
		presentes[equipo]++
		if (gz_listo[id]) continue
		new nombre[32]
		get_user_name(id, nombre, charsmax(nombre))
		if (faltan[0]) add(faltan, charsmax(faltan), ", ")
		add(faltan, charsmax(faltan), nombre)
	}
	// En un pug no: los equipos son los que pusieron .ready (todos estan) y el
	// conteo saltea los bots ("ch"), que en un pug con bots daba 0/1 para siempre.
	if (!gz_informal && (presentes[1] < gz_porEquipo || presentes[2] < gz_porEquipo)) {
		gz_decir(0, TX(15), gz_nombre[1], presentes[1], gz_porEquipo, gz_nombre[2], presentes[2], gz_porEquipo)
	}
	if (faltan[0]) gz_decir(0, TX(16), faltan)
	if (gz_freno && gz_estado == GS_WARMUP) {
		gz_decir(0, TX(7))
		return
	}
	// Tambien aca: alguien pudo entrar ya listo, o los bots de prueba.
	gz_revisar_listos()
}

// Sin partido: el recordatorio del informal, como MatchZy: a todos, cada vez,
// mientras haya alguien en CT o T sin .ready (con el texto de MatchZy). Solo
// los humanos: los bots cuentan como listos. Mientras se veta por el chat
// (el cartel de regie_veto_hud), solo si alguien ya puso .ready.
stock gz_aviso_informal() {
	if (get_pcvar_num(gz_pcvarInformal) != 1) return
	new faltan[192], alguno
	new jugadores[32], n
	get_players(jugadores, n, "ch")
	for (new i = 0; i < n; i++) {
		new id = jugadores[i]
		new CsTeams:lado = CsTeams:get_member(id, m_iTeam)
		if (lado != CS_TEAM_T && lado != CS_TEAM_CT) continue
		if (gz_listo[id]) {
			alguno++
			continue
		}
		new nombre[32]
		get_user_name(id, nombre, charsmax(nombre))
		if (faltan[0]) add(faltan, charsmax(faltan), ", ")
		add(faltan, charsmax(faltan), nombre)
	}
	if (faltan[0] && (alguno || !g_vetoHud[0])) gz_decir(0, TX(131), faltan)
	if (alguno) gz_revisar_informal()
}

// --------------------------------------------------------------------
// En vivo
// --------------------------------------------------------------------

stock gz_salir_en_vivo() {
	if (gz_estado != GS_WARMUP) return
	// regie_loadbackup: en vez de empezar el mapa, la ronda del respaldo.
	if (gz_rondaPendiente >= 0) {
		new ronda = gz_rondaPendiente
		gz_rondaPendiente = -1
		gz_guardar_estado()
		if (gz_aplicar_respaldo(ronda)) return
		gz_decir(0, TX(17), ronda + 1)
	}
	if (equal(gz_lados[gz_mapa], "knife") && !gz_cuchilloHecho) {
		gz_empezar_cuchillo()
		return
	}
	remove_task(GZ_TASK_AVISO)
	gz_estado = GS_GOING_LIVE
	server_cmd("exec cfg/regiegg/live.cfg")
	server_exec()
	gz_ladoT1 = gz_ladoInicialT1
	gz_acomodar_todos()
	gz_reset_mapa()
	gz_esperandoVivo = true
	// Que el juego ya de la partida por empezada: si no, la primera muerte
	// en vivo dispara un "Game_Commencing" que reinicia la ronda (medido con
	// bots que entraron tarde).
	set_member_game(m_bGameStarted, true)
	gz_esperar_reinicio()
	server_cmd("sv_restartround 1")
	// El cartel de calentamiento se saca ya, no al segundo.
	gz_task_cartel()
}

// --------------------------------------------------------------------
// Cuchillo: .stay / .switch
// --------------------------------------------------------------------

stock gz_empezar_cuchillo() {
	remove_task(GZ_TASK_AVISO)
	gz_estado = GS_KNIFE
	server_cmd("exec cfg/regiegg/knife.cfg")
	server_exec()
	gz_ladoT1 = gz_ladoInicialT1
	gz_acomodar_todos()
	gz_reset_mapa()
	// Como al salir en vivo: que la primera muerte no dispare un Game_Commencing.
	set_member_game(m_bGameStarted, true)
	gz_esperar_reinicio()
	server_cmd("sv_restartround 1")
	gz_decir(0, TX(18))
	gz_decir(0, TX(18))
	gz_decir(0, TX(18))
}

// knife.cfg y live.cfg apagan las rondas sin fin, y la ronda del calentamiento
// sigue corriendo hasta el reinicio: si su tiempo ya vencio (un calentamiento
// de mas de mp_roundtime), el juego la termina en el acto ("Target_Saved") y el
// cuchillo la tomaba como suya: lo "ganaba" el CT y se elegia lado con las
// reglas del calentamiento (compra libre). Hasta el reinicio no termina
// ninguna ronda (gz_fin_ronda_pre).
stock gz_esperar_reinicio() {
	gz_reinicioPendiente = true
	remove_task(GZ_TASK_REINICIO)
	set_task(3.0, "gz_task_reinicio_vencido", GZ_TASK_REINICIO)
}

public gz_task_reinicio_vencido() {
	gz_reinicioPendiente = false
}

public gz_fin_ronda_pre(WinStatus:estado, ScenarioEventEndRound:evento, Float:demora) {
	if (!gz_reinicioPendiente) return HC_CONTINUE
	if (evento == ROUND_GAME_RESTART || evento == ROUND_GAME_COMMENCE || evento == ROUND_NONE) return HC_CONTINUE
	SetHookChainReturn(ATYPE_BOOL, false)
	return HC_SUPERCEDE
}

// Solo cuchillo: ni compra, ni lo que da el juego al aparecer, ni lo del piso.
public gz_restringido(const id, ItemID:item, ItemRestType:tipo) {
	if (gz_estado != GS_KNIFE || item == ITEM_KNIFE) return HC_CONTINUE
	SetHookChainReturn(ATYPE_BOOL, true)
	return HC_SUPERCEDE
}

// Y chaleco con casco, como el cuchillo de MatchZy, sin plata (no hay nada
// que comprar y el HUD no muestra la del calentamiento). En el calentamiento,
// al aparecer, toda la plata (mp_maxmoney): con la compra gratis
// (gz_plata_pre), alcanza para cualquier cosa.
public gz_equipar(const id) {
	if (!is_user_alive(id)) return
	if (gz_estado == GS_KNIFE) {
		rg_set_user_armor(id, 100, ARMOR_VESTHELM)
		rg_add_account(id, 0, AS_SET, false)
		return
	}
	if (gz_en_calentamiento()) rg_add_account(id, gz_plata_maxima(), AS_SET, false)
}

// Calentamiento: lo que se compra no se descuenta (plata infinita).
public gz_plata_pre(const id, amount, RewardType:tipo, bool:cambio) {
	if (tipo != RT_PLAYER_BOUGHT_SOMETHING || !gz_en_calentamiento()) return HC_CONTINUE
	return HC_SUPERCEDE
}

// Gana el cuchillo el equipo que elimina al otro; si se acaba el tiempo, el
// que tiene mas vivos y, si empatan, el de mas vida sumada (como MatchZy).
stock gz_fin_cuchillo(WinStatus:estado, ScenarioEventEndRound:evento) {
	if (evento == ROUND_GAME_COMMENCE || evento == ROUND_GAME_RESTART || evento == ROUND_NONE) return
	if (!GetHookChainReturn(ATYPE_BOOL)) return
	new CsTeams:lado = estado == WINSTATUS_CTS ? CS_TEAM_CT : estado == WINSTATUS_TERRORISTS ? CS_TEAM_T : CS_TEAM_UNASSIGNED
	if (lado == CS_TEAM_UNASSIGNED) {
		new vivos[2], vida[2]
		new jugadores[32], n
		get_players(jugadores, n, "ah")
		for (new i = 0; i < n; i++) {
			new CsTeams:l = CsTeams:get_member(jugadores[i], m_iTeam)
			if (l != CS_TEAM_CT && l != CS_TEAM_T) continue
			new k = l == CS_TEAM_CT ? 0 : 1
			vivos[k]++
			vida[k] += get_user_health(jugadores[i])
		}
		if (vivos[0] != vivos[1]) lado = vivos[0] > vivos[1] ? CS_TEAM_CT : CS_TEAM_T
		else if (vida[0] != vida[1]) lado = vida[0] > vida[1] ? CS_TEAM_CT : CS_TEAM_T
		else lado = random_num(0, 1) ? CS_TEAM_CT : CS_TEAM_T
	}
	gz_cuchilloHecho = true
	// El juego le sumo la ronda al que gano: mientras se elige lado el
	// marcador (TAB, HUD) queda 0-0, como en MatchZy.
	rg_update_teamscores(0, 0, false)
	gz_ganadorCuchillo = gz_equipo_del_lado(lado)
	gz_estado = GS_WAIT_KNIFE
	// Mientras deciden, como en el calentamiento (rondas sin fin, reaparicion).
	gz_exec_calentamiento(true, false)
	gz_decir(0, TX(19), gz_nombre[gz_ganadorCuchillo])
	gz_poner_aviso()
}

// 0 .stay, 1 .switch, 2 .ct, 3 .t
stock gz_decidir_lado(id, eleccion) {
	new equipo = gz_equipo_de(id)
	if (!equipo && gz_bot_libre(id)) equipo = gz_equipo_del_lado(CsTeams:get_member(id, m_iTeam))
	if (equipo != gz_ganadorCuchillo) {
		gz_decir(id, TX(20), gz_nombre[gz_ganadorCuchillo])
		return
	}
	new CsTeams:actual = gz_lado_de_equipo(equipo)
	new CsTeams:quiere = actual
	if (eleccion == 1) quiere = actual == CS_TEAM_CT ? CS_TEAM_T : CS_TEAM_CT
	else if (eleccion == 2) quiere = CS_TEAM_CT
	else if (eleccion == 3) quiere = CS_TEAM_T
	gz_aplicar_decision(quiere)
}

stock gz_aplicar_decision(CsTeams:ladoGanador) {
	new equipo = gz_ganadorCuchillo
	new CsTeams:ladoT1 = equipo == 1 ? ladoGanador : (ladoGanador == CS_TEAM_CT ? CS_TEAM_T : CS_TEAM_CT)
	if (ladoGanador == gz_lado_de_equipo(equipo)) gz_decir(0, TX(21), gz_nombre[equipo])
	else gz_decir(0, TX(22), gz_nombre[equipo])
	gz_ladoInicialT1 = ladoT1
	gz_estado = GS_WARMUP
	gz_salir_en_vivo()
}

// Un equipo de bots (regie_bots_libres, para probar, o los de un pug) no puede
// escribir: se queda.
stock gz_aviso_cuchillo() {
	new jugadores[32], n, humanos
	get_players(jugadores, n, "ch")
	for (new i = 0; i < n; i++) if (gz_equipo_de(jugadores[i]) == gz_ganadorCuchillo) humanos++
	if (!humanos && (get_pcvar_num(gz_pcvarBotsLibres) == 1 || gz_informal)) {
		gz_aplicar_decision(gz_lado_de_equipo(gz_ganadorCuchillo))
		return
	}
	gz_decir(0, TX(19), gz_nombre[gz_ganadorCuchillo])
}

public gz_reinicio_pre() {
	gz_reinicioPendiente = false
	remove_task(GZ_TASK_REINICIO)
	if (gz_mitadPendiente) {
		gz_mitadPendiente = false
		rg_swap_all_players()
		gz_ladoT1 = gz_ladoT1 == CS_TEAM_CT ? CS_TEAM_T : CS_TEAM_CT
		// Como un reinicio: plata y armas de arranque para la segunda mitad.
		set_member_game(m_bCompleteReset, true)
		gz_decir(0, TX(23))
	}
	for (new i = 1; i <= 32; i++) {
		gz_rJugo[i] = false
		gz_rKill[i] = false
		gz_rAsist[i] = false
		gz_rMuerto[i] = false
		gz_rTrade[i] = false
		gz_rKills[i] = 0
		gz_rDanio[i] = 0
		for (new j = 1; j <= 32; j++) {
			gz_danioTope[i][j] = 0
			gz_danioReal[i][j] = 0
			gz_golpes[i][j] = 0
			gz_repDanio[i][j] = 0
			gz_repGolpes[i][j] = 0
		}
		gz_repVisto[i] = false
	}
	gz_muertes = 0
	gz_plantador = 0
	gz_desactivador = 0
}

public gz_reinicio_post() {
	if (gz_marcadorACero) {
		gz_marcadorACero = false
		set_member_game(m_flGameStartTime, gz_inicioJuego)
		set_member_game(m_flTimeLimit, gz_limiteTiempo)
		set_member_game(m_iTotalRoundsPlayed, gz_rondasJugadas)
		// El feed del HUD (RegieGG.sma) lleva el marcador con el mensaje
		// TeamScore, que el reinicio ya manda en cero; por las dudas.
		g_scoreCt = 0
		g_scoreT = 0
	}
	if (gz_restaurando) {
		gz_restaurando = false
		rg_update_teamscores(gz_puntos[gz_equipo_del_lado(CS_TEAM_CT)], gz_puntos[gz_equipo_del_lado(CS_TEAM_T)], false)
		// Las armas y la plata, cuando el juego ya termino de equipar a todos.
		set_task(0.3, "gz_task_reponer", GZ_TASK_REPONER)
		set_task(0.6, "gz_task_respaldo", GZ_TASK_RESPALDO)
		gz_iniciar_pausa(PZ_RESTAURO, 0)
		gz_decir(0, TX(24))
		new JSON:ev = json_init_object()
		json_object_set_number(ev, "map_number", gz_mapa)
		json_object_set_number(ev, "round_number", gz_ronda)
		new archivo[96]
		formatex(archivo, charsmax(archivo), "regie_%d_%d_round%02d.json", gz_matchid, gz_mapa, gz_ronda)
		json_object_set_string(ev, "filename", archivo)
		gz_enviar(ev, "backup_loaded")
		return
	}
	if (gz_esperandoVivo) {
		gz_esperandoVivo = false
		gz_estado = GS_LIVE
		gz_decir(0, TX(25))
		gz_decir(0, TX(25))
		gz_decir(0, TX(25))
		new JSON:ev = json_init_object()
		json_object_set_number(ev, "map_number", gz_mapa)
		gz_enviar(ev, "going_live")
		set_task(0.5, "gz_task_respaldo", GZ_TASK_RESPALDO)
		gz_avisos_ronda()
		return
	}
	// Despues del cambio de lados, el marcador del juego vuelve a lo que iba.
	if (gz_estado == GS_LIVE && gz_ronda > 0) {
		new ct = gz_puntos[gz_equipo_del_lado(CS_TEAM_CT)]
		new t = gz_puntos[gz_equipo_del_lado(CS_TEAM_T)]
		rg_update_teamscores(ct, t, false)
	}
	// Prorroga: como en CS2, cada una es un reinicio de economia y de
	// inventario (1.15.0). Al empezar cada prorroga y en su cambio de lados
	// todos arrancan con lo de fabrica (cuchillo y la pistola de su lado, sin
	// chaleco, granadas ni kit: no se guarda nada de la ronda anterior) y con
	// la misma plata. Antes solo se ponia la plata y las armas del que habia
	// sobrevivido pasaban a la prorroga. Va en el post del reinicio: despues
	// del m_bCompleteReset del cambio de lados (que ya da la plata de
	// mp_startmoney) y de que el juego le dio la C4 a un T, que se respeta.
	if (gz_estado == GS_LIVE && gz_plataProrroga) {
		gz_plataProrroga = false
		new plata = get_pcvar_num(gz_pcvarProrrogaPlata)
		new jugadores[32], n
		get_players(jugadores, n, "h")
		for (new i = 0; i < n; i++) {
			new id = jugadores[i]
			new CsTeams:l = CsTeams:get_member(id, m_iTeam)
			if (l != CS_TEAM_CT && l != CS_TEAM_T) continue
			if (is_user_alive(id)) gz_equipo_de_fabrica(id)
			rg_add_account(id, plata, AS_SET, false)
		}
	}
	// El respaldo de la ronda que empieza (con la plata ya dada).
	if (gz_estado == GS_LIVE) set_task(0.5, "gz_task_respaldo", GZ_TASK_RESPALDO)
	if (gz_estado == GS_LIVE) gz_avisos_ronda()
}

// Lo de fabrica de cada ronda nueva (1.15.0, la prorroga): se le saca todo
// (armas, granadas, chaleco y kit) y se le da lo que el juego da al aparecer
// (rg_give_default_items: cuchillo y pistola de su lado, con las cvars
// mp_*_default_* de ReGameDLL). La C4, si se la habia dado el juego en este
// reinicio, se le devuelve (rg_give_item la marca como suya, m_bHasC4).
stock gz_equipo_de_fabrica(id) {
	new bool:bomba = bool:user_has_weapon(id, CSW_C4)
	rg_remove_all_items(id)
	rg_set_user_armor(id, 0, ARMOR_NONE)
	if (CsTeams:get_member(id, m_iTeam) == CS_TEAM_CT) rg_give_defusekit(id, false)
	rg_give_default_items(id)
	if (bomba) rg_give_item(id, "weapon_c4")
}

// Al empezar cada ronda en vivo, a todos: el marcador (desde la segunda) y,
// si toca, la ultima ronda de la primera mitad (o de la de la prorroga) y el
// punto de mapa o de partida de cada equipo. Las mitades y la prorroga, con
// la misma cuenta que gz_revisar_mapa: la ronda que empieza es gz_ronda + 1.
stock gz_avisos_ronda() {
	if (get_pcvar_num(gz_pcvarAvisosRonda) != 1 || gz_rondaAvisada == gz_ronda) return
	gz_rondaAvisada = gz_ronda
	new ronda = gz_ronda + 1
	// El marcador desde la segunda; la primera tambien puede ser la ultima de
	// la mitad (un mapa de 2 o 3 rondas: la mitad es maximo / 2 = 1).
	if (gz_ronda > 0) gz_decir(0, TX(158), ronda, gz_nombre[1], gz_puntos[1], gz_puntos[2], gz_nombre[2])
	new maximo = max(2, get_pcvar_num(gz_pcvarMaxRounds))
	new prorroga = max(2, get_pcvar_num(gz_pcvarProrrogaRondas) & ~1)
	new limite = maximo
	if (ronda > maximo) {
		// Sin prorroga, el mapa ya termino en el tiempo reglamentario.
		if (get_pcvar_num(gz_pcvarProrroga) != 1) return
		limite = maximo + ((ronda - maximo - 1) / prorroga + 1) * prorroga
	}
	// Lo mismo, ademas, grande en el centro durante el tiempo de compra:
	// en el chat, en medio de la ronda, se pierde.
	new anuncio[192]
	if (ronda == maximo / 2) {
		gz_decir(0, TX(159))
		copy(anuncio, charsmax(anuncio), TX(163))
	} else if (ronda > maximo && ronda == limite - prorroga / 2) {
		gz_decir(0, TX(160))
		copy(anuncio, charsmax(anuncio), TX(164))
	}
	// Ganar esta ronda gana el mapa; si ese mapa tambien gana la serie, es
	// punto de partido. La serie entera, tambien en una retomada mapa a mapa.
	new total = gz_numMapsSerie > 0 ? gz_numMapsSerie : gz_numMaps
	new necesarios = total / 2 + 1
	// Cada equipo: si ganar esta ronda le da el mapa (mas de la mitad del
	// tramo: con 5 rondas, 3) y si ese mapa le da la serie.
	new bool:enPunto[3], bool:serie[3]
	for (new e = 1; e <= 2; e++) {
		enPunto[e] = gz_puntos[e] + 1 > limite / 2
		serie[e] = gz_seriePrevia[e] + gz_serie[e] + 1 >= necesarios
	}
	// Los dos a una ronda (con rondas impares: 2-2 de 5, la ronda que decide
	// el mapa): el mismo aviso para los dos. Punto de partido si el mapa
	// decide la serie para cualquiera de los dos lados por igual (un Mejor de
	// 1, o 1-1 en un Mejor de 3); si no, punto de mapa. Antes cada uno se
	// calculaba por su lado y con 1-0 en la serie salia "punto de partido"
	// para uno y "punto de mapa" para el otro con el mismo marcador.
	if (enPunto[1] && enPunto[2]) {
		new bool:ambos = serie[1] && serie[2]
		serie[1] = ambos
		serie[2] = ambos
	}
	for (new e = 1; e <= 2; e++) {
		if (!enPunto[e]) continue
		gz_decir(0, serie[e] ? TX(162) : TX(161), gz_nombre[e])
		new linea[96]
		formatex(linea, charsmax(linea), serie[e] ? TX(166) : TX(165), gz_nombre[e])
		if (anuncio[0]) add(anuncio, charsmax(anuncio), "^n")
		add(anuncio, charsmax(anuncio), linea)
	}
	if (anuncio[0]) {
		set_hudmessage(255, 170, 60, -1.0, 0.2, 0, 0.0, 5.0, 0.2, 0.6, -1)
		ShowSyncHudMsg(0, gz_anuncioSync, "%s", anuncio)
	}
}

// La pausa: mientras haya una, el freezetime no termina (el juego lo vuelve
// a intentar en cada cuadro y se lo frena de nuevo). Al reanudar, la ronda
// arranca con su tiempo entero.
public gz_fin_freeze_pre() {
	if (gz_estado != GS_LIVE) return HC_CONTINUE
	// Alguien se fue en el cuchillo o durante otra pausa y sigue afuera: la
	// pausa automatica empieza ahora (ver gz_task_desconexion).
	if (gz_pausa == PZ_NINGUNA && gz_cantAus > 0 && get_pcvar_num(gz_pcvarPausaDesconexion) == 1) gz_iniciar_pausa(PZ_DESCONEXION, gz_ausEquipo[0])
	if (gz_pausa == PZ_NINGUNA) return HC_CONTINUE
	if (!gz_pausaDesde) gz_empezo_pausa()
	return HC_SUPERCEDE
}

public gz_fin_freeze() {
	if (gz_estado != GS_LIVE) return
	gz_inicioRonda = get_gametime()
	new jugadores[32], n
	get_players(jugadores, n, "ah")
	for (new i = 0; i < n; i++) {
		new CsTeams:lado = CsTeams:get_member(jugadores[i], m_iTeam)
		if (lado == CS_TEAM_T || lado == CS_TEAM_CT) gz_rJugo[jugadores[i]] = true
	}
}

// --------------------------------------------------------------------
// Estadisticas
// --------------------------------------------------------------------

// Asistencias del partido de un jugador, para el feed del HUD (RegieGG.sma).
// Sin partido cargado no hay estadisticas: 0.
stock gz_asistencias(id) {
	if (gz_estado == GS_NONE) return 0
	new sid[GZ_SID], st[ST_TOTAL]
	gz_steam64(id, sid, charsmax(sid))
	if (!TrieGetArray(gz_stats, sid, st, ST_TOTAL)) return 0
	return st[ST_ASSISTS]
}

// Plantas y desactivadas del partido (1.20.0), para el marcador del HUD.
stock gz_stat_de(id, campo) {
	if (gz_estado == GS_NONE) return 0
	new sid[GZ_SID], st[ST_TOTAL]
	gz_steam64(id, sid, charsmax(sid))
	if (!TrieGetArray(gz_stats, sid, st, ST_TOTAL)) return 0
	return st[campo]
}

stock gz_stat(id, campo, n) {
	new sid[GZ_SID], st[ST_TOTAL]
	gz_steam64(id, sid, charsmax(sid))
	if (!TrieGetArray(gz_stats, sid, st, ST_TOTAL)) {
		arrayset(st, 0, ST_TOTAL)
	}
	st[campo] += n
	new equipo = gz_equipo_de(id)
	if (!equipo) equipo = gz_equipo_del_lado(CsTeams:get_member(id, m_iTeam))
	st[ST_TEAM] = equipo
	TrieSetArray(gz_stats, sid, st, ST_TOTAL)
	new nombre[32]
	get_user_name(id, nombre, charsmax(nombre))
	TrieSetString(gz_nombres, sid, nombre)
}

stock bool:gz_jugador(id) {
	return 1 <= id <= 32 && is_user_connected(id)
}

stock bool:gz_enemigos(a, b) {
	return CsTeams:get_member(a, m_iTeam) != CsTeams:get_member(b, m_iTeam)
}

public gz_danio_pre(const victima, inflictor, atacante, Float:danio, tipo) {
	if (gz_jugador(victima)) gz_vidaAntes[victima] = Float:get_entvar(victima, var_health)
	return HC_CONTINUE
}

public gz_danio_post(const victima, inflictor, atacante, Float:danio, tipo) {
	if (!gz_jugador(victima) || !gz_jugador(atacante) || atacante == victima) return HC_CONTINUE
	if (!gz_enemigos(atacante, victima)) return HC_CONTINUE
	// La explosion de la bomba no es daño de quien la planto (ver gz_es_c4).
	if (gz_es_c4(inflictor)) return HC_CONTINUE
	new Float:ahora = Float:get_entvar(victima, var_health)
	if (ahora < 0.0) ahora = 0.0
	new real = floatround(gz_vidaAntes[victima] - ahora)
	if (real <= 0) return HC_CONTINUE
	gz_repDanio[atacante][victima] += real
	gz_repGolpes[atacante][victima]++
	if (gz_estado != GS_LIVE) return HC_CONTINUE
	gz_danioReal[atacante][victima] += real
	gz_golpes[atacante][victima]++
	new suma = min(real, GZ_TOPE_DANIO - gz_danioTope[atacante][victima])
	if (suma > 0) {
		gz_danioTope[atacante][victima] += suma
		gz_rDanio[atacante] += suma
		gz_stat(atacante, ST_DAMAGE, suma)
	}
	return HC_CONTINUE
}

public gz_muerte(const victima, atacante, gib) {
	// Al que muere, su reporte, en cualquier fase. Un instante despues:
	// ReGameDLL llama a Killed adentro de TakeDamage, antes de gz_danio_post,
	// y el golpe que lo mato todavia no esta sumado. Si ya lo vio al terminar
	// la ronda (lo matan despues del fin, mientras se espera la siguiente),
	// no se le repite: el reporte es uno por ronda. Con rondas sin fin es
	// uno por vida y sale igual.
	if (gz_jugador(victima) && (!gz_repVisto[victima] || gz_rondas_sin_fin())) {
		gz_repVisto[victima] = true
		remove_task(GZ_TASK_REPORTE + victima)
		set_task(0.1, "gz_task_reporte", GZ_TASK_REPORTE + victima)
	}
	if (gz_estado != GS_LIVE || !gz_jugador(victima)) return HC_CONTINUE
	gz_stat(victima, ST_DEATHS, 1)
	gz_rMuerto[victima] = true
	new Float:ahora = get_gametime()
	// Muerto por la bomba: cuenta la muerte, pero no es una kill (ni da
	// asistencias ni "traded") de quien la planto.
	if (gz_jugador(atacante) && atacante != victima && gz_enemigos(atacante, victima) && !gz_muerto_por_bomba(victima)) {
		gz_stat(atacante, ST_KILLS, 1)
		gz_rKill[atacante] = true
		gz_rKills[atacante]++
		if (get_member(victima, m_bHeadshotKilled)) gz_stat(atacante, ST_HS, 1)
		// "Traded": la victima habia matado hace poco a un compañero del atacante.
		for (new i = 0; i < gz_muertes; i++) {
			if (gz_muerteAsesino[i] == victima && ahora - gz_muerteCuando[i] <= GZ_TRADE) {
				new vengado = gz_muerteVictima[i]
				if (gz_jugador(vengado) && !gz_enemigos(vengado, atacante)) gz_rTrade[vengado] = true
			}
		}
		// Asistencia: mas de 40 de daño a la victima, de otro del equipo del asesino.
		for (new p = 1; p <= 32; p++) {
			if (p == atacante || !gz_jugador(p) || gz_danioReal[p][victima] <= GZ_ASISTENCIA) continue
			if (!gz_enemigos(p, victima)) continue
			gz_stat(p, ST_ASSISTS, 1)
			gz_rAsist[p] = true
		}
		if (gz_muertes < sizeof(gz_muerteAsesino)) {
			gz_muerteAsesino[gz_muertes] = atacante
			gz_muerteVictima[gz_muertes] = victima
			gz_muerteCuando[gz_muertes] = ahora
			gz_muertes++
		}
	}
	return HC_CONTINUE
}

public gz_log_plant() {
	gz_plantador = gz_jugador_del_log()
	if (gz_estado == GS_LIVE && gz_jugador(gz_plantador)) gz_stat(gz_plantador, ST_PLANTS, 1)
}

public gz_log_defuse() {
	gz_desactivador = gz_jugador_del_log()
	if (gz_estado == GS_LIVE && gz_jugador(gz_desactivador)) gz_stat(gz_desactivador, ST_DEFUSES, 1)
}

// "Nombre<userid><STEAMID><TEAM>" del primer argumento del log.
stock gz_jugador_del_log() {
	new arg[96]
	read_logargv(0, arg, charsmax(arg))
	new nombre[32], uid
	parse_loguser(arg, nombre, charsmax(nombre), uid)
	return uid > 0 ? find_player("k", uid) : 0
}

// --------------------------------------------------------------------
// Fin de ronda, mapa y serie
// --------------------------------------------------------------------

public gz_fin_ronda(WinStatus:estado, ScenarioEventEndRound:evento, Float:demora) {
	// El reporte de daño, siempre: a los que no lo vieron al morir. Solo si el
	// juego acepto el fin de ronda y no en los reinicios (arranque, restart),
	// que no son el fin de una ronda. Un instante despues, como al morir: la
	// ronda que termina con una muerte termina adentro de TakeDamage, antes
	// de que se sume ese golpe.
	if (evento != ROUND_GAME_COMMENCE && evento != ROUND_GAME_RESTART && evento != ROUND_NONE && GetHookChainReturn(ATYPE_BOOL)) {
		remove_task(GZ_TASK_REPORTE_RONDA)
		set_task(0.1, "gz_task_reporte_ronda", GZ_TASK_REPORTE_RONDA)
	}
	if (gz_estado == GS_KNIFE) {
		gz_fin_cuchillo(estado, evento)
		return HC_CONTINUE
	}
	if (gz_estado != GS_LIVE) return HC_CONTINUE
	// Si igual llega un "Game_Commencing" en vivo, el juego vuelve el
	// marcador a cero: lo de esa ronda no cuenta.
	if (evento == ROUND_GAME_COMMENCE) {
		gz_reset_mapa()
		return HC_CONTINUE
	}
	if (evento == ROUND_GAME_RESTART || evento == ROUND_NONE) return HC_CONTINUE
	if (!GetHookChainReturn(ATYPE_BOOL)) return HC_CONTINUE

	new CsTeams:ladoGanador = estado == WINSTATUS_CTS ? CS_TEAM_CT : estado == WINSTATUS_TERRORISTS ? CS_TEAM_T : CS_TEAM_UNASSIGNED
	new ganador = ladoGanador == CS_TEAM_UNASSIGNED ? 0 : gz_equipo_del_lado(ladoGanador)
	gz_ronda++
	if (ganador) {
		gz_puntos[ganador]++
		gz_ctT[ganador][ladoGanador == CS_TEAM_CT ? 0 : 1]++
	}

	// KAST y rondas jugadas.
	for (new id = 1; id <= 32; id++) {
		if (!gz_rJugo[id] || !is_user_connected(id)) continue
		gz_stat(id, ST_ROUNDS, 1)
		if (gz_rKill[id] || gz_rAsist[id] || !gz_rMuerto[id] || gz_rTrade[id]) gz_stat(id, ST_KAST, 1)
	}
	// MVP: el que desactivo o planto la que exploto; si no, el que mas mato
	// del equipo ganador.
	new mvp = 0
	if (evento == ROUND_BOMB_DEFUSED && gz_jugador(gz_desactivador)) mvp = gz_desactivador
	else if (evento == ROUND_TARGET_BOMB && gz_jugador(gz_plantador)) mvp = gz_plantador
	else if (ladoGanador != CS_TEAM_UNASSIGNED) {
		for (new id = 1; id <= 32; id++) {
			if (!is_user_connected(id) || CsTeams:get_member(id, m_iTeam) != ladoGanador) continue
			if (!mvp || gz_rKills[id] > gz_rKills[mvp] || (gz_rKills[id] == gz_rKills[mvp] && gz_rDanio[id] > gz_rDanio[mvp])) mvp = id
		}
		if (mvp && gz_rKills[mvp] == 0) mvp = 0
	}
	if (mvp) gz_stat(mvp, ST_MVP, 1)

	new JSON:ev = json_init_object()
	json_object_set_number(ev, "map_number", gz_mapa)
	json_object_set_number(ev, "round_number", gz_ronda)
	json_object_set_number(ev, "round_time", max(0, floatround(get_gametime() - gz_inicioRonda)))
	// ScenarioEventEndRound de ReGameDLL numera igual que el RoundEndReason de CS2.
	json_object_set_number(ev, "reason", _:evento)
	gz_poner(ev, "winner", gz_ganador_json(ganador, ladoGanador))
	gz_poner(ev, "team1", gz_equipo_json(1))
	gz_poner(ev, "team2", gz_equipo_json(2))
	gz_enviar(ev, "round_end")

	// Un .stop de un solo equipo vence con la ronda.
	if (gz_stop[1] != gz_stop[2]) gz_decir(0, TX(26), gz_nombre[gz_stop[1] ? 1 : 2])
	gz_stop[1] = false
	gz_stop[2] = false
	gz_revisar_mapa()
	return HC_CONTINUE
}

// Como MatchZy: a un jugador, por cada rival (no los compañeros), lo que le
// hizo y lo que recibio (daño y golpes) y la vida que le queda, aunque no
// haya habido daño. Sale al morir y, al que siguio vivo, al terminar la
// ronda, en cualquier fase. Una linea por rival, con el prefijo de Regie.gg
// y el rival primero:
// "[Regie.gg] Juan (55 de vida): le has hecho 45 en 2 golpes, te ha hecho 0."
stock gz_reporte_danio(a) {
	if (get_pcvar_num(gz_pcvarReporteDanio) != 1) return
	if (!is_user_connected(a) || is_user_bot(a)) return
	new CsTeams:lado = CsTeams:get_member(a, m_iTeam)
	if (lado != CS_TEAM_T && lado != CS_TEAM_CT) return
	for (new v = 1; v <= 32; v++) {
		if (v == a || !is_user_connected(v)) continue
		new CsTeams:ladoV = CsTeams:get_member(v, m_iTeam)
		if ((ladoV != CS_TEAM_T && ladoV != CS_TEAM_CT) || ladoV == lado) continue
		gz_linea_danio(a, v)
	}
}

// La linea de un rival v en el reporte de a.
stock gz_linea_danio(a, v) {
	new nombre[32]
	get_user_name(v, nombre, charsmax(nombre))
	new vida = is_user_alive(v) ? get_user_health(v) : 0
	new estado[24], hecho[64], recibido[64]
	if (vida > 0) formatex(estado, charsmax(estado), TX(123), vida)
	else copy(estado, charsmax(estado), TX(124))
	gz_parte_danio(hecho, charsmax(hecho), gz_repDanio[a][v], gz_repGolpes[a][v], 125)
	gz_parte_danio(recibido, charsmax(recibido), gz_repDanio[v][a], gz_repGolpes[v][a], 128)
	client_print_color(a, print_team_default, TX(114), nombre, estado, hecho, recibido)
}

// El reporte del que murio. Con rondas que no terminan (calentamiento,
// eleccion de lado, reaparicion) lo de esta vida queda contado y la proxima
// arranca de cero; antes de olvidarlo, los que le hicieron daño y siguen ven
// su linea de ese rival: sin fin de ronda no hay otro momento, y matando sin
// morir no se veia nada.
public gz_task_reporte(taskid) {
	new id = taskid - GZ_TASK_REPORTE
	if (!is_user_connected(id)) return
	gz_reporte_danio(id)
	if (!gz_rondas_sin_fin()) return
	if (get_pcvar_num(gz_pcvarReporteDanio) == 1) {
		new CsTeams:lado = CsTeams:get_member(id, m_iTeam)
		for (new a = 1; a <= 32; a++) {
			if (a == id || !is_user_connected(a) || is_user_bot(a) || !gz_repGolpes[a][id]) continue
			new CsTeams:ladoA = CsTeams:get_member(a, m_iTeam)
			if ((ladoA != CS_TEAM_T && ladoA != CS_TEAM_CT) || ladoA == lado) continue
			gz_linea_danio(a, id)
		}
	}
	gz_rep_olvidar(id)
}

// El de fin de ronda, a los que no lo vieron al morir. Quedan como que ya
// lo vieron: si los matan despues del fin de la ronda (antes del reinicio,
// que lo pone en cero), gz_muerte no se lo vuelve a mandar.
// Si la ronda que termino da paso a una fase sin fin de ronda (el cuchillo,
// que sigue con la eleccion de lado y reaparicion), no hay reinicio que lo
// ponga en cero: se olvida aca. Si no, cada muerte mientras eligen lado
// volvia a mandar el daño de la ronda de cuchillo.
public gz_task_reporte_ronda() {
	for (new a = 1; a <= 32; a++) {
		if (gz_repVisto[a]) continue
		gz_reporte_danio(a)
		gz_repVisto[a] = true
	}
	if (gz_estado == GS_WAIT_KNIFE || gz_rondas_sin_fin()) {
		for (new a = 1; a <= 32; a++) gz_rep_olvidar(a)
	}
}

// mp_round_infinite: "0" o vacio, las rondas terminan; "1" o letras
// ("abcdef"), no todas o ninguna. mp_forcerespawn: se reaparece al morir.
stock bool:gz_rondas_sin_fin() {
	new infinita[32]
	get_cvar_string("mp_round_infinite", infinita, charsmax(infinita))
	return (infinita[0] && !equal(infinita, "0")) || get_cvar_float("mp_forcerespawn") > 0.0
}

// Lo que un jugador hizo y recibio en esta vida (calentamiento: sin fin de ronda).
stock gz_rep_olvidar(id) {
	for (new j = 1; j <= 32; j++) {
		gz_repDanio[id][j] = 0
		gz_repGolpes[id][j] = 0
		gz_repDanio[j][id] = 0
		gz_repGolpes[j][id] = 0
	}
	gz_repVisto[id] = false
}

// "le has hecho 45 en 2 golpes" / "en 1 golpe" / sin golpes si no hubo daño.
// base: la fila del singular; base + 1, la del plural; base + 2, la de solo
// el daño.
stock gz_parte_danio(out[], len, danio, golpes, base) {
	if (!danio || !golpes) formatex(out, len, TX(base + 2), danio)
	else if (golpes == 1) formatex(out, len, TX(base), danio, golpes)
	else formatex(out, len, TX(base + 1), danio, golpes)
}

stock JSON:gz_ganador_json(ganador, CsTeams:lado) {
	new JSON:w = json_init_object()
	json_object_set_string(w, "team", ganador == 1 ? "team1" : ganador == 2 ? "team2" : "none")
	json_object_set_string(w, "side", lado == CS_TEAM_CT ? "ct" : lado == CS_TEAM_T ? "t" : "spec")
	return w
}

stock JSON:gz_equipo_json(equipo) {
	new JSON:t = gz_equipo_corto(equipo)
	json_object_set_number(t, "series_score", gz_serie[equipo])
	json_object_set_number(t, "score", gz_puntos[equipo])
	json_object_set_number(t, "score_ct", gz_ctT[equipo][0])
	json_object_set_number(t, "score_t", gz_ctT[equipo][1])
	json_object_set_string(t, "side", gz_lado_de_equipo(equipo) == CS_TEAM_CT ? "ct" : "t")
	new CsTeams:inicial = equipo == 1 ? gz_ladoInicialT1 : (gz_ladoInicialT1 == CS_TEAM_CT ? CS_TEAM_T : CS_TEAM_CT)
	json_object_set_string(t, "starting_side", inicial == CS_TEAM_CT ? "ct" : "t")
	new JSON:jugadores = json_init_array()
	new TrieIter:it = TrieIterCreate(gz_stats)
	while (!TrieIterEnded(it)) {
		new sid[GZ_SID], st[ST_TOTAL]
		TrieIterGetKey(it, sid, charsmax(sid))
		TrieIterGetArray(it, st, ST_TOTAL)
		if (st[ST_TEAM] == equipo) {
			new nombre[32]
			TrieGetString(gz_nombres, sid, nombre, charsmax(nombre))
			new JSON:j = json_init_object()
			json_object_set_string(j, "steamid", sid)
			json_object_set_string(j, "name", nombre)
			new JSON:s = json_init_object()
			json_object_set_number(s, "kills", st[ST_KILLS])
			json_object_set_number(s, "deaths", st[ST_DEATHS])
			json_object_set_number(s, "assists", st[ST_ASSISTS])
			json_object_set_number(s, "damage", st[ST_DAMAGE])
			json_object_set_number(s, "headshot_kills", st[ST_HS])
			json_object_set_number(s, "mvp", st[ST_MVP])
			json_object_set_number(s, "kast", st[ST_KAST])
			json_object_set_number(s, "rounds_played", st[ST_ROUNDS])
			json_object_set_number(s, "bomb_plants", st[ST_PLANTS])
			json_object_set_number(s, "bomb_defuses", st[ST_DEFUSES])
			json_object_set_value(j, "stats", s)
			json_free(s)
			json_array_append_value(jugadores, j)
			json_free(j)
		}
		TrieIterNext(it)
	}
	TrieIterDestroy(it)
	json_object_set_value(t, "players", jugadores)
	json_free(jugadores)
	return t
}

// El mapa se gana con mas de la mitad de las rondas del tramo que se esta
// jugando: el tiempo reglamentario (regie_maxrounds) o, empatado, cada
// prorroga de regie_overtime_maxrounds (MR3: 15-15 y se gana con 19). Como en
// CS2, al empezar la prorroga no se cambia de lado: se cambia a la mitad.
stock gz_revisar_mapa() {
	new maximo = max(2, get_pcvar_num(gz_pcvarMaxRounds))
	new prorroga = max(2, get_pcvar_num(gz_pcvarProrrogaRondas) & ~1)
	new limite = maximo
	if (gz_ronda > maximo) limite = maximo + ((gz_ronda - maximo - 1) / prorroga + 1) * prorroga
	new ganador = 0
	if (gz_puntos[1] > limite / 2) ganador = 1
	else if (gz_puntos[2] > limite / 2) ganador = 2
	if (ganador) {
		gz_fin_mapa(ganador)
		return
	}
	if (gz_ronda >= limite) {
		if (get_pcvar_num(gz_pcvarProrroga) != 1) {
			gz_fin_mapa(0)
			return
		}
		gz_plataProrroga = true
		gz_decir(0, TX(27), gz_puntos[1], gz_puntos[2], (gz_ronda - maximo) / prorroga + 1)
		return
	}
	if (gz_ronda <= maximo) {
		if (gz_ronda == maximo / 2) gz_mitadPendiente = true
	} else if (gz_ronda == limite - prorroga / 2) {
		gz_mitadPendiente = true
		gz_plataProrroga = true
	}
}

stock gz_fin_mapa(ganador) {
	gz_estado = GS_POST
	new JSON:ev = json_init_object()
	json_object_set_number(ev, "map_number", gz_mapa)
	if (ganador) gz_serie[ganador]++
	gz_poner(ev, "team1", gz_equipo_json(1))
	gz_poner(ev, "team2", gz_equipo_json(2))
	gz_poner(ev, "winner", gz_ganador_json(ganador, ganador ? gz_lado_de_equipo(ganador) : CS_TEAM_UNASSIGNED))
	gz_enviar(ev, "map_result")
	if (ganador) gz_decir(0, TX(28), gz_nombre[ganador], gz_puntos[ganador], gz_puntos[3 - ganador])
	else gz_decir(0, TX(29), gz_puntos[1], gz_puntos[2])

	new necesarios = gz_numMaps / 2 + 1
	new jugados = gz_mapa + 1
	if (gz_serie[1] >= necesarios || gz_serie[2] >= necesarios || jugados >= gz_numMaps || jugados >= gz_cantMapas) {
		gz_fin_serie(false)
		return
	}
	gz_mapa++
	gz_guardar_estado()
	gz_decir(0, TX(30), gz_seriePrevia[1] + gz_serie[1], gz_seriePrevia[2] + gz_serie[2], gz_mapas[gz_mapa])
	set_task(15.0, "gz_task_cambiar_mapa", GZ_TASK_MAPA)
}

stock gz_fin_serie(bool:forzado) {
	new ganador = gz_serie[1] > gz_serie[2] ? 1 : gz_serie[2] > gz_serie[1] ? 2 : 0
	if (forzado) ganador = 0
	new JSON:ev = json_init_object()
	json_object_set_number(ev, "team1_series_score", gz_serie[1])
	json_object_set_number(ev, "team2_series_score", gz_serie[2])
	new JSON:w = json_init_object()
	json_object_set_string(w, "team", ganador == 1 ? "team1" : ganador == 2 ? "team2" : "none")
	json_object_set_value(ev, "winner", w)
	json_free(w)
	json_object_set_number(ev, "time_until_restore", 0)
	gz_enviar(ev, "series_end")
	if (forzado) gz_decir(0, TX(31))
	else gz_decir_serie()
	new bool:pug = gz_informal
	gz_terminar()
	// Un pug que termino jugando (no con regie_endmatch): se vota el mapa.
	if (pug && !forzado) gz_votacion_fin_pug()
}

// El marcador de la serie al terminar esta carga. En una serie retomada
// (cada mapa que falta es un Mejor de 1) se cuenta la serie entera: si
// todavia no esta definida, el mapa que sigue lo carga Regie.gg. Una serie de
// un mapa (tambien el informal) no dice nada: el mapa ya se dijo (TX 28/29).
stock gz_decir_serie() {
	new t1 = gz_seriePrevia[1] + gz_serie[1]
	new t2 = gz_seriePrevia[2] + gz_serie[2]
	new total = gz_numMapsSerie > 0 ? gz_numMapsSerie : gz_numMaps
	if (total <= 1) return
	new necesarios = total / 2 + 1
	// Solo una serie retomada mapa a mapa puede quedar sin definir aca.
	new bool:definida = gz_numMapsSerie == 0 || t1 >= necesarios || t2 >= necesarios || t1 + t2 >= total
	if (!definida) {
		if (t1 == t2) gz_decir(0, TX(32), t1, t2)
		else gz_decir(0, TX(33), gz_nombre[t1 > t2 ? 1 : 2], max(t1, t2), min(t1, t2))
		return
	}
	if (t1 == t2) gz_decir(0, TX(34), gz_nombre[1], gz_nombre[2], t1, t2)
	else gz_decir(0, TX(35), gz_nombre[t1 > t2 ? 1 : 2], max(t1, t2), min(t1, t2))
}

// Vuelve todo a como estaba antes de cargar el partido: las cvars de la foto
// (mp_timelimit, mp_maxrounds y mp_winlimit un rato despues) y el nombre del
// servidor. Despues, con el informal, el calentamiento libre.
stock gz_terminar() {
	gz_estado = GS_NONE
	gz_cantAus = 0
	remove_task(GZ_TASK_AVISO)
	remove_task(GZ_TASK_MAPA)
	gz_devolver_foto(true)
	gz_borrar_partido()
	TrieClear(gz_plantel)
	if (gz_hostnameOriginal[0]) set_cvar_string("hostname", gz_hostnameOriginal)
	gz_nombre[1][0] = 0
	gz_nombre[2][0] = 0
	gz_matchid = 0
	gz_freno = false
	gz_informal = false
	gz_rondaPendiente = -1
	gz_restaurando = false
	gz_vetoPendiente = false
	gz_vetoHecho = false
	gz_ladoPendiente = false
	remove_task(GZ_TASK_VETO)
	remove_task(GZ_TASK_VETOBOT)
	gz_terminar_pausa()
	TrieClear(gz_plata)
	for (new i = 1; i <= 32; i++) gz_listo[i] = false
	// La foto del calentamiento libre se vuelve a sacar con lo recien devuelto.
	gz_borrar_archivo("calentamiento.json")
	gz_libre = false
	if (get_pcvar_num(gz_pcvarInformal) == 1) gz_calentamiento_libre(true)
	// El marcador del partido no queda en el calentamiento: vuelve a cero
	// cuando termina el post partido.
	remove_task(GZ_TASK_MARCADOR)
	if (gz_libre) set_task(GZ_DEMORA_MARCADOR, "gz_task_marcador_a_cero", GZ_TASK_MARCADOR)
	// El recordatorio sigue para el informal que alguien arme despues.
	gz_poner_aviso()
}

// El marcador (y frags, muertes y plata) a cero en el calentamiento libre,
// con un sv_restartround como el que hace el juego. Lo que el reinicio
// tambien pone en cero (el reloj de mp_timelimit y las rondas jugadas) se
// devuelve en gz_reinicio_post: la rotacion de mapas sigue igual.
public gz_task_marcador_a_cero() {
	if (gz_estado != GS_NONE || !gz_libre) return
	gz_inicioJuego = Float:get_member_game(m_flGameStartTime)
	gz_limiteTiempo = Float:get_member_game(m_flTimeLimit)
	gz_rondasJugadas = get_member_game(m_iTotalRoundsPlayed)
	gz_marcadorACero = true
	gz_decir(0, TX(167))
	server_cmd("sv_restartround 1")
}

// --------------------------------------------------------------------
// Votacion de mapa (solo sin partido de Regie.gg cargado)
// --------------------------------------------------------------------
//
// Un menu con mapas del mapcycle del servidor (sin el actual): el que le
// sigue al actual y otros al azar, hasta regie_mapvote_maps, y "seguir en
// este mapa". Dura regie_mapvote_time segundos. Gana el mas votado; empate, al
// azar entre los empatados; nadie vota, el que sigue en el mapcycle. Se abre
// al terminar un pug (regie_pug_mapvote), con .rtv de mas de la mitad de los
// jugadores (regie_rtv_enabled) o con regie_mapvote (la consola). Un partido
// que se carga (o un pug que arranca) la cancela.

// Los mapas del mapcycle que existen, sin repetir y en orden. Cuantos.
stock gz_leer_mapcycle() {
	new archivo[64]
	get_cvar_string("mapcyclefile", archivo, charsmax(archivo))
	if (!archivo[0]) copy(archivo, charsmax(archivo), "mapcycle.txt")
	new f = fopen(archivo, "rt")
	if (!f) return 0
	new cant, linea[128], mapa[GZ_MAPA]
	while (cant < GZ_CICLO_MAX && !feof(f)) {
		fgets(f, linea, charsmax(linea))
		new c = contain(linea, "//")
		if (c != -1) linea[c] = 0
		trim(linea)
		if (!linea[0]) continue
		parse(linea, mapa, charsmax(mapa))
		if (!mapa[0] || !is_map_valid(mapa)) continue
		new bool:repetido = false
		for (new i = 0; i < cant; i++) {
			if (equali(gz_ciclo[i], mapa)) {
				repetido = true
				break
			}
		}
		if (!repetido) copy(gz_ciclo[cant++], GZ_MAPA - 1, mapa)
	}
	fclose(f)
	return cant
}

// Las opciones de la votacion. false si el mapcycle no tiene otro mapa que el
// actual (entonces no se vota: el panel hace lo de siempre).
stock bool:gz_armar_votacion() {
	new cant = gz_leer_mapcycle()
	new actual[GZ_MAPA]
	get_mapname(actual, charsmax(actual))
	// Los otros, en ronda desde el que sigue al actual.
	new desde = 0
	for (new i = 0; i < cant; i++) {
		if (equali(gz_ciclo[i], actual)) {
			desde = i + 1
			break
		}
	}
	new cantOtros
	for (new k = 0; k < cant; k++) {
		new i = (desde + k) % cant
		if (!equali(gz_ciclo[i], actual)) copy(gz_otros[cantOtros++], GZ_MAPA - 1, gz_ciclo[i])
	}
	if (!cantOtros) return false
	copy(gz_votoSiguiente, charsmax(gz_votoSiguiente), gz_otros[0])
	// El que sigue queda primero; los demas, mezclados (Fisher-Yates).
	new tmp[GZ_MAPA]
	for (new i = cantOtros - 1; i > 1; i--) {
		new j = random_num(1, i)
		copy(tmp, charsmax(tmp), gz_otros[i])
		copy(gz_otros[i], GZ_MAPA - 1, gz_otros[j])
		copy(gz_otros[j], GZ_MAPA - 1, tmp)
	}
	gz_votoCant = min(clamp(get_pcvar_num(gz_pcvarVotoMapas), 1, GZ_VOTO_MAX), cantOtros)
	for (new i = 0; i < gz_votoCant; i++) copy(gz_votoMapas[i], GZ_MAPA - 1, gz_otros[i])
	return true
}

stock gz_humanos() {
	new jugadores[32], n
	get_players(jugadores, n, "ch")
	return n
}

stock gz_contar_rtv() {
	new cuantos
	new jugadores[32], n
	get_players(jugadores, n, "ch")
	for (new i = 0; i < n; i++) if (gz_rtv[jugadores[i]]) cuantos++
	return cuantos
}

stock gz_olvidar_rtv() {
	for (new i = 1; i <= 32; i++) gz_rtv[i] = false
}

stock bool:gz_votacion_en_curso() {
	return gz_voto == VT_VOTANDO || gz_voto == VT_CAMBIA
}

// El menu a todos los jugadores (gz_armar_votacion ya armo las opciones).
stock gz_abrir_votacion() {
	gz_cerrar_menu_voto()
	new titulo[64]
	formatex(titulo, charsmax(titulo), "\y%s", TX(183))
	gz_votoMenu = menu_create(titulo, "gz_menu_voto")
	for (new i = 0; i < gz_votoCant; i++) menu_additem(gz_votoMenu, gz_votoMapas[i])
	menu_additem(gz_votoMenu, TX(184))
	// Sin paginas ni "salir": todas las opciones en una sola pantalla.
	menu_setprop(gz_votoMenu, MPROP_PERPAGE, 0)
	menu_setprop(gz_votoMenu, MPROP_EXIT, MEXIT_NEVER)
	for (new i = 0; i <= GZ_VOTO_MAX; i++) gz_votos[i] = 0
	for (new i = 1; i <= 32; i++) gz_votoDe[i] = -1
	gz_olvidar_rtv()
	new tiempo = clamp(get_pcvar_num(gz_pcvarVotoTiempo), 5, 120)
	gz_votoHasta = get_gametime() + float(tiempo)
	gz_votoAbreEn = 0.0
	gz_voto = VT_VOTANDO
	new jugadores[32], n
	get_players(jugadores, n, "ch")
	for (new i = 0; i < n; i++) menu_display(jugadores[i], gz_votoMenu, 0, tiempo)
	gz_decir(0, TX(170), tiempo)
	remove_task(GZ_TASK_VOTO_FIN)
	set_task(float(tiempo), "gz_task_fin_voto", GZ_TASK_VOTO_FIN)
}

public gz_menu_voto(id, menu, item) {
	if (gz_voto != VT_VOTANDO || menu != gz_votoMenu || item < 0 || item > gz_votoCant) return PLUGIN_HANDLED
	if (gz_votoDe[id] != -1) return PLUGIN_HANDLED
	gz_votoDe[id] = item
	gz_votos[item]++
	new nombre[32]
	get_user_name(id, nombre, charsmax(nombre))
	if (item == gz_votoCant) gz_decir(0, TX(192), nombre)
	else gz_decir(0, TX(171), nombre, gz_votoMapas[item])
	return PLUGIN_HANDLED
}

// Saca el menu de la pantalla de todos y lo destruye.
stock gz_cerrar_menu_voto() {
	remove_task(GZ_TASK_VOTO_FIN)
	if (gz_votoMenu == -1) return
	new menu = gz_votoMenu
	// Antes de cancelar: el handler ya no lo toma como un voto.
	gz_votoMenu = -1
	new jugadores[32], n
	get_players(jugadores, n, "ch")
	for (new i = 0; i < n; i++) {
		new viejo, nuevo
		player_menu_info(jugadores[i], viejo, nuevo)
		if (nuevo != menu) continue
		menu_cancel(jugadores[i])
		show_menu(jugadores[i], 0, "^n", 1)
	}
	menu_destroy(menu)
}

public gz_task_fin_voto() {
	if (gz_voto != VT_VOTANDO) return
	gz_cerrar_menu_voto()
	if (gz_estado != GS_NONE) {
		gz_voto = VT_NADA
		return
	}
	new actual[GZ_MAPA]
	get_mapname(actual, charsmax(actual))
	new mejor = -1, total, empatados[GZ_VOTO_MAX + 1], cantEmp
	for (new i = 0; i <= gz_votoCant; i++) {
		total += gz_votos[i]
		if (gz_votos[i] > mejor) {
			mejor = gz_votos[i]
			cantEmp = 0
		}
		if (gz_votos[i] == mejor) empatados[cantEmp++] = i
	}
	new elegido
	if (!total) {
		// Nadie voto: el que sigue en el mapcycle (la primera opcion).
		elegido = 0
		gz_decir(0, TX(174), gz_votoSiguiente)
	} else {
		elegido = empatados[cantEmp > 1 ? random_num(0, cantEmp - 1) : 0]
		new etiqueta[GZ_MAPA]
		copy(etiqueta, charsmax(etiqueta), elegido == gz_votoCant ? actual : gz_votoMapas[elegido])
		if (cantEmp > 1) gz_decir(0, TX(173), mejor, etiqueta)
		else gz_decir(0, TX(172), etiqueta, mejor)
	}
	if (elegido == gz_votoCant) {
		gz_voto = VT_SIGUE
		gz_decir(0, TX(175), actual)
		return
	}
	copy(gz_votoGanador, charsmax(gz_votoGanador), gz_votoMapas[elegido])
	gz_voto = VT_CAMBIA
	gz_decir(0, TX(4), gz_votoGanador)
	remove_task(GZ_TASK_VOTO_MAPA)
	gz_votoCambioEn = get_gametime() + GZ_DEMORA_CAMBIO
	set_task(GZ_DEMORA_CAMBIO, "gz_task_cambio_voto", GZ_TASK_VOTO_MAPA)
}

public gz_task_cambio_voto() {
	if (gz_voto != VT_CAMBIA || gz_estado != GS_NONE || !gz_votoGanador[0]) return
	server_cmd("changelevel %s", gz_votoGanador)
}

// Un partido que se carga (o un pug que arranca): la votacion no sigue.
stock gz_cancelar_votacion(bool:avisar) {
	new bool:habia = gz_votacion_en_curso()
	remove_task(GZ_TASK_VOTO)
	remove_task(GZ_TASK_VOTO_MAPA)
	gz_cerrar_menu_voto()
	gz_voto = VT_NADA
	gz_olvidar_rtv()
	if (habia && avisar) gz_decir(0, TX(191))
}

// Al terminar un pug: el resultado se lee unos segundos y despues el menu.
// Queda "voting" desde ya, para que el panel no cambie de mapa mientras.
stock gz_votacion_fin_pug() {
	if (get_pcvar_num(gz_pcvarVotoPug) != 1 || gz_votacion_en_curso()) return
	if (!gz_armar_votacion()) return
	gz_voto = VT_VOTANDO
	gz_votoAbreEn = get_gametime() + GZ_DEMORA_VOTO
	remove_task(GZ_TASK_VOTO)
	set_task(GZ_DEMORA_VOTO, "gz_task_abrir_voto", GZ_TASK_VOTO)
}

public gz_task_abrir_voto() {
	if (gz_voto != VT_VOTANDO || gz_votoMenu != -1) return
	if (gz_estado != GS_NONE || !gz_humanos()) {
		// Se cargo algo o se fueron todos: el panel hace lo de siempre.
		gz_voto = VT_NADA
		return
	}
	gz_abrir_votacion()
}

// .rtv / .votemap: como rock the vote, se abre cuando lo pide mas de la mitad
// de los jugadores conectados (sin bots).
stock gz_pedir_rtv(id) {
	if (gz_estado != GS_NONE) {
		gz_decir(id, TX(178))
		return
	}
	if (get_pcvar_num(gz_pcvarRtv) != 1) {
		gz_decir(id, TX(180))
		return
	}
	if (gz_votacion_en_curso()) {
		gz_decir(id, TX(179))
		return
	}
	new necesarios = gz_humanos() / 2 + 1
	if (gz_rtv[id]) {
		gz_decir(id, TX(177), gz_contar_rtv(), necesarios)
		return
	}
	gz_rtv[id] = true
	new cuantos = gz_contar_rtv()
	if (cuantos < necesarios) {
		new nombre[32]
		get_user_name(id, nombre, charsmax(nombre))
		gz_decir(0, TX(176), nombre, cuantos, necesarios)
		return
	}
	if (!gz_armar_votacion()) {
		gz_olvidar_rtv()
		gz_decir(0, TX(181))
		return
	}
	gz_abrir_votacion()
}

// regie_mapvote (RCON, el boton de la consola): la abre ya.
public gz_cmd_mapvote() {
	if (gz_estado != GS_NONE) server_print(TX(187))
	else if (gz_votacion_en_curso()) server_print(TX(188))
	else if (!gz_humanos()) server_print(TX(190))
	else if (!gz_armar_votacion()) server_print(TX(189))
	else {
		gz_abrir_votacion()
		server_print(TX(186))
	}
	return PLUGIN_HANDLED
}

// --------------------------------------------------------------------
// Veto de mapas (como MapVeto.cs de MatchZy)
// --------------------------------------------------------------------

// El lado de un mapa que nadie elige, segun side_type: "standard" y
// "always_knife" van a cuchillo, "never_knife" team1 de CT, "random" al azar.
stock gz_lado_automatico(lado[], len) {
	if (equal(gz_tipoLados, "random")) copy(lado, len, random_num(0, 1) ? "team1_ct" : "team1_t")
	else if (equal(gz_tipoLados, "never_knife")) copy(lado, len, "team1_ct")
	else copy(lado, len, "knife")
}

// veto_mode del match config tal cual o, si no viene, el orden de fabrica
// de MatchZy (GenerateDefaultVetoSetup), empezando por veto_first.
stock gz_armar_orden(JSON:cfg) {
	gz_cantOrden = 0
	new JSON:modo = json_object_get_value(cfg, "veto_mode")
	if (modo != Invalid_JSON) {
		new n = min(GZ_MAX_POOL, json_array_get_count(modo))
		for (new i = 0; i < n; i++) {
			new paso[12]
			json_array_get_string(modo, i, paso, charsmax(paso))
			if (equal(paso, "team1_ban") || equal(paso, "team2_ban") || equal(paso, "team1_pick") || equal(paso, "team2_pick")) copy(gz_orden[gz_cantOrden++], charsmax(gz_orden[]), paso)
		}
		json_free(modo)
		if (gz_cantOrden) return
	}
	new primero[8] = "team1"
	if (json_object_has_value(cfg, "veto_first")) json_object_get_string(cfg, "veto_first", primero, charsmax(primero))
	if (equal(primero, "random")) copy(primero, charsmax(primero), random_num(0, 1) ? "team1" : "team2")
	new bool:empiezaT1 = !equal(primero, "team2")
	if (gz_numMaps == 1) {
		for (new i = 0; i < gz_cantPool - 1; i++) gz_paso_orden(i % 2 == 0, empiezaT1, false)
	} else if (gz_numMaps == 2) {
		if (gz_cantPool >= 5) {
			gz_paso_orden(true, empiezaT1, false)
			gz_paso_orden(false, empiezaT1, false)
		}
		gz_paso_orden(true, empiezaT1, true)
		gz_paso_orden(false, empiezaT1, true)
	} else if (gz_cantPool >= gz_numMaps + 2) {
		// Mejor de 3 con 7 mapas: 2 bans, 2 picks, 2 bans y el que queda.
		new picks = gz_numMaps - 1
		new bansAntes = gz_cantPool - (gz_numMaps + 2)
		for (new i = 0; i < bansAntes; i++) gz_paso_orden(gz_cantOrden % 2 == 0, empiezaT1, false)
		for (new i = 0; i < picks; i++) gz_paso_orden(gz_cantOrden % 2 == 0, empiezaT1, true)
		new bansDespues = gz_cantPool - 1 - picks - bansAntes
		for (new i = 0; i < bansDespues; i++) gz_paso_orden(gz_cantOrden % 2 == 0, empiezaT1, false)
	} else {
		for (new i = 0; i < gz_numMaps; i++) gz_paso_orden(i % 2 == 0, empiezaT1, true)
	}
}

// Agrega un paso: el del que empieza (primero = true) o el del otro.
stock gz_paso_orden(bool:primero, bool:empiezaT1, bool:pick) {
	if (gz_cantOrden >= GZ_MAX_POOL) return
	new bool:t1 = primero == empiezaT1
	formatex(gz_orden[gz_cantOrden++], charsmax(gz_orden[]), "%s_%s", t1 ? "team1" : "team2", pick ? "pick" : "ban")
}

stock gz_quedan_cuantos() {
	new n
	for (new i = 0; i < gz_cantPool; i++) if (gz_quedan[i]) n++
	return n
}

// El paso que toca: "team1_ban", "team2_pick"... o "" si el orden no tiene mas.
stock gz_paso_actual(paso[], len) {
	new vetados = gz_cantPool - (gz_quedan_cuantos() + gz_cantMapas)
	new i = gz_cantMapas + vetados
	if (i >= gz_cantOrden) {
		paso[0] = 0
		return
	}
	copy(paso, len, gz_orden[i])
}

stock gz_lista_quedan(out[], len) {
	out[0] = 0
	for (new i = 0; i < gz_cantPool; i++) {
		if (!gz_quedan[i]) continue
		if (out[0]) add(out, len, ", ")
		add(out, len, gz_pool[i])
	}
}

stock gz_empezar_preveto() {
	gz_estado = GS_PREVETO
	gz_ladoInicialT1 = CS_TEAM_CT
	gz_ladoT1 = CS_TEAM_CT
	for (new i = 1; i <= 32; i++) gz_listo[i] = false
	gz_exec_calentamiento(true, true)
	gz_acomodar_todos()
	gz_poner_aviso()
	gz_decir(0, TX(36), gz_numMaps)
}

stock gz_empezar_veto() {
	if (gz_estado != GS_PREVETO) return
	remove_task(GZ_TASK_AVISO)
	gz_estado = GS_VETO
	gz_vetoCuenta = 5
	remove_task(GZ_TASK_VETO)
	set_task(1.0, "gz_task_veto_cuenta", GZ_TASK_VETO, _, _, "b")
}

public gz_task_veto_cuenta() {
	if (gz_estado != GS_VETO) {
		remove_task(GZ_TASK_VETO)
		return
	}
	if (gz_vetoCuenta > 0) {
		gz_decir(0, TX(37), gz_vetoCuenta--)
		return
	}
	remove_task(GZ_TASK_VETO)
	for (new e = 1; e <= 2; e++) {
		if (!gz_asegurar_capitan(e, false)) {
			gz_abortar_veto()
			return
		}
	}
	for (new e = 1; e <= 2; e++) {
		new nombre[32]
		get_user_name(gz_capitan[e], nombre, charsmax(nombre))
		gz_decir(0, TX(38), gz_nombre[e], nombre)
	}
	gz_paso_veto()
}

// El capitan del equipo: el de antes si sigue en el equipo; si no, el
// primero que haya (con regie_bots_libres, tambien un bot). 0 si no hay nadie.
stock gz_asegurar_capitan(equipo, bool:avisar) {
	new actual = gz_capitan[equipo]
	if (actual && is_user_connected(actual) && gz_equipo_que_juega(actual) == equipo) return actual
	new nuevo = 0
	new jugadores[32], n
	get_players(jugadores, n, "ch")
	for (new i = 0; i < n && !nuevo; i++) if (gz_equipo_que_juega(jugadores[i]) == equipo) nuevo = jugadores[i]
	if (!nuevo && get_pcvar_num(gz_pcvarBotsLibres) == 1) {
		get_players(jugadores, n, "dh")
		for (new i = 0; i < n && !nuevo; i++) if (gz_equipo_que_juega(jugadores[i]) == equipo) nuevo = jugadores[i]
	}
	gz_capitan[equipo] = nuevo
	if (nuevo && avisar) {
		new nombre[32]
		get_user_name(nuevo, nombre, charsmax(nombre))
		gz_decir(0, TX(39), gz_nombre[equipo], nombre)
	}
	return nuevo
}

// Como HandleVetoStep: lado pendiente, el paso que toca o el final.
stock gz_paso_veto() {
	if (gz_estado != GS_VETO) return
	if (gz_ladoPendiente) {
		new equipo = 3 - gz_ultimoEnElegir
		gz_decir(0, TX(40), gz_nombre[equipo], gz_mapas[gz_cantMapas - 1])
		new capitan = gz_asegurar_capitan(equipo, true)
		if (!capitan) {
			gz_abortar_veto()
			return
		}
		gz_decir(capitan, TX(41))
		if (is_user_bot(capitan)) set_task(2.0, "gz_task_veto_bot", GZ_TASK_VETOBOT)
		return
	}
	if (gz_cantMapas >= gz_numMaps) {
		gz_terminar_veto()
		return
	}
	new paso[12]
	gz_paso_actual(paso, charsmax(paso))
	// Queda uno solo, o el orden no tiene mas pasos: los que faltan salen
	// por descarte, en el orden del pool, con su lado automatico.
	if (gz_quedan_cuantos() == 1 || !paso[0]) {
		for (new i = 0; i < gz_cantPool && gz_cantMapas < gz_numMaps; i++) {
			if (!gz_quedan[i]) continue
			gz_quedan[i] = false
			gz_agregar_mapa(i, 0)
			gz_lado_automatico(gz_lados[gz_cantMapas - 1], charsmax(gz_lados[]))
		}
		gz_terminar_veto()
		return
	}
	new equipo = paso[4] == '1' ? 1 : 2
	new bool:pick = contain(paso, "pick") != -1
	new capitan = gz_asegurar_capitan(equipo, true)
	if (!capitan) {
		gz_abortar_veto()
		return
	}
	if (pick) gz_decir(0, TX(42), gz_nombre[equipo], gz_cantMapas + 1)
	else gz_decir(0, TX(43), gz_nombre[equipo])
	new lista[192]
	gz_lista_quedan(lista, charsmax(lista))
	gz_decir(0, TX(44), lista)
	gz_decir(capitan, pick ? TX(115) : TX(116))
	if (is_user_bot(capitan)) set_task(2.0, "gz_task_veto_bot", GZ_TASK_VETOBOT)
}

// Busca el mapa en lo que queda: primero por una parte del nombre (4 letras
// o mas, si coincide uno solo), despues por el nombre entero. -1 si no.
stock gz_buscar_mapa(const nombre[]) {
	new encontrado = -1
	if (strlen(nombre) >= 4) {
		for (new i = 0; i < gz_cantPool; i++) {
			if (!gz_quedan[i] || containi(gz_pool[i], nombre) == -1) continue
			if (encontrado >= 0) {
				encontrado = -1
				break
			}
			encontrado = i
		}
	}
	if (encontrado == -1) {
		for (new i = 0; i < gz_cantPool; i++) if (gz_quedan[i] && equali(gz_pool[i], nombre)) return i
	}
	return encontrado
}

// Pasa el mapa i del pool a la serie. equipo 0: por descarte (como MatchZy,
// el evento dice team1).
stock gz_agregar_mapa(i, equipo) {
	copy(gz_mapas[gz_cantMapas], charsmax(gz_mapas[]), gz_pool[i])
	gz_lados[gz_cantMapas][0] = 0
	gz_cantMapas++
	new JSON:ev = json_init_object()
	json_object_set_string(ev, "map_name", gz_pool[i])
	json_object_set_number(ev, "map_number", gz_cantMapas - 1)
	json_object_set_string(ev, "team", equipo == 2 ? "team2" : "team1")
	gz_enviar(ev, "map_picked")
}

stock gz_vetar(id, const mapa[], bool:pick) {
	if (gz_ladoPendiente || task_exists(GZ_TASK_VETO)) return
	new paso[12]
	gz_paso_actual(paso, charsmax(paso))
	if (!paso[0]) return
	new equipo = paso[4] == '1' ? 1 : 2
	if (gz_equipo_que_juega(id) != equipo || gz_asegurar_capitan(equipo, false) != id) return
	if (pick != (contain(paso, "pick") != -1)) {
		gz_decir(id, pick ? TX(117) : TX(118))
		return
	}
	new nombre[GZ_MAPA]
	copy(nombre, charsmax(nombre), mapa)
	trim(nombre)
	new i = gz_buscar_mapa(nombre)
	if (i == -1) {
		gz_decir(id, TX(45), nombre)
		return
	}
	remove_task(GZ_TASK_VETOBOT)
	gz_quedan[i] = false
	if (pick) {
		gz_decir(0, TX(46), gz_nombre[equipo], gz_pool[i], gz_cantMapas + 1)
		gz_agregar_mapa(i, equipo)
		gz_ultimoEnElegir = equipo
		if (equal(gz_tipoLados, "standard")) gz_ladoPendiente = true
		else gz_lado_automatico(gz_lados[gz_cantMapas - 1], charsmax(gz_lados[]))
	} else {
		gz_decir(0, TX(47), gz_nombre[equipo], gz_pool[i])
		new JSON:ev = json_init_object()
		json_object_set_string(ev, "map_name", gz_pool[i])
		json_object_set_string(ev, "team", equipo == 1 ? "team1" : "team2")
		gz_enviar(ev, "map_vetoed")
	}
	gz_paso_veto()
}

stock gz_lado_del_veto(id, CsTeams:lado) {
	if (!gz_ladoPendiente) return
	new equipo = 3 - gz_ultimoEnElegir
	if (gz_equipo_que_juega(id) != equipo || gz_asegurar_capitan(equipo, false) != id) return
	remove_task(GZ_TASK_VETOBOT)
	new m = gz_cantMapas - 1
	new bool:t1ct = (equipo == 1) == (lado == CS_TEAM_CT)
	copy(gz_lados[m], charsmax(gz_lados[]), t1ct ? "team1_ct" : "team1_t")
	gz_ladoPendiente = false
	gz_decir(0, TX(48), gz_nombre[equipo], lado == CS_TEAM_CT ? "CT" : "T", gz_mapas[m])
	new JSON:ev = json_init_object()
	json_object_set_string(ev, "map_name", gz_mapas[m])
	json_object_set_number(ev, "map_number", m)
	json_object_set_string(ev, "team", equipo == 1 ? "team1" : "team2")
	json_object_set_string(ev, "side", lado == CS_TEAM_CT ? "ct" : "t")
	gz_enviar(ev, "side_picked")
	gz_paso_veto()
}

// Un capitan bot (regie_bots_libres, para probar) no escribe: elige el
// primer mapa que queda y CT.
public gz_task_veto_bot() {
	if (gz_estado != GS_VETO) return
	if (gz_ladoPendiente) {
		gz_lado_del_veto(gz_capitan[3 - gz_ultimoEnElegir], CS_TEAM_CT)
		return
	}
	new paso[12]
	gz_paso_actual(paso, charsmax(paso))
	if (!paso[0]) return
	new equipo = paso[4] == '1' ? 1 : 2
	for (new i = 0; i < gz_cantPool; i++) {
		if (!gz_quedan[i]) continue
		gz_vetar(gz_capitan[equipo], gz_pool[i], contain(paso, "pick") != -1)
		return
	}
}

stock gz_abortar_veto() {
	remove_task(GZ_TASK_VETO)
	remove_task(GZ_TASK_VETOBOT)
	gz_decir(0, TX(49))
	gz_decir(0, TX(50))
	gz_capitan[1] = 0
	gz_capitan[2] = 0
	gz_empezar_preveto()
}

stock gz_terminar_veto() {
	remove_task(GZ_TASK_VETOBOT)
	gz_decir(0, TX(51))
	for (new i = 0; i < gz_cantMapas; i++) gz_decir(0, TX(52), i + 1, gz_mapas[i])
	gz_vetoPendiente = false
	gz_vetoHecho = true
	gz_mapa = 0
	gz_estado = GS_WARMUP
	gz_guardar_estado()
	// Como MatchZy: unos segundos para leerlo y al primer mapa (o al
	// calentamiento, si ya se esta en ese mapa).
	set_task(7.0, "gz_task_tras_veto", GZ_TASK_VETO)
}

public gz_task_tras_veto() {
	if (gz_estado != GS_WARMUP || gz_vetoPendiente) return
	gz_ir_al_mapa_o_calentar()
}

// --------------------------------------------------------------------
// Respaldos de ronda (como los de MatchZy)
// --------------------------------------------------------------------

stock gz_dir_respaldos(path[], len) {
	gz_dir_datos(path, len)
	add(path, len, "/respaldos")
	if (!dir_exists(path)) mkdir(path)
}

// data/regiegg/respaldos/, los de un plugin anterior al 1.19.0: no se mueven
// (no se sabe de que servidor eran), se siguen listando y cargando desde ahi.
stock bool:gz_dir_respaldos_viejos(path[], len) {
	gz_dir_base(path, len)
	add(path, len, "/respaldos")
	return dir_exists(path) ? true : false
}

stock gz_archivo_respaldo(ronda, path[], len) {
	gz_dir_respaldos(path, len)
	new nombre[64]
	formatex(nombre, charsmax(nombre), "/regie_%d_%d_round%02d.json", gz_matchid, gz_mapa, ronda)
	add(path, len, nombre)
}

public gz_task_respaldo() {
	if (gz_estado == GS_LIVE) gz_guardar_respaldo()
}

// Al empezar cada ronda: lo necesario para volver a jugarla desde el
// principio. NN son las rondas ya jugadas, como en MatchZy (round14 con
// 12-2 arranca la ronda 15).
stock gz_guardar_respaldo() {
	new path[256], datos[256]
	gz_archivo_respaldo(gz_ronda, path, charsmax(path))
	gz_archivo("partido.json", datos, charsmax(datos))
	new JSON:r = json_init_object()
	json_object_set_number(r, "matchid", gz_matchid)
	json_object_set_number(r, "mapnumber", gz_mapa)
	json_object_set_number(r, "round", gz_ronda)
	json_object_set_string(r, "map_name", gz_mapas[gz_mapa])
	json_object_set_string(r, "team1_name", gz_nombre[1])
	json_object_set_string(r, "team2_name", gz_nombre[2])
	json_object_set_number(r, "team1_score", gz_puntos[1])
	json_object_set_number(r, "team2_score", gz_puntos[2])
	json_object_set_string(r, "team1_side", gz_ladoT1 == CS_TEAM_CT ? "ct" : "t")
	json_object_set_string(r, "team2_side", gz_ladoT1 == CS_TEAM_CT ? "t" : "ct")
	json_object_set_number(r, "team1_series_score", gz_serie[1])
	json_object_set_number(r, "team2_series_score", gz_serie[2])
	new fecha[24]
	format_time(fecha, charsmax(fecha), "%Y-%m-%d %H:%M:%S")
	json_object_set_string(r, "fecha", fecha)
	new JSON:cfg = json_parse(datos, true)
	if (cfg != Invalid_JSON) gz_poner(r, "match_config", cfg)
	// Lo que en MatchZy es el respaldo de ronda del juego: que no este vacio
	// es lo que dice "tiene los datos de la ronda" (respaldosMatchzy.ts).
	json_object_set_string(r, "valve_backup", "regie")

	new JSON:regie = json_init_object()
	json_object_set_string(regie, "lado_inicial_t1", gz_ladoInicialT1 == CS_TEAM_CT ? "ct" : "t")
	new JSON:nums = json_init_array()
	for (new e = 1; e <= 2; e++) {
		json_array_append_number(nums, gz_ctT[e][0])
		json_array_append_number(nums, gz_ctT[e][1])
	}
	gz_poner(regie, "ct_t", nums)
	nums = json_init_array()
	json_array_append_number(nums, gz_tecnicasUsadas[1])
	json_array_append_number(nums, gz_tecnicasUsadas[2])
	json_array_append_number(nums, gz_tacticasUsadas[1])
	json_array_append_number(nums, gz_tacticasUsadas[2])
	gz_poner(regie, "pausas", nums)

	new JSON:stats = json_init_object()
	new JSON:nombres = json_init_object()
	new TrieIter:it = TrieIterCreate(gz_stats)
	while (!TrieIterEnded(it)) {
		new sid[GZ_SID], st[ST_TOTAL], nombre[32]
		TrieIterGetKey(it, sid, charsmax(sid))
		TrieIterGetArray(it, st, ST_TOTAL)
		new JSON:a = json_init_array()
		for (new k = 0; k < ST_TOTAL; k++) json_array_append_number(a, st[k])
		gz_poner(stats, sid, a)
		TrieGetString(gz_nombres, sid, nombre, charsmax(nombre))
		json_object_set_string(nombres, sid, nombre)
		TrieIterNext(it)
	}
	TrieIterDestroy(it)
	gz_poner(regie, "stats", stats)
	gz_poner(regie, "nombres", nombres)

	new JSON:todos = json_init_object()
	new jugadores[32], n
	get_players(jugadores, n, "h")
	for (new i = 0; i < n; i++) {
		new id = jugadores[i]
		new CsTeams:lado = CsTeams:get_member(id, m_iTeam)
		if (lado != CS_TEAM_CT && lado != CS_TEAM_T) continue
		new sid[GZ_SID]
		gz_steam64(id, sid, charsmax(sid))
		new JSON:j = json_init_object()
		json_object_set_number(j, "plata", get_member(id, m_iAccount))
		new CsArmorType:tipo
		json_object_set_number(j, "armor", cs_get_user_armor(id, tipo))
		json_object_set_bool(j, "casco", tipo == CS_ARMOR_VESTHELM)
		json_object_set_bool(j, "kit", bool:get_member(id, m_bHasDefuser))
		new JSON:armas = json_init_array()
		if (is_user_alive(id)) {
			new ids[32], cuantas
			get_user_weapons(id, ids, cuantas)
			for (new w = 0; w < cuantas; w++) {
				if (ids[w] == CSW_KNIFE) continue
				new arma[32], item[48]
				get_weaponname(ids[w], arma, charsmax(arma))
				formatex(item, charsmax(item), "%s:%d", arma, rg_get_user_bpammo(id, WeaponIdType:ids[w]))
				json_array_append_string(armas, item)
			}
		}
		gz_poner(j, "armas", armas)
		gz_poner(todos, sid, j)
	}
	gz_poner(regie, "jugadores", todos)
	gz_poner(r, "regie", regie)
	json_serial_to_file(r, path, true)
	json_free(r)
}

// Vuelve al principio de la ronda `ronda` + 1 con lo del respaldo: el
// marcador, los lados, las estadisticas y, despues del reinicio, la plata y
// las armas de cada uno (gz_task_reponer). false si no hay respaldo de esa
// ronda para este partido y este mapa.
stock bool:gz_aplicar_respaldo(ronda) {
	new path[256]
	gz_archivo_respaldo(ronda, path, charsmax(path))
	if (!file_exists(path)) return false
	new JSON:r = json_parse(path, true)
	if (r == Invalid_JSON) return false
	if (json_object_get_number(r, "matchid") != gz_matchid || json_object_get_number(r, "mapnumber") != gz_mapa) {
		json_free(r)
		return false
	}
	gz_ronda = ronda
	gz_puntos[1] = json_object_get_number(r, "team1_score")
	gz_puntos[2] = json_object_get_number(r, "team2_score")
	new lado[4]
	json_object_get_string(r, "team1_side", lado, charsmax(lado))
	gz_ladoT1 = equal(lado, "t") ? CS_TEAM_T : CS_TEAM_CT
	new JSON:regie = json_object_get_value(r, "regie")
	if (regie != Invalid_JSON) {
		json_object_get_string(regie, "lado_inicial_t1", lado, charsmax(lado))
		gz_ladoInicialT1 = equal(lado, "t") ? CS_TEAM_T : CS_TEAM_CT
		new JSON:nums = json_object_get_value(regie, "ct_t")
		if (nums != Invalid_JSON) {
			gz_ctT[1][0] = json_array_get_number(nums, 0)
			gz_ctT[1][1] = json_array_get_number(nums, 1)
			gz_ctT[2][0] = json_array_get_number(nums, 2)
			gz_ctT[2][1] = json_array_get_number(nums, 3)
			json_free(nums)
		}
		nums = json_object_get_value(regie, "pausas")
		if (nums != Invalid_JSON) {
			gz_tecnicasUsadas[1] = json_array_get_number(nums, 0)
			gz_tecnicasUsadas[2] = json_array_get_number(nums, 1)
			gz_tacticasUsadas[1] = json_array_get_number(nums, 2)
			gz_tacticasUsadas[2] = json_array_get_number(nums, 3)
			json_free(nums)
		}
		TrieClear(gz_stats)
		new JSON:stats = json_object_get_value(regie, "stats")
		new JSON:nombres = json_object_get_value(regie, "nombres")
		if (stats != Invalid_JSON) {
			new cuantos = json_object_get_count(stats)
			for (new i = 0; i < cuantos; i++) {
				new sid[GZ_SID], st[ST_TOTAL], nombre[32]
				json_object_get_name(stats, i, sid, charsmax(sid))
				new JSON:a = json_object_get_value(stats, sid)
				for (new k = 0; k < ST_TOTAL && k < json_array_get_count(a); k++) st[k] = json_array_get_number(a, k)
				json_free(a)
				TrieSetArray(gz_stats, sid, st, ST_TOTAL)
				if (nombres != Invalid_JSON) {
					json_object_get_string(nombres, sid, nombre, charsmax(nombre))
					TrieSetString(gz_nombres, sid, nombre)
				}
			}
			json_free(stats)
		}
		if (nombres != Invalid_JSON) json_free(nombres)
		json_free(regie)
	}
	json_free(r)

	remove_task(GZ_TASK_AVISO)
	gz_terminar_pausa()
	gz_stop[1] = false
	gz_stop[2] = false
	gz_estado = GS_LIVE
	gz_cuchilloHecho = true
	gz_mitadPendiente = false
	gz_esperandoVivo = false
	server_cmd("exec cfg/regiegg/live.cfg")
	server_exec()
	gz_acomodar_todos()
	set_member_game(m_bGameStarted, true)
	gz_restaurando = true
	server_cmd("sv_restartround 1")
	gz_decir(0, TX(53), ronda + 1, gz_nombre[1], gz_puntos[1], gz_puntos[2], gz_nombre[2])
	return true
}

// Despues del reinicio: a cada uno lo que tenia al empezar esa ronda. Los
// que no estan en el respaldo (entraron despues) quedan con lo de siempre.
public gz_task_reponer() {
	new path[256]
	gz_archivo_respaldo(gz_ronda, path, charsmax(path))
	new JSON:r = json_parse(path, true)
	if (r == Invalid_JSON) return
	new JSON:regie = json_object_get_value(r, "regie")
	new JSON:todos = regie != Invalid_JSON ? json_object_get_value(regie, "jugadores") : Invalid_JSON
	new jugadores[32], n
	get_players(jugadores, n, "h")
	for (new i = 0; i < n; i++) {
		new id = jugadores[i]
		new CsTeams:lado = CsTeams:get_member(id, m_iTeam)
		if (lado != CS_TEAM_CT && lado != CS_TEAM_T) continue
		new sid[GZ_SID]
		gz_steam64(id, sid, charsmax(sid))
		// Kills y muertes de la tabla, de las estadisticas restauradas.
		new st[ST_TOTAL]
		if (TrieGetArray(gz_stats, sid, st, ST_TOTAL)) {
			set_entvar(id, var_frags, float(st[ST_KILLS]))
			set_member(id, m_iDeaths, st[ST_DEATHS])
			gz_mandar_puntaje(id)
		}
		if (todos == Invalid_JSON || !json_object_has_value(todos, sid) || !is_user_alive(id)) continue
		new JSON:j = json_object_get_value(todos, sid)
		rg_remove_all_items(id)
		rg_give_item(id, "weapon_knife")
		new JSON:armas = json_object_get_value(j, "armas")
		if (armas != Invalid_JSON) {
			new cuantas = json_array_get_count(armas)
			for (new w = 0; w < cuantas; w++) {
				new item[48], arma[32], balas[12]
				json_array_get_string(armas, w, item, charsmax(item))
				strtok2(item, arma, charsmax(arma), balas, charsmax(balas), ':')
				if (!equal(arma, "weapon_", 7)) continue
				rg_give_item(id, arma)
				new bp = str_to_num(balas)
				new WeaponIdType:wid = WeaponIdType:rg_get_weapon_info(arma, WI_ID)
				if (bp > 0 && wid != WEAPON_NONE) rg_set_user_bpammo(id, wid, bp)
			}
			json_free(armas)
		}
		new armor = json_object_get_number(j, "armor")
		if (armor > 0) rg_set_user_armor(id, armor, json_object_get_bool(j, "casco") ? ARMOR_VESTHELM : ARMOR_KEVLAR)
		if (lado == CS_TEAM_CT && json_object_get_bool(j, "kit")) rg_give_defusekit(id, true)
		rg_add_account(id, json_object_get_number(j, "plata"), AS_SET, false)
		json_free(j)
	}
	if (todos != Invalid_JSON) json_free(todos)
	if (regie != Invalid_JSON) json_free(regie)
	json_free(r)
	// Que haya bomba: si nadie la tenia en el respaldo, a un terrorista.
	new conBomba
	for (new i = 0; i < n; i++) if (is_user_alive(jugadores[i]) && user_has_weapon(jugadores[i], CSW_C4)) conBomba = jugadores[i]
	if (!conBomba) {
		for (new i = 0; i < n; i++) {
			if (is_user_alive(jugadores[i]) && CsTeams:get_member(jugadores[i], m_iTeam) == CS_TEAM_T) {
				rg_give_item(jugadores[i], "weapon_c4")
				break
			}
		}
	}
}

// .stop de los dos equipos: vuelve al principio de la ronda en curso.
stock gz_pedir_stop(id) {
	if (get_pcvar_num(gz_pcvarStop) != 1 || gz_estado != GS_LIVE || gz_pausa != PZ_NINGUNA) return
	new equipo = gz_equipo_que_juega(id)
	if (!equipo) return
	gz_stop[equipo] = true
	if (gz_stop[1] && gz_stop[2]) {
		gz_stop[1] = false
		gz_stop[2] = false
		gz_decir(0, TX(54))
		if (!gz_aplicar_respaldo(gz_ronda)) gz_decir(0, TX(55))
		return
	}
	gz_decir(0, TX(56), gz_nombre[equipo], gz_nombre[3 - equipo])
}

// regie_restore <N>: como css_restore de MatchZy, la ronda N del mapa en
// juego (N = rondas ya jugadas: restoreBackupIndex en serverControl.ts).
public gz_cmd_restore() {
	new arg[8]
	read_argv(1, arg, charsmax(arg))
	if (gz_estado != GS_LIVE) {
		server_print(TX(94))
		return PLUGIN_HANDLED
	}
	if (!arg[0]) {
		server_print(TX(95))
		return PLUGIN_HANDLED
	}
	new ronda = str_to_num(arg)
	if (!gz_aplicar_respaldo(ronda)) server_print(TX(96), ronda)
	else server_print(TX(97), ronda + 1)
	return PLUGIN_HANDLED
}

// regie_listbackups [matchid]: una linea por respaldo, el mas nuevo
// primero, como matchzy_listbackups (respaldosMatchzy.ts la lee igual):
//   /<ruta> <fecha> <hora> <equipo 1> <equipo 2> <mapa> <marcador 1> <marcador 2>
// La ruta es relativa a cstrike/ con una barra adelante (AMX Mod X no sabe
// la absoluta): regie_loadbackup la acepta asi.
public gz_cmd_listbackups() {
	new arg[16]
	read_argv(1, arg, charsmax(arg))
	new prefijo[32]
	if (arg[0]) formatex(prefijo, charsmax(prefijo), "regie_%s_", arg)
	else copy(prefijo, charsmax(prefijo), "regie_")
	new dir[192]
	gz_dir_respaldos(dir, charsmax(dir))
	new Array:lineas = ArrayCreate(256)
	new Array:claves = ArrayCreate(32)
	gz_listar_respaldos_de(dir, prefijo, lineas, claves)
	// Y los de un plugin anterior al 1.19.0, que quedaron sin puerto.
	if (gz_dir_respaldos_viejos(dir, charsmax(dir))) gz_listar_respaldos_de(dir, prefijo, lineas, claves)
	new total = ArraySize(lineas)
	if (!total) server_print(TX(98))
	// Los mas nuevos primero (por fecha y, a igual fecha, por ronda).
	new bool:usado[256]
	for (new k = 0; k < total && k < GZ_MAX_LISTA && k < sizeof(usado); k++) {
		new mejor = -1, clave[32], claveMejor[32]
		for (new i = 0; i < total && i < sizeof(usado); i++) {
			if (usado[i]) continue
			ArrayGetString(claves, i, clave, charsmax(clave))
			if (mejor == -1 || strcmp(clave, claveMejor) > 0) {
				mejor = i
				copy(claveMejor, charsmax(claveMejor), clave)
			}
		}
		if (mejor == -1) break
		usado[mejor] = true
		new linea[256]
		ArrayGetString(lineas, mejor, linea, charsmax(linea))
		server_print("%s", linea)
	}
	ArrayDestroy(lineas)
	ArrayDestroy(claves)
	return PLUGIN_HANDLED
}

// Los respaldos de una carpeta que empiezan con prefijo: una linea de
// regie_listbackups cada uno, con la clave para ordenarlos.
stock gz_listar_respaldos_de(dir[], const prefijo[], Array:lineas, Array:claves) {
	new archivo[96], FileType:tipo
	new h = open_dir(dir, archivo, charsmax(archivo), tipo)
	if (!h) return
	do {
		if (tipo != FileType_File || !equal(archivo, prefijo, strlen(prefijo)) || contain(archivo, ".json") == -1) continue
		new path[256]
		formatex(path, charsmax(path), "%s/%s", dir, archivo)
		new JSON:r = json_parse(path, true)
		if (r == Invalid_JSON) continue
		new fecha[24], t1[GZ_NOMBRE], t2[GZ_NOMBRE], mapa[GZ_MAPA], linea[256], clave[32]
		json_object_get_string(r, "fecha", fecha, charsmax(fecha))
		json_object_get_string(r, "team1_name", t1, charsmax(t1))
		json_object_get_string(r, "team2_name", t2, charsmax(t2))
		json_object_get_string(r, "map_name", mapa, charsmax(mapa))
		formatex(linea, charsmax(linea), "/%s/%s %s %s %s %s %d %d", dir, archivo, fecha, t1, t2, mapa, json_object_get_number(r, "team1_score"), json_object_get_number(r, "team2_score"))
		formatex(clave, charsmax(clave), "%s %03d", fecha, json_object_get_number(r, "round"))
		json_free(r)
		ArrayPushString(lineas, linea)
		ArrayPushString(claves, clave)
	} while (next_file(h, archivo, charsmax(archivo), tipo))
	close_dir(h)
}

// regie_loadbackup "<archivo>": como matchzy_loadbackup, carga el partido
// entero desde un respaldo con el servidor libre (despues de una caida): el
// match config, el mapa y la serie. Cuando todos ponen .ready (o con
// regie_forceready) se restaura esa ronda. Solo archivos de la carpeta de
// respaldos, por nombre o con la ruta que contesta regie_listbackups.
public gz_cmd_loadbackup() {
	new arg[256]
	read_argv(1, arg, charsmax(arg))
	trim(arg)
	if (gz_estado != GS_NONE) {
		server_print(TX(82), gz_matchid)
		return PLUGIN_HANDLED
	}
	// El nombre del archivo, sin la carpeta.
	new nombre[96], inicio = 0
	for (new i = 0; arg[i]; i++) if (arg[i] == '/' || arg[i] == 92) inicio = i + 1
	copy(nombre, charsmax(nombre), arg[inicio])
	new largo = strlen(nombre)
	if (!equal(nombre, "regie_", 6) || largo < 12 || !equali(nombre[largo - 5], ".json") || contain(nombre, "..") != -1) {
		server_print(TX(99))
		return PLUGIN_HANDLED
	}
	new path[256]
	gz_dir_respaldos(path, charsmax(path))
	add(path, charsmax(path), "/")
	add(path, charsmax(path), nombre)
	// Si no esta en la de este servidor, en la de un plugin anterior al 1.19.0.
	if (!file_exists(path) && gz_dir_respaldos_viejos(path, charsmax(path))) {
		add(path, charsmax(path), "/")
		add(path, charsmax(path), nombre)
	}
	if (!file_exists(path)) {
		server_print(TX(100), nombre)
		return PLUGIN_HANDLED
	}
	new JSON:r = json_parse(path, true)
	if (r == Invalid_JSON) {
		server_print(TX(101), nombre)
		return PLUGIN_HANDLED
	}
	new JSON:cfg = json_object_get_value(r, "match_config")
	get_cvar_string("hostname", gz_hostnameOriginal, charsmax(gz_hostnameOriginal))
	// La foto de las cvars: la del partido, si el respaldo la trae; si no, la
	// de ahora.
	if (cfg != Invalid_JSON) gz_sacar_foto(cfg)
	if (cfg == Invalid_JSON || !gz_leer_config(cfg)) {
		if (cfg != Invalid_JSON) json_free(cfg)
		json_free(r)
		gz_estado = GS_NONE
		server_print(TX(102), nombre)
		return PLUGIN_HANDLED
	}
	gz_informal = false
	gz_cancelar_pendientes()
	gz_cancelar_votacion(true)
	gz_libre = false
	gz_mapa = clamp(json_object_get_number(r, "mapnumber"), 0, gz_cantMapas - 1)
	gz_serie[1] = json_object_get_number(r, "team1_series_score")
	gz_serie[2] = json_object_get_number(r, "team2_series_score")
	gz_rondaPendiente = json_object_get_number(r, "round")
	// El respaldo trae el match config con lo que era en ese momento (con
	// su hostname y su freno): se guarda con lo de ahora.
	gz_guardar(cfg)
	json_free(cfg)
	json_free(r)
	gz_poner_hostname()
	server_print(TX(103), gz_nombre[1], gz_nombre[2], gz_mapa + 1, gz_mapas[gz_mapa], gz_rondaPendiente + 1)
	gz_echar_ajenos()
	gz_ir_al_mapa_o_calentar()
	gz_reiniciar_si_queda()
	return PLUGIN_HANDLED
}

// --------------------------------------------------------------------
// Comandos de admin por RCON
// --------------------------------------------------------------------

public gz_cmd_estado() {
	new JSON:o = json_init_object()
	json_object_set_string(o, "gamestate", GZ_ESTADOS[gz_estado])
	if (gz_estado == GS_NONE) {
		json_object_set_null(o, "matchid")
		json_object_set_null(o, "team1")
		json_object_set_null(o, "team2")
	} else {
		new id[16]
		num_to_str(gz_matchid, id, charsmax(id))
		json_object_set_string(o, "matchid", id)
		new JSON:t1 = json_init_object()
		json_object_set_string(t1, "name", gz_nombre[1])
		json_object_set_value(o, "team1", t1)
		json_free(t1)
		new JSON:t2 = json_init_object()
		json_object_set_string(t2, "name", gz_nombre[2])
		json_object_set_value(o, "team2", t2)
		json_free(t2)
		json_object_set_number(o, "map_number", gz_mapa)
	}
	// 1.16.0: el mapa de ahora y la votacion de mapa, para el panel (no cambia
	// el mapa al terminar un pug si la votacion ya se encarga).
	new mapa[GZ_MAPA]
	get_mapname(mapa, charsmax(mapa))
	json_object_set_string(o, "map", mapa)
	json_object_set_string(o, "mapvote", GZ_VOTO_ESTADOS[gz_voto])
	// 1.24.0: a que mapa va (el ganador, o este si se quedan) y en cuantos
	// segundos sale el changelevel si gana otro mapa: el aviso del panel cuenta
	// con eso y no con el mapcycle ni con sus propios minutos.
	if (gz_voto == VT_CAMBIA) json_object_set_string(o, "mapvote_map", gz_votoGanador)
	else if (gz_voto == VT_SIGUE) json_object_set_string(o, "mapvote_map", mapa)
	new Float:ahora = get_gametime()
	new Float:falta = -1.0
	if (gz_voto == VT_CAMBIA) falta = gz_votoCambioEn - ahora
	else if (gz_voto == VT_VOTANDO && gz_votoMenu != -1) falta = gz_votoHasta - ahora + GZ_DEMORA_CAMBIO
	else if (gz_voto == VT_VOTANDO && gz_votoAbreEn > 0.0) falta = gz_votoAbreEn - ahora + float(clamp(get_pcvar_num(gz_pcvarVotoTiempo), 5, 120)) + GZ_DEMORA_CAMBIO
	if (falta >= 0.0) json_object_set_number(o, "mapvote_in", floatround(falta, floatround_ceil))
	json_serial_to_string(o, gz_buf, charsmax(gz_buf))
	json_free(o)
	server_print("%s", gz_buf)
	return PLUGIN_HANDLED
}

public gz_cmd_forceready() {
	if (gz_estado == GS_PREVETO) {
		gz_empezar_veto()
		server_print(TX(104))
		return PLUGIN_HANDLED
	}
	if (gz_estado != GS_WARMUP) {
		server_print(TX(105))
		return PLUGIN_HANDLED
	}
	gz_decir(0, TX(57))
	gz_salir_en_vivo()
	server_print(TX(106))
	return PLUGIN_HANDLED
}

public gz_cmd_restart() {
	if (gz_estado == GS_NONE) {
		server_print(TX(107))
		return PLUGIN_HANDLED
	}
	gz_empezar_calentamiento()
	server_cmd("sv_restartround 1")
	server_print(TX(108))
	return PLUGIN_HANDLED
}

// regie_hold <0|1>: inicio a cargo de la produccion (como css_regie_hold en
// CS2). Con el freno, el .ready queda anotado y el partido arranca con
// regie_forceready. Contesta como el plugin de CS2.
public gz_cmd_hold() {
	new arg[4]
	read_argv(1, arg, charsmax(arg))
	new bool:antes = gz_freno
	gz_freno = str_to_num(arg) == 1
	if (gz_estado != GS_NONE) gz_guardar_estado()
	if (gz_freno && !antes && gz_estado == GS_WARMUP) gz_decir(0, TX(58))
	// Sin el freno, si ya estaban todos listos, arranca como siempre.
	if (!gz_freno && antes && gz_estado == GS_WARMUP) gz_revisar_listos()
	server_print("%s hold=%d", GZ_OK_PARTIDO, gz_freno ? 1 : 0)
	return PLUGIN_HANDLED
}

// Quienes pusieron .ready: steamid64|nombre por linea.
public gz_cmd_holdstatus() {
	new jugadores[32], n, listos
	get_players(jugadores, n, "ch")
	for (new i = 0; i < n; i++) if (gz_listo[jugadores[i]]) listos++
	server_print("%s hold=%d listos=%d", GZ_OK_PARTIDO, gz_freno ? 1 : 0, listos)
	for (new i = 0; i < n; i++) {
		new id = jugadores[i]
		if (!gz_listo[id]) continue
		new sid[GZ_SID], nombre[32]
		gz_steam64(id, sid, charsmax(sid))
		get_user_name(id, nombre, charsmax(nombre))
		server_print("%s|%s", sid, nombre)
	}
	return PLUGIN_HANDLED
}

// "Game Commencing" y "Game will restart in": con un partido cargado los
// reinicios los maneja Regie.gg y avisa el, como MatchZy en CS2.
public gz_textmsg(msgid, dest, id) {
	if (gz_estado == GS_NONE) return PLUGIN_CONTINUE
	new texto[48]
	get_msg_arg_string(2, texto, charsmax(texto))
	if (equal(texto, "#Game_Commencing") || equal(texto, "#Game_will_restart_in")) return PLUGIN_HANDLED
	return PLUGIN_CONTINUE
}

// --------------------------------------------------------------------
// Cartel fijo de arriba (regie_warmup_hud): en que fase esta el servidor y
// que escribir, mientras no se juega en vivo. Arriba y centrado, por encima
// del cartel de las pausas (0.25) y lejos de la mira. Se vuelve a mandar
// cada segundo; al salir en vivo (o en la pausa, o al terminar) se saca.
// --------------------------------------------------------------------

public gz_task_cartel() {
	new texto[384]
	new jugadores[32], n
	get_players(jugadores, n, "ch")
	if (!gz_texto_cartel(texto, charsmax(texto))) {
		if (gz_cartelPuesto) {
			gz_cartelPuesto = false
			for (new i = 0; i < n; i++) ClearSyncHud(jugadores[i], gz_cartelSync)
		}
		return
	}
	gz_cartelPuesto = true
	set_hudmessage(120, 200, 255, -1.0, 0.14, 0, 0.0, GZ_CARTEL_CADA + 0.5, 0.0, 0.0, -1)
	for (new i = 0; i < n; i++) ShowSyncHudMsg(jugadores[i], gz_cartelSync, "%s", texto)
}

// Agrega una linea al cartel.
stock gz_linea(out[], len, const fmt[], any:...) {
	new linea[128]
	vformat(linea, charsmax(linea), fmt, 4)
	if (out[0]) add(out, len, "^n")
	add(out, len, linea)
}

// El texto del cartel segun la fase; false si no va (en vivo, pausa, fin
// del mapa, apagado o mientras se veta por el chat con regie_veto_hud).
stock bool:gz_texto_cartel(out[], len) {
	out[0] = 0
	if (get_pcvar_num(gz_pcvarCartel) != 1 || g_vetoHud[0]) return false
	switch (gz_estado) {
		case GS_NONE: {
			// Con el menu de la votacion de mapa abierto, lo que le queda.
			if (gz_voto == VT_VOTANDO && gz_votoMenu != -1) {
				gz_linea(out, len, TX(183))
				gz_linea(out, len, TX(185), max(0, floatround(gz_votoHasta - get_gametime(), floatround_ceil)))
				return true
			}
			// La misma condicion que la fase del feed (gz_estado_feed): solo en
			// el calentamiento libre.
			if (!gz_libre) return false
			gz_linea(out, len, TX(132))
			gz_linea(out, len, TX(133))
			// Con la misma cuenta que gz_revisar_informal (los bots en CT o T
			// cuentan como listos): de cuantos hacen falta, el minimo o todos.
			new faltan
			new listos = gz_listos_informal(false, faltan)
			new minimo = get_pcvar_num(gz_pcvarMinListos)
			if (minimo > 0) gz_linea(out, len, TX(169), minimo)
			else gz_linea(out, len, TX(136))
			if (faltan) gz_linea(out, len, TX(134), listos, faltan)
		}
		case GS_WARMUP, GS_PREVETO: {
			gz_linea(out, len, TX(132))
			gz_linea(out, len, gz_estado == GS_PREVETO ? TX(137) : TX(133))
			new minimo = get_pcvar_num(gz_pcvarMinListos)
			gz_linea(out, len, TX(134), gz_contar_listos(), minimo > 0 ? minimo : gz_porEquipo * 2)
			if (gz_freno && gz_estado == GS_WARMUP) gz_linea(out, len, TX(135))
		}
		case GS_KNIFE: {
			gz_linea(out, len, TX(138))
			gz_linea(out, len, TX(139))
		}
		case GS_WAIT_KNIFE: {
			gz_linea(out, len, TX(138))
			gz_linea(out, len, TX(140), gz_nombre[gz_ganadorCuchillo])
		}
		case GS_VETO: {
			gz_linea(out, len, TX(141))
			if (gz_ladoPendiente) gz_linea(out, len, TX(144), gz_nombre[3 - gz_ultimoEnElegir])
			else {
				new paso[12]
				gz_paso_actual(paso, charsmax(paso))
				if (paso[0]) gz_linea(out, len, contain(paso, "pick") != -1 ? TX(143) : TX(142), gz_nombre[paso[4] == '1' ? 1 : 2])
			}
		}
		default: return false
	}
	return true
}

// Los listos para arrancar, con la misma cuenta que gz_revisar_listos.
stock gz_contar_listos() {
	new listos
	new jugadores[32], n
	get_players(jugadores, n, "h")
	for (new i = 0; i < n; i++) {
		new id = jugadores[i]
		new equipo = gz_equipo_de(id)
		if (!equipo && gz_bot_listo(id)) equipo = gz_equipo_del_lado(CsTeams:get_member(id, m_iTeam))
		if (equipo != 1 && equipo != 2) continue
		if (gz_listo[id] || gz_bot_listo(id)) listos++
	}
	return listos
}

// --------------------------------------------------------------------
// Pausas (como MatchZy)
// --------------------------------------------------------------------

// El equipo (1/2) del que escribio, o 0 si es espectador o no juega.
stock gz_equipo_que_juega(id) {
	new equipo = gz_equipo_de(id)
	if (!equipo && gz_bot_libre(id)) equipo = gz_equipo_del_lado(CsTeams:get_member(id, m_iTeam))
	if (!equipo) {
		new CsTeams:lado = CsTeams:get_member(id, m_iTeam)
		if (lado == CS_TEAM_CT || lado == CS_TEAM_T) equipo = gz_equipo_del_lado(lado)
	}
	return equipo
}

stock gz_pedir_pausa(id, tipo) {
	if (gz_estado == GS_POST) {
		gz_decir(id, TX(59))
		return
	}
	if (gz_estado != GS_LIVE) return
	if (gz_pausa != PZ_NINGUNA) {
		gz_decir(id, gz_pausa == PZ_TACTICA ? TX(119) : TX(120))
		return
	}
	new equipo = gz_equipo_que_juega(id)
	if (!equipo) return
	if (tipo == PZ_TECNICA) {
		if (get_pcvar_num(gz_pcvarTecnica) != 1) {
			gz_decir(id, TX(60))
			return
		}
		new maximo = get_pcvar_num(gz_pcvarTecnicaMax)
		if (maximo > 0 && gz_tecnicasUsadas[equipo] >= maximo) {
			gz_decir(0, TX(61), gz_nombre[equipo])
			return
		}
		gz_iniciar_pausa(PZ_TECNICA, equipo)
		if (maximo > 0) gz_decir(0, TX(62), gz_nombre[equipo], gz_tecnicasUsadas[equipo] + 1, maximo)
		else gz_decir(0, TX(63), gz_nombre[equipo])
	} else {
		if (gz_tacticasUsadas[equipo] >= get_pcvar_num(gz_pcvarTacticaMax)) {
			gz_decir(id, TX(64))
			return
		}
		gz_iniciar_pausa(PZ_TACTICA, equipo)
		gz_decir(0, TX(65), gz_nombre[equipo], gz_tacticasUsadas[equipo] + 1, get_pcvar_num(gz_pcvarTacticaMax))
	}
	if (!get_member_game(m_bFreezePeriod)) gz_decir(0, TX(66))
}

stock gz_iniciar_pausa(tipo, equipo) {
	gz_pausa = tipo
	gz_pausaEquipo = equipo
	gz_pausaDesde = 0
	gz_pausaLibre = false
	gz_quiereSeguir[1] = false
	gz_quiereSeguir[2] = false
	// Si ya estamos en el freezetime, empieza ya.
	if (get_member_game(m_bFreezePeriod)) gz_empezo_pausa()
}

// Toma efecto: cuenta para el cupo del equipo y arranca el cartel.
stock gz_empezo_pausa() {
	gz_pausaDesde = get_systime()
	if (gz_pausa == PZ_TECNICA) gz_tecnicasUsadas[gz_pausaEquipo]++
	else if (gz_pausa == PZ_TACTICA) gz_tacticasUsadas[gz_pausaEquipo]++
	remove_task(GZ_TASK_PAUSA)
	set_task(1.0, "gz_task_pausa", GZ_TASK_PAUSA, _, _, "b")
	gz_task_pausa()
}

public gz_task_pausa() {
	if (gz_pausa == PZ_NINGUNA || !gz_pausaDesde) {
		remove_task(GZ_TASK_PAUSA)
		return
	}
	new pasaron = get_systime() - gz_pausaDesde
	if (gz_pausa == PZ_TACTICA) {
		new resta = get_pcvar_num(gz_pcvarTacticaDuracion) - pasaron
		if (resta <= 0) {
			gz_decir(0, TX(67), gz_nombre[gz_pausaEquipo])
			gz_terminar_pausa()
			return
		}
		gz_cartel(TX(77), gz_nombre[gz_pausaEquipo], resta / 60, resta % 60)
		return
	}
	if (gz_pausa == PZ_ADMIN) {
		gz_cartel(TX(78))
		return
	}
	if (gz_pausa == PZ_RESTAURO) {
		gz_cartel(TX(79))
		return
	}
	if (gz_pausa == PZ_DESCONEXION) {
		// Ya volvieron todos: espera el .unpause de los dos equipos.
		if (!gz_cantAus) {
			gz_cartel(TX(201))
			return
		}
		new tiempo = get_pcvar_num(gz_pcvarPausaDesconexionTiempo)
		if (tiempo <= 0) {
			gz_cartel(TX(200), gz_ausNombre[0])
			return
		}
		new resta = tiempo - pasaron
		if (resta <= 0) {
			gz_decir(0, TX(198), gz_ausNombre[0])
			gz_terminar_pausa()
			return
		}
		gz_cartel(TX(199), gz_ausNombre[0], resta / 60, resta % 60)
		return
	}
	new duracion = get_pcvar_num(gz_pcvarTecnicaDuracion)
	new maximo = get_pcvar_num(gz_pcvarTecnicaMax)
	new cuenta[24]
	if (maximo > 0) formatex(cuenta, charsmax(cuenta), " (%d/%d)", gz_tecnicasUsadas[gz_pausaEquipo], maximo)
	if (duracion < 0 || gz_pausaLibre) {
		gz_cartel(TX(80), gz_nombre[gz_pausaEquipo], cuenta)
		return
	}
	new resta = duracion - pasaron
	if (resta <= 0) {
		gz_pausaLibre = true
		gz_decir(0, TX(68))
		return
	}
	gz_cartel(TX(81), gz_nombre[gz_pausaEquipo], cuenta, resta / 60, resta % 60)
}

stock gz_cartel(const fmt[], any:...) {
	new msg[192]
	vformat(msg, charsmax(msg), fmt, 2)
	set_hudmessage(255, 209, 102, -1.0, 0.25, 0, 0.0, 1.1, 0.0, 0.0, -1)
	show_hudmessage(0, "%s", msg)
}

stock gz_pedir_seguir(id) {
	if (gz_estado != GS_LIVE || gz_pausa == PZ_NINGUNA) return
	if (gz_pausa == PZ_ADMIN) {
		gz_decir(id, TX(69))
		return
	}
	new equipo = gz_equipo_que_juega(id)
	if (!equipo) return
	new nombre[32]
	get_user_name(id, nombre, charsmax(nombre))
	// Pedida y todavia sin empezar: el equipo que la pidio la cancela.
	if (!gz_pausaDesde && equipo == gz_pausaEquipo) {
		if (gz_pausa == PZ_TACTICA) gz_decir(0, TX(71), gz_nombre[equipo])
		else gz_decir(0, TX(70), gz_nombre[equipo])
		gz_terminar_pausa()
		return
	}
	// La tactica la termina antes el que la pidio; la tecnica, pasado su
	// tiempo, cualquiera.
	if ((gz_pausa == PZ_TACTICA && equipo == gz_pausaEquipo) || (gz_pausa == PZ_TECNICA && gz_pausaLibre)) {
		gz_decir(0, TX(72), nombre)
		gz_terminar_pausa()
		return
	}
	if (gz_pausa == PZ_TACTICA) return
	gz_quiereSeguir[equipo] = true
	if (gz_quiereSeguir[1] && gz_quiereSeguir[2]) {
		gz_decir(0, TX(73))
		gz_terminar_pausa()
		return
	}
	gz_decir(0, TX(74), gz_nombre[equipo], gz_nombre[3 - equipo])
}

stock gz_terminar_pausa() {
	// La automatica se levanta (volvio, no volvio a tiempo, .unpause de los
	// dos o el admin): los que siguen afuera ya no frenan la partida.
	if (gz_pausa == PZ_DESCONEXION) gz_cantAus = 0
	gz_pausa = PZ_NINGUNA
	gz_pausaEquipo = 0
	gz_pausaDesde = 0
	gz_pausaLibre = false
	remove_task(GZ_TASK_PAUSA)
}

// La fase para el feed del HUD durante una pausa (las de GSI de CS2:
// "paused", o "timeout_ct" / "timeout_t" para la tactica del que esta de
// ese lado). Sin pausa en efecto, deja la que trae.
// Los nombres de los equipos por lado, para el HUD ("names":[CT, T] del
// snap): el 1.6 no tiene nombres de equipo, y sin esto el HUD solo ve
// "Counter-Terrorists"/"Terrorists". Sale del lado actual, asi que se da
// vuelta con .switch, el entretiempo y la prorroga. false sin partido.
// La fase del gestor para el feed ("gz" del snap), con los nombres de
// get5_status / MatchZy: warmup, knife, waiting_for_knife_decision,
// going_live, live, post_game, pre_veto, veto. Sin partido, "warmup" en el
// calentamiento libre (regie_pug_enabled): para el panel y el HUD es un
// calentamiento como el del partido. false sin partido ni calentamiento libre.
stock bool:gz_estado_feed(out[], len) {
	if (gz_estado == GS_NONE) {
		if (!gz_libre) return false
		copy(out, len, "warmup")
		return true
	}
	copy(out, len, GZ_ESTADOS[gz_estado])
	return true
}

// Las rondas del mapa y de cada prorroga para el feed (1.13.0): con partido
// (tambien el informal), las del gestor, con la misma cuenta que
// gz_revisar_mapa; sin partido, mp_maxrounds del servidor (0 = sin limite) y
// sin prorroga.
stock gz_rondas_feed(&maximo, &prorroga) {
	if (gz_estado == GS_NONE) {
		maximo = max(0, get_cvar_num("mp_maxrounds"))
		prorroga = 0
		return
	}
	maximo = max(2, get_pcvar_num(gz_pcvarMaxRounds))
	prorroga = get_pcvar_num(gz_pcvarProrroga) == 1 ? max(2, get_pcvar_num(gz_pcvarProrrogaRondas) & ~1) : 0
}

// La serie para el feed (1.15.0): los mapas que lleva ganados el equipo que
// esta en cada lado (tambien los de una serie retomada mapa a mapa) y de
// cuantos mapas es, con la misma cuenta que gz_avisos_ronda. false sin
// partido.
stock bool:gz_serie_feed(&ct, &t, &mapas) {
	if (gz_estado == GS_NONE) return false
	new eCt = gz_equipo_del_lado(CS_TEAM_CT)
	ct = gz_seriePrevia[eCt] + gz_serie[eCt]
	t = gz_seriePrevia[3 - eCt] + gz_serie[3 - eCt]
	mapas = gz_numMapsSerie > 0 ? gz_numMapsSerie : gz_numMaps
	return true
}

// Los segundos que le quedan al tiempo muerto (pausa tactica) en curso, para
// el reloj del HUD (1.15.0); -1.0 sin uno en efecto. La misma cuenta que el
// cartel de gz_task_pausa.
stock Float:gz_pausa_resta() {
	if (gz_estado != GS_LIVE || gz_pausa != PZ_TACTICA || !gz_pausaDesde) return -1.0
	return float(max(0, get_pcvar_num(gz_pcvarTacticaDuracion) - (get_systime() - gz_pausaDesde)))
}

// La entidad de la C4 (la "grenade" que deja la bomba plantada y que explota).
// Su explosion le pasa el daño a quien la planto (pevOwner), pero no es daño
// de el: como en CS2 y MatchZy, no cuenta para sus estadisticas, el reporte de
// daño ni las asistencias.
stock bool:gz_es_c4(ent) {
	if (ent <= get_maxplayers() || !pev_valid(ent)) return false
	new clase[16]
	get_entvar(ent, var_classname, clase, charsmax(clase))
	if (!equal(clase, "grenade")) return false
	return bool:get_member(ent, m_Grenade_bIsC4)
}

// Lo mato la bomba (ReGameDLL lo marca en TakeDamage, antes de Killed).
stock bool:gz_muerto_por_bomba(id) {
	return gz_jugador(id) && bool:get_member(id, m_bKilledByBomb)
}

stock bool:gz_nombres_feed(ct[], t[], len) {
	if (gz_estado == GS_NONE) return false
	new eCt = gz_equipo_del_lado(CS_TEAM_CT)
	copy(ct, len, gz_nombre[eCt])
	copy(t, len, gz_nombre[3 - eCt])
	return true
}

stock gz_fase_feed(fase[], len) {
	if (gz_estado != GS_LIVE || gz_pausa == PZ_NINGUNA || !gz_pausaDesde) return
	if (gz_pausa == PZ_TACTICA) copy(fase, len, gz_lado_de_equipo(gz_pausaEquipo) == CS_TEAM_CT ? "timeout_ct" : "timeout_t")
	else copy(fase, len, "paused")
}

public gz_cmd_forcepause() {
	if (gz_estado != GS_LIVE) {
		server_print(TX(94))
		return PLUGIN_HANDLED
	}
	if (gz_pausa != PZ_NINGUNA && gz_pausa != PZ_TECNICA && gz_pausa != PZ_TACTICA && gz_pausa != PZ_DESCONEXION) {
		server_print(TX(109))
		return PLUGIN_HANDLED
	}
	// El admin pisa la de un equipo: queda una pausa que solo el levanta.
	gz_terminar_pausa()
	gz_iniciar_pausa(PZ_ADMIN, 0)
	gz_decir(0, TX(75))
	server_print(TX(110))
	return PLUGIN_HANDLED
}

public gz_cmd_forceunpause() {
	if (gz_pausa == PZ_NINGUNA) {
		server_print(TX(111))
		return PLUGIN_HANDLED
	}
	// El admin reanuda: los que se fueron no vuelven a pausar la ronda.
	gz_cantAus = 0
	gz_terminar_pausa()
	gz_decir(0, TX(76))
	server_print(TX(112))
	return PLUGIN_HANDLED
}

// regie_lang <es|en>: el idioma de Regie.gg para los mensajes del gestor.
public gz_cmd_lang() {
	new arg[8]
	read_argv(1, arg, charsmax(arg))
	trim(arg)
	if (equali(arg, "es")) gz_idioma = 0
	else if (equali(arg, "en")) gz_idioma = 1
	gz_guardar_idioma()
	server_print("regie-lang ok %s", gz_idioma == 1 ? "en" : "es")
	return PLUGIN_HANDLED
}

stock gz_leer_idioma() {
	new path[256], linea[8]
	gz_archivo_comun("idioma.txt", path, charsmax(path))
	if (!file_exists(path)) return
	new f = fopen(path, "rt")
	if (!f) return
	fgets(f, linea, charsmax(linea))
	fclose(f)
	trim(linea)
	gz_idioma = equali(linea, "en") ? 1 : 0
}

stock gz_guardar_idioma() {
	new path[256]
	gz_archivo_comun("idioma.txt", path, charsmax(path))
	if (file_exists(path)) delete_file(path)
	write_file(path, gz_idioma == 1 ? "en" : "es")
}

public gz_cmd_endmatch() {
	if (gz_estado == GS_NONE) {
		server_print(TX(107))
		return PLUGIN_HANDLED
	}
	gz_fin_serie(true)
	server_print(TX(113))
	return PLUGIN_HANDLED
}

// --------------------------------------------------------------------
// Eventos al backend, con cola de reenvio
// --------------------------------------------------------------------

stock gz_enviar(JSON:ev, const evento[]) {
	json_object_set_string(ev, "t", "gestor")
	json_object_set_number(ev, "seq", ++gz_seq)
	json_object_set_string(ev, "event", evento)
	json_object_set_number(ev, "matchid", gz_matchid)
	json_serial_to_string(ev, gz_buf, charsmax(gz_buf))
	json_free(ev)
	if (ArraySize(gz_cola) >= GZ_COLA_MAX) {
		ArrayDeleteItem(gz_cola, 0)
		ArrayDeleteItem(gz_colaSeq, 0)
		ArrayDeleteItem(gz_colaHora, 0)
	}
	ArrayPushString(gz_cola, gz_buf)
	ArrayPushCell(gz_colaSeq, gz_seq)
	ArrayPushCell(gz_colaHora, 0.0)
	gz_mandar_cola()
}

stock gz_mandar_cola() {
	if (g_socket == -1 || !g_helloSent) return
	new n = ArraySize(gz_cola)
	new Float:ahora = get_gametime()
	for (new i = 0; i < n; i++) {
		new Float:cuando = ArrayGetCell(gz_colaHora, i)
		if (cuando > 0.0 && ahora - cuando < GZ_REENVIO && ahora >= cuando) continue
		ArrayGetString(gz_cola, i, gz_buf, charsmax(gz_buf) - 1)
		add(gz_buf, charsmax(gz_buf), "^n")
		if (socket_send(g_socket, gz_buf, strlen(gz_buf)) <= 0) return
		ArraySetCell(gz_colaHora, i, ahora)
	}
}

public gz_task_cola() {
	gz_mandar_cola()
}

// "gestorack <seq> <status>": 2xx o 4xx (salvo 408/429) sale de la cola.
stock gz_ack(seq, status) {
	if ((status >= 500 || status == 408 || status == 429) || status < 200) return
	new n = ArraySize(gz_colaSeq)
	for (new i = 0; i < n; i++) {
		if (ArrayGetCell(gz_colaSeq, i) == seq) {
			ArrayDeleteItem(gz_cola, i)
			ArrayDeleteItem(gz_colaSeq, i)
			ArrayDeleteItem(gz_colaHora, i)
			return
		}
	}
}

stock gz_guardar_cola() {
	new path[256]
	gz_archivo("cola.txt", path, charsmax(path))
	if (file_exists(path)) delete_file(path)
	new n = ArraySize(gz_cola)
	for (new i = 0; i < n; i++) {
		ArrayGetString(gz_cola, i, gz_buf, charsmax(gz_buf))
		write_file(path, gz_buf)
	}
}

stock gz_leer_cola() {
	new path[256]
	gz_archivo("cola.txt", path, charsmax(path))
	if (!file_exists(path)) return
	new f = fopen(path, "rt")
	if (!f) return
	while (!feof(f)) {
		fgets(f, gz_buf, charsmax(gz_buf))
		trim(gz_buf)
		if (!gz_buf[0]) continue
		// El seq es el que traia: se lo saca del JSON guardado.
		new JSON:o = json_parse(gz_buf)
		if (o == Invalid_JSON) continue
		new seq = json_object_get_number(o, "seq")
		json_free(o)
		ArrayPushString(gz_cola, gz_buf)
		ArrayPushCell(gz_colaSeq, seq)
		ArrayPushCell(gz_colaHora, 0.0)
		if (seq > gz_seq) gz_seq = seq
	}
	fclose(f)
	delete_file(path)
}
