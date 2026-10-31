#include <amxmodx>
#include <fakemeta>
#include <cstrike>
#include <sockets>

// Plugin de AMX Mod X para CS 1.6: manda un feed NDJSON en vivo (posicion,
// vida, plata, arma por jugador) al backend de cs2-live-viewer, analogo a
// cs2-plugin/DirectorFeed.cs (el plugin C# que cumple el mismo rol para
// CS2). Ver docs/plan-cs16-cod4.md, Paso 6.
//
// Diferencia de arquitectura clave respecto del plugin de CS2: alla el
// plugin es SERVIDOR TCP y el backend se conecta a el. Aca es al reves,
// porque el modulo "sockets" de AMX Mod X (confirmado leyendo
// sockets.inc) solo tiene socket_open() hacia afuera -- no hay
// listen/accept. Entonces el plugin es CLIENTE del backend (que corre un
// servidor TCP, ver backend/src/games/cs16/feedServer.ts).
//
// Desde la 1.21.0 tambien manda los eventos del partido (kills con todos
// sus datos, rondas, marcador, jugadores, chat): regiegg_partido.inc. Son
// la fuente principal del backend; el log de HLDS (logaddress_add) queda
// de respaldo para cuando el plugin no esta conectado.

#define PLUGIN_NAME    "RegieGG"
// 0.7.0: regie_veto_hud, el estado del veto de mapas por chat en el HUD.
// 1.1.0: etapa 2 del gestor (regiegg_gestor.inc): cuchillo y .stay/.switch,
// prorroga, regie_hold, regie_loadmatch, partido informal y reconexion.
// 1.2.0: etapa 3: pausas (.pause/.tech/.tac/.unpause, regie_forcepause y
// regie_forceunpause), reporte de daño y la fase de pausa en el feed.
// 1.3.0: etapa 4: respaldos de ronda (regie_restore, .stop, regie_listbackups,
// regie_loadbackup) y la serie retomada mapa a mapa.
// 1.4.0: etapa 5: el veto de mapas de MatchZy en el servidor (.ban/.pick/.ct/.t).
// 1.5.0: los mensajes del gestor en el idioma de Regie.gg (regie_lang: castellano o ingles).
// 1.6.0: nombres de equipo por lado en el HUD, y los textos del gestor en el
// español de España de MatchZy con el registro de daño nuevo.
// 1.6.1: las asistencias del partido ("a") en el feed del HUD.
// 1.7.0: la fase del gestor ("gz": warmup, knife...) en el feed del HUD.
// 1.8.0: lee configs/regie-gestor.cfg (seccion "Gestor de partido" de la consola).
// 1.9.0: mensajes nuevos del gestor: cartel fijo de calentamiento
// (regie_warmup_hud), .help, bienvenida al entrar (regie_welcome_enabled),
// recordatorio del informal como MatchZy y avisos de ronda en vivo
// (regie_round_messages: marcador, ultima de la mitad, punto de mapa/partido),
// y la frase de los sponsors en el chat con prefijo y colores (regie_ads_chat).
// Ademas, punto de mapa/partido y ultima de la mitad grandes en el centro,
// y el reporte de dano solo con los rivales.
// 1.9.1: el reporte de dano sale siempre (al morir y al terminar la ronda,
// en cualquier fase) y con todos los rivales, aunque no haya habido dano.
// 1.10.0: la barra de desactivacion del HUD con el estado real de la C4
// ("timer", "defuseLeft" y "defuseTotal" de la bomba en el feed), y arreglos
// del gestor: el reporte de dano con el golpe que mata; al terminar un
// partido o un informal vuelven las cvars de antes (mp_timelimit,
// mp_maxrounds y mp_winlimit a los 2 minutos); sin partido, calentamiento de
// verdad con warmup.cfg (regie_pug_enabled); plata infinita en todo
// calentamiento; la serie solo se anuncia si es de mas de un mapa; y el
// nombre del partido vuelve despues de server.cfg en cada mapa.
// 1.11.0: el plantado en el feed del HUD ("planting": quien, cuanto falta),
// la fase "warmup" en el feed tambien en el calentamiento libre (sin partido),
// y al terminar un partido el marcador vuelve a cero un rato despues.
// 1.12.0: con un partido cargado desde Regie.gg (no el informal) se echa a
// quien no sea de un equipo ni espectador del partido ("spectators" del
// match config, por SteamID64, como MatchZy), tambien a los que ya estaban.
// 1.13.0: la fase de la ronda con los hooks de ReGameDLL (el "Round_Start" del
// log de 1.6 sale al TERMINAR el freezetime: el numero de ronda llegaba tarde
// al HUD); las rondas del mapa y de la prorroga en el feed ("maxr", "otr");
// la bomba ya no le da daño, kills ni asistencias a quien la planto; las
// opciones del gestor (regie-gestor.cfg) se leen directo del archivo al
// empezar cada mapa y otra vez despues de server.cfg; el minimo de listos
// como MatchZy; el informal avisa que lo es (sin veto en el panel); y los
// avisos de punto de mapa o de partido iguales para los dos equipos.
// 1.14.0: la frase de los sponsors (regie_ads_chat) conserva las tildes: los
// bytes de UTF-8 llegan como celdas negativas y el filtro de caracteres de
// control los borraba ("Cafe" en vez de "Café").
// 1.15.0: el reloj de la ronda del feed ("endsIn") sale del reloj del juego
// (m_iRoundTimeSecs y m_fRoundStartTime) y no de mp_freezetime/mp_roundtime;
// en un tiempo muerto, lo que le queda; la serie del gestor en el feed
// ("ser" y "bo") para el punto de partido del HUD; y cada prorroga arranca
// como en CS2: sin las armas ni el equipo de antes y con la plata de prorroga.
// 1.16.0: votacion de mapa sin partido cargado: al terminar un pug
// (regie_pug_mapvote, regie_mapvote_time, regie_mapvote_maps), con .rtv o
// .votemap de mas de la mitad (regie_rtv_enabled) y con regie_mapvote; el
// menu tiene mapas del mapcycle y "seguir en este mapa". regie_status dice el
// mapa ("map") y la votacion ("mapvote") para el panel.
// 1.17.0: pausa automatica cuando se desconecta un jugador de un equipo en
// juego (regie_pause_on_disconnect, regie_disconnect_pause_time,
// regie_disconnect_unpause_on_return); y el bando del otro equipo ya no se
// puede elegir ni la primera vez (el SetHookChainReturn del menu de equipos
// iba con ATYPE_BOOL y cortaba el hook con un error).
// 1.18.0: el estado de la bomba completo en el feed del HUD, como el de GSI
// de CS2: la C4 tirada en el piso ("c4drop", la weaponbox con la mochila) y
// la bomba explotada o desactivada ("bev", de los hooks de ReGameDLL), que
// queda en el snap hasta el reinicio de la ronda o BOMB_EVENT_SECONDS.
// 1.19.0: varios servidores desde la misma carpeta (etapa 3 de la Fase 2 de
// docs/plan-paridad-juegos.md): el "hello" del feed dice el puerto del juego
// ("hostport", la cvar hostport: el -port de HLDS), asi el backend separa
// a cada servidor que se conecta al mismo feed; y los datos del gestor van
// por puerto
// (data/regiegg/<puerto>/: partido.json, cola.txt, calentamiento.json y
// respaldos). Un partido o una cola que dejo un plugin de antes en
// data/regiegg/ los toma el primer servidor que arranca con este, y los
// respaldos viejos se siguen listando y cargando desde ahi. idioma.txt
// sigue en data/regiegg/: el idioma es uno solo para todos.
// 1.20.0: Fase 3 de docs/plan-paridad-juegos.md. Las plantas y desactivadas
// de cada jugador en las estadisticas del gestor (bomb_plants y bomb_defuses,
// como MatchZy) y en el feed del HUD ("pl", "df"); y la placa de publicidad
// con imagen en el centro de la pantalla (regiegg_placa.inc): los sprites
// de sprites/regie/placas.ini se registran al cargar el mapa (los jugadores
// los bajan al entrar) y regie_ads_plate los muestra delante de la camara
// de cada uno, con las reglas de la placa de CS2.
// 1.21.0: los eventos del partido por el feed (regiegg_partido.inc), la fuente
// principal del backend en lugar del log de HLDS, que queda de respaldo: kills
// con tiro a la cabeza, asistencia (tambien con flash), a traves de una pared,
// a ciegas, sin mira, desde el aire y a traves del humo; rondas con el ganador y
// el motivo; el marcador por equipo del gestor; los jugadores; los vivos exactos
// para el clutch; el chat y el fin del mapa, que queda en cola (y en un archivo
// entre mapas) hasta que el backend lo confirma. Y la placa de publicidad se le
// puede sacar al camarografo de la transmision (regie_ads_plate, quinto y sexto
// argumento: ver regiegg_placa.inc).
// 1.22.0: en un pug no sale «Esperando a que se unan los jugadores»: los
// equipos se arman con los que ya pusieron .ready, asi que no falta nadie, y
// el conteo salteaba los bots (con uno en el pug repetia "0/1" cada 13 s).
// 1.23.0: los comandos del chat (.ready, .stay...) corren un instante despues
// (el server_exec del arranque borraba el mensaje del chat y del log); el
// cuchillo y la salida en vivo no toman como suya la ronda vieja del
// calentamiento que vence al apagar las rondas sin fin (el cuchillo terminaba
// al instante y se elegia lado con compra libre); el cuchillo deja el marcador
// en 0-0, y el fin de ronda le dice al backend en que fase esta para que no lo
// cuente; despues de un restart los bots del pug cuentan como listos; y en las
// fases sin fin de ronda el que mata ve su linea del reporte de daño.
// 1.24.0: regie_status dice a que mapa va la votacion ("mapvote_map": el
// ganador, o el actual si se quedan) y en cuantos segundos sale el changelevel
// ("mapvote_in"), para el aviso del panel.
// 1.25.0: las derrotas seguidas de cada lado ("loss": [CT, T]) de las reglas
// del juego (ReAPI), para el bonus por derrota de la caja de economia.
// 1.26.0: como termino cada ronda del mapa ("res": [[motivo, ganador], ...]),
// para el historial de rondas del HUD (antes no habia en 1.6). Lo lleva el
// plugin, asi vuelve entero aunque el backend se reinicie a mitad del mapa.
// 1.27.0: teclas del HUD de la transmision desde el juego: el camarografo
// (el mismo de regie_observe) escribe regie_hud <accion> [show|hide] en su
// consola, o la tiene en un bind, y la accion va al backend ("t": "hud") como
// un boton de Control de transmision. Tambien por RCON (regie_hud).
// 1.28.0: quien cego a quien, al momento (t: "ev"): "player_blind" con el
// cegado, el que tiro la flash, cuanto queda ciego (ms), si es compañero y el
// numero de la granada, y "flashbang_detonate" cuando explota cada una (para
// las que no ciegan a nadie). Para la etiqueta de la tarjeta, la flash buena
// y el resumen de flashes del HUD (send_flash; los hooks en regiegg_partido.inc).
// 1.28.1: el reporte de daño ya no se repite al que lo matan despues del fin
// de la ronda: lo habia visto al terminar la ronda y gz_muerte se lo volvia
// a mandar (ver gz_task_reporte_ronda en regiegg_gestor.inc). Y tras la
// ronda de cuchillo se olvida: mientras eligen lado (reaparicion) cada
// muerte volvia a mandar el daño del cuchillo.
#define PLUGIN_VERSION "1.28.1"
#define PLUGIN_AUTHOR  "Regie.gg"

