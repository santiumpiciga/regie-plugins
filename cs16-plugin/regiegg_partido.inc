// Los eventos del partido por el feed (1.21.0): todo lo que el backend
// sacaba del log del servidor (logaddress_add, por UDP) sale tambien de
// aca, por la misma conexion TCP del feed, y con mas datos. Es la fuente
// principal: el log queda de respaldo para cuando este plugin no esta
// conectado (backend/src/games/cs16/fuentePartido.ts).
//
// Cada evento es una linea {"t":"m","seq":N,"ts":<unix>,"k":"<tipo>",...}:
//   map       empezo el mapa (al cargar el plugin)           "Started map"
//   commence  "Game_Commencing"
//   rini      termino el freezetime (el "Round_Start" del log), con los
//             vivos de cada equipo ("pl"): la cuenta exacta para el clutch
//   rfin      termino la ronda: bando ganador ("w", vacio en un empate),
//             motivo ("why", el nombre del log), el marcador por bando
//             ("sc": [CT, T]; no "t", que es el tipo del mensaje) y, con un
//             partido del gestor, el de cada equipo
//             ("gz": quien esta de CT, puntos y nombres)
//   mapend    el fin del mapa (intermision): el "Team scored" del log ("sc")
//   kill      con todo: tiro a la cabeza ("hs"), asistencia ("as", y "fa"
//             si fue con un flash), a traves de una pared ("wb"), a ciegas
//             ("bl"), sin mira ("ns"), desde el aire ("air"), a traves del
//             humo ("sm") y los vivos que quedan ("va": [[CT],[T]] por
//             userid)
//   suicide   muerte sin asesino (caida, el mundo, kill en la consola)
//   hurt      daño real (lo que bajo la vida) a un rival
//   connect, enter, team, name, disc: los jugadores (como el log)
//   say       el chat (con los comandos del gestor incluidos)
// Un jugador va como {"n":nombre,"u":userid,"s":authid,"t":bando,"g":equipo
// del gestor (1, 2, o 0 sin partido)}; el bando como lo escribe el log.
//
// No se pierden: van a una cola (mz_cola) y salen de ahi recien cuando el
// backend confirma ("mack <seq>", que confirma esa y todas las anteriores).
// Si se corta la conexion, a la vuelta se manda lo que no se confirmo. En
// cada cambio de mapa AMX Mod X recarga el plugin y la conexion se cae unos
// segundos: lo que no se confirmo se guarda en data/regiegg/<puerto>/
// (MZ_ARCHIVO) y el plugin del mapa nuevo lo manda primero. Por eso el fin
// del mapa nunca se pierde aunque el backend todavia no este cuando sale.
//
// El "hello" dice desde que numero empieza lo nuevo de esta conexion
// ("mseq": lo de antes es lo que estaba en la cola) y la "epoca" de la
// numeracion ("ep"): si el plugin perdio la cola (se cayo el servidor) la
// epoca cambia y el backend no confunde los numeros con los de antes.

#define MZ_ITEM        1536
#define MZ_COLA_MAX    600
#define MZ_POR_CUADRO  32
#define MZ_ARCHIVO     "feed-partido.txt"
// Asistencia: mas de 40 de daño a la victima (como el gestor y MatchZy).
#define MZ_ASISTENCIA  40
// El humo de 1.6 tapa unas 115 unidades alrededor de la granada.
#define MZ_RADIO_HUMO  115.0
// Ciego: medio blanco o mas (flash_alpha va de 0 a 255).
#define MZ_CIEGO       128

new Array:mz_cola
new Array:mz_colaSeq
new mz_ep
new mz_seq
// El ultimo numero que ya salio por esta conexion (0: ninguno). Por numero y
// no por posicion en la cola: las confirmaciones sacan de adelante.
new mz_ultimoEnviado
new mz_buf[MZ_ITEM]
new mz_envio[MZ_ITEM]
new mz_cuerpo[MZ_ITEM]
new mz_j1[256]
new mz_j2[256]
new mz_j3[256]
new bool:mz_mapaCerrado

