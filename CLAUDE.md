# Crests of Courage (fork) — contexto para continuar

Fork de `remiafterdark/crests-of-courage` (mod coop para Dusklight, Twilight Princess PC), base v1.10.0.
Se juega entre 2 personas: Santo es el **host**, su amigo es el **joiner** (`backtojuan`).
Responder en español.

## Ya hecho (commit `87e2aa6`, antes parche `crests-fixes-completo.patch`)

Compila en GitHub Actions en las 8 plataformas (run 41737354490), **pero no está probado en el juego**.

1. **Puertas con llave** — `src/world.cpp`: `unlock_opened_key_door()` / `unlock_opened_key_doors()`.
   Causa: `daDoor20_c` decide si está cerrada (`field_0x5f0`) solo en `setKey()` al cargar. Si otro jugador la
   abre con el jugador ya en la sala, el switch bit llega por sync pero el candado se queda. El arreglo recorre
   las puertas `fpcNm_DOOR20_e` con llave (front/back option == 2) cuyo switch ya está activo, pone
   `field_0x5f0 = false` y borra el actor del candado (`field_0x5ec`). Se ejecuta en `scan()` cuando hay sync de mazmorra.
2. **"Own small keys"** (opción, apagada por defecto) — `kSessKeepKeys`, `kRegionKeyGain = 11`.
   Las llaves recogidas se envían como suma (no como valor absoluto); usar una llave no se sincroniza.
3. **"Shared rupees"** — `features.cpp`: `update_shared_rupees`, `apply_rupee_delta`, `apply_rupee_total`.
   Se usa `rupia + dComIfGp_getItemRupeeCount()` (incluye lo que aún cuenta el medidor), se mandan deltas
   y el host manda el total cada 120 ticks para corregir diferencias. `join_sync.cpp` ya no conserva las
   rupias propias si la billetera es compartida.
4. **"Shared hearts"** — en `on_item_given`, `dItemNo_HEART_e` con origen GAME → `kMsgSharedHeal` →
   `dComIfGp_setItemLifeCount(4.0f, 0)` en los demás.
5. **Enemigos y pausas** — `mod.cpp`: `coop_player_returning()` (45 ticks de gracia después de cerrar un menú);
   `enemies.cpp`: `player_is_live`, `player_reachable` e `is_owned_locally` ceden el control mientras estás
   pausado o volviendo. `kUnpauseSnapTicks = 45`.
   Causa: al pausar, el jugador pausado seguía considerándose dueño de sus enemigos (vía `targetPlayer`),
   ignoraba el estado del otro y al volver los retomaba congelados en el momento de pausar.

Mensajes nuevos: `kMsgRupeeDelta = 79`, `kMsgRupeeTotal = 80`, `kMsgSharedHeal = 81`.
Flags de sesión nuevas: bits 9, 10 y 11. `kCoopProtocolVersion` pasó a 75.
Ambos jugadores deben usar el mismo build.

Release `v1.10.1` (commit `06cd53a`: lo anterior + versión). Probado el 2026-11-04 (Lanayru en crepúsculo):
las rupias compartidas funcionan; los conflictos de enemigos bajaron de 850–1.500/h a ~217/h.

## Portales de las Bestias Sombrías, Llaves/Puertas y Jefes (`v1.10.2`, 2026-11-09)

### 1. Portales de las Bestias Sombrías (`src/twilight.cpp`, `src/enemies.cpp`, `src/mod.hpp`)
Bug: si un jugador vence a las bestias de un portal y el otro está en el mismo escenario pero lejos, al otro
se le abre el portal y queda congelado hasta reiniciar. En el log del host: `[EVENT] 'WITHWARP_OPEN'` justo
al llegar el switch y `gameplay=0` durante 30+ s (dos veces: F_SP115 ~2453 s y F_SP122 ~3316 s).
Cadena (decomp `d_a_e_warpappear.cpp`, `d_a_kytag04.cpp`, `d_a_e_s1.cpp`):
1. `E_WAP` (inactivo) registra cada `E_S1` con `mAction != 20` y suma `target_info_count`. El sync de enemigos
   reescribía la acción de las copias en cada cambio de decisión del otro: contador inflado a cientos.