// 0.2.0 manda inventario y stats por jugador: con 32 slots llenos la
// linea pasa holgada los 4 KB de antes.
#define MAX_LINE      16384
#define MAX_FIELD     192
#define RECONNECT_SEC 3.0

new g_pcvarHost
new g_pcvarPort
new g_pcvarHz
new g_pcvarObserver
new g_pcvarLos

new g_socket = -1
new bool:g_helloSent = false
new Float:g_lastSnapAt = 0.0
new Float:g_lastConnectAttemptAt = 0.0

// Estado de la bomba plantada (Paso 6, Fase 4). No hay native de "fase
// de la bomba" para GoldSrc: se detecta buscando la entidad de la C4
// plantada cada snap (classname "grenade" + modelo "models/w_c4.mdl",
// asi la desambigua de HE/flash/smoke -- ver bitacora del plan). A
// proposito NO se usan cs_get_c4_explode_time()/cs_get_c4_defusing():
// este mismo modulo cstrike ya tiene deshabilitados otros natives
// "de mas" (cs_get_weapon_info, cs_find_ent_by_class -- ver Paso 6) y
// un native deshabilitado corta en seco la funcion Pawn que lo llama
// (no hay try/catch), lo que rompería el snap entero a mitad de
// armado. g_bombPlantedAt + mp_c4timer (cvar de motor, siempre
// presente) alcanza para el timer sin arriesgar eso.
new bool:g_bombPlanted = false
new Float:g_bombPlantedAt = 0.0
new Float:g_bombLastPos[3]

// Explotada o desactivada (1.18.0): son un instante, no un estado que se
// pueda leer en cada snap (la C4 desactivada sigue tirada en el piso, y la
// que explota la borra el juego enseguida). Las marcan los hooks de
// ReGameDLL (RG_CGrenade_ExplodeBomb y RG_CGrenade_DefuseBombEnd, por ReAPI,
// como el resto de los hooks del plugin) y el snap las manda hasta que
// reinicia la ronda o pasan BOMB_EVENT_SECONDS, como GSI de CS2, que deja
// "exploded"/"defused" hasta la ronda siguiente.
#define BOMB_EVENT_SECONDS 10.0
new g_bombEvent[12]
new Float:g_bombEventAt = 0.0
new Float:g_bombEventPos[3]
new g_bombEventUid = -1
new g_bombEventSid[40]

// Fase de la ronda, llevada a mano con los mismos log events que ya
// escucha el tracker de log existente (backend/src/games/cs16/tracker.ts)
// -- no es un native, GoldSrc no expone "fase actual" directo.
new g_phase[16] = "warmup"
new g_round = 0
new Float:g_freezetimeEndsAt = 0.0

new g_line[MAX_LINE]
new g_escaped[MAX_FIELD]

// Lo que llega por el socket del feed en el otro sentido (backend ->
// plugin): comandos de camara del observador automatico, una linea
// cada uno (ver cmd_line_apply). Se junta hasta el salto de linea.
new g_inbuf[MAX_LINE]
new g_inlen = 0

// Paridad del HUD con CS2 (plan, Paso 6, "plan de accion"). Todo sale de
// natives del NUCLEO de AMX Mod X (get_user_ammo, get_user_frags,
// get_user_deaths) o de mensajes del motor (DeathMsg, Damage, TeamScore,
// ScreenFade) -- a proposito nada nuevo del modulo cstrike, que en este
// server tiene natives deshabilitados que cortan el snap entero.
new g_roundKills[33]
new g_roundHs[33]
new g_roundDamage[33]
new g_matchDamage[33]
new g_scoreCt = 0
// Como termino cada ronda del mapa (1.26.0): el ScenarioEventEndRound de
// ReGameDLL (numera igual que el motivo de fin de ronda de CS2) y el
// WinStatus (1 CT, 2 T). Se vacia con un Game_Commencing o un restart.
#define MAX_RESULTADOS 64
new g_resMotivo[MAX_RESULTADOS]
new g_resGanador[MAX_RESULTADOS]
new g_resCant = 0
new g_scoreT = 0
new Float:g_liveStartedAt = 0.0
// Ceguera: hasta cuando dura el blanco total (hold) y hasta cuando el
// fundido. Sale del mensaje ScreenFade blanco que manda el juego al
// cegar -- SIN CALIBRAR contra un humano cegado de verdad (ver plan).
new Float:g_flashHoldUntil[33]
new Float:g_flashUntil[33]
// Intensidad del blanco que mando el juego (alfa del ScreenFade): 255 con
// el flash de frente, 200 de costado (CS 1.6 manda las dos). Antes de la
// 0.5.1 se mostraba siempre 255.
new g_flashAlpha[33]
new g_inv[512]

// Linea de vision entre enemigos para el observador automatico (Paso 7,
// segunda vuelta): la traza el motor del juego (TraceLine de ojo a ojo,
// ignorando jugadores), que sabe de paredes, puertas y cajas mejor que
// cualquier copia del mapa que se cargue en el backend. g_los[a][b]: 1 se
// ven, 2 se van a ver en LOS_LOOKAHEAD segundos (posiciones extrapoladas
// con la velocidad), 0 no. Se recalcula en cada snapshot; 5v5 son 25
// pares, a lo sumo 50 trazas por snapshot (800 por segundo a 16 Hz).
#define LOS_LOOKAHEAD 1.0
new g_los[33][33]
new bool:g_losActive[33]

// Humos (0.6.0), para el radar y para marcar en el HUD a quien esta
// adentro de uno. Un humo es la entidad "grenade" con el modelo
// w_smokegrenade.mdl: despliega cuando pasa su dmgtime estando apoyado
// (el juego la hace detonar en el primer think despues de eso) y de ahi
// la entidad sigue viva mientras sale humo; cuando el humo se termina el
// juego la borra. Se manda la posicion y hace cuantos segundos desplego
// (0 = todavia en el aire o rodando).

// Veto de mapas por chat (0.7.0, backend/src/vetoServidor/): el estado
// del veto (lo que se veto y eligio, los mapas que quedan, de quien es el
// turno) en el HUD de todos, mientras dure. El chat de 1.6 se va enseguida
// y el capitan que entra tarde no sabe que se esta vetando: esto queda.
// Lo arma el backend y lo manda por RCON:
//   regie_veto_hud "<linea>|<linea>|..."   muestra (o actualiza) el estado
//   regie_veto_hud off                     lo saca (veto terminado o cancelado)
// El '|' es el salto de linea (por RCON no viaja uno de verdad). El
// backend le saca al texto las comillas, el ';' y lo que no es ASCII
// (comandoHudCs16 en vetoServidor/comandos.ts): es el nombre de un equipo,
// lo escribio una persona, y no puede cerrar el argumento ni encadenar
// otro comando. Igual aca se muestra con "%s": un '%' del nombre no se
// toma como formato.
#define VETO_HUD_MAX  512
#define VETO_HUD_TASK 7401
// Se vuelve a mostrar cada VETO_HUD_CADA segundos y dura un segundo mas:
// queda fijo sin parpadear, y al sacarlo se va solo en ese tiempo.
#define VETO_HUD_CADA 4.0
new g_vetoHud[VETO_HUD_MAX]
new g_vetoHudSync

// El gestor de partido (lo que hace MatchZy en CS2): regiegg_gestor.inc.
// Va despues de las globales del feed, que usa (g_socket, g_helloSent).
#include "regiegg_gestor"
// La placa de publicidad con imagen (1.20.0). Despues del gestor: mira su
// estado (pausas, calentamiento) y el veto en pantalla (g_vetoHud).
#include "regiegg_placa"
// Los eventos del partido (1.21.0). Despues del gestor: usa su estado y sus
// archivos (gz_archivo).
#include "regiegg_partido"

// Los sprites de la placa de publicidad: tienen que registrarse al cargar el
// mapa, antes de plugin_init.
public plugin_precache() {
	placa_precache()
}

public plugin_init() {
	register_plugin(PLUGIN_NAME, PLUGIN_VERSION, PLUGIN_AUTHOR)

	g_pcvarHost = create_cvar("regie_feed_host", "127.0.0.1", FCVAR_NONE, "Host del backend de Regie.gg")
	g_pcvarPort = create_cvar("regie_feed_port", "27200", FCVAR_NONE, "Puerto TCP del backend (feedServer.ts)")
	g_pcvarHz   = create_cvar("regie_feed_hz", "16", FCVAR_NONE, "Snapshots por segundo a mandar")
	g_pcvarObserver = create_cvar("regie_feed_observer", "", FCVAR_NONE, "Nombre del espectador de la transmision (vacio: el primero que este mirando)")
	g_pcvarLos = create_cvar("regie_feed_los", "1", FCVAR_NONE, "Trazar la linea de vision entre enemigos para el observador automatico (0 la apaga)")

	register_logevent("le_round_start", 2, "0=World triggered", "1=Round_Start")
	register_logevent("le_round_end", 2, "0=World triggered", "1=Round_End")
	register_logevent("le_match_reset", 2, "0=World triggered", "1=Game_Commencing")
	register_logevent("le_match_reset", 2, "0=World triggered", "1&Restart_Round_")

	// La fase de la ronda (1.13.0): el freezetime empieza con el reinicio de
	// ronda y termina con OnRoundFreezeEnd. El "Round_Start" del log de 1.6 se
	// escribe recien al terminar el freezetime (medido en los logs del servidor:
	// "Spawned_With_The_Bomb" y 15 s despues "Round_Start", con mp_freezetime
	// 15), asi que con el log solo la fase y el numero de ronda llegaban tarde.
	RegisterHookChain(RG_CSGameRules_RestartRound, "fd_reinicio_ronda", true)
	RegisterHookChain(RG_RoundEnd, "fd_fin_ronda", true)
	RegisterHookChain(RG_CSGameRules_OnRoundFreezeEnd, "fd_fin_freeze", true)

	// La bomba explotada o desactivada (1.18.0, ver g_bombEvent).
	RegisterHookChain(RG_CGrenade_ExplodeBomb, "fd_bomba_explota", true)
	RegisterHookChain(RG_CGrenade_DefuseBombEnd, "fd_bomba_fin_desarme", true)

	register_event("DeathMsg", "ev_death", "a")
	register_event("Damage", "ev_damage", "b", "2!0")
	register_event("TeamScore", "ev_teamscore", "a")
	// Flash: fundido blanco (r=g=b=255) y casi opaco, el patron de siempre
	// de los plugins de "anti-flash" para distinguirlo de otros fundidos.
	register_event("ScreenFade", "ev_screenfade", "be", "4=255", "5=255", "6=255", "7>199")

	register_forward(FM_StartFrame, "fw_StartFrame")

	// Camara del observador automatico (Paso 7 del plan): por RCON
	// (regie_observe) o por el socket del feed (misma sintaxis, sin el
	// nombre del comando). regie_observe_status es para depurar: muestra
	// el modo y el objetivo de cada observador.
	register_srvcmd("regie_observe", "cmd_observe")
	register_srvcmd("regie_hud", "cmd_regie_hud_srv")
	register_clcmd("regie_hud", "cmd_regie_hud")
	register_srvcmd("regie_observe_status", "cmd_observe_status")

	// Veto de mapas por chat (ver arriba de plugin_init). Un canal de HUD
	// propio (sync object): otro plugin que muestre algo no lo pisa.
	register_srvcmd("regie_veto_hud", "cmd_veto_hud")
	g_vetoHudSync = CreateHudSyncObj()

	// La frase de los sponsors en el chat (backend/src/publicidad.ts).
	register_srvcmd("regie_ads_chat", "cmd_ads_chat")
	// La placa con imagen (regiegg_placa.inc).
	placa_init()

	set_task(RECONNECT_SEC, "task_try_connect", 0, _, _, "b")
	// El chat del feed antes que el del gestor (que se queda con sus comandos);
	// el resto despues: su fin de ronda ya sumo los puntos de cada equipo.
	partido_init_chat()
	gestor_init()
	partido_init()
}