// Daño real de cada uno a cada uno en la ronda (para la asistencia).
new mz_danio[33][33]
new Float:mz_vidaAntes[33]
// Quien lo cego y hasta cuando (para la asistencia con flash).
new mz_flashDe[33]
new Float:mz_flashHasta[33]
// El ultimo balazo que recibio cada uno: de quien, si atraveso una pared y cuando.
new mz_golpeDe[33]
new bool:mz_golpePared[33]
new Float:mz_golpeCuando[33]
// El bando que ya se aviso (TeamInfo se repite).
new mz_bandoAvisado[33][12]
new mz_tr

// Los motivos de fin de ronda con el nombre que les pone el log de 1.6, por
// ScenarioEventEndRound de ReGameDLL.
new const MZ_MOTIVOS[][] = {
	"", "Target_Bombed", "VIP_Escaped", "VIP_Assassinated", "Terrorists_Escaped",
	"CTs_PreventEscape", "Escaping_Terrorists_Neutralized", "Bomb_Defused", "CTs_Win",
	"Terrorists_Win", "Round_Draw", "All_Hostages_Rescued", "Target_Saved",
	"Hostages_Not_Rescued", "Terrorists_Not_Escaped", "VIP_Not_Escaped"
}

// Antes del gestor: el chat se ve aunque el gestor se quede con el comando
// (.ready no llega al log, pero aca si).
stock partido_init_chat() {
	register_clcmd("say", "mz_say")
	register_clcmd("say_team", "mz_say")
}

// Despues del gestor: su fin de ronda ya sumo los puntos de cada equipo.
stock partido_init() {
	mz_cola = ArrayCreate(MZ_ITEM)
	mz_colaSeq = ArrayCreate(1)
	mz_tr = create_tr2()
	RegisterHookChain(RG_RoundEnd, "mz_fin_ronda", true)
	RegisterHookChain(RG_CSGameRules_GoToIntermission, "mz_intermision", true)
	RegisterHookChain(RG_PlayerBlind, "mz_ciego", true)
	// Antes de que explote (pre): la granada todavia es valida y el aviso sale
	// antes que las cegueras de esa flash.
	RegisterHookChain(RG_CGrenade_ExplodeFlashbang, "mz_flash_explota", false)
	RegisterHookChain(RG_CBasePlayer_TraceAttack, "mz_balazo", true)
	RegisterHookChain(RG_CBasePlayer_TakeDamage, "mz_danio_pre", false)
	RegisterHookChain(RG_CBasePlayer_TakeDamage, "mz_danio_post", true)
	register_event("DeathMsg", "mz_muerte", "a")
	register_event("TeamInfo", "mz_bando", "a")
	register_logevent("mz_log_commence", 2, "0=World triggered", "1=Game_Commencing")
	mz_leer_cola()
	new mapa[32]
	get_mapname(mapa, charsmax(mapa))
	formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"map^":^"%s^"", mapa)
	mz_encolar("map", mz_cuerpo)
}

stock partido_end() {
	mz_guardar_cola()
}

// --------------------------------------------------------------------
// La cola
// --------------------------------------------------------------------

stock mz_encolar(const tipo[], const cuerpo[]) {
	mz_seq++
	formatex(mz_buf, charsmax(mz_buf), "{^"t^":^"m^",^"seq^":%d,^"ts^":%d,^"k^":^"%s^"%s}", mz_seq, get_systime(), tipo, cuerpo)
	if (ArraySize(mz_cola) >= MZ_COLA_MAX) {
		ArrayDeleteItem(mz_cola, 0)
		ArrayDeleteItem(mz_colaSeq, 0)
	}
	ArrayPushString(mz_cola, mz_buf)
	ArrayPushCell(mz_colaSeq, mz_seq)
	partido_mandar()
}

// Lo que va en el hello: la epoca, desde donde es nuevo y la hora del servidor.
stock partido_hello(out[], len) {
	return formatex(out, len, ",^"m^":1,^"ep^":%d,^"mseq^":%d,^"now^":%d", mz_ep, mz_seq + 1, get_systime())
}