2. Llega el switch del portal → `kytag04` (tipo 4) lanza `WITHWARP_OPEN` y teletransporta al jugador al portal.
3. Ya cerca, `E_WAP` lanza su demo (Link en demo 23, `PROC_WOLF_ATN_AC_MOVE` en el `[HITCH]`) y suelta una
   bestia cada 20 frames `target_info_count` veces.
Arreglo (`twilight.cpp` `update_portals()` + `enemies.cpp` `send_state`/`apply_remote`):
- Una `E_S1` esperando en el cielo (`mAction 20`, `mMode <= 1`) no se envía ni recibe estado.
- Si llega estado activo del otro para una de ellas, se retira el `E_WAP` inactivo de su grupo y la bestia
  sigue al otro (`twilight_peer_started_beasts`).
- Con el portal abierto se borran las copias sobrantes (`mSwBit` activo, solo escenarios de campo), y también
  las que quedan en `ACT_FAIL` paso 20 sin demo propia; se retira el `E_WAP` inactivo sin bestias que soltar.
- La demo de soltar bestias nunca espera más bestias de las que quedan en el cielo.
- Si el switch se activa con el jugador a más de 4000 del portal, el portal aparece abierto sin cinemática.
Logs: `[PORTAL]`.

### 2. Puertas con llave, rejas del Templo del Agua y Puertas del Jefe (`src/world.cpp`)
- **Aclaración del menú (`Host -> Share -> Own small keys`):** Apagado por defecto = llaves compartidas (recoger o gastar una llave se sincroniza en ambos). Encendido = llaves individuales (recoger da llave a ambos, pero abrir una puerta solo gasta la llave de quien la abre).
- **Causa del bug en Templo del Agua (`D_MN01` sala 10):** La reja con candado (`Lv3shut00`) es `daObjKshtr_c` (`fpcNm_Obj_Kshutter_e`, `mType == 2`), no `daDoor20_c`. `daObjKshtr_c::actionWaitEvent()` nunca comprueba `fopAcM_isSwitch(this, mSwNo)` después de `Create()`, y `unlock_opened_key_door()` solo contemplaba `fpcNm_DOOR20_e`. Por eso al abrirla con llaves compartidas (`Own small keys` apagado), el otro jugador perdía la llave por sync (`region=4`) pero la reja seguía cerrada con candado hasta recargar la sala. Además, la **puerta del jefe del Templo del Agua (`K_l3bdoor`, `TYPE_L3BOSS`)** también es `daObjKshtr_c`.
- **Arreglo (`unlock_opened_key_door` / `unlock_opened_key_doors` en `src/world.cpp`):**
  - `fpcNm_DOOR20_e` (`daDoor20_c`): pasa `roomNo` a `dComIfGs_isSwitch` para soportar switches locales de sala (`0xC0..0xEF`).
  - `fpcNm_Obj_Kshutter_e` (`daObjKshtr_c`): cuando `mSwNo` se activa remotamente con `mIsCheckKey && mAction == 0`, abre la reja/puerta (`field_0x5ec = true`, frame final de `mBck` o `field_0x5fc = -450.0f` en `TYPE_L3BOSS`), libera colisión `mpBgW`, borra el candado hijo (`mKeyHoleId`), limpia `attention_info.flags = 0` y pasa a `mAction = 2` (`actionDead`). Cubre `D_MN01` sala 10, la puerta del jefe del Templo del Agua (`TYPE_L3BOSS`) y las jaulas del Templo del Bosque (`S_shut00`).
  - `fpcNm_L1MBOSS_DOOR_e` (`daMBdoorL1_c`): puertas de subjefe con llave pequeña (`frontOption == 2`), borra `mKeyHoleId`.
  - `fpcNm_L1BOSS_DOOR_e` (`daBdoorL1_c`), `fpcNm_L5BOSS_DOOR_e` (`daBdoorL5_c`), `fpcNm_BOSS_DOOR_e` (`daBdoor_c`): puertas de jefe estándar y de Snowpeak, borran el candado de Gran Llave y actualizan su estado/evento para que ambos jugadores puedan entrar tras desbloquearla.
  - `fpcNm_Obj_KkrGate_e` (`daObjKGate_c`) y `fpcNm_Obj_RiderGate_e` (`daObjRgate_c`): rejas con llave del mundo exterior, pasan de `mAction = 0` a `2` cuando su switch llega activo.
  - Se invoca `unlock_opened_key_doors()` también al aplicar regiones `kRegionMemory`, `kRegionDan`, `kRegionZone` y switches de mundo exterior.