// --------------------------------------------------------------------
// Camara del espectador (Paso 7). En GoldSrc el modo de observador lo
// lleva el servidor (el game DLL) en dos campos del cliente: pev_iuser1
// = modo (OBS_IN_EYE primera persona, OBS_CHASE_LOCKED tercera...) y
// pev_iuser2 = indice de la entidad que sigue. El cliente dibuja lo
// que dicen esos campos, que viajan en el delta de su propia entidad.
// Se escriben de una, sin tocar nada mas del game DLL: si el objetivo
// muere, el propio juego busca otro (Observer_CheckTarget); si el
// camarografo aprieta una tecla, vuelve a mandar el.
//
// regie_observe <userid objetivo> [modo=4] [userid del observador]
//   objetivo -1: no hace nada (la camara queda como esta).
//   sin observador: el espectador de la transmision (regie_feed_observer
//   o el primero en Espectadores; los bots no cuentan). Con observador
//   explicito vale cualquiera, incluso un bot muerto: es para probar
//   contra un server con bots.
// --------------------------------------------------------------------

#define OBSERVE_OK          0
#define OBSERVE_NO_OBSERVER 1
#define OBSERVE_NO_TARGET   2
#define OBSERVE_TARGET_DEAD 3

// El cliente que hace de camarografo: regie_feed_observer por nombre,
// o si no uno en Espectadores, o si no un muerto que este observando.
// HLTV y bots quedan afuera (ver regie_observe para forzar uno).
stock pick_observer() {
	new wanted[32]
	get_pcvar_string(g_pcvarObserver, wanted, charsmax(wanted))
	new best = 0
	new bestRank = 0
	new maxPlayers = get_maxplayers()
	for (new id = 1; id <= maxPlayers; id++) {
		if (!is_user_connected(id) || is_user_hltv(id)) continue
		if (is_user_alive(id)) continue
		new rank = 1
		if (cs_get_user_team(id) == CS_TEAM_SPECTATOR) rank = 2
		if (wanted[0]) {
			new name[32]
			get_user_name(id, name, charsmax(name))
			if (equali(name, wanted)) rank = 3
		}
		// Un bot solo si el operador lo eligio por nombre (sirve para probar).
		if (is_user_bot(id) && rank < 3) continue
		if (rank > bestRank) {
			bestRank = rank
			best = id
		}
	}
	return best
}

stock observe(targetUserid, mode, observerUserid) {
	new observer = observerUserid > 0 ? find_player("k", observerUserid) : pick_observer()
	if (observer < 1 || !is_user_connected(observer)) return OBSERVE_NO_OBSERVER
	new target = find_player("k", targetUserid)
	if (target < 1 || !is_user_connected(target)) return OBSERVE_NO_TARGET
	if (!is_user_alive(target)) return OBSERVE_TARGET_DEAD
	if (mode < OBS_CHASE_LOCKED || mode > OBS_MAP_CHASE) mode = OBS_IN_EYE
	set_pev(observer, pev_iuser1, mode)
	set_pev(observer, pev_iuser2, target)
	return OBSERVE_OK
}

stock observe_reply(result, reply[], maxlen, targetUserid) {
	switch (result) {
		case OBSERVE_OK: formatex(reply, maxlen, "ok %d", targetUserid)
		case OBSERVE_NO_OBSERVER: copy(reply, maxlen, "sin camarografo: nadie en Espectadores ni muerto observando (o regie_feed_observer no esta conectado)")
		case OBSERVE_NO_TARGET: formatex(reply, maxlen, "objetivo %d no esta conectado", targetUserid)
		case OBSERVE_TARGET_DEAD: formatex(reply, maxlen, "objetivo %d esta muerto", targetUserid)
		default: copy(reply, maxlen, "error")
	}
}

public cmd_observe() {
	new arg[16]
	read_argv(1, arg, charsmax(arg))
	new targetUserid = str_to_num(arg)
	new mode = OBS_IN_EYE
	if (read_argc() > 2) mode = read_argv_int(2)
	new observerUserid = 0
	if (read_argc() > 3) observerUserid = read_argv_int(3)
	if (targetUserid < 0) {
		server_print("[Regie.gg] observe: sin objetivo, la camara queda como esta")
		return PLUGIN_HANDLED
	}
	new reply[128]
	observe_reply(observe(targetUserid, mode, observerUserid), reply, charsmax(reply), targetUserid)
	server_print("[Regie.gg] observe: %s", reply)
	return PLUGIN_HANDLED
}

public cmd_observe_status() {
	new maxPlayers = get_maxplayers()
	new name[32], tname[32]
	server_print("[Regie.gg] observadores (userid, nombre, vivo, equipo, modo iuser1, objetivo iuser2):")
	for (new id = 1; id <= maxPlayers; id++) {
		if (!is_user_connected(id)) continue
		get_user_name(id, name, charsmax(name))
		new target = pev(id, pev_iuser2)
		tname[0] = 0
		if (target >= 1 && target <= maxPlayers && is_user_connected(target)) get_user_name(target, tname, charsmax(tname))
		server_print("[Regie.gg]  #%d %s vivo=%d equipo=%d modo=%d objetivo=%d (%s, userid %d)",
			get_user_userid(id), name, is_user_alive(id) ? 1 : 0, _:cs_get_user_team(id), pev(id, pev_iuser1), target, tname,
			(target >= 1 && target <= maxPlayers && is_user_connected(target)) ? get_user_userid(target) : 0)
	}
	return PLUGIN_HANDLED
}

// Una linea del backend por el socket: "observe <userid> [modo] [observador]"
// o "observer <nombre>" (cambia regie_feed_observer). La respuesta va
// de vuelta por el mismo socket, con el prefijo "ack ".
stock cmd_line_apply(const line[]) {
	// Del gestor de partido: el match config pedido y la confirmacion de
	// cada evento (ver regiegg_gestor.inc y backend/src/games/cs16/gestor.ts).
	if (equal(line, "cfg ", 4)) {
		gz_al_recibir_config(line[4])
		return
	}
	if (equal(line, "cfgerr", 6)) {
		server_print(TX(121), line[6])
		return
	}
	// La confirmacion de los eventos del partido (regiegg_partido.inc).
	if (equal(line, "mack ", 5)) {
		partido_ack(str_to_num(line[5]))
		return
	}
	if (equal(line, "gestorack ", 10)) {
		new seq[16], status[8]
		parse(line[10], seq, charsmax(seq), status, charsmax(status))
		gz_ack(str_to_num(seq), str_to_num(status))
		return
	}
	new cmd[16], a1[16], a2[16], a3[16]
	new n = parse(line, cmd, charsmax(cmd), a1, charsmax(a1), a2, charsmax(a2), a3, charsmax(a3))
	new reply[160]
	if (n >= 2 && equal(cmd, "observe")) {
		new targetUserid = str_to_num(a1)
		if (targetUserid < 0) {
			copy(reply, charsmax(reply), "ok -1")
		} else {
			new mode = n >= 3 ? str_to_num(a2) : OBS_IN_EYE
			new observerUserid = n >= 4 ? str_to_num(a3) : 0
			observe_reply(observe(targetUserid, mode, observerUserid), reply, charsmax(reply), targetUserid)
		}
	} else if (n >= 1 && equal(cmd, "observer")) {
		set_pcvar_string(g_pcvarObserver, n >= 2 ? a1 : "")
		copy(reply, charsmax(reply), "ok observer")
	} else {
		formatex(reply, charsmax(reply), "comando desconocido: %s", cmd)
	}
	if (g_socket != -1) {
		new out[192]
		new len = formatex(out, charsmax(out), "{^"t^":^"ack^",^"text^":^"%s^"}^n", reply)
		socket_send(g_socket, out, len)
	}
}

// --------------------------------------------------------------------
// Teclas del HUD de la transmision (1.27.0)
// --------------------------------------------------------------------

// regie_hud <accion> [show|hide] desde la consola del camarografo (o un
// bind): solo el espectador de la transmision, el mismo que elige
// pick_observer para regie_observe. Un jugador no puede mover el HUD.
public cmd_regie_hud(id) {
	if (id != pick_observer()) {
		console_print(id, "[Regie.gg] regie_hud: solo lo puede usar el camarografo de la transmision (en Espectadores).")
		return PLUGIN_HANDLED
	}
	new sid[40]
	get_user_authid(id, sid, charsmax(sid))
	hud_accion(id, sid)
	return PLUGIN_HANDLED
}

// Lo mismo por RCON (para probar, o desde una botonera que hable RCON).
public cmd_regie_hud_srv() {
	hud_accion(0, "")
	return PLUGIN_HANDLED
}

stock hud_accion(id, const sid[]) {
	new accion[32], dato[8]
	read_argv(1, accion, charsmax(accion))
	read_argv(2, dato, charsmax(dato))
	new largo = strlen(accion)
	new bool:valida = largo >= 2 && largo <= 30
	for (new i = 0; i < largo && valida; i++) {
		if (!isalpha(accion[i])) valida = false
	}
	if (!valida) {
		hud_responder(id, "[Regie.gg] uso: regie_hud <accion> [show|hide|none]")
		return
	}
	// show/hide para prender o apagar; none para overlay (volver al juego), como en CS2.
	new bool:conDato = equali(dato, "show") || equali(dato, "hide") || equali(dato, "none")
	if (g_socket == -1 || !g_helloSent) {
		hud_responder(id, "[Regie.gg] regie_hud: el servidor no esta conectado a Regie.gg.")
		return
	}
	new line[192], len
	if (conDato) len = formatex(line, charsmax(line), "{^"t^":^"hud^",^"accion^":^"%s^",^"data^":^"%s^",^"sid^":^"%s^"}^n", accion, dato, sid)
	else len = formatex(line, charsmax(line), "{^"t^":^"hud^",^"accion^":^"%s^",^"data^":null,^"sid^":^"%s^"}^n", accion, sid)
	if (socket_send(g_socket, line, len) <= 0) {
		close_and_retry()
		return
	}
	new ok[64]
	formatex(ok, charsmax(ok), "[Regie.gg] hud: %s%s%s", accion, conDato ? " " : "", conDato ? dato : "")
	hud_responder(id, ok)
}

stock hud_responder(id, const texto[]) {
	if (id > 0) console_print(id, "%s", texto)
	else server_print("%s", texto)
}

