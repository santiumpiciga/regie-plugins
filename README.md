# regie-plugins

Los plugins de servidor de [Regie.gg](https://regie.gg), con su código fuente.
*The game-server plugins of Regie.gg, with source code.*

| Carpeta | Para | Qué es |
|---|---|---|
| `cs2-plugin/` | Counter-Strike 2 | Plugin de [CounterStrikeSharp](https://github.com/roflmuffin/CounterStrikeSharp) (C#, .NET 8): feed del estado del juego, eventos del partido, gestor del partido, veto y publicidad en pantalla. |
| `cs16-plugin/` | Counter-Strike 1.6 | Plugin de [AMX Mod X](https://www.amxmodx.org/) (`RegieGG.sma`): feed del estado del juego y gestor del partido. Necesita ReHLDS, ReGameDLL_CS, Metamod-R y ReAPI. |

Corren **dentro del servidor de juego** y le hablan a Regie.gg por TCP y por RCON.

## Licencia

**GPL-3.0 o posterior**, ver [`LICENSE`](LICENSE). Los dos se construyen sobre
una base GPL (CounterStrikeSharp y AMX Mod X); el resto de Regie.gg no está en
este repositorio ni se rige por esta licencia. Cada carpeta tiene además su
propio `LICENSE` con el aviso.

## Releases

Cada Release (`vX.Y.Z`) trae:

- `regie-cs2-plugin-vX.Y.Z.zip`: `RegieGG.dll`, `.deps.json`, la licencia y `fuente/`.
- `regie-cs16-plugin-vX.Y.Z.zip`: `RegieGG.amxx` y `fuente/` (el `.sma`, los `.inc` y la licencia).
- `SHA256SUMS.txt`: la huella de cada zip.

Cada versión de Regie.gg baja **un Release fijo** (no «el último») y verifica
la huella. Quien instala los plugins a mano: el `.dll` va en
`game/csgo/addons/counterstrikesharp/plugins/RegieGG/` y el `.amxx`
en `cstrike/addons/amxmodx/plugins/` (más su línea en `configs/plugins.ini`).

## Compilar

En Windows, con el SDK de .NET 8 o posterior:

```powershell
pwsh ./scripts/empaquetar.ps1 -Tag prueba
```

Arma los dos zips en `dist/`. Para CS 1.6 baja AMX Mod X 1.9.0.5303 y ReAPI
5.29.0.358 para tener `amxxpc` y los `include`; con `-ScriptingAmxx <carpeta
scripting>` se usa uno ya instalado. El instalador de Regie.gg recompila el
`.sma` con el `amxxpc` del propio servidor, para que el `.amxx` coincida con
la versión de AMX Mod X que tiene.

## De dónde sale este repositorio

El código vive en el proyecto de Regie.gg y este repositorio es su espejo
público, con el CI y los Releases. Los cambios entran desde allá.