// Conexion nueva: se vuelve a mandar todo lo que no se confirmo.
stock partido_conexion_nueva() {
	mz_ultimoEnviado = 0
}

// Lo que falta mandar, de a poco por cuadro (despues del hello).
stock partido_mandar() {
	if (g_socket == -1 || !g_helloSent) return
	new n = ArraySize(mz_cola)
	new mandados = 0
	for (new i = 0; i < n && mandados < MZ_POR_CUADRO; i++) {
		new seq = ArrayGetCell(mz_colaSeq, i)
		if (seq <= mz_ultimoEnviado) continue
		// Aparte de mz_buf: se puede llamar mientras mz_encolar lo esta usando.
		ArrayGetString(mz_cola, i, mz_envio, charsmax(mz_envio) - 1)
		add(mz_envio, charsmax(mz_envio), "^n")
		if (socket_send(g_socket, mz_envio, strlen(mz_envio)) <= 0) {
			close_and_retry()
			return
		}
		mz_ultimoEnviado = seq
		mandados++
	}
}

// "mack <seq>": esa y las anteriores ya las tiene el backend.
stock partido_ack(seq) {
	while (ArraySize(mz_colaSeq) > 0 && ArrayGetCell(mz_colaSeq, 0) <= seq) {
		ArrayDeleteItem(mz_cola, 0)
		ArrayDeleteItem(mz_colaSeq, 0)
	}
}

stock mz_guardar_cola() {
	new path[256]
	gz_archivo(MZ_ARCHIVO, path, charsmax(path))
	new f = fopen(path, "wt")
	if (!f) return
	fprintf(f, "%d %d^n", mz_ep, mz_seq)
	new n = ArraySize(mz_cola)
	for (new i = 0; i < n; i++) {
		ArrayGetString(mz_cola, i, mz_buf, charsmax(mz_buf))
		fprintf(f, "%s^n", mz_buf)
	}
	fclose(f)
}

// La epoca y el numero siguen los del mapa anterior; sin archivo (primera
// vez, o el servidor se cayo sin guardarlo), epoca nueva.
stock mz_leer_cola() {
	mz_ep = get_systime()
	mz_seq = 0
	mz_ultimoEnviado = 0
	new path[256]
	gz_archivo(MZ_ARCHIVO, path, charsmax(path))
	if (!file_exists(path)) return
	new f = fopen(path, "rt")
	if (!f) return
	new bool:primera = true
	while (!feof(f)) {
		mz_buf[0] = 0
		fgets(f, mz_buf, charsmax(mz_buf))
		trim(mz_buf)
		if (!mz_buf[0]) continue
		if (primera) {
			primera = false
			new a[16], b[16]
			parse(mz_buf, a, charsmax(a), b, charsmax(b))
			if (str_to_num(a) > 0) {
				mz_ep = str_to_num(a)
				mz_seq = str_to_num(b)
			}
			continue
		}
		// El numero del evento guardado, del texto: "seq":N.
		new i = contain(mz_buf, "^"seq^":")
		if (i == -1) continue
		new seq = str_to_num(mz_buf[i + 6])
		if (seq <= 0 || ArraySize(mz_cola) >= MZ_COLA_MAX) continue
		ArrayPushString(mz_cola, mz_buf)
		ArrayPushCell(mz_colaSeq, seq)
		if (seq > mz_seq) mz_seq = seq
	}
	fclose(f)
	delete_file(path)
}

// --------------------------------------------------------------------
// Jugadores
// --------------------------------------------------------------------

// El bando del marcador (TeamInfo): sirve tambien recien conectado, cuando el
// jugador todavia no tiene los datos del juego y cs_get_user_team da error.
stock mz_bando_de(id, out[], len) {
	get_user_team(id, out, len)
	if (!equal(out, "TERRORIST") && !equal(out, "CT") && !equal(out, "SPECTATOR")) out[0] = 0
}

