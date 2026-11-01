# Plugin auxiliar de CounterStrikeSharp

Un plugin chico, separado de MatchZy, con piezas independientes:
expone por RCON el SteamID64 real de los jugadores humanos conectados
(`css_liststeamids`), publica un feed del estado del juego a 16 Hz para
el observador automático (`DirectorFeed.cs`), manda al backend los
eventos del partido con stats exactas (`MatchEvents.cs`, abajo) y muestra
la publicidad de los sponsors (`PublicidadEnPantalla.cs`, más abajo).

## Varias instancias de la misma instalación (1.8.0)

El panel puede levantar dos servidores de CS2 con la misma instalación
(`Servidor 1` en 27015, `Servidor 2` en 27016). Comparten la carpeta del
plugin y su config, así que hasta el 1.6.0 los dos intentaban abrir el
feed en el mismo puerto: el segundo no podía, y con eso perdía el chat
del veto, el veto en pantalla y la cámara del director.

- **Un puerto de feed por servidor.** Con `director_port_por_instancia`
  en `true` (el valor por defecto), cada instancia escucha en
  `director_port + (puerto de juego - 27015)`: 27015 → 27100, 27016 →
  27101. El backend hace la misma cuenta (`backend/src/servidoresCs2/puertos.ts`)
  y abre un feed por cada servidor del panel.
- **El `hello` dice de qué servidor es** (`hostport`, `feedport`). Si en el
  puerto de un servidor contesta otro, el backend no le atribuye ese partido.
- **El snapshot trae el marcador**: `teams` (nombre y rondas ganadas de
  cada lado) y, de cada jugador, plata, K/D/A, daño, MVPs, puntaje, casco,
  kit y el inventario entero. Con eso el backend arma el estado del
  partido de cada servidor (`servidoresCs2/estado.ts`) sin un observador
  con GSI. Desde el 1.13.0 también la munición del arma en mano (`clip`,
  `clipMax`, `reserve`), las granadas que lleva repetidas (`nades`) y los
  timeouts que le quedan a cada equipo (`teams[].timeouts`): con eso el
  HUD con el servidor como fuente muestra lo mismo que con el GSI.
- **Teclas del HUD desde el juego** (1.14.0): `css_regie_hud <acción> [show|hide]`
  manda una acción del HUD de la transmisión por el feed (tabla, economía,
  veto, radar grande, ocultar todo, `overlay none`...), que el backend
  aplica como un botón de Control de transmisión. Solo lo puede usar el
  camarógrafo autorizado (su SteamID64 en Producción) o la consola del
  servidor. El panel suma los binds (F5, F6...) al `cs2liveviewer_hud.cfg`.
- **Eventos del partido por instancia**: los reintentos van a
  `<plugin>/outbox/<puerto de juego>/`, y el backup de MatchZy se elige
  por los equipos que tiene puestos ese servidor (`mp_teamname_1/2`) y
  su mapa, porque `MatchZyDataBackup` es uno para todas las instancias.

## Eventos del partido al backend (`MatchEvents.cs`)

El webhook de MatchZy tiene fallas que no se arreglan desde afuera:
descarta `matchzy_remote_log_url` al cargar el match (se pierde
`series_start`) y `round_end` trae KAST y otras stats en cero. Este
módulo manda los mismos eventos (`series_start`, `going_live`,
`round_end` con stats por jugador, `map_result`, `series_end`) más
`player_death` exacto, con `"source":"plugin"`, acumulando
kills/deaths/assists/daño/headshots/MVP/KAST/rondas desde los eventos
del juego. El backend prefiere lo del plugin cuando la misma ronda
llega por las dos vías (`backend/src/routes/webhook.ts`).

- **Contexto del match** (matchid, número de mapa, qué equipo es
  team1/team2 y de qué lado está, marcador de la serie, cantidad de
  mapas, URL y header del webhook): se lee del backup más nuevo que
  MatchZy escribe al arrancar cada ronda en
  `<csgo>/MatchZyDataBackup/matchzy_<matchid>_<mapa>_round<NN>.json`.
  Por eso no hace falta configurar nada: usa la misma URL y el mismo
  header que MatchZy ya tiene para ese match.
- **De qué servidor es** (1.12.0): cada evento lleva `server_port`, el
  puerto de juego de la instancia. El webhook de MatchZy no dice nada
  del servidor, todas las instancias comparten su config (la misma URL
  y el mismo header, que ya usa el token) y un pug (`.ready` sin config)
  no aparece en `get5_status`: con varios servidores, esto es lo que le
  dice al backend dónde se juega cada matchid, también el de un pug.
- **Si el backend no responde**, el evento queda en
  `<plugin>/outbox/<puerto de juego>/*.json` y se reintenta cada 10 s en orden (lo que
  MatchZy no hace). Se descarta si el backend contesta 4xx (salvo
  408/429) y después de 7 días.
- **Config** (`configs/plugins/RegieGG/RegieGG.json`):
  `match_events_enabled` (default `true`) y `match_events_url` (default
  vacío = la URL del match cargado en MatchZy; ponerla solo si el
  backend escucha en otra dirección que la del webhook de MatchZy).