// Lee lo que haya llegado por el socket sin bloquear (socket_is_readable
// con timeout 0) y aplica cada linea completa.
stock read_commands() {
	if (g_socket == -1) return
	if (!socket_is_readable(g_socket, 0)) return
	new chunk[128]
	new got = socket_recv(g_socket, chunk, charsmax(chunk))
	if (got <= 0) {
		// 0 = el backend cerro; -1 = error: se reconecta como siempre.
		close_and_retry()
		return
	}
	for (new i = 0; i < got; i++) {
		new ch = chunk[i]
		if (ch == 10 || ch == 13) {
			if (g_inlen > 0) {
				g_inbuf[g_inlen] = 0
				cmd_line_apply(g_inbuf)
				g_inlen = 0
			}
		} else if (g_inlen < charsmax(g_inbuf) - 1) {
			g_inbuf[g_inlen++] = ch
		} else {
			g_inlen = 0 // linea absurda: se descarta
		}
	}
}

// A donde mandar el feed: configs/regie-hud.cfg, que escribe el panel al
// instalar (desktop/games/cs16/installer.js). Se ejecuta ACA y no desde
// amxx.cfg: medido contra un HLDS real, AMX Mod X corre amxx.cfg antes de
// que existan las cvars de este plugin y los valores se perdian (quedaba
// el puerto por defecto). En plugin_cfg las cvars ya estan creadas.
public plugin_cfg() {
	// Las opciones del gestor que se guardan en la consola (seccion
	// "Gestor de partido") van antes de gestor_cfg(): al retomar un
	// partido, las cvars de su match config les ganan.
	leer_config("regie-hud.cfg")
	leer_config("regie-gestor.cfg")
	gestor_cfg()
}

// En AMX Mod X 1.9 corre una vez por mapa, despues de server.cfg (que vuelve
// a poner su hostname en cada mapa), de amxx.cfg y del cfg del mapa; plugin_cfg
// puede correr antes que server.cfg. Las opciones del gestor se vuelven a leer
// aca: lo que se ejecuto entre plugin_cfg y este momento no las deshace, y
// gestor_configs_ejecutadas vuelve a poner las del match config encima.
public OnConfigsExecuted() {
	leer_config("regie-gestor.cfg")
	gestor_configs_ejecutadas()
}

// Un .cfg de AMX Mod X con cvars regie_ (los que escribe la consola:
// regie-hud.cfg y regie-gestor.cfg), leido directo del archivo y puesto con
// set_cvar_string, sin pasar por el "exec" del motor. Hasta la 1.12.0 se hacia
// con server_cmd("exec ...") + server_exec() en plugin_cfg, que corre en medio
// de la carga del mapa (adentro del "map" o "changelevel" que esta ejecutando
// el buffer de comandos del motor): el resultado dependia de ese buffer, y el
// dueño vio que las opciones guardadas en la consola no tomaban efecto al
// reiniciar el servidor. Leido aca, quedan puestas en el acto. Solo las
// regie_ (nada que pueda tocar otra cosa) y solo si existen. Devuelve cuantas
// puso.
stock leer_config(const archivo[]) {
	new path[256]
	// Lo mismo que get_configsdir() de amxmisc.inc, sin incluirlo.
	get_localinfo("amxx_configsdir", path, charsmax(path))
	if (!path[0]) copy(path, charsmax(path), "addons/amxmodx/configs")
	format(path, charsmax(path), "%s/%s", path, archivo)
	new f = fopen(path, "rt")
	if (!f) return 0
	new puestas = 0
	new linea[256], nombre[64], valor[192]
	while (!feof(f)) {
		fgets(f, linea, charsmax(linea))
		trim(linea)
		if (!linea[0] || equal(linea, "//", 2)) continue
		nombre[0] = 0
		valor[0] = 0
		parse(linea, nombre, charsmax(nombre), valor, charsmax(valor))
		strtolower(nombre)
		if (!equal(nombre, "regie_", 6) || !gz_nombre_seguro(nombre) || !get_cvar_pointer(nombre)) continue
		set_cvar_string(nombre, valor)
		puestas++
	}
	fclose(f)
	return puestas
}

// --------------------------------------------------------------------
// Publicidad en el chat, como la de CS2 (css_regie_ads_chat): un prefijo
// entre corchetes de un color y la frase de otro.
//
// regie_ads_chat <colorPrefijo> <colorTexto> <0|1> "<prefijo>|<frase>"
//
// El 0|1 son los corchetes en blanco, como MatchZy. Los colores llegan
// por nombre (la paleta de CS2, COLORES_CHAT en publicidadChat.ts) y se
// pasan a los del chat del 1.6, que tiene tres: verde (^4), el de siempre
// (^1) y el del equipo (^3), que en un mensaje a todos se elige con
// print_team_red/blue/grey. Hay uno solo de esos por mensaje: si el
// prefijo y la frase piden dos distintos, gana el del prefijo. Contesta
// "regie-ads ok"; un plugin anterior no conoce el comando y el backend
// manda la frase por `say`.
// --------------------------------------------------------------------

public cmd_ads_chat() {
	new colorPrefijo[16], colorTexto[16], blanco[4], resto[256]
	read_argv(1, colorPrefijo, charsmax(colorPrefijo))
	read_argv(2, colorTexto, charsmax(colorTexto))
	read_argv(3, blanco, charsmax(blanco))
	read_argv(4, resto, charsmax(resto))
	new sep = contain(resto, "|")
	if (sep == -1) {
		server_print("regie-ads error: falta el separador")
		return PLUGIN_HANDLED
	}
	new prefijo[64], frase[192]
	copy(prefijo, min(sep, charsmax(prefijo)), resto)
	copy(frase, charsmax(frase), resto[sep + 1])
	sin_control(prefijo)
	sin_control(frase)
	trim(prefijo)
	trim(frase)
	if (!frase[0]) {
		server_print("regie-ads error: frase vacia")
		return PLUGIN_HANDLED
	}
	new equipo = print_team_default
	new cp = color_chat16(colorPrefijo, equipo)
	new ct = color_chat16(colorTexto, equipo)
	new linea[300]
	if (!prefijo[0]) formatex(linea, charsmax(linea), "%c%s", ct, frase)
	else if (blanco[0] == '1') formatex(linea, charsmax(linea), "^1[%c%s^1] %c%s", cp, prefijo, ct, frase)
	else formatex(linea, charsmax(linea), "%c[%s] %c%s", cp, prefijo, ct, frase)
	client_print_color(0, equipo, "%s", linea)
	server_print("regie-ads ok")
	return PLUGIN_HANDLED
}

// El caracter de color del chat del 1.6 para un nombre de la paleta de
// CS2. Si usa el del equipo (^3) y todavia no hay uno elegido, deja en
// `equipo` cual: rojo, azul o gris.
stock color_chat16(const nombre[], &equipo) {
	if (equali(nombre, "green") || equali(nombre, "lime")) return 4
	new quiere = print_team_default
	if (equali(nombre, "red") || equali(nombre, "purple")) quiere = print_team_red
	else if (equali(nombre, "lightblue")) quiere = print_team_blue
	else if (equali(nombre, "grey")) quiere = print_team_grey
	if (quiere == print_team_default) return 1
	if (equipo == print_team_default) equipo = quiere
	return equipo == quiere ? 3 : 1
}

// Saca los caracteres de control: en el chat del 1.6 son colores. Los
// bytes de UTF-8 (las tildes) quedan: AMX Mod X los guarda como celdas
// negativas (char con signo), y un "s[i] >= 32" solo los borraba.
stock sin_control(s[]) {
	new j = 0
	for (new i = 0; s[i]; i++) {
		if (s[i] < 0 || (s[i] >= 32 && s[i] != 127)) s[j++] = s[i]
	}
	s[j] = 0
}

public cmd_veto_hud() {
	new arg[VETO_HUD_MAX]
	read_args(arg, charsmax(arg))
	remove_quotes(arg)
	trim(arg)
	if (!arg[0] || equali(arg, "off")) {
		g_vetoHud[0] = 0
		remove_task(VETO_HUD_TASK)
		ClearSyncHud(0, g_vetoHudSync)
		server_print("[Regie.gg] veto_hud: off")
		return PLUGIN_HANDLED
	}
	replace_all(arg, charsmax(arg), "|", "^n")
	copy(g_vetoHud, charsmax(g_vetoHud), arg)
	if (!task_exists(VETO_HUD_TASK)) set_task(VETO_HUD_CADA, "task_veto_hud", VETO_HUD_TASK, _, _, "b")
	task_veto_hud()
	server_print("[Regie.gg] veto_hud: ok")
	return PLUGIN_HANDLED
}

public task_veto_hud() {
	if (!g_vetoHud[0]) return
	// Arriba y centrado (x -1.0), debajo del reloj de la ronda; dorado como
	// el turno en el chat de CS2. Sin fundido: se re-muestra encima.
	set_hudmessage(255, 210, 80, -1.0, 0.12, 0, 0.0, VETO_HUD_CADA + 1.0, 0.0, 0.0, -1)
	ShowSyncHudMsg(0, g_vetoHudSync, "%s", g_vetoHud)
}

public plugin_end() {
	gestor_end()
	partido_end()
	if (g_socket != -1) {
		socket_close(g_socket)
		g_socket = -1
	}
}

// --------------------------------------------------------------------
// Conexion (cliente no bloqueante: ver nota en sockets.inc de que un
// socket_open bloqueante puede colgar el tick unico del engine si el
// backend no responde a tiempo).
// --------------------------------------------------------------------

public task_try_connect() {
	if (g_socket != -1) return

	new host[64]
	get_pcvar_string(g_pcvarHost, host, charsmax(host))
	new port = get_pcvar_num(g_pcvarPort)

	new error
	g_socket = socket_open(host, port, SOCKET_TCP, error, SOCK_NON_BLOCKING)
	if (g_socket == -1) {
		// No hace falta loguear cada intento fallido (cada 3s, server
		// caido puede tardar en volver): server_print solo, no chat.
		return
	}

	g_helloSent = false
	g_lastConnectAttemptAt = get_gametime()
}

// Un socket no bloqueante recien abierto puede no estar listo para
// escribir todavia -- hay que esperar a que socket_is_writable() lo
// confirme antes de mandar el "hello". Si tarda mas de 5s, se descarta
// y se reintenta desde cero (evita quedar con un socket "zombie").
stock bool:ensure_connected() {
	if (g_socket == -1) return false
	if (!socket_is_writable(g_socket, 0)) {
		if (get_gametime() - g_lastConnectAttemptAt > 5.0) {
			socket_close(g_socket)
			g_socket = -1
		}
		return false
	}
	if (!g_helloSent) send_hello()
	return true
}

// El puerto del juego de este servidor (el -port de HLDS). Con dos
// servidores desde la misma carpeta es lo que los distingue: va en el hello
// del feed y en la carpeta de datos del gestor (gz_dir_datos). Medido el
// 24/10/2026 con ReHLDS: el -port queda en la cvar hostport ("27216"),
// mientras que port sigue en "27015" en todas las instancias.
stock regie_puerto() {
	new puerto = get_cvar_num("hostport")
	if (puerto <= 0) puerto = get_cvar_num("port")
	return puerto > 0 ? puerto : 27015
}

