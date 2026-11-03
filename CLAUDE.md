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

## Pendiente

1. **Probar en el juego** (con backup de saves): puertas con llave estando ambos en la sala, pausar en
   medio de una pelea, rupias compartidas y corazones compartidos.
2. **El ~20% restante de conflictos de enemigos.** Ocurren al cargar salas o durante cinemáticas, sin pausa de por medio.
   Un "conflicto" es `[ENEMY-CONFLICT]`: llega estado del otro para un enemigo que creemos nuestro. El contador
   acumulado aparece en las líneas `[ENEMY] host stage=... conflicts=N`. Hipótesis: al entrar a una sala,
   `targetPlayer` y el dueño de la sala (`s_rooms.owner`) no coinciden entre los dos juegos durante unos ticks.
   Hace falta el log del **joiner**: solo hay logs del host.
3. **Jefes** (`boss.cpp`): no se tocó. Usa `coop_player_paused` para cambiar de dueño
   (`boss.cpp:655`, `:1980`). Seguramente tiene el mismo problema de volver de una pausa; revisar si
   conviene usar también `coop_player_returning`.
4. Opcional: si se actualiza el mod original, rebasar estos cambios.

## Análisis de logs

Los logs están en la carpeta de Dusklight como `dusklight-YYYYMMDD-HHMMSS.log`. Este script mide en qué
situaciones sube el contador de conflictos:

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
- `src/enemies.cpp` (~6.400 líneas) y `src/boss.cpp` son lo más delicado: hacer cambios pequeños y con logs.
- Los errores `HookSystem: funchook failed` del log son de otro mod (`twilit_essentials`), no de este.
- Los saves `.gci` tienen el nombre interno en la cabecera (bytes 0x08–0x27). El modo coop usa `crests-of-courage`.