- **Estado**: `css_lv_events_status` por RCON (enviados, encolados,
  contexto leído, último error).
- **Límites**: sin match cargado en MatchZy (sin backup) no se manda
  nada; las stats no retroceden si se restaura una ronda con
  `css_restore` (MatchZy sí las restaura); las rondas de cuchillo no
  cuentan (no hay backup todavía cuando pasan); la ronda de cuchillo
  del mapa siguiente tampoco, porque el mapa anterior ya se dio por
  terminado. `series_end` se deduce de `NumMaps` y de los mapas ganados
  (`.endmatch` a mano no lo genera: lo manda MatchZy si su webhook
  anda).

## Chat del veto por servidor (desde la 1.3.0)

Reenvía por el mismo feed del observador los comandos del veto de mapas
en el chat (`.vetar`, `.elegir`, `.lado`, `.veto`), con el SteamID64 de
quien los escribió. **Solo esos**: el resto del chat no sale del
servidor. Los usa `backend/src/vetoServidor/`, que exige esta versión
del plugin (ver [docs/veto-en-servidor.md](../docs/veto-en-servidor.md)).

Desde la 1.4.0 además lo muestra (`VetoEnPantalla.cs`), con dos comandos
que manda el backend por RCON y que solo valen desde la consola del
servidor:

- `css_regie_veto_chat <tipo> <base64url>`: un mensaje del veto, con
  tildes, el prefijo «[Veto]» y el color de su tipo (`info`, `ban`,
  `pick`, `lado`, `turno`, `aviso`, `error`, `resultado`).
- `css_regie_veto_panel <id> <parte> <total> <base64url>` / `off`: el
  estado del veto, fijo en el centro de la pantalla mientras dure, y el
  menú del capitán de turno; lo que elige ahí se reenvía como si lo
  hubiera escrito.

Los dos contestan `regie-veto ok`; si no, el backend usa `say`.

Desde la 1.7.0, si el panel trae la clave `i` (la dirección http(s) de la
tarjeta del veto en imagen, que arma `backend/src/vetoServidor/tarjeta.ts`),
el centro de la pantalla muestra esa imagen en vez del texto. Una dirección
con comillas, `<>` o espacios se ignora y queda el texto.

Desde la 1.9.0, la clave `f` del panel es una línea al pie (debajo de la
tarjeta o del texto): la cuenta para pasar al primer mapa cuando termina
el veto. El cambio de mapa lo manda el backend (`changelevel`).

Los comandos de consola del plugin (`css_director_*`, `css_liststeamids`,
`css_lv_events_status`) solo responden desde la consola del servidor (el
backend por RCON) o al camarógrafo autorizado (el SteamID64 de
Producción). `css_director_cameraman`, que cambia quién es el
camarógrafo, solo desde el servidor.
Necesita `director_enabled` (el feed), que viene prendido.

## Publicidad de sponsors en pantalla (desde la 1.5.0)

`PublicidadEnPantalla.cs`: una placa fija en el centro de la pantalla
con la imagen del sponsor de turno y una o dos líneas de texto, cada una
con su color. Reemplaza a `sv_server_graphic1` (la placa nativa del
espectador), que CS2 no muestra. La arma y la rota
`backend/src/publicidad.ts`; el plugin solo la muestra, con tres
comandos que solo valen desde la consola del servidor:

- `css_regie_ads_set <id> <parte> <total> <base64url>`: la placa, en JSON
  (`{"a":1,"i":"<url de la imagen>","w":360,"h":60,"l":[["texto","#rrggbb","m"]]}`),
  partida si no entra en una línea. Un color que no sea `#rrggbb`, o una
  URL con comillas o `<>`, se rechaza.
- `css_regie_ads_test <segundos>`: la muestra a todos un rato (máximo 60 s).
- `css_regie_ads_off`: la saca.

Contestan `regie-ads ok <versión>`; con un plugin anterior el backend
deja solo la frase del chat. Se ve a los muertos y a los espectadores, y
a todos en el calentamiento, el freezetime, las pausas, el entretiempo y
el final del mapa; nunca a un jugador vivo en plena ronda. El panel del
veto le gana, y al que tiene un menú del centro abierto no se le manda.
La imagen la baja el juego de cada jugador por HTTP desde el backend: para
jugadores por internet, el backend necesita una dirección pública. No
necesita `director_enabled`.

Desde la 1.11.0 el JSON puede traer `"x":"<SteamID64>"`: ese jugador (el
camarógrafo de la transmisión, el que tiene cargado el observador
automático) no la ve fuera de la prueba. El backend lo manda cuando en
Producción → Publicidad se apaga «Mostrar la placa también en la
transmisión». El GOTV no la ve nunca.

### La frase del sponsor en el chat (desde la 1.6.0)