// {"n":..,"u":..,"s":..,"t":..,"g":..}. Con otro nombre (el viejo, al cambiarlo).
stock mz_jugador(id, out[], len, const nombre[] = "") {
	new n[32], esc[72], sid[40], bando[12]
	if (nombre[0]) copy(n, charsmax(n), nombre)
	else get_user_name(id, n, charsmax(n))
	json_escape(n, esc, charsmax(esc))
	get_user_authid(id, sid, charsmax(sid))
	mz_bando_de(id, bando, charsmax(bando))
	return formatex(out, len, "{^"n^":^"%s^",^"u^":%d,^"s^":^"%s^",^"t^":^"%s^",^"g^":%d}",
		esc, get_user_userid(id), sid, bando, gz_estado != GS_NONE ? gz_equipo_de(id) : 0)
}

public client_authorized(id, const authid[]) {
	if (is_user_hltv(id)) return
	new ip[24]
	get_user_ip(id, ip, charsmax(ip), 1)
	mz_jugador(id, mz_j1, charsmax(mz_j1))
	formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"p^":%s,^"ip^":^"%s^"", mz_j1, ip)
	mz_encolar("connect", mz_cuerpo)
}

// Lo llama client_putinserver del gestor (hay uno solo por plugin).
stock partido_entra(id) {
	mz_bandoAvisado[id][0] = 0
	mz_olvidar(id)
	if (is_user_hltv(id)) return
	mz_jugador(id, mz_j1, charsmax(mz_j1))
	formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"p^":%s", mz_j1)
	mz_encolar("enter", mz_cuerpo)
}

// Lo llama client_disconnected del gestor.
stock partido_sale(id) {
	mz_bandoAvisado[id][0] = 0
	if (is_user_hltv(id)) return
	mz_jugador(id, mz_j1, charsmax(mz_j1))
	formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"p^":%s", mz_j1)
	mz_encolar("disc", mz_cuerpo)
	mz_olvidar(id)
}

stock mz_olvidar(id) {
	for (new i = 0; i < 33; i++) {
		mz_danio[id][i] = 0
		mz_danio[i][id] = 0
	}
	mz_flashDe[id] = 0
	mz_golpeDe[id] = 0
}

// El bando cuando cambia (el "joined team" del log).
public mz_bando() {
	new id = read_data(1)
	if (id < 1 || id > 32 || !is_user_connected(id) || is_user_hltv(id)) return
	new bando[12]
	read_data(2, bando, charsmax(bando))
	if (!equal(bando, "TERRORIST") && !equal(bando, "CT") && !equal(bando, "SPECTATOR")) return
	if (equal(bando, mz_bandoAvisado[id])) return
	copy(mz_bandoAvisado[id], charsmax(mz_bandoAvisado[]), bando)
	mz_jugador(id, mz_j1, charsmax(mz_j1))
	formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"p^":%s,^"team^":^"%s^"", mz_j1, bando)
	mz_encolar("team", mz_cuerpo)
}

public client_infochanged(id) {
	if (!is_user_connected(id) || is_user_hltv(id)) return
	new viejo[32], nuevo[32]
	get_user_name(id, viejo, charsmax(viejo))
	get_user_info(id, "name", nuevo, charsmax(nuevo))
	if (!nuevo[0] || equal(viejo, nuevo)) return
	new esc[72]
	json_escape(nuevo, esc, charsmax(esc))
	mz_jugador(id, mz_j1, charsmax(mz_j1), viejo)
	formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"p^":%s,^"nn^":^"%s^"", mz_j1, esc)
	mz_encolar("name", mz_cuerpo)
}

public mz_say(id) {
	if (!is_user_connected(id) || is_user_hltv(id)) return PLUGIN_CONTINUE
	new cmd[12], texto[192], esc[200]
	read_argv(0, cmd, charsmax(cmd))
	read_args(texto, charsmax(texto))
	remove_quotes(texto)
	trim(texto)
	if (!texto[0]) return PLUGIN_CONTINUE
	json_escape(texto, esc, charsmax(esc))
	mz_jugador(id, mz_j1, charsmax(mz_j1))
	formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"p^":%s,^"msg^":^"%s^",^"tm^":%d", mz_j1, esc, equal(cmd, "say_team") ? 1 : 0)
	mz_encolar("say", mz_cuerpo)
	return PLUGIN_CONTINUE
}