stock send_hello() {
	new mapname[32]
	get_mapname(mapname, charsmax(mapname))
	new line[320]
	// "hostport" (1.19.0), como el del plugin de CS2: el backend acepta
	// varios plugins en el mismo feed y los separa por este puerto. Lo de los
	// eventos del partido (1.21.0) va antes del cierre (partido_hello).
	new len = formatex(line, charsmax(line),
		"{^"t^":^"hello^",^"v^":1,^"map^":^"%s^",^"hz^":%d,^"plugin^":^"%s^",^"hostport^":%d",
		mapname, get_pcvar_num(g_pcvarHz), PLUGIN_VERSION, regie_puerto())
	len += partido_hello(line[len], charsmax(line) - len)
	len += formatex(line[len], charsmax(line) - len, "}^n")
	if (socket_send(g_socket, line, len) > 0) {
		g_helloSent = true
		partido_conexion_nueva()
	}
}

stock close_and_retry() {
	if (g_socket != -1) socket_close(g_socket)
	g_socket = -1
	g_inlen = 0
}

// --------------------------------------------------------------------
// Fase de la ronda (aproximada -- ver comentario de arriba)
// --------------------------------------------------------------------

// Empieza una ronda (1.13.0): el reinicio de ronda de ReGameDLL, que es
// cuando arranca el freezetime (la compra). Ahi ya se sabe que ronda es: el
// HUD la muestra durante la compra, no recien cuando se sale.
public fd_reinicio_ronda() {
	g_round++
	copy(g_phase, charsmax(g_phase), "freezetime")
	new Float:freezetime = get_pcvar_float(get_cvar_pointer("mp_freezetime"))
	g_freezetimeEndsAt = get_gametime() + freezetime
	for (new i = 0; i < 33; i++) {
		g_roundKills[i] = 0
		g_roundHs[i] = 0
		g_roundDamage[i] = 0
	}
	g_bombEvent[0] = 0
	partido_reinicio()
}

// La C4 exploto (1.18.0). La posicion, de la entidad si sigue ahi; si no, la
// ultima en la que el snap la vio plantada.
public fd_bomba_explota(const this, tracehandle, const bitsDamageType) {
	marcar_evento_bomba("exploded", this, 0)
}

// Termino un desarme: bDefused dice si fue de verdad (si no, el CT solto la
// tecla, se fue o lo mataron, y la bomba sigue plantada).
public fd_bomba_fin_desarme(const this, const player, bool:bDefused) {
	if (!bDefused) return
	marcar_evento_bomba("defused", this, player)
}

stock marcar_evento_bomba(const estado[], c4, quien) {
	copy(g_bombEvent, charsmax(g_bombEvent), estado)
	g_bombEventAt = get_gametime()
	if (c4 > 0 && pev_valid(c4)) pev(c4, pev_origin, g_bombEventPos)
	else for (new k = 0; k < 3; k++) g_bombEventPos[k] = g_bombLastPos[k]
	g_bombEventUid = -1
	g_bombEventSid[0] = 0
	if (1 <= quien <= get_maxplayers() && is_user_connected(quien)) {
		g_bombEventUid = get_user_userid(quien)
		get_user_authid(quien, g_bombEventSid, charsmax(g_bombEventSid))
	}
}

// Termina el freezetime. Con una pausa del gestor el juego lo vuelve a
// intentar en cada cuadro y gz_fin_freeze_pre lo frena (sigue en freezetime):
// solo cuenta si de verdad termino.
public fd_fin_freeze() {
	if (get_member_game(m_bFreezePeriod)) return
	salir_en_vivo_la_ronda()
}

stock salir_en_vivo_la_ronda() {
	if (equal(g_phase, "live")) return
	copy(g_phase, charsmax(g_phase), "live")
	g_liveStartedAt = get_gametime()
	// El "Round_Start" del log, con los vivos de cada equipo.
	partido_ronda_en_vivo()
}

// "Round_Start" del log: en 1.6 se escribe al terminar el freezetime (ver
// plugin_init). Por si el hook no estuvo (la ronda ya estaba en vivo).
public le_round_start() {
	salir_en_vivo_la_ronda()
}

// Partido nuevo (arranque o sv_restart): el dano acumulado vuelve a cero,
// igual que los frags del juego.
public le_match_reset() {
	for (new i = 0; i < 33; i++) g_matchDamage[i] = 0
	g_resCant = 0
}

// El fin de cada ronda (1.26.0), para el historial del HUD. Los reinicios
// (arranque, restart) vacian la lista; la ronda de cuchillo del gestor no
// cuenta (despues viene un restart igual).
public fd_fin_ronda(WinStatus:estado, ScenarioEventEndRound:evento, Float:demora) {
	if (evento == ROUND_GAME_COMMENCE || evento == ROUND_GAME_RESTART) {
		g_resCant = 0
		return HC_CONTINUE
	}
	if (evento == ROUND_NONE || !GetHookChainReturn(ATYPE_BOOL)) return HC_CONTINUE
	if (gz_estado == GS_KNIFE) return HC_CONTINUE
	if (g_resCant >= MAX_RESULTADOS) {
		// Lleno: se corre todo uno y se pierde la mas vieja.
		for (new i = 1; i < MAX_RESULTADOS; i++) {
			g_resMotivo[i - 1] = g_resMotivo[i]
			g_resGanador[i - 1] = g_resGanador[i]
		}
		g_resCant = MAX_RESULTADOS - 1
	}
	g_resMotivo[g_resCant] = _:evento
	g_resGanador[g_resCant] = _:estado
	g_resCant++
	return HC_CONTINUE
}

// ,"res":[[motivo,ganador],...] -- las rondas del mapa, de la primera a la ultima.
stock append_resultados(pos) {
	if (g_resCant <= 0) return 0
	new start = pos
	// Si quedaron mas que las rondas jugadas (mas una: el fin de ronda llega
	// antes que el marcador), sobran las mas viejas.
	new sobran = g_resCant - (g_scoreCt + g_scoreT + 1)
	if (sobran > 0) {
		for (new i = sobran; i < g_resCant; i++) {
			g_resMotivo[i - sobran] = g_resMotivo[i]
			g_resGanador[i - sobran] = g_resGanador[i]
		}
		g_resCant -= sobran
	}
	pos += format(g_line[pos], charsmax(g_line) - pos, ",^"res^":[")
	for (new i = 0; i < g_resCant; i++)
		pos += formatex(g_line[pos], charsmax(g_line) - pos, i ? ",[%d,%d]" : "[%d,%d]", g_resMotivo[i], g_resGanador[i])
	pos += format(g_line[pos], charsmax(g_line) - pos, "]")
	return pos - start
}

stock bool:enemies(a, b) {
	if (a < 1 || a > 32 || b < 1 || b > 32 || a == b) return false
	if (!is_user_connected(a) || !is_user_connected(b)) return false
	return get_user_team(a) != get_user_team(b)
}

// Ademas de contar, cada kill y cada dano se mandan al momento por el
// socket (t: "ev"), con los userid: es lo que el observador automatico
// usa para pesar la camara (backend/src/director/engine.ts, player_death
// y player_hurt). Las kills exactas del marcador siguen saliendo del log.
stock send_event(const name[], attacker, victim, damage, headshot) {
	if (g_socket == -1 || !g_helloSent) return
	new line[160]
	new len = formatex(line, charsmax(line),
		"{^"t^":^"ev^",^"name^":^"%s^",^"time^":%.2f,^"attacker^":%d,^"victim^":%d,^"dmg^":%d,^"headshot^":%d}^n",
		name, get_gametime(), attacker > 0 ? get_user_userid(attacker) : -1, victim > 0 ? get_user_userid(victim) : -1, damage, headshot)
	if (socket_send(g_socket, line, len) <= 0) close_and_retry()
}

// Una flash (1.28.0), por el mismo socket y con la misma forma que las kills:
//   player_blind        "victim" el cegado, "attacker" el que la tiro (userid,
//                       -1 si no se sabe), "ms" cuanto queda ciego, "tm" 1 si
//                       son del mismo equipo y "gid" el numero de entidad de
//                       la granada (las cegueras de una misma flash llevan el
//                       mismo)
//   flashbang_detonate  "attacker" el que la tiro y "gid" la granada; sale
//                       antes que sus cegueras
// Es al momento: no va a la cola del partido, una flash vieja reenviada al
// volver la conexion ya no sirve para nada en el HUD.
stock send_flash(const name[], victim, attacker, ms, teammate, grenade) {
	if (g_socket == -1 || !g_helloSent) return
	new line[200]
	new len = formatex(line, charsmax(line),
		"{^"t^":^"ev^",^"name^":^"%s^",^"time^":%.2f,^"attacker^":%d,^"victim^":%d,^"dmg^":0,^"headshot^":0,^"ms^":%d,^"tm^":%d,^"gid^":%d}^n",
		name, get_gametime(), attacker > 0 ? get_user_userid(attacker) : -1, victim > 0 ? get_user_userid(victim) : -1, ms, teammate, grenade)
	if (socket_send(g_socket, line, len) <= 0) close_and_retry()
}

public ev_death() {
	new killer = read_data(1)
	new victim = read_data(2)
	if (!enemies(killer, victim)) return
	// La bomba no es una kill de quien la planto (como en CS2 y MatchZy).
	if (gz_muerto_por_bomba(victim)) return
	g_roundKills[killer]++
	new headshot = read_data(3)
	if (headshot) g_roundHs[killer]++
	send_event("player_death", killer, victim, 0, headshot ? 1 : 0)
}

public ev_damage(victim) {
	new damage = read_data(2)
	new attacker = get_user_attacker(victim)
	if (damage <= 0 || !enemies(attacker, victim)) return
	// Ni el daño de la bomba.
	if (gz_es_c4(pev(victim, pev_dmg_inflictor))) return
	g_roundDamage[attacker] += damage
	g_matchDamage[attacker] += damage
	send_event("player_hurt", attacker, victim, damage, 0)
}

public ev_teamscore() {
	new team[16]
	read_data(1, team, charsmax(team))
	new score = read_data(2)
	if (equal(team, "CT")) g_scoreCt = score
	else if (equal(team, "TERRORIST")) g_scoreT = score
}

// duration y holdtime vienen en unidades de 1/4096 s (formato 4.12 del
// mensaje ScreenFade de GoldSrc); el argumento 7 es el alfa del blanco.
// Es lo mismo que usa el cliente para dibujarlo: el blanco se sostiene
// `hold` segundos con ese alfa y despues se va en linea recta durante
// `duration` (V_ScreenFade del motor, sin la bandera FFADE_OUT).
public ev_screenfade(id) {
	if (id < 1 || id > 32) return
	new Float:now = get_gametime()
	new Float:duration = float(read_data(1)) / 4096.0
	new Float:hold = float(read_data(2)) / 4096.0
	g_flashHoldUntil[id] = now + hold
	g_flashUntil[id] = now + hold + duration
	g_flashAlpha[id] = read_data(7)
}

// 0..255 como el "flashed" de GSI: el alfa del blanco mientras dura el
// blanco total, despues baja lineal hasta 0 mientras dura el fundido.
stock flash_alpha(id) {
	new Float:now = get_gametime()
	if (now >= g_flashUntil[id]) return 0
	if (now < g_flashHoldUntil[id]) return g_flashAlpha[id]
	new Float:span = g_flashUntil[id] - g_flashHoldUntil[id]
	if (span <= 0.0) return 0
	return floatround(float(g_flashAlpha[id]) * (g_flashUntil[id] - now) / span)
}

