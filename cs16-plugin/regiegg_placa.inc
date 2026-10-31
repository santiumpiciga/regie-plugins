// La placa de publicidad con imagen en el centro de la pantalla (1.20.0),
// como la de CS2 (cs2-plugin/PublicidadEnPantalla.cs). La imagen del
// sponsor la convierte el backend (backend/src/placaCs16.ts) en sprites de
// GoldSrc, uno o varios en grilla (cada uno de hasta 256x256, 255 colores
// y el 255 transparente), y los deja en cstrike/sprites/regie/ con una
// lista: sprites/regie/placas.ini.
//
// Por que asi: un servidor de 1.6 no puede dibujar una imagen propia en la
// pantalla del jugador (el HUD del cliente solo conoce los sprites de su
// hud.txt, y una descarga nunca pisa un archivo que el jugador ya tiene).
// Lo que si puede es poner una entidad con un sprite en el mundo y
// mandarsela a cada jugador en otro lugar (AddToFullPack): justo delante de
// SU camara, con el tamaño que ocupa en su pantalla. Los sprites de tipo
// VP_PARALLEL miran siempre a la camara: queda como una placa sobre la
// pantalla. Es la tecnica de los "HUD sprites" de los plugins de AMX Mod X
// (env_sprite a unos pocos unidades de los ojos, escala ~0.03).
//
// Al cargar el mapa (plugin_precache) se registran los sprites de la lista
// con precache_model: el juego de cada jugador los baja al entrar (o con la
// descarga rapida, sv_downloadurl). Una placa nueva entra recien en el mapa
// siguiente: el precache es al cargar el mapa.
//
// Comandos (solo desde la consola del servidor o RCON):
//   regie_ads_plate <nombre> <segundos> <ancho> [transmision] [steamid64]
//     La muestra esos segundos (hasta 60) segun las reglas de abajo. ancho:
//     el de la placa en una pantalla de 1080 de alto. transmision (1.21.0):
//     0 = el camarografo de la transmision no la ve (el SteamID64 del
//     camarografo del observador automatico, y el de regie_feed_observer
//     por nombre); 1 o sin el argumento, la ve como cualquier espectador.
//   regie_ads_plate_test <nombre> <segundos> <ancho>
//     A todos los conectados, sin mirar las reglas (la prueba de Produccion).
//   regie_ads_plate_off
//   regie_ads_plates
// Contestan «regie-ads ok <version> placas=<las cargadas>», al mostrar
// tambien «jugadores=<n>» (los que la ven ahora), y si algo no se pudo,
// «regie-ads error: <motivo>» en otra linea.
//
// Quien la ve (fuera de la prueba), como en CS2:
//   - los muertos y los espectadores, siempre;
//   - todos en el calentamiento, el freezetime, las pausas, el veto, la
//     eleccion de lado despues del cuchillo y el fin del mapa;
//   - NUNCA un vivo en plena ronda; y mientras este el veto en pantalla
//     (regie_veto_hud), nadie.
// Los bots y el HLTV no; el camarografo de la transmision, segun la opcion.
//
// La camara de cada uno: la propia (vivo, recien muerto, espectador libre),
// los ojos del que mira en primera persona, o la de la camara de
// seguimiento, que el cliente pone detras del que sigue (cl_chasedist, 112
// unidades, recortada contra las paredes): esa se calcula igual aca, y es
// la unica aproximada. En el mapa desde arriba no se muestra.

#define PL_MAX_PLACAS   8
#define PL_MAX_PEDAZOS  8
#define PL_MAX_TOTAL    24
#define PL_NOMBRE       12
#define PL_LISTA        "sprites/regie/placas.ini"
#define PL_CARPETA      "sprites/regie"
#define PL_CLASE        "regie_placa"
// Distancia de la camara a la placa: cerca, para que no la tape una pared
// (el plano de recorte del motor esta a 4 unidades); en la camara de
// seguimiento mas lejos, por si la camara real no esta justo donde se calcula.
#define PL_DIST_OJO     10.0
#define PL_DIST_SEGUIR  40.0
#define PL_SEGUIR       112.0
// Alto visible a una distancia d con el campo de vision de siempre (90
// horizontal en 4:3, el vertical se mantiene en pantallas anchas): 1.5 * d.
#define PL_ALTO_VISIBLE 1.5
#define PL_PANTALLA     1080.0