// --------------------------------------------------------------------
// Rondas y mapa
// --------------------------------------------------------------------

public mz_log_commence() {
	mz_encolar("commence", "")
}

// La ronda nueva (el reinicio, antes del freezetime): lo de la anterior no cuenta.
stock partido_reinicio() {
	for (new i = 0; i < 33; i++) {
		for (new j = 0; j < 33; j++) mz_danio[i][j] = 0
		mz_flashDe[i] = 0
		mz_golpeDe[i] = 0
	}
}

// Termino el freezetime: el "Round_Start" del log, con los vivos.
stock partido_ronda_en_vivo() {
	new pos = formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"pl^":[")
	new bool:primero = true
	for (new id = 1; id <= MaxClients; id++) {
		if (!is_user_connected(id) || !is_user_alive(id)) continue
		new CsTeams:lado = cs_get_user_team(id)
		if (lado != CS_TEAM_T && lado != CS_TEAM_CT) continue
		new n = mz_jugador(id, mz_j1, charsmax(mz_j1))
		// Que no se pase del tamaño de un evento (32 jugadores con nombres largos).
		if (pos + n + 4 >= charsmax(mz_cuerpo)) break
		pos += formatex(mz_cuerpo[pos], charsmax(mz_cuerpo) - pos, "%s%s", primero ? "" : ",", mz_j1)
		primero = false
	}
	formatex(mz_cuerpo[pos], charsmax(mz_cuerpo) - pos, "]")
	mz_encolar("rini", mz_cuerpo)
}

public mz_fin_ronda(WinStatus:estado, ScenarioEventEndRound:evento, Float:demora) {
	if (!GetHookChainReturn(ATYPE_BOOL)) return HC_CONTINUE
	if (evento == ROUND_NONE || evento == ROUND_GAME_COMMENCE || evento == ROUND_GAME_RESTART || evento == ROUND_GAME_OVER) return HC_CONTINUE
	new e = _:evento
	new motivo[40]
	if (e >= 0 && e < sizeof(MZ_MOTIVOS)) copy(motivo, charsmax(motivo), MZ_MOTIVOS[e])
	new ganador[12]
	if (estado == WINSTATUS_CTS) copy(ganador, charsmax(ganador), "CT")
	else if (estado == WINSTATUS_TERRORISTS) copy(ganador, charsmax(ganador), "TERRORIST")
	new pos = formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"w^":^"%s^",^"why^":^"%s^",^"sc^":[%d,%d]",
		ganador, motivo, get_member_game(m_iNumCTWins), get_member_game(m_iNumTerroristWins))
	// Con un partido del gestor: los puntos de cada equipo y quien esta de CT.
	// GS_POST tambien: la ultima ronda (gz_fin_ronda corre antes y ya cerro el
	// mapa).
	if (gz_estado == GS_LIVE || gz_estado == GS_POST) {
		new n1[72], n2[72]
		json_escape(gz_nombre[1], n1, charsmax(n1))
		json_escape(gz_nombre[2], n2, charsmax(n2))
		formatex(mz_cuerpo[pos], charsmax(mz_cuerpo) - pos, ",^"gz^":{^"ct^":%d,^"s1^":%d,^"s2^":%d,^"n1^":^"%s^",^"n2^":^"%s^"}",
			gz_equipo_del_lado(CS_TEAM_CT), gz_puntos[1], gz_puntos[2], n1, n2)
	} else if (gz_estado != GS_NONE) {
		// Una ronda del partido que no es del marcador (el cuchillo: al llegar
		// aca ya es waiting_for_knife_decision): la fase, para que el backend
		// no la cuente (1.23.0).
		formatex(mz_cuerpo[pos], charsmax(mz_cuerpo) - pos, ",^"gzf^":^"%s^"", GZ_ESTADOS[gz_estado])
	}
	mz_encolar("rfin", mz_cuerpo)
	return HC_CONTINUE
}