// Inventario como "nombre:cargador:reserva,..." (una sola cadena: corta y
// facil de partir en el backend). get_user_ammo es del nucleo de AMX Mod X.
// Para las granadas la reserva es la cantidad. Devuelve el valor de todo
// lo que lleva (para el valor de equipo).
stock build_inventory(id) {
	new weapons = pev(id, pev_weapons)
	new len = 0
	new value = 0
	g_inv[0] = 0
	for (new wid = 1; wid <= CSW_P90; wid++) {
		if (!(weapons & (1 << wid))) continue
		new wname[24]
		weapon_name(wid, wname, charsmax(wname))
		if (!wname[0]) continue
		new clip, bp
		get_user_ammo(id, wid, clip, bp)
		len += formatex(g_inv[len], charsmax(g_inv) - len, "%s%s:%d:%d", len ? "," : "", wname, clip, bp)
		value += weapon_price(wid)
	}
	return value
}

// A quien sigue la camara del espectador (el "observed" de GSI): el
// objetivo (pev_iuser2) de un cliente que esta observando (pev_iuser1 > 0).
// Se prefiere a uno en Espectadores antes que a un muerto mirando la
// partida, y si regie_feed_observer tiene un nombre, a ese cliente. HLTV
// y los bots no cuentan. Devuelve el userid del observado, o -1.
stock observed_userid() {
	new wanted[32]
	get_pcvar_string(g_pcvarObserver, wanted, charsmax(wanted))
	new best = -1
	new bestRank = 0
	new maxPlayers = get_maxplayers()
	for (new id = 1; id <= maxPlayers; id++) {
		if (!is_user_connected(id) || is_user_bot(id) || is_user_hltv(id)) continue
		if (pev(id, pev_iuser1) <= 0) continue
		new target = pev(id, pev_iuser2)
		if (target < 1 || target > maxPlayers || !is_user_connected(target) || !is_user_alive(target)) continue
		new rank = 1
		if (cs_get_user_team(id) == CS_TEAM_SPECTATOR) rank = 2
		if (wanted[0]) {
			new name[32]
			get_user_name(id, name, charsmax(name))
			if (equali(name, wanted)) rank = 3
		}
		if (rank > bestRank) {
			bestRank = rank
			best = get_user_userid(target)
		}
	}
	return best
}

// Segundos que le quedan a la fase actual (el "phase_countdowns" de GSI):
// congelamiento, ronda o bomba plantada.
//
// 1.15.0: del reloj del propio juego, el mismo que ve el jugador en pantalla
// (GetRoundRemainingTime de ReGameDLL: m_iRoundTimeSecs, que en la compra es
// el freezetime y despues el tiempo de la ronda, desde m_fRoundStartTime).
// Antes se armaba con las cvars: mp_freezetime leida al reiniciar la ronda y
// mp_roundtime leida en cada snapshot. Se iba de lo que juega el servidor
// cuando las cvars no son las de la ronda en curso: el primer freezetime
// despues de cambiar de mapa (el juego usa el que habia al cargar el mapa y
// el gestor ya puso otro: en los logs, mp_freezetime 0 y 15 s de compra), un
// cfg que se ejecuta en mitad de una ronda (server.cfg, warmup.cfg) o una
// cvar que el juego redondea (mp_roundtime 1.92 son 115 s, no 115.2).
stock Float:phase_ends_in(Float:bombTimeLeft) {
	new Float:now = get_gametime()
	if (equal(g_phase, "live") && bombTimeLeft >= 0.0) return bombTimeLeft
	if (!equal(g_phase, "freezetime") && !equal(g_phase, "live")) return 0.0
	new Float:delJuego = reloj_del_juego()
	if (delJuego >= 0.0) return delJuego
	// Respaldo (no deberia hacer falta): la cuenta de antes, con las cvars.
	if (equal(g_phase, "freezetime")) return floatmax(0.0, g_freezetimeEndsAt - now)
	new Float:roundtime = get_pcvar_float(get_cvar_pointer("mp_roundtime")) * 60.0
	return floatmax(0.0, g_liveStartedAt + roundtime - now)
}

// Lo que le queda a la fase del juego (compra o ronda), o -1.0 si el juego
// no tiene un tiempo que sirva (ronda sin limite o datos fuera de rango).
// En una pausa del gestor la compra no termina: da 0 hasta que se reanuda.
stock Float:reloj_del_juego() {
	new segundos = get_member_game(m_iRoundTimeSecs)
	if (segundos <= 0) return -1.0
	new Float:inicio = Float:get_member_game(m_fRoundStartTime)
	new Float:resta = float(segundos) - (get_gametime() - inicio)
	if (resta > float(segundos) + 1.0) return -1.0
	return floatmax(0.0, resta)
}

public le_round_end() {
	copy(g_phase, charsmax(g_phase), "over")
}

// --------------------------------------------------------------------
// Snapshot por frame, throttled a regie_feed_hz
// --------------------------------------------------------------------

public fw_StartFrame() {
	// El fin del freezetime lo dice fd_fin_freeze (frenado en una pausa); este
	// reloj queda de respaldo, solo sin pausa y con un segundo de margen.
	if (equal(g_phase, "freezetime") && get_gametime() >= g_freezetimeEndsAt + 1.0 && !get_member_game(m_bFreezePeriod)) {
		salir_en_vivo_la_ronda()
	}

	if (!ensure_connected()) return

	read_commands()
	if (g_socket == -1) return
	// Los eventos del partido que falten (regiegg_partido.inc).
	partido_mandar()
	if (g_socket == -1) return

	new Float:hz = float(get_pcvar_num(g_pcvarHz))
	if (hz < 1.0) hz = 1.0
	new Float:now = get_gametime()
	if (now - g_lastSnapAt < 1.0 / hz) return
	g_lastSnapAt = now

	build_and_send_snap()
}

stock json_escape(const src[], dest[], maxlen) {
	new si = 0
	new di = 0
	new ch
	// 92 = '\' -- ASCII a proposito, no el literal de caracter: el
	// escape por defecto de este compilador (AMX Mod X Compiler 1.9.x,
	// dialecto Pawn/Small viejo) es '^', no '\' (ver send_hello() mas
	// arriba, que ya usaba ^" para las comillas del NDJSON). '\\' como
	// literal de caracter compilaba distinto de lo esperado y encima
	// rompia el compilador mas abajo en el archivo con un
	// "invalid character constant" en ESTA linea -- verificado en vivo
	// contra amxxpc.exe: con 92 compila limpio, con '\\' no.
	while ((ch = src[si]) != 0 && di < maxlen - 2) {
		if (ch == '"' || ch == 92) {
			dest[di++] = 92
			dest[di++] = ch
		} else if (ch == 10 || ch == 13) {
			// nombres no deberian traer saltos de linea, pero si
			// llegan rompen el NDJSON (una linea = un mensaje)
			dest[di++] = ' '
		} else if (ch < 32) {
			// caracteres de control: se saltean
		} else {
			dest[di++] = ch
		}
		si++
	}
	dest[di] = 0
	return di
}

stock weapon_name(id, name[], maxlen) {
	// Tabla chica CSW_* -> nombre corto, arma por arma (no hay native
	// que lo de directo para GoldSrc como cs_get_item_alias, que da el
	// alias de COMPRA, no el nombre de arma que el HUD quiere mostrar).
	switch (id) {
		case CSW_P228: copy(name, maxlen, "p228")
		case CSW_SCOUT: copy(name, maxlen, "scout")
		case CSW_HEGRENADE: copy(name, maxlen, "hegrenade")
		case CSW_XM1014: copy(name, maxlen, "xm1014")
		case CSW_C4: copy(name, maxlen, "c4")
		case CSW_MAC10: copy(name, maxlen, "mac10")
		case CSW_AUG: copy(name, maxlen, "aug")
		case CSW_SMOKEGRENADE: copy(name, maxlen, "smokegrenade")
		case CSW_ELITE: copy(name, maxlen, "elite")
		case CSW_FIVESEVEN: copy(name, maxlen, "fiveseven")
		case CSW_UMP45: copy(name, maxlen, "ump45")
		case CSW_SG550: copy(name, maxlen, "sg550")
		case CSW_GALIL: copy(name, maxlen, "galil")
		case CSW_FAMAS: copy(name, maxlen, "famas")
		case CSW_USP: copy(name, maxlen, "usp")
		case CSW_GLOCK18: copy(name, maxlen, "glock18")
		case CSW_AWP: copy(name, maxlen, "awp")
		case CSW_MP5NAVY: copy(name, maxlen, "mp5navy")
		case CSW_M249: copy(name, maxlen, "m249")
		case CSW_M3: copy(name, maxlen, "m3")
		case CSW_M4A1: copy(name, maxlen, "m4a1")
		case CSW_TMP: copy(name, maxlen, "tmp")
		case CSW_G3SG1: copy(name, maxlen, "g3sg1")
		case CSW_FLASHBANG: copy(name, maxlen, "flashbang")
		case CSW_DEAGLE: copy(name, maxlen, "deagle")
		case CSW_SG552: copy(name, maxlen, "sg552")
		case CSW_AK47: copy(name, maxlen, "ak47")
		case CSW_KNIFE: copy(name, maxlen, "knife")
		case CSW_P90: copy(name, maxlen, "p90")
		default: copy(name, maxlen, "")
	}
}

// Precio de compra por arma: cs_get_weapon_info(..., CS_WEAPONINFO_COST)
// tira "native disabled" en el modulo cstrike de este server (confirmado
// en vivo contra bots), asi que se hardcodea la tabla de precios de
// stock de CS 1.6 -- mismo enfoque que weapon_name() de arriba.
stock weapon_price(id) {
	switch (id) {
		case CSW_P228: return 600
		case CSW_SCOUT: return 2750
		case CSW_HEGRENADE: return 300
		case CSW_XM1014: return 3000
		case CSW_MAC10: return 1400
		case CSW_AUG: return 3500
		case CSW_SMOKEGRENADE: return 300
		case CSW_ELITE: return 800
		case CSW_FIVESEVEN: return 750
		case CSW_UMP45: return 1700
		case CSW_SG550: return 4200
		case CSW_GALIL: return 2000
		case CSW_FAMAS: return 2250
		case CSW_USP: return 500
		case CSW_GLOCK18: return 400
		case CSW_AWP: return 4750
		case CSW_MP5NAVY: return 1500
		case CSW_M249: return 5750
		case CSW_M3: return 1700
		case CSW_M4A1: return 3100
		case CSW_TMP: return 1250
		case CSW_G3SG1: return 5000
		case CSW_FLASHBANG: return 200
		case CSW_DEAGLE: return 650
		case CSW_SG552: return 3500
		case CSW_AK47: return 2500
		case CSW_P90: return 2350
		default: return 0
	}
	return 0
}

// Valor de equipo del jugador: GoldSrc no lo expone armado, hay que
// sumarlo a mano -- todo el inventario (build_inventory) + armadura + kit.
stock equip_value_of(id, inventoryValue) {
	new value = inventoryValue
	new CsArmorType:armorType
	cs_get_user_armor(id, armorType)
	if (armorType == CS_ARMOR_KEVLAR) value += 650
	else if (armorType == CS_ARMOR_VESTHELM) value += 1000
	if (cs_get_user_defuse(id) > 0) value += 200
	return value
}