### 3. Jefes y Subjefes (`src/boss.cpp`)
- **Por qué el sapo del Templo del Agua (`Deku Toad`, `daE_DT_c` / `fpcNm_E_DT_e` en `D_MN01B` sala 51) no se sincronizó:** `src/boss.cpp` actualmente solo implementa adaptadores de sincronización para **Ook** y **Diababa** (Templo del Bosque). El resto de jefes y subjefes están en `procname_is_boss()` (`src/enemies.cpp`), por lo que cada jugador los pelea localmente en su instancia, aunque al derrotarlos el flag de subjefe (`dComIfGs_onStageMiddleBoss`) y el switch del cofre/puertas sí se sincronizan por `world.cpp`.
- **Arreglos en `src/boss.cpp` (F1, F2, F3, F5, F6):**
  - **F1 (`decide_fight_owner`):** exige `!coop_player_returning(id) && player_ready(id)` en `playing(id)` y mantiene `s_fightOwner` mientras siga activo, evitando que el host al entrar tarde o despausar robe la autoridad y rebobine el jefe.
  - **F2 (`write_ook`):** si Ook local sigue en `ACT_S_DEMO` (10) cuando el dueño ya está en combate (`m.action != ACT_S_DEMO`), termina la intro (`DEMO_MODE_NONE` o `DEMO_MODE_FINISH`) para no dejar la cámara bloqueada.
  - **F3 (`write_diababa`):** sincroniza `actor->health = m.health` en ambas direcciones para que al pasar a fase 2 (`health = 50`) el otro jugador no conserve `health = 0` y mate a Diababa prematuramente.
  - **F5 (`fight_in_progress` y `s_fightStarted`):** incluye `kBossKindOok` (`action != ACT_S_DEMO`) y añade latch `s_fightStarted` para no volver a bloquear con `"Waiting for 1 Player"` a mitad de pelea.
  - **F6 (`collect_boss` y `retired`):** ignora actores aún en construcción (`fpcM_IsCreating`), excluye al Ook guionizado de `D_MN05` sala 4 y consulta `dComIfGs_isStageBossEnemy()` / `dComIfGs_isStageMiddleBoss()` en `retired`.

## Pendiente

1. **Probar en el juego (`v1.10.2`)** (con backup de saves):
   - portal: uno vence a las bestias y el otro lejos en el mismo escenario; luego llegar tarde a una pelea;
   - llaves pequeñas (mantener `"Own small keys"` apagado para compartir llaves) y puertas/rejas con llave o Gran Llave estando ambos en la sala;
   - pausar en medio de una pelea, corazones compartidos.
2. **Conflictos de enemigos (~217/h).** 45% sin pausa, carga de sala ni evento, sobre todo en zonas abiertas
   con voladores (lago Hylia sala 0, campo de Hyrule). En `[ENEMY-CONFLICT]` el host tiene `target=0` y el
   dueño de sala es el joiner, que igualmente manda estado. `retargets=N` (nuevo, al final de la línea
   `[ENEMY]`) cuenta cambios de objetivo. Hace falta el log del **joiner**
   (`%APPDATA%\TwilitRealm\Dusklight\mod_data\dev.remiafterdark.coop_mod\coop-log.txt` en su PC).
   Un "conflicto" es `[ENEMY-CONFLICT]`: llega estado del otro para un enemigo que creemos nuestro.
3. **Enemigos generados en ejecución** (leevers `E_RB` param 1, guays `E_GE`, bulblins `E_RD`): su clave es
   por secuencia (`compute_sequence_key`) y diverge entre juegos, cada uno ve los suyos (`[ENEMY-DYN] ...
   unknown to peer`, 82 en la sesión). Emparejarlos haría falta por proximidad; no se tocó.
4. **Sincronización de otros jefes/subjefes (Deku Toad, Morpheel, etc.):** actualmente `src/boss.cpp` solo tiene
   adaptadores para Ook y Diababa; añadir nuevos jefes requiere escribir un adaptador por actor en `src/boss.cpp`.