// El fin del mapa: sale antes del cambio, y si el backend no esta, queda en
// la cola (y en el archivo) hasta que vuelva.
public mz_intermision() {
	if (mz_mapaCerrado) return HC_CONTINUE
	mz_mapaCerrado = true
	formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"sc^":[%d,%d]", get_member_game(m_iNumCTWins), get_member_game(m_iNumTerroristWins))
	mz_encolar("mapend", mz_cuerpo)
	return HC_CONTINUE
}

// --------------------------------------------------------------------
// Daño, flashes y balazos
// --------------------------------------------------------------------

public mz_ciego(const index, const inflictor, const attacker, const Float:fadeTime, const Float:fadeHold, const alpha, Float:color[3]) {
	if (index < 1 || index > 32) return HC_CONTINUE
	mz_flashDe[index] = (1 <= attacker <= 32) ? attacker : 0
	// Cuenta mientras dura el blanco y la mitad del fundido.
	mz_flashHasta[index] = get_gametime() + fadeHold + fadeTime * 0.5
	// Al HUD (1.28.0, send_flash): cuanto queda ciego de verdad. El blanco se
	// sostiene fadeHold con ese alfa y despues baja en linea recta durante
	// fadeTime (como lo dibuja el cliente, ver ev_screenfade); ciego es
	// mientras el alfa sea MZ_CIEGO o mas, lo mismo que el "bl" de la kill.
	// Una flash de costado (alfa 200) dura menos que una de frente (255).
	if (!is_user_connected(index)) return HC_CONTINUE
	new ms = 0
	if (alpha >= MZ_CIEGO)
		ms = floatround((fadeHold + fadeTime * (1.0 - float(MZ_CIEGO) / float(alpha))) * 1000.0)
	new tirador = (1 <= attacker <= 32 && is_user_connected(attacker)) ? attacker : 0
	new companero = tirador && tirador != index && get_user_team(tirador) == get_user_team(index) ? 1 : 0
	send_flash("player_blind", index, tirador, ms, companero, inflictor > 0 ? inflictor : 0)
	return HC_CONTINUE
}

// Exploto una flash (1.28.0): cuenta para el resumen aunque no ciegue a nadie.
public mz_flash_explota(const this, tracehandle, const bitsDamageType) {
	if (this <= 0 || !pev_valid(this)) return HC_CONTINUE
	new duenio = get_entvar(this, var_owner)
	if (duenio < 1 || duenio > 32 || !is_user_connected(duenio)) return HC_CONTINUE
	send_flash("flashbang_detonate", 0, duenio, 0, 0, this)
	return HC_CONTINUE
}

// Un balazo: si entre los ojos del que tiro y donde pego hay una pared, la atraveso.
public mz_balazo(const this, pevAttacker, Float:flDamage, Float:vecDir[3], tracehandle, bitsDamageType) {
	if (this < 1 || this > 32 || !(bitsDamageType & DMG_BULLET)) return HC_CONTINUE
	if (pevAttacker < 1 || pevAttacker > 32 || !is_user_connected(pevAttacker)) return HC_CONTINUE
	new Float:ojo[3], Float:ofs[3], Float:fin[3]
	pev(pevAttacker, pev_origin, ojo)
	pev(pevAttacker, pev_view_ofs, ofs)
	ojo[0] += ofs[0]
	ojo[1] += ofs[1]
	ojo[2] += ofs[2]
	get_tr2(tracehandle, TR_vecEndPos, fin)
	engfunc(EngFunc_TraceLine, ojo, fin, IGNORE_MONSTERS, pevAttacker, mz_tr)
	new Float:fraccion
	get_tr2(mz_tr, TR_flFraction, fraccion)
	mz_golpeDe[this] = pevAttacker
	mz_golpePared[this] = fraccion < 1.0
	mz_golpeCuando[this] = get_gametime()
	return HC_CONTINUE
}

public mz_danio_pre(const victima, inflictor, atacante, Float:danio, tipo) {
	if (1 <= victima <= 32) mz_vidaAntes[victima] = Float:get_entvar(victima, var_health)
	return HC_CONTINUE
}