new pl_nombre[PL_MAX_PLACAS][PL_NOMBRE]
new pl_ancho[PL_MAX_PLACAS]
new pl_alto[PL_MAX_PLACAS]
new pl_cant[PL_MAX_PLACAS]
new Float:pl_dx[PL_MAX_PLACAS][PL_MAX_PEDAZOS]
new Float:pl_dy[PL_MAX_PLACAS][PL_MAX_PEDAZOS]
new pl_cantPlacas

// La que se esta mostrando.
new pl_actual = -1
new pl_ent[PL_MAX_PEDAZOS]
new pl_cantEnt
new Float:pl_hasta
new bool:pl_prueba
new Float:pl_anchoPx = 360.0
// Sin la placa para el camarografo de la transmision (1.21.0).
new bool:pl_sinCamara
new pl_camSid[24]
new pl_esCamCuadro[33]
new bool:pl_esCam[33]

// La camara de cada jugador, calculada una vez por cuadro del servidor.
new pl_cuadro
new pl_camCuadro[33]
new bool:pl_camOk[33]
new Float:pl_camOjo[33][3]
new Float:pl_camFwd[33][3]
new Float:pl_camRight[33][3]
new Float:pl_camUp[33][3]
new Float:pl_camDist[33]
new bool:pl_camSigue[33]
new pl_ultimoVisto[33]

// --------------------------------------------------------------------
// Al cargar el mapa: la lista y el precache
// --------------------------------------------------------------------

stock placa_precache() {
	pl_cantPlacas = 0
	new f = fopen(PL_LISTA, "rt")
	if (!f) return
	new linea[256], total = 0
	while (!feof(f) && pl_cantPlacas < PL_MAX_PLACAS) {
		fgets(f, linea, charsmax(linea))
		trim(linea)
		if (!linea[0] || linea[0] == ';' || linea[0] == '/') continue
		new p = pl_cantPlacas
		new tok[32], resto[256]
		strtok2(linea, tok, charsmax(tok), resto, charsmax(resto), ' ', 1)
		if (!placa_nombre_valido(tok)) continue
		copy(pl_nombre[p], PL_NOMBRE - 1, tok)
		copy(linea, charsmax(linea), resto)
		strtok2(linea, tok, charsmax(tok), resto, charsmax(resto), ' ', 1)
		pl_ancho[p] = str_to_num(tok)
		copy(linea, charsmax(linea), resto)
		strtok2(linea, tok, charsmax(tok), resto, charsmax(resto), ' ', 1)
		pl_alto[p] = str_to_num(tok)
		copy(linea, charsmax(linea), resto)
		strtok2(linea, tok, charsmax(tok), resto, charsmax(resto), ' ', 1)
		new n = str_to_num(tok)
		if (pl_ancho[p] <= 0 || pl_alto[p] <= 0 || n <= 0 || n > PL_MAX_PEDAZOS || total + n > PL_MAX_TOTAL) continue
		new bool:bien = true
		for (new i = 0; i < n && bien; i++) {
			copy(linea, charsmax(linea), resto)
			strtok2(linea, tok, charsmax(tok), resto, charsmax(resto), ' ', 1)
			if (!tok[0]) bien = false
			pl_dx[p][i] = str_to_float(tok)
			copy(linea, charsmax(linea), resto)
			strtok2(linea, tok, charsmax(tok), resto, charsmax(resto), ' ', 1)
			if (!tok[0]) bien = false
			pl_dy[p][i] = str_to_float(tok)
			// Un .spr que falta o que no es un sprite de Half-Life tira el
			// servidor entero al registrarlo: se revisa antes.
			new archivo[64]
			formatex(archivo, charsmax(archivo), "%s/%s_%d.spr", PL_CARPETA, pl_nombre[p], i)
			if (!placa_es_sprite(archivo)) bien = false
		}
		if (!bien) continue
		for (new i = 0; i < n; i++) {
			new archivo[64]
			formatex(archivo, charsmax(archivo), "%s/%s_%d.spr", PL_CARPETA, pl_nombre[p], i)
			engfunc(EngFunc_PrecacheModel, archivo)
		}
		pl_cant[p] = n
		total += n
		pl_cantPlacas++
	}
	fclose(f)
	if (pl_cantPlacas) server_print("[Regie.gg] placas de publicidad cargadas: %d (%d sprites)", pl_cantPlacas, total)
}