5. **Templo del Agua (D_MN01), baja prioridad.** Palanca colgante `daObjSwHang` + escalera `daObjRotStair_c`:
   la palanca "se activó dos veces" y la escalera giró dos veces, solo la primera vez (durante la cinemática
   con cámara centrada); después no se repitió. Hipótesis sin confirmar: en el juego del otro,
   `modeOffWait` trata `checkDown()` (switch ya activo por sync) como si alguien colgara, la palanca baja sola
   y ordena su evento; las de tipo 1 vuelven arriba y `changeOffStatus()` apaga el switch, y la escalera
   (`checkChangeSw` reacciona a cualquier cambio, también on→off) vuelve a girar. La cinemática de la
   escalera solo se ordena una vez (`ACTION_DEAD`), por eso luego no se nota.
6. **Dos jinetes en un caballo:** hoy no existe (el caballo del otro es solo un modelo; `epona.cpp`
   `enabled()` devuelve `false`). Factible pero grande: el pasajero monta su propia Epona oculta y pegada al
   caballo del conductor, en el asiento de Zelda (`daHoZelda_c`); hooks en `daHorse_c`/`daAlink_c`, mensaje
   nuevo y subir `kCoopProtocolVersion`.
7. Opcional: si se actualiza el mod original, rebasar estos cambios.

## Análisis de logs

Carpeta `%APPDATA%\TwilitRealm\Dusklight` (el juego está en `F:\Dusklight-v2.0.3-win32-x86_64`):
- `logs\dusklight-YYYYMMDD-HHMMSS.log`: log completo del motor y del mod, sin marcas de tiempo.
- `mod_data\dev.remiafterdark.coop_mod\coop-log.txt` (y `-1`, `-2`, `-3` de sesiones anteriores): solo las líneas
  del mod, con segundos desde el arranque. Mejor para cronologías.

Este script mide en qué situaciones sube el contador de conflictos (correr en `logs\`):

```python
import re, glob
from collections import Counter
tot = Counter()
for f in sorted(glob.glob('dusklight-*.log')):
    prev = None; since = set()
    for line in open(f, errors='ignore'):
        if '[PAUSE] local opened' in line: since.add('local_open')
        if '[PAUSE] local closed' in line: since.add('local_close')
        if '] opened a menu' in line or 'player 1 opened' in line: since.add('peer_open')
        if 'is back' in line: since.add('peer_back')
        if 'loaded, later actors' in line: since.add('roomload')
        m = re.search(r"\[ENEMY\] (?:host|joiner) .*?conflicts=(\d+)", line)
        if m:
            c = int(m.group(1))
            if prev is not None and c > prev: tot[tuple(sorted(since))] += c - prev
            prev = c; since = set()
for k, v in tot.most_common(): print(v, k)
```

Antes del arreglo: unos 6.500 conflictos, cerca del 80% con una pausa en el intervalo.

## Notas

- **CI**: `.github/workflows/build.yml` no corre con push a `main`. Se dispara con push a la rama `ci`
  (`git push origin main:ci`), con tags `v*` (crean release) o a mano desde la pestaña Actions (Run workflow).
  El `.dusk` combinado queda como artefacto `crests_of_courage` del run.
- **Chequeo local sin CI**: hay MSVC (`Visual Studio\18\BuildTools`, `vcvars64.bat`). Con el decomp de
  Dusklight en el commit de `DUSKLIGHT_VERSION` (sparse: `include`, `src`, `sdk`, `libs`, `assets/GZ2E01/res/**/*.h`),
  los headers de `extern/aurora` (commit del submódulo) y `fmt` 12.1.0, `cl /Zs /std:c++20 /EHsc /utf-8
  /Zc:__cplusplus /FIglobal.h` con los defines de `cmake/GameABIConfig.cmake` (`TARGET_PC=1 WIDESCREEN_SUPPORT=1
  AVOID_UB=1 VERSION=0 MTX_USE_PS=1 PARTIAL_DEBUG=1`) más `DUSK_MOD_FEATURE_GAME=1 DUSK_MOD_FEATURE_FMT=1
  FMT_HEADER_ONLY=1` revisa un `.cpp` del mod en segundos.
- `src/enemies.cpp` (~6.400 líneas) y `src/boss.cpp` son lo más delicado: hacer cambios pequeños y con logs.
- Los errores `HookSystem: funchook failed` del log son de otro mod (`twilit_essentials`), no de este.
- Los saves `.gci` tienen el nombre interno en la cabecera (bytes 0x08–0x27). El modo coop usa `crests-of-courage`.