public mz_danio_post(const victima, inflictor, atacante, Float:danio, tipo) {
	if (victima < 1 || victima > 32 || atacante < 1 || atacante > 32 || atacante == victima) return HC_CONTINUE
	if (!is_user_connected(victima) || !is_user_connected(atacante)) return HC_CONTINUE
	if (get_user_team(atacante) == get_user_team(victima)) return HC_CONTINUE
	// La bomba no es daño de quien la planto (como el gestor y MatchZy).
	if (gz_es_c4(inflictor)) return HC_CONTINUE
	new Float:ahora = Float:get_entvar(victima, var_health)
	if (ahora < 0.0) ahora = 0.0
	new real = floatround(mz_vidaAntes[victima] - ahora)
	if (real <= 0) return HC_CONTINUE
	mz_danio[atacante][victima] += real
	mz_jugador(atacante, mz_j1, charsmax(mz_j1))
	mz_jugador(victima, mz_j2, charsmax(mz_j2))
	formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"a^":%s,^"v^":%s,^"d^":%d", mz_j1, mz_j2, real)
	mz_encolar("hurt", mz_cuerpo)
	return HC_CONTINUE
}

// --------------------------------------------------------------------
// Muertes
// --------------------------------------------------------------------

stock bool:mz_francotirador(const arma[]) {
	return equal(arma, "awp") || equal(arma, "scout") || equal(arma, "g3sg1") || equal(arma, "sg550")
}

// Con la mira puesta: el AWP y el scout la sacan al tirar (y la vuelven a
// poner despues: m_bResumeZoom), asi que en la muerte ya no esta.
stock bool:mz_con_mira(id) {
	new fov = get_member(id, m_iFOV)
	if (fov > 0 && fov < 90) return true
	return bool:get_member(id, m_bResumeZoom)
}

// La distancia de un punto a un segmento, al cuadrado.
stock Float:mz_dist2_segmento(const Float:p[3], const Float:a[3], const Float:b[3]) {
	new Float:ab[3], Float:ap[3]
	for (new k = 0; k < 3; k++) {
		ab[k] = b[k] - a[k]
		ap[k] = p[k] - a[k]
	}
	new Float:largo2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]
	new Float:t = 0.0
	if (largo2 > 0.0) t = floatclamp((ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / largo2, 0.0, 1.0)
	new Float:d2 = 0.0
	for (new k = 0; k < 3; k++) {
		new Float:c = a[k] + ab[k] * t - p[k]
		d2 += c * c
	}
	return d2
}

// Entre los ojos de los dos hay un humo desplegado (ver append_smokes).
stock bool:mz_por_el_humo(a, b) {
	new Float:ojoA[3], Float:ojoB[3], Float:ofs[3]
	pev(a, pev_origin, ojoA)
	pev(a, pev_view_ofs, ofs)
	for (new k = 0; k < 3; k++) ojoA[k] += ofs[k]
	pev(b, pev_origin, ojoB)
	new Float:ahora = get_gametime()
	new ent = -1
	while ((ent = engfunc(EngFunc_FindEntityByString, ent, "classname", "grenade")) != 0) {
		if (!pev_valid(ent)) continue
		new modelo[32]
		pev(ent, pev_model, modelo, charsmax(modelo))
		if (!equal(modelo, "models/w_smokegrenade.mdl")) continue
		new Float:dmgtime
		pev(ent, pev_dmgtime, dmgtime)
		if (!(pev(ent, pev_flags) & FL_ONGROUND) || ahora < dmgtime) continue
		new Float:centro[3]
		pev(ent, pev_origin, centro)
		// La nube sube desde la granada: el centro, a la altura de una persona.
		centro[2] += 48.0
		if (mz_dist2_segmento(centro, ojoA, ojoB) <= MZ_RADIO_HUMO * MZ_RADIO_HUMO) return true
	}
	return false
}