stock bool:placa_nombre_valido(const s[]) {
	if (strlen(s) != 8) return false
	for (new i = 0; i < 8; i++) {
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false
	}
	return true
}

// "IDSP", version 2 y el tamaño que tiene que tener con su cabecera y paleta.
stock bool:placa_es_sprite(const archivo[]) {
	if (!file_exists(archivo)) return false
	new tam = file_size(archivo)
	if (tam < 830) return false
	new f = fopen(archivo, "rb")
	if (!f) return false
	new magia, version, tipo, formato, radio, ancho, alto
	fread(f, magia, BLOCK_INT)
	fread(f, version, BLOCK_INT)
	fread(f, tipo, BLOCK_INT)
	fread(f, formato, BLOCK_INT)
	fread(f, radio, BLOCK_INT)
	fread(f, ancho, BLOCK_INT)
	fread(f, alto, BLOCK_INT)
	fclose(f)
	// 0x50534449: "IDSP" leido como entero.
	if (magia != 0x50534449 || version != 2) return false
	if (ancho <= 0 || alto <= 0 || ancho > 256 || alto > 256) return false
	return tam == 40 + 2 + 768 + 4 + 16 + ancho * alto
}

stock placa_buscar(const nombre[]) {
	for (new p = 0; p < pl_cantPlacas; p++) if (equal(pl_nombre[p], nombre)) return p
	return -1
}

// --------------------------------------------------------------------
// Ciclo de vida (lo llama RegieGG.sma)
// --------------------------------------------------------------------

stock placa_init() {
	register_srvcmd("regie_ads_plate", "cmd_placa")
	register_srvcmd("regie_ads_plate_test", "cmd_placa_prueba")
	register_srvcmd("regie_ads_plate_off", "cmd_placa_off")
	register_srvcmd("regie_ads_plates", "cmd_placas")
	register_forward(FM_AddToFullPack, "fw_placa_atfp", 1)
	register_forward(FM_StartFrame, "fw_placa_cuadro")
	pl_actual = -1
	pl_cantEnt = 0
}

public fw_placa_cuadro() {
	pl_cuadro++
	if (pl_actual >= 0 && get_gametime() > pl_hasta) placa_sacar()
}

// --------------------------------------------------------------------
// Comandos
// --------------------------------------------------------------------

stock placa_respuesta(jugadores = -1) {
	new lista[PL_MAX_PLACAS * PL_NOMBRE + 8], pos = 0
	for (new p = 0; p < pl_cantPlacas; p++) pos += formatex(lista[pos], charsmax(lista) - pos, "%s%s", p ? "," : "", pl_nombre[p])
	if (jugadores >= 0) server_print("regie-ads ok %s placas=%s jugadores=%d", PLUGIN_VERSION, lista, jugadores)
	else server_print("regie-ads ok %s placas=%s", PLUGIN_VERSION, lista)
}

public cmd_placas() {
	placa_respuesta()
	return PLUGIN_HANDLED
}

public cmd_placa() {
	placa_desde_comando(false)
	return PLUGIN_HANDLED
}