// Busca la entidad de la C4 plantada. "grenade" tambien es el classname
// de HE/flash/smoke mientras estan en el aire o en el piso -- por eso
// hace falta desambiguar por el modelo, que para la C4 plantada es
// siempre "models/w_c4.mdl" (a diferencia del backpack que sueltan los
// T al morir con la bomba encima, que es un "weaponbox" con otro
// modelo, asi que esto no confunde "cayo" con "plantada").
// La entidad de la C4 plantada (0 si no hay): de ahi salen la cuenta real
// de la bomba y el estado del desarme (miembros de ReGameDLL por ReAPI).
stock find_bomb_entity(Float:pos[3]) {
	new ent = -1
	while ((ent = engfunc(EngFunc_FindEntityByString, ent, "classname", "grenade")) != 0) {
		if (!pev_valid(ent)) continue
		new model[32]
		pev(ent, pev_model, model, charsmax(model))
		if (equal(model, "models/w_c4.mdl")) {
			pev(ent, pev_origin, pos)
			return ent
		}
	}
	return 0
}

// La C4 tirada en el piso (1.18.0): la "weaponbox" que deja el que la suelta
// (o el T que muere con ella), siempre con el modelo de la mochila,
// "models/w_backpack.mdl" (el modelo de mundo de la C4; las otras armas
// tiradas tienen el suyo). Solo fakemeta: nada del modulo cstrike.
stock bool:find_dropped_c4(Float:pos[3]) {
	new ent = -1
	while ((ent = engfunc(EngFunc_FindEntityByString, ent, "classname", "weaponbox")) != 0) {
		if (!pev_valid(ent)) continue
		new model[32]
		pev(ent, pev_model, model, charsmax(model))
		if (equal(model, "models/w_backpack.mdl")) {
			pev(ent, pev_origin, pos)
			return true
		}
	}
	return false
}

// Plantando (1.11.0): el jugador que tiene la C4 en la mano y ya empezo a
// armarla, del estado real del arma (m_C4_bStartedArming de ReGameDLL, por
// ReAPI). m_C4_fArmedTime es cuando queda plantada: el juego lo pone en
// "ahora + C4_ARMING_ON_TIME" (3 s) al empezar, y si se suelta el clic o se
// sale de la zona vuelve m_C4_bStartedArming a false. Devuelve el jugador
// (0 si nadie planta) y en left los segundos que le faltan.
#define C4_PLANT_SECONDS 3.0
stock find_planter(&Float:left) {
	new Float:ahora = get_gametime()
	new maxPlayers = get_maxplayers()
	for (new id = 1; id <= maxPlayers; id++) {
		if (!is_user_connected(id) || !is_user_alive(id)) continue
		new arma = get_member(id, m_pActiveItem)
		if (arma <= 0 || !pev_valid(arma)) continue
		if (get_member(arma, m_iId) != CSW_C4) continue
		if (!get_member(arma, m_C4_bStartedArming)) continue
		left = Float:get_member(arma, m_C4_fArmedTime) - ahora
		if (left < 0.0) left = 0.0
		if (left > C4_PLANT_SECONDS) left = C4_PLANT_SECONDS
		return id
	}
	left = 0.0
	return 0
}

// Desarme en curso: mejor esfuerzo por posicion + tecla de uso, no hay
// native de "esta desarmando" que no arriesgue el crash de mas arriba
// (ver comentario de g_bombPlanted). Radio de 96 unidades ~ el rango
// de interaccion de GoldSrc para entidades (+use). Devuelve tambien el
// authid de quien desarma (vacio si nadie), asi el HUD puede mostrar
// "Fulano esta desactivando" igual que con GSI/CS2.
stock bool:find_defuser(const Float:bombPos[3], authid[], maxlen) {
	new maxPlayers = get_maxplayers()
	for (new id = 1; id <= maxPlayers; id++) {
		if (!is_user_connected(id) || !is_user_alive(id)) continue
		if (cs_get_user_team(id) != CS_TEAM_CT) continue
		if (!(pev(id, pev_button) & IN_USE)) continue

		new Float:origin[3]
		pev(id, pev_origin, origin)
		new Float:dx = origin[0] - bombPos[0]
		new Float:dy = origin[1] - bombPos[1]
		new Float:dz = origin[2] - bombPos[2]
		if (dx * dx + dy * dy + dz * dz <= 96.0 * 96.0) {
			get_user_authid(id, authid, maxlen)
			return true
		}
	}
	authid[0] = 0
	return false
}

// Un rayo libre de paredes entre dos puntos (los jugadores no cuentan).
// Si arranca adentro de algo solido (una posicion extrapolada que cae
// en una pared) no vale como "se ven".
stock bool:los_clear(const Float:from[3], const Float:to[3], ignore) {
	engfunc(EngFunc_TraceLine, from, to, IGNORE_MONSTERS, ignore, 0)
	if (get_tr2(0, TR_AllSolid) || get_tr2(0, TR_StartSolid)) return false
	new Float:fraction
	get_tr2(0, TR_flFraction, fraction)
	return fraction >= 1.0
}

stock compute_los() {
	new maxPlayers = get_maxplayers()
	new Float:eye[33][3]
	new Float:soon[33][3]
	new teamOf[33]
	new bool:enabled = get_pcvar_num(g_pcvarLos) != 0

	for (new id = 1; id <= maxPlayers; id++) {
		g_losActive[id] = false
		for (new j = 0; j < 33; j++) g_los[id][j] = 0
		if (!enabled) continue
		if (!is_user_connected(id) || !is_user_alive(id)) continue
		new CsTeams:team = cs_get_user_team(id)
		if (team != CS_TEAM_T && team != CS_TEAM_CT) continue
		g_losActive[id] = true
		teamOf[id] = _:team

		new Float:origin[3], Float:ofs[3], Float:vel[3]
		pev(id, pev_origin, origin)
		pev(id, pev_view_ofs, ofs)
		pev(id, pev_velocity, vel)
		for (new k = 0; k < 3; k++) {
			eye[id][k] = origin[k] + ofs[k]
			soon[id][k] = eye[id][k] + vel[k] * LOS_LOOKAHEAD
		}
		// La altura no se extrapola: un salto mandaria el "ojo de dentro
		// de un segundo" al techo.
		soon[id][2] = eye[id][2]
	}

	for (new a = 1; a <= maxPlayers; a++) {
		if (!g_losActive[a]) continue
		for (new b = a + 1; b <= maxPlayers; b++) {
			if (!g_losActive[b] || teamOf[a] == teamOf[b]) continue
			new v = 0
			if (los_clear(eye[a], eye[b], a)) v = 1
			else if (los_clear(soon[a], soon[b], a)) v = 2
			g_los[a][b] = v
			g_los[b][a] = v
		}
	}
}

// ,"los":[userids que ve],"ls":[userids que va a ver] -- solo para vivos.
stock append_los(id, pos) {
	new start = pos
	new maxPlayers = get_maxplayers()
	pos += format(g_line[pos], charsmax(g_line) - pos, ",^"los^":[")
	new bool:first = true
	for (new o = 1; o <= maxPlayers; o++) {
		if (g_los[id][o] != 1) continue
		pos += formatex(g_line[pos], charsmax(g_line) - pos, first ? "%d" : ",%d", get_user_userid(o))
		first = false
	}
	pos += format(g_line[pos], charsmax(g_line) - pos, "],^"ls^":[")
	first = true
	for (new o = 1; o <= maxPlayers; o++) {
		if (g_los[id][o] != 2) continue
		pos += formatex(g_line[pos], charsmax(g_line) - pos, first ? "%d" : ",%d", get_user_userid(o))
		first = false
	}
	pos += format(g_line[pos], charsmax(g_line) - pos, "]")
	return pos - start
}

// ,"smokes":[{"id":ent,"pos":[x,y,z],"et":segundos,"own":userid}] -- vacio si no hay.
// "own" es quien la tiro (-1 si ya no esta): el radar pinta la nube del
// color de su equipo.
stock append_smokes(pos) {
	new start = pos
	new Float:now = get_gametime()
	pos += format(g_line[pos], charsmax(g_line) - pos, ",^"smokes^":[")
	new bool:first = true
	new ent = -1
	while ((ent = engfunc(EngFunc_FindEntityByString, ent, "classname", "grenade")) != 0) {
		if (!pev_valid(ent)) continue
		new model[32]
		pev(ent, pev_model, model, charsmax(model))
		if (!equal(model, "models/w_smokegrenade.mdl")) continue
		new Float:origin[3], Float:dmgtime
		pev(ent, pev_origin, origin)
		pev(ent, pev_dmgtime, dmgtime)
		new Float:active = 0.0
		if ((pev(ent, pev_flags) & FL_ONGROUND) && now >= dmgtime) active = now - dmgtime
		new owner = pev(ent, pev_owner)
		new ownerUid = (owner >= 1 && owner <= get_maxplayers() && is_user_connected(owner)) ? get_user_userid(owner) : -1
		pos += formatex(g_line[pos], charsmax(g_line) - pos,
			first ? "{^"id^":%d,^"pos^":[%.0f,%.0f,%.0f],^"et^":%.1f,^"own^":%d}" : ",{^"id^":%d,^"pos^":[%.0f,%.0f,%.0f],^"et^":%.1f,^"own^":%d}",
			ent, origin[0], origin[1], origin[2], active, ownerUid)
		first = false
	}
	pos += format(g_line[pos], charsmax(g_line) - pos, "]")
	return pos - start
}