// ,"va":[[userids de CT vivos],[de T]] sin el que acaba de morir.
stock mz_vivos(muerto, out[], len) {
	new pos = formatex(out, len, ",^"va^":[[")
	for (new lado = 0; lado < 2; lado++) {
		new CsTeams:quiero = lado == 0 ? CS_TEAM_CT : CS_TEAM_T
		new bool:primero = true
		for (new id = 1; id <= MaxClients; id++) {
			if (id == muerto || !is_user_connected(id) || !is_user_alive(id)) continue
			if (cs_get_user_team(id) != quiero) continue
			pos += formatex(out[pos], len - pos, primero ? "%d" : ",%d", get_user_userid(id))
			primero = false
		}
		pos += formatex(out[pos], len - pos, lado == 0 ? "],[" : "]]")
	}
	return pos
}

public mz_muerte() {
	new asesino = read_data(1)
	new victima = read_data(2)
	if (victima < 1 || victima > 32 || !is_user_connected(victima)) return
	new arma[32]
	read_data(4, arma, charsmax(arma))
	new vivos[200]
	mz_vivos(victima, vivos, charsmax(vivos))
	mz_jugador(victima, mz_j2, charsmax(mz_j2))
	new bool:conAsesino = asesino >= 1 && asesino <= 32 && asesino != victima && is_user_connected(asesino)
	if (!conAsesino) {
		formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"v^":%s,^"w^":^"%s^"%s", mz_j2, arma, vivos)
		mz_encolar("suicide", mz_cuerpo)
		return
	}
	mz_jugador(asesino, mz_j1, charsmax(mz_j1))
	new Float:ahora = get_gametime()
	new bool:granada = bool:equal(arma, "grenade")
	new bool:vivo = bool:is_user_alive(asesino)
	new bool:enemigos = get_user_team(asesino) != get_user_team(victima)

	// La asistencia: el que mas daño le hizo (mas de 40) del equipo del
	// asesino; si nadie, el que lo cego (si sigue ciego).
	new asistio = 0, flash = 0
	if (enemigos) {
		new mejor = MZ_ASISTENCIA
		for (new p = 1; p <= MaxClients; p++) {
			if (p == asesino || p == victima || !is_user_connected(p)) continue
			if (get_user_team(p) != get_user_team(asesino)) continue
			if (mz_danio[p][victima] > mejor) {
				mejor = mz_danio[p][victima]
				asistio = p
			}
		}
		if (!asistio) {
			new f = mz_flashDe[victima]
			if (f >= 1 && f <= 32 && f != asesino && f != victima && is_user_connected(f) && get_user_team(f) == get_user_team(asesino) && ahora < mz_flashHasta[victima]) {
				asistio = f
				flash = 1
			}
		}
	}
	new asist[280]
	asist[0] = 0
	if (asistio) {
		mz_jugador(asistio, mz_j3, charsmax(mz_j3))
		formatex(asist, charsmax(asist), ",^"as^":%s,^"fa^":%d", mz_j3, flash)
	}
	new bool:pared = mz_golpeDe[victima] == asesino && mz_golpePared[victima] && ahora - mz_golpeCuando[victima] < 0.5
	new bool:sinMira = vivo && mz_francotirador(arma) && !mz_con_mira(asesino)
	new bool:aire = vivo && !granada && !(pev(asesino, pev_flags) & FL_ONGROUND) && pev(asesino, pev_movetype) != MOVETYPE_FLY
	new bool:ciego = vivo && flash_alpha(asesino) >= MZ_CIEGO
	new bool:humo = !granada && mz_por_el_humo(asesino, victima)
	formatex(mz_cuerpo, charsmax(mz_cuerpo), ",^"a^":%s,^"v^":%s,^"w^":^"%s^",^"hs^":%d%s,^"wb^":%d,^"bl^":%d,^"ns^":%d,^"air^":%d,^"sm^":%d%s",
		mz_j1, mz_j2, arma, read_data(3) ? 1 : 0, asist, pared ? 1 : 0, ciego ? 1 : 0, sinMira ? 1 : 0, aire ? 1 : 0, humo ? 1 : 0, vivos)
	mz_encolar("kill", mz_cuerpo)
}