public cmd_placa_prueba() {
	placa_desde_comando(true)
	return PLUGIN_HANDLED
}

public cmd_placa_off() {
	placa_sacar()
	placa_respuesta()
	return PLUGIN_HANDLED
}

stock placa_desde_comando(bool:prueba) {
	new nombre[16], seg[8], ancho[8]
	read_argv(1, nombre, charsmax(nombre))
	read_argv(2, seg, charsmax(seg))
	read_argv(3, ancho, charsmax(ancho))
	new p = placa_buscar(nombre)
	if (p < 0) {
		placa_respuesta()
		server_print("regie-ads error: no_cargada")
		return
	}
	new Float:s = floatclamp(str_to_float(seg), 1.0, 60.0)
	new Float:w = str_to_float(ancho)
	pl_anchoPx = w >= 60.0 && w <= 960.0 ? w : 360.0
	// La transmision (1.21.0): sin el argumento, como antes (la ve).
	new transmision[4]
	read_argv(4, transmision, charsmax(transmision))
	pl_sinCamara = !prueba && transmision[0] == '0'
	read_argv(5, pl_camSid, charsmax(pl_camSid))
	if (!pl_sinCamara || !equal(pl_camSid, "7656119", 7)) pl_camSid[0] = 0
	arrayset(pl_esCamCuadro, -1, sizeof(pl_esCamCuadro))
	if (!placa_mostrar(p, s, prueba)) {
		placa_respuesta()
		server_print("regie-ads error: sin_entidades")
		return
	}
	new cuantos = 0
	for (new id = 1; id <= MaxClients; id++) if (placa_la_ve(id)) cuantos++
	placa_respuesta(cuantos)
}

// --------------------------------------------------------------------
// Las entidades
// --------------------------------------------------------------------

stock bool:placa_mostrar(p, Float:segundos, bool:prueba) {
	if (pl_actual != p) {
		placa_sacar()
		for (new i = 0; i < pl_cant[p]; i++) {
			new ent = engfunc(EngFunc_CreateNamedEntity, engfunc(EngFunc_AllocString, "info_target"))
			if (!pev_valid(ent)) {
				placa_sacar()
				return false
			}
			new archivo[64]
			formatex(archivo, charsmax(archivo), "%s/%s_%d.spr", PL_CARPETA, pl_nombre[p], i)
			set_pev(ent, pev_classname, PL_CLASE)
			engfunc(EngFunc_SetModel, ent, archivo)
			set_pev(ent, pev_movetype, MOVETYPE_NONE)
			set_pev(ent, pev_solid, SOLID_NOT)
			// Alphatest: el 255 de la paleta no se dibuja (kRenderTransAlpha a
			// 255, el "Solid" de los sprites con recorte de los mapas).
			set_pev(ent, pev_rendermode, kRenderTransAlpha)
			set_pev(ent, pev_renderamt, 255.0)
			set_pev(ent, pev_frame, 0.0)
			set_pev(ent, pev_scale, 0.03)
			// Una caja que cubre el mapa entero: el motor la cuenta como visible
			// desde cualquier lado y AddToFullPack se la ofrece a todos (la
			// posicion de verdad la pone fw_placa_atfp para cada uno).
			engfunc(EngFunc_SetSize, ent, Float:{-4096.0, -4096.0, -4096.0}, Float:{4096.0, 4096.0, 4096.0})
			engfunc(EngFunc_SetOrigin, ent, Float:{0.0, 0.0, 0.0})
			pl_ent[i] = ent
			pl_cantEnt = i + 1
		}
		pl_actual = p
		arrayset(pl_ultimoVisto, -10, sizeof(pl_ultimoVisto))
	}
	pl_prueba = prueba
	pl_hasta = get_gametime() + segundos
	return true
}

stock placa_sacar() {
	for (new i = 0; i < pl_cantEnt; i++) {
		if (pev_valid(pl_ent[i])) engfunc(EngFunc_RemoveEntity, pl_ent[i])
		pl_ent[i] = 0
	}
	pl_cantEnt = 0
	pl_actual = -1
	pl_prueba = false
}