- `css_regie_ads_chat <colorPrefijo> <colorTexto> <base64url>`: la frase
  en el chat de todos, como los mensajes de MatchZy: `[Prefijo] frase`,
  el prefijo de un color y la frase de otro, con tildes. El texto va en
  JSON (`{"p":"prefijo","t":"frase"}`) y base64url; el plugin le saca los
  caracteres de control (en el chat de CS2 los colores son caracteres de
  control) y lo corta si es muy largo. Contesta `regie-ads ok <versión>`.

Los colores van por nombre y el plugin solo acepta estos (cualquier otro
se rechaza): `default` (blanco), `green`, `lime`, `yellow`, `gold`,
`red`, `purple`, `lightblue` y `grey`. Son los de `ChatColors` de
CounterStrikeSharp sin los alias (`White`, `LightYellow`, `Blue`,
`Magenta`, `Orange`) y sin los que en el chat no se distinguen de otro
de la lista (`DarkRed` y `LightRed` de `Red`, `Silver` de `Grey`,
`Olive` de `Lime`, `LightPurple` de `Purple`) o se leen mal sobre el
fondo oscuro del chat (`DarkBlue`). La lista es la misma que
`COLORES_CHAT` en `backend/src/publicidadChat.ts`.

Con un plugin anterior a la 1.6.0, o si el comando no contesta
`regie-ads ok`, el backend manda la frase con `say` como antes (sin
prefijo ni colores, y sin tildes).

## El partido como lo quiere Regie.gg (desde la 1.9.0)

`PartidoRegie.cs`, con MatchZy intacto. Todos los comandos solo valen
desde la consola del servidor y contestan `regie-partido ok`:

- `css_regie_hold <0|1>`: inicio a cargo de la producción. Con el freno
  prendido, el `.ready` / `.r` / `!ready` de los jugadores queda anotado
  y no le llega a MatchZy (se frena el `say` y el `css_ready` antes de
  que los procese el juego). El partido arranca con el `css_start` de MatchZy.
  Vale para cada mapa de la serie; el backend lo apaga al terminarla.
- `css_regie_holdstatus`: quiénes pidieron listo, `steamid64|nombre`
  por línea.
- `css_regie_serie_on` / `css_regie_serie_off`: tapa (o deja de tapar)
  los mensajes de MatchZy sobre la serie y el final («is winning the
  series», «The series is tied at», «has won the match», «have tied the
  match»), que están escritos en su código en inglés y en una serie
  retomada salen mal. Se tapan interceptando el mensaje de chat
  (`TextMsg`) antes de que salga.
- `css_regie_serie_chat <base64url>`: el mensaje de la serie que manda el
  backend, con el prefijo `[Regie.gg]`; `{g}` y `{d}` pasan a verde y al
  color normal.

Sin el `on` (el partido no lo cargó Regie.gg), MatchZy dice lo suyo como
siempre.

## Por qué existe

`status` y `users` por RCON en CS2 no traen el SteamID de los
jugadores conectados -- a diferencia de CS:GO clásico, la tabla de
`status` ni siquiera tiene una columna para eso (confirmado a mano
contra un servidor real: el header es `id time ping loss state rate
adr name`, sin ningún campo de identidad). Sin ese dato, el panel de
[`../desktop`](../desktop) no puede ofrecer el botón de "asignar a
Team1/Team2/Spec" para un jugador realmente conectado.

CounterStrikeSharp sí tiene acceso a esa identidad real, porque habla
directo con la API del juego (`CCSPlayerController.SteamID`), no con
la consola de texto. Este plugin solo expone eso como un comando de
consola:

```
css_liststeamids
```

Devuelve una línea por jugador humano conectado (bots y SourceTV
excluidos), formato `<steamid64>|<nombre>`.

## Instalación

Es opcional -- todo el resto del panel funciona sin esto, solo que sin
completar el SteamID en "Jugadores conectados". Se instala desde el
panel de escritorio, sección "1. Instalación" → "Plugin auxiliar:
SteamID real de los jugadores conectados". Requiere tener el [SDK de
.NET](https://dotnet.microsoft.com/download) instalado (no hace falta
para nada más de este proyecto) -- el botón compila este proyecto
(`dotnet build -c Release`) y copia el `.dll` resultante a
`<csgo>/addons/counterstrikesharp/plugins/RegieGG/` (antes `Cs2LiveViewerHelper/`: al instalar, la carpeta
vieja se borra y su config pasa a `configs/plugins/RegieGG/`).

Para que cargue hace falta (re)iniciar el servidor, o correr
`css_plugins load RegieGG` por RCON si ya está corriendo.
Lo mismo para actualizarlo: el `.dll` cargado está bloqueado por el
proceso, así que instalar una versión nueva pide parar el servidor
(no hacerlo con un partido en curso).

## Desarrollo

```bash
cd cs2-plugin
dotnet build            # compila a bin/Debug/net8.0/
dotnet build -c Release # lo que usa el instalador del panel
```

La referencia a `CounterStrikeSharp.API` es la misma versión que usa
MatchZy en su propio `.csproj` (`1.0.342`) -- la API pública es
estable entre parches menores, no hace falta que coincida exacto con
el build instalado en el servidor.