stock build_and_send_snap() {
	new mapname[32]
	get_mapname(mapname, charsmax(mapname))

	compute_los()

	// En pausa, la fase de GSI de CS2 ("paused", "timeout_ct"/"timeout_t"):
	// la pone el gestor (regiegg_gestor.inc, gz_fase_feed).
	new fase[16]
	copy(fase, charsmax(fase), g_phase)
	gz_fase_feed(fase, charsmax(fase))

	new pos = 0
	pos += formatex(g_line[pos], charsmax(g_line) - pos,
		"{^"t^":^"snap^",^"time^":%.2f,^"round^":%d,^"phase^":^"%s^",^"map^":^"%s^",^"players^":[",
		get_gametime(), g_round, fase, mapname)

	new bool:first = true
	new maxPlayers = get_maxplayers()
	for (new id = 1; id <= maxPlayers; id++) {
		if (!is_user_connected(id)) continue
		if (cs_get_user_team(id) == CS_TEAM_SPECTATOR || cs_get_user_team(id) == CS_TEAM_UNASSIGNED) continue

		if (!first) pos += format(g_line[pos], charsmax(g_line) - pos, ",")
		first = false

		new authid[40]
		get_user_authid(id, authid, charsmax(authid))
		new name[MAX_FIELD]
		get_user_name(id, name, charsmax(name))
		json_escape(name, g_escaped, charsmax(g_escaped))

		// En un server sin Steam el authid es STEAM_ID_LAN para TODOS,
		// asi que no distingue jugadores: el backend los sigue por
		// nombre y desempata con estos dos cuando dos se llaman igual
		// (ver games/cs16/identity.ts). La IP sin puerto (el puerto
		// cambia en cada conexion); el userid como ultimo recurso,
		// cuando dos comparten IP (mismo NAT).
		new ip[24]
		get_user_ip(id, ip, charsmax(ip), 1)

		new CsTeams:team = cs_get_user_team(id)
		new bool:alive = bool:is_user_alive(id)

		new Float:origin[3], Float:velocity[3], Float:vAngle[3]
		pev(id, pev_origin, origin)
		pev(id, pev_velocity, velocity)
		pev(id, pev_v_angle, vAngle)

		new health = pev(id, pev_health)
		if (health < 0) health = 0
		new CsArmorType:armorType
		new armor = cs_get_user_armor(id, armorType)
		new money = cs_get_user_money(id)
		new flags = pev(id, pev_flags)
		new weapons = pev(id, pev_weapons)
		new hasBomb = (weapons & (1 << CSW_C4)) ? 1 : 0

		new clip, ammo
		new activeWeapon = get_user_weapon(id, clip, ammo)
		new wname[24]
		weapon_name(activeWeapon, wname, charsmax(wname))

		pos += formatex(g_line[pos], charsmax(g_line) - pos,
			"{^"sid^":^"%s^",^"uid^":%d,^"ip^":^"%s^",^"name^":^"%s^",^"team^":%d,^"alive^":%d,^"bot^":%d,",
			authid, get_user_userid(id), ip, g_escaped, _:team, alive ? 1 : 0, is_user_bot(id) ? 1 : 0)

		pos += formatex(g_line[pos], charsmax(g_line) - pos,
			"^"pos^":[%.1f,%.1f,%.1f],^"vel^":[%.1f,%.1f,%.1f],^"ang^":[%.1f,%.1f],",
			origin[0], origin[1], origin[2], velocity[0], velocity[1], velocity[2], vAngle[0], vAngle[1])

		pos += formatex(g_line[pos], charsmax(g_line) - pos,
			"^"hp^":%d,^"armor^":%d,^"helmet^":%d,^"money^":%d,^"defuseKit^":%d,^"ducking^":%d,",
			health, armor, (armorType == CS_ARMOR_VESTHELM) ? 1 : 0, money,
			cs_get_user_defuse(id) > 0 ? 1 : 0, (flags & FL_DUCKING) ? 1 : 0)

		new invValue = build_inventory(id)
		pos += formatex(g_line[pos], charsmax(g_line) - pos,
			"^"hasBomb^":%d,^"inBuyZone^":%d,^"inBombZone^":%d,^"scoped^":%d,^"weapon^":^"%s^",^"equip^":%d,",
			hasBomb,
			(cs_get_user_mapzones(id) & CS_MAPZONE_BUY) ? 1 : 0,
			(cs_get_user_mapzones(id) & CS_MAPZONE_BOMBTARGET) ? 1 : 0,
			cs_get_user_zoom(id),
			wname,
			equip_value_of(id, invValue))

		pos += formatex(g_line[pos], charsmax(g_line) - pos,
			"^"clip^":%d,^"ammo^":%d,^"inv^":^"%s^",^"k^":%d,^"d^":%d,^"a^":%d,^"rk^":%d,^"rhs^":%d,^"rdmg^":%d,^"dmg^":%d,^"flash^":%d",
			clip, ammo, g_inv,
			get_user_frags(id), get_user_deaths(id), gz_asistencias(id),
			g_roundKills[id], g_roundHs[id], g_roundDamage[id], g_matchDamage[id],
			flash_alpha(id))
		// Plantas y desactivadas del partido (1.20.0): solo con un partido
		// cargado, asi el marcador del HUD no muestra columnas en 0 sin partido.
		if (gz_estado != GS_NONE) pos += formatex(g_line[pos], charsmax(g_line) - pos,
			",^"pl^":%d,^"df^":%d", gz_stat_de(id, ST_PLANTS), gz_stat_de(id, ST_DEFUSES))
		if (g_losActive[id]) pos += append_los(id, pos)
		pos += format(g_line[pos], charsmax(g_line) - pos, "}")
	}

	pos += format(g_line[pos], charsmax(g_line) - pos, "]")

	new Float:bombPos[3]
	new Float:bombTimeLeft = -1.0
	// Explotada o desactivada (1.18.0): manda sobre lo demas mientras dure
	// (la C4 desactivada sigue en el piso y se leeria como plantada).
	if (g_bombEvent[0] && get_gametime() - g_bombEventAt > BOMB_EVENT_SECONDS) g_bombEvent[0] = 0
	new c4 = g_bombEvent[0] ? 0 : find_bomb_entity(bombPos)
	if (g_bombEvent[0]) {
		g_bombPlanted = false
		pos += formatex(g_line[pos], charsmax(g_line) - pos,
			",^"bomb^":null,^"bev^":{^"st^":^"%s^",^"pos^":[%.1f,%.1f,%.1f],^"uid^":%d,^"sid^":^"%s^"}",
			g_bombEvent, g_bombEventPos[0], g_bombEventPos[1], g_bombEventPos[2], g_bombEventUid, g_bombEventSid)
	} else if (c4) {
		for (new k = 0; k < 3; k++) g_bombLastPos[k] = bombPos[k]
		new Float:ahora = get_gametime()
		new Float:c4timer = get_pcvar_float(get_cvar_pointer("mp_c4timer"))
		if (!g_bombPlanted) {
			g_bombPlanted = true
			g_bombPlantedAt = ahora
		}

		// La cuenta real de la bomba (m_flC4Blow); si no esta, la de antes
		// (desde que se la vio, con mp_c4timer).
		new Float:explota = Float:get_member(c4, m_Grenade_flC4Blow)
		new Float:timeLeft = explota > 0.0 ? explota - ahora : g_bombPlantedAt + c4timer - ahora
		if (timeLeft < 0.0) timeLeft = 0.0
		bombTimeLeft = timeLeft

		// El desarme, del estado real de la C4: si empezo, quien, cuanto le
		// falta y si tiene kit (5 s con kit, 10 sin). Antes se adivinaba por
		// un CT con +use cerca de la bomba, y la barra seguia a la bomba.
		new defuserAuthid[40]
		new bool:defusing = bool:get_member(c4, m_Grenade_bStartDefuse)
		new Float:defuseLeft = 0.0
		new Float:defuseTotal = 10.0
		if (defusing) {
			new quien = get_member(c4, m_Grenade_pBombDefuser)
			if (1 <= quien <= get_maxplayers() && is_user_connected(quien)) {
				get_user_authid(quien, defuserAuthid, charsmax(defuserAuthid))
				if (get_member(quien, m_bHasDefuser)) defuseTotal = 5.0
			} else {
				find_defuser(bombPos, defuserAuthid, charsmax(defuserAuthid))
			}
			defuseLeft = Float:get_member(c4, m_Grenade_flDefuseCountDown) - ahora
			if (defuseLeft < 0.0) defuseLeft = 0.0
			if (defuseLeft > defuseTotal) defuseLeft = defuseTotal
		}

		pos += formatex(g_line[pos], charsmax(g_line) - pos,
			",^"bomb^":{^"planted^":1,^"pos^":[%.1f,%.1f,%.1f],^"timeLeft^":%.1f,^"timer^":%.0f,^"defusing^":%d,^"defuserSid^":^"%s^"",
			bombPos[0], bombPos[1], bombPos[2], timeLeft, c4timer, defusing ? 1 : 0, defuserAuthid)
		if (defusing) {
			pos += formatex(g_line[pos], charsmax(g_line) - pos, ",^"defuseLeft^":%.1f,^"defuseTotal^":%.0f", defuseLeft, defuseTotal)
		}
		pos += format(g_line[pos], charsmax(g_line) - pos, "}")
	} else {
		g_bombPlanted = false
		pos += format(g_line[pos], charsmax(g_line) - pos, ",^"bomb^":null")
		// Sin bomba plantada, si alguien la esta plantando: quien (authid y
		// userid, como los jugadores del snap), cuanto le falta y el total.
		new Float:plantLeft
		new planter = find_planter(plantLeft)
		if (planter) {
			new planterAuthid[40]
			get_user_authid(planter, planterAuthid, charsmax(planterAuthid))
			pos += formatex(g_line[pos], charsmax(g_line) - pos,
				",^"planting^":{^"sid^":^"%s^",^"uid^":%d,^"left^":%.1f,^"total^":%.0f}",
				planterAuthid, get_user_userid(planter), plantLeft, C4_PLANT_SECONDS)
		}
		// Tirada en el piso (1.18.0). Quien la lleva no hace falta: va en
		// "hasBomb" de cada jugador.
		new Float:dropPos[3]
		if (find_dropped_c4(dropPos)) {
			pos += formatex(g_line[pos], charsmax(g_line) - pos,
				",^"c4drop^":{^"pos^":[%.1f,%.1f,%.1f]}", dropPos[0], dropPos[1], dropPos[2])
		}
	}

	pos += append_smokes(pos)

	// Con un partido del gestor, su fase (calentamiento, cuchillo...); sin
	// partido, "warmup" en el calentamiento libre.
	new estadoGz[32]
	if (gz_estado_feed(estadoGz, charsmax(estadoGz))) {
		pos += formatex(g_line[pos], charsmax(g_line) - pos, ",^"gz^":^"%s^"", estadoGz)
	}

	// Con un partido del gestor, el nombre de cada equipo segun su lado.
	new nombreCt[64], nombreT[64]
	if (gz_nombres_feed(nombreCt, nombreT, charsmax(nombreCt))) {
		new escCt[130], escT[130]
		json_escape(nombreCt, escCt, charsmax(escCt))
		json_escape(nombreT, escT, charsmax(escT))
		pos += formatex(g_line[pos], charsmax(g_line) - pos, ",^"names^":[^"%s^",^"%s^"]", escCt, escT)
	}

	// Las rondas del mapa y de cada prorroga (1.13.0), para que la consola y
	// el HUD sepan el total real (regie_maxrounds del gestor o mp_maxrounds).
	new maxr, otr
	gz_rondas_feed(maxr, otr)

	// La serie del gestor (1.15.0): mapas ganados por lado y de cuantos es,
	// para que el HUD distinga punto de mapa de punto de partido.
	new serieCt, serieT, serieMapas
	if (gz_serie_feed(serieCt, serieT, serieMapas)) {
		pos += formatex(g_line[pos], charsmax(g_line) - pos, ",^"ser^":[%d,%d],^"bo^":%d", serieCt, serieT, serieMapas)
	}

	// Las derrotas seguidas de cada lado (1.25.0), como las cuenta el juego:
	// con eso el HUD muestra el bonus por derrota.
	pos += formatex(g_line[pos], charsmax(g_line) - pos, ",^"loss^":[%d,%d]",
		get_member_game(m_iNumConsecutiveCTLoses), get_member_game(m_iNumConsecutiveTerroristLoses))
	// Como termino cada ronda del mapa (1.26.0).
	pos += append_resultados(pos)

	// En un tiempo muerto, lo que le queda (1.15.0); si no, la fase del juego.
	new Float:endsIn = gz_pausa_resta()
	if (endsIn < 0.0) endsIn = phase_ends_in(bombTimeLeft)
	pos += formatex(g_line[pos], charsmax(g_line) - pos,
		",^"endsIn^":%.1f,^"score^":[%d,%d],^"obs^":%d,^"maxr^":%d,^"otr^":%d}^n", endsIn, g_scoreCt, g_scoreT, observed_userid(), maxr, otr)

	if (pos >= charsmax(g_line) - 1) {
		// Linea recortada por el limite del buffer (server lleno):
		// mejor no mandar un JSON roto -- se descarta este snapshot,
		// el siguiente tick reintenta.
		return
	}

	new sent = socket_send(g_socket, g_line, pos)
	if (sent <= 0) close_and_retry()
}