stock placa_pedazo(ent) {
	for (new i = 0; i < pl_cantEnt; i++) if (pl_ent[i] == ent) return i
	return -1
}

// --------------------------------------------------------------------
// Quien la ve y donde
// --------------------------------------------------------------------

stock bool:placa_sin_juego() {
	if (gz_pausa != PZ_NINGUNA) return true
	if (equal(g_phase, "freezetime") || equal(g_phase, "warmup")) return true
	// Con el gestor: calentamiento, veto, la eleccion de lado despues del
	// cuchillo y el fin del mapa. La ronda de cuchillo y la cuenta para ir en
	// vivo se juegan: ahi no.
	switch (gz_estado) {
		case GS_WARMUP, GS_PREVETO, GS_VETO, GS_WAIT_KNIFE, GS_POST: return true
	}
	return false
}

stock bool:placa_la_ve(id) {
	if (pl_actual < 0 || !is_user_connected(id) || is_user_bot(id) || is_user_hltv(id)) return false
	if (pl_prueba) return true
	if (g_vetoHud[0]) return false
	if (pl_sinCamara && placa_es_camarografo(id)) return false
	new CsTeams:equipo = cs_get_user_team(id)
	new bool:jugando = is_user_alive(id) && (equipo == CS_TEAM_T || equipo == CS_TEAM_CT)
	if (!jugando) return true
	return placa_sin_juego()
}

// El camarografo de la transmision: por su SteamID64 (el del observador
// automatico) o por el nombre de regie_feed_observer. Una vez por cuadro.
stock bool:placa_es_camarografo(id) {
	if (pl_esCamCuadro[id] == pl_cuadro) return pl_esCam[id]
	pl_esCamCuadro[id] = pl_cuadro
	pl_esCam[id] = false
	if (pl_camSid[0]) {
		new sid[32]
		gz_steam64(id, sid, charsmax(sid))
		if (equal(sid, pl_camSid)) pl_esCam[id] = true
	}
	if (!pl_esCam[id]) {
		new quiero[32]
		get_pcvar_string(g_pcvarObserver, quiero, charsmax(quiero))
		if (quiero[0]) {
			new nombre[32]
			get_user_name(id, nombre, charsmax(nombre))
			if (equali(nombre, quiero)) pl_esCam[id] = true
		}
	}
	return pl_esCam[id]
}

stock placa_ojos(id, Float:ojo[3]) {
	new Float:ofs[3]
	pev(id, pev_origin, ojo)
	pev(id, pev_view_ofs, ofs)
	ojo[0] += ofs[0]
	ojo[1] += ofs[1]
	ojo[2] += ofs[2]
}

stock bool:placa_camara(id) {
	if (pl_camCuadro[id] == pl_cuadro) return pl_camOk[id]
	pl_camCuadro[id] = pl_cuadro
	pl_camOk[id] = false
	pl_camSigue[id] = false
	new Float:ang[3]
	new modo = pev(id, pev_iuser1)
	new obj = pev(id, pev_iuser2)
	new bool:conObjetivo = obj >= 1 && obj <= MaxClients && obj != id && is_user_alive(obj)
	pl_camDist[id] = PL_DIST_OJO
	if (is_user_alive(id) || modo == 0 || modo == OBS_ROAMING || ((modo == OBS_IN_EYE || modo == OBS_CHASE_LOCKED || modo == OBS_CHASE_FREE) && !conObjetivo)) {
		// La propia: vivo, recien muerto (todavia sin modo de espectador),
		// espectador libre, o un modo que sigue a alguien sin a quien seguir.
		placa_ojos(id, pl_camOjo[id])
		pev(id, pev_v_angle, ang)
	} else if (modo == OBS_IN_EYE) {
		placa_ojos(obj, pl_camOjo[id])
		pev(obj, pev_v_angle, ang)
		pl_camSigue[id] = true
	} else if (modo == OBS_CHASE_LOCKED || modo == OBS_CHASE_FREE) {
		// Como V_GetChasePos del SDK de Half-Life: desde el que sigue, 28
		// unidades arriba de su origen, hacia atras cl_chasedist, y si hay
		// una pared en el medio, contra la pared.
		if (modo == OBS_CHASE_LOCKED) pev(obj, pev_v_angle, ang)
		else pev(id, pev_v_angle, ang)
		new Float:base[3], Float:fwd[3], Float:atras[3]
		pev(obj, pev_origin, base)
		base[2] += 28.0
		angle_vector(ang, ANGLEVECTOR_FORWARD, fwd)
		atras[0] = base[0] - fwd[0] * PL_SEGUIR
		atras[1] = base[1] - fwd[1] * PL_SEGUIR
		atras[2] = base[2] - fwd[2] * PL_SEGUIR
		engfunc(EngFunc_TraceLine, base, atras, IGNORE_MONSTERS, obj, 0)
		new Float:fin[3], Float:fraccion
		get_tr2(0, TR_flFraction, fraccion)
		get_tr2(0, TR_vecEndPos, fin)
		if (fraccion < 1.0) {
			// Un poco hacia adelante de la pared, como el cliente.
			fin[0] += fwd[0] * 4.0
			fin[1] += fwd[1] * 4.0
			fin[2] += fwd[2] * 4.0
		}
		pl_camOjo[id] = fin
		pl_camDist[id] = PL_DIST_SEGUIR
		pl_camSigue[id] = true
	} else {
		// El mapa desde arriba (OBS_MAP_FREE, OBS_MAP_CHASE): no hay donde ponerla.
		return false
	}
	angle_vector(ang, ANGLEVECTOR_FORWARD, pl_camFwd[id])
	angle_vector(ang, ANGLEVECTOR_RIGHT, pl_camRight[id])
	angle_vector(ang, ANGLEVECTOR_UP, pl_camUp[id])
	pl_camOk[id] = true
	return true
}

public fw_placa_atfp(es, e, ent, host, hostflags, player, pSet) {
	if (pl_actual < 0 || player || !get_orig_retval()) return FMRES_IGNORED
	new i = placa_pedazo(ent)
	if (i < 0) return FMRES_IGNORED
	if (!placa_la_ve(host) || !placa_camara(host)) {
		set_es(es, ES_Effects, get_es(es, ES_Effects) | EF_NODRAW)
		return FMRES_IGNORED
	}
	new p = pl_actual
	new Float:d = pl_camDist[host]
	// Unidades del mundo por pixel del sprite: el ancho elegido (en una
	// pantalla de 1080 de alto) sobre el ancho de la imagen.
	new Float:k = (pl_anchoPx / PL_PANTALLA) * PL_ALTO_VISIBLE * d / float(pl_ancho[p])
	new Float:pos[3]
	for (new c = 0; c < 3; c++) {
		pos[c] = pl_camOjo[host][c] + pl_camFwd[host][c] * d + pl_camRight[host][c] * (pl_dx[p][i] * k) + pl_camUp[host][c] * (pl_dy[p][i] * k)
	}
	set_es(es, ES_Origin, pos)
	set_es(es, ES_Scale, k)
	// La primera vez que la ve, sin interpolar desde donde estaba (si no
	// vuela desde el centro del mapa). Con su propia camara, nunca: la
	// interpolacion del cliente (cl_interp) la haria llegar tarde a donde
	// mira; mirando a otro, si, igual que la camara que sigue a ese.
	new efectos = get_es(es, ES_Effects) & ~EF_NODRAW
	if (!pl_camSigue[host] || pl_ultimoVisto[host] < pl_cuadro - 1) efectos |= EF_NOINTERP
	set_es(es, ES_Effects, efectos)
	if (i == pl_cantEnt - 1) pl_ultimoVisto[host] = pl_cuadro
	return FMRES_IGNORED
}
